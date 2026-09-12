#include "mc_fastdoom_scanout_st7796s.h"

#include "gfx.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/psram.h"
#include "pico/platform.h"
#include "pico/stdlib.h"

#include <stdint.h>
#include <string.h>

#define MC_FD_SRC_W 320
#define MC_FD_SRC_H 200
#define MC_FD_SRC_BYTES (MC_FD_SRC_W * MC_FD_SRC_H)

#define MC_FD_PANEL_W 480
#define MC_FD_PANEL_H 320
#define MC_FD_CONTENT_H 300
#define MC_FD_CONTENT_Y ((MC_FD_PANEL_H - MC_FD_CONTENT_H) / 2)

#define MC_FD_SCAN_BLOCK_H 8
#define MC_FD_SCAN_DEFAULT_PHASES 1u


typedef struct mc_fd_st7796s_scanout
{
    mr_pico_ili9341_t *lcd;
    const volatile uint16_t *palettes;

    volatile int running;

    /*
     * The IRQ reads active_frame only. Doom writes pending_frame only while
     * DMA IRQ 1 is masked. At a five-phase boundary the IRQ swaps indices.
     */
    volatile unsigned int active_frame;
    volatile unsigned int pending_frame;
    volatile int pending_ready;

    volatile int active_palette;
    volatile int pending_palette;

    volatile unsigned int phase_count;
    volatile unsigned int requested_phase_count;

    unsigned int phase;
    unsigned int block;
    unsigned int buffer_index;

    unsigned long blocks_completed;
    unsigned long phases_completed;
    unsigned long cycles_completed;

    unsigned long published_frames;
    unsigned long latched_frames;
    unsigned long replaced_pending_frames;

    uint32_t rate_start_ms;
    unsigned long rate_start_phases;
    unsigned long rate_start_cycles;

    volatile unsigned int phase_hz10;
    volatile unsigned int cycle_hz10;

    volatile uint32_t last_service_us;
    volatile uint32_t max_service_us;
    volatile uint32_t last_publish_us;
    volatile uint32_t max_publish_us;

    int launched_last_block;
} mc_fd_st7796s_scanout_t;

static mc_fd_st7796s_scanout_t g_scan;

/*
 * Two complete INDEX8 snapshots live in PSRAM rather than SRAM:
 *
 *   active  = immutable for one complete five-phase panel cycle
 *   pending = newest completed Doom frame
 *
 * 2 * 320 * 200 = 128,000 bytes.
 *
 * FastDoom already reserves 6 MiB of the Pico Plus 2's 8 MiB PSRAM for Z_Zone,
 * leaving ample headroom. Keeping these snapshots out of SRAM avoids pushing
 * the RP2350's 512 KiB SRAM image over the linker limit.
 */
static uint8_t __uninitialized_psram("fastdoom_scanout")
    g_scan_frame[2][MC_FD_SRC_BYTES];

/* Two 480x8 RGB565 DMA staging bands remain in fast SRAM: 15,360 bytes. */
static gfx_color_t g_scan_rows[MC_FD_PANEL_W * MC_FD_SCAN_BLOCK_H];

static int scan_valid_phases(unsigned int phases)
{
    return phases == 1u || phases == 2u || phases == 4u || phases == 5u;
}

static unsigned int scan_period_h(void)
{
    return MC_FD_SCAN_BLOCK_H * g_scan.phase_count;
}

static unsigned int scan_blocks_per_phase(void)
{
    unsigned int period = scan_period_h();
    return period ? (MC_FD_PANEL_H / period) : 1u;
}

static int scan_clamp_palette(int palette)
{
    if (palette < 0)
        palette = 0;
    if (palette > 13)
        palette = 13;
    return palette;
}

static void __not_in_flash_func(scan_convert_block)(
    gfx_color_t *dst,
    int panel_y)
{
    const uint8_t *frame;
    const volatile uint16_t *pal;
    unsigned int active;
    int palette;
    int oy;

    active = g_scan.active_frame & 1u;
    frame = g_scan_frame[active];

    palette = scan_clamp_palette(g_scan.active_palette);
    pal = g_scan.palettes + palette * 256;

    for (oy = 0; oy < MC_FD_SCAN_BLOCK_H; ++oy)
    {
        int py = panel_y + oy;
        gfx_color_t *row = dst + oy * MC_FD_PANEL_W;

        if (py < MC_FD_CONTENT_Y ||
            py >= MC_FD_CONTENT_Y + MC_FD_CONTENT_H)
        {
            int x;

            for (x = 0; x < MC_FD_PANEL_W; ++x)
                row[x] = GFX_RGB565_BLACK;
        }
        else
        {
            int content_y = py - MC_FD_CONTENT_Y;
            int sy = (content_y * 2) / 3;
            const uint8_t *src;
            int sx;
            int dx;

            if (sy < 0)
                sy = 0;
            if (sy >= MC_FD_SRC_H)
                sy = MC_FD_SRC_H - 1;

            src = frame + sy * MC_FD_SRC_W;

            /*
             * Exact 3:2 horizontal nearest-neighbor expansion:
             * two source pixels become three physical pixels.
             */
            dx = 0;
            for (sx = 0; sx < MC_FD_SRC_W; sx += 2)
            {
                gfx_color_t a = (gfx_color_t)pal[src[sx + 0]];
                gfx_color_t b = (gfx_color_t)pal[src[sx + 1]];

                row[dx++] = a;
                row[dx++] = a;
                row[dx++] = b;
            }
        }
    }
}

static int scan_current_y(void)
{
    return (int)(
        g_scan.phase * MC_FD_SCAN_BLOCK_H +
        g_scan.block * scan_period_h());
}

static void scan_advance(void)
{
    ++g_scan.block;

    if (g_scan.block >= scan_blocks_per_phase())
    {
        g_scan.block = 0u;
        ++g_scan.phase;

        if (g_scan.phase >= g_scan.phase_count)
            g_scan.phase = 0u;
    }
}

static void scan_update_rates(void)
{
    uint32_t now;
    uint32_t delta_ms;

    now = to_ms_since_boot(get_absolute_time());

    if (g_scan.rate_start_ms == 0u)
    {
        g_scan.rate_start_ms = now;
        g_scan.rate_start_phases = g_scan.phases_completed;
        g_scan.rate_start_cycles = g_scan.cycles_completed;
        return;
    }

    delta_ms = now - g_scan.rate_start_ms;

    if (delta_ms >= 1000u)
    {
        unsigned long phase_delta =
            g_scan.phases_completed - g_scan.rate_start_phases;
        unsigned long cycle_delta =
            g_scan.cycles_completed - g_scan.rate_start_cycles;

        g_scan.phase_hz10 =
            (unsigned int)((phase_delta * 10000ul) / delta_ms);

        g_scan.cycle_hz10 =
            (unsigned int)((cycle_delta * 10000ul) / delta_ms);

        g_scan.rate_start_ms = now;
        g_scan.rate_start_phases = g_scan.phases_completed;
        g_scan.rate_start_cycles = g_scan.cycles_completed;
    }
}

/*
 * Called only from DMA IRQ 1 immediately after phase 4 has fully completed and
 * before phase 0 of the next physical cycle is converted.
 */
static void __not_in_flash_func(scan_latch_pending_at_cycle_boundary)(void)
{
    unsigned int old_active;

    if (g_scan.pending_ready)
    {
        old_active = g_scan.active_frame & 1u;

        g_scan.active_frame = g_scan.pending_frame & 1u;
        g_scan.active_palette = g_scan.pending_palette;

        g_scan.pending_frame = old_active;
        g_scan.pending_ready = 0;

        ++g_scan.latched_frames;
    }

    /*
     * A scan-mode request is independent of whether Doom happened to publish a
     * new frame before this cycle boundary.
     */
    if (scan_valid_phases(g_scan.requested_phase_count) &&
        g_scan.requested_phase_count != g_scan.phase_count)
    {
        g_scan.phase_count = g_scan.requested_phase_count;
        g_scan.phase = 0u;
        g_scan.block = 0u;
    }

    __compiler_memory_barrier();
}

static void scan_launch_next(void)
{
    int y;

    if (!g_scan.running || !g_scan.lcd)
        return;

    y = scan_current_y();
    scan_convert_block(g_scan_rows, y);

    g_scan.launched_last_block =
        (g_scan.block + 1u == scan_blocks_per_phase()) ? 1 : 0;

    mr_pico_ili9341_flush_begin(
        NULL,
        0,
        y,
        MC_FD_PANEL_W,
        MC_FD_SCAN_BLOCK_H,
        g_scan_rows,
        g_scan.lcd);
    scan_advance();
}

static void __not_in_flash_func(scan_dma_irq1)(void)
{
    uint32_t mask;
    uint32_t begin_us;
    uint32_t elapsed_us;

    if (!g_scan.lcd)
        return;

    mask = 1u << g_scan.lcd->dma_chan;

    if ((dma_hw->ints1 & mask) == 0u)
        return;

    begin_us = time_us_32();

    dma_hw->ints1 = mask;

    /*
     * DMA count can reach zero before the last SPI word exits the shifter.
     * Finish the band before changing CS, D/C or SPI word size.
     */
    mr_pico_ili9341_flush_wait(NULL, g_scan.lcd);

    ++g_scan.blocks_completed;

    if (g_scan.launched_last_block)
    {
        ++g_scan.phases_completed;

        /*
         * scan_advance() ran when this band was launched. phase==0 therefore
         * means the just-completed band was the final band of phase 4.
         */
        if (g_scan.phase == 0u)
        {
            ++g_scan.cycles_completed;

            /*
             * Critical coherency rule:
             * swap Doom frames only here, never between phase 0..4.
             */
            scan_latch_pending_at_cycle_boundary();
        }

        scan_update_rates();
    }

    if (g_scan.running)
        scan_launch_next();

    elapsed_us = time_us_32() - begin_us;

    g_scan.last_service_us = elapsed_us;

    if (elapsed_us > g_scan.max_service_us)
        g_scan.max_service_us = elapsed_us;
}

void mc_fd_st7796s_scanout_publish(
    const volatile uint8_t *source_index8,
    int palette_index)
{
    uint32_t begin_us;
    uint32_t elapsed_us;
    unsigned int target;
    int was_running;

    if (!source_index8)
        return;

    begin_us = time_us_32();

    /*
     * scan_latch_pending_at_cycle_boundary() runs in DMA IRQ 1 on this core.
     * Mask it while selecting and replacing the pending frame so it cannot
     * swap the buffer halfway through the 64 KB memcpy.
     *
     * Audio IRQ 0/Core 1 is unaffected.
     */
    was_running = g_scan.running ? 1 : 0;

    if (was_running)
        irq_set_enabled(DMA_IRQ_1, false);

    if (g_scan.pending_ready)
        ++g_scan.replaced_pending_frames;

    target = g_scan.pending_frame & 1u;

    memcpy(
        g_scan_frame[target],
        (const void *)source_index8,
        MC_FD_SRC_BYTES);

    g_scan.pending_palette = scan_clamp_palette(palette_index);
    __compiler_memory_barrier();

    g_scan.pending_ready = 1;
    ++g_scan.published_frames;

    if (was_running)
        irq_set_enabled(DMA_IRQ_1, true);

    elapsed_us = time_us_32() - begin_us;

    g_scan.last_publish_us = elapsed_us;

    if (elapsed_us > g_scan.max_publish_us)
        g_scan.max_publish_us = elapsed_us;
}

int mc_fd_st7796s_scanout_start(
    mr_pico_ili9341_t *lcd,
    const volatile uint8_t *source_index8,
    const volatile uint16_t *palette565,
    const volatile int *palette_index)
{
    uint32_t mask;
    int initial_palette;

    if (!lcd ||
        !source_index8 ||
        !palette565 ||
        !palette_index)
    {
        return 0;
    }

    if (g_scan.running)
        return 1;

    memset(&g_scan, 0, sizeof(g_scan));
    memset(g_scan_frame, 0, sizeof(g_scan_frame));
    memset(g_scan_rows, 0, sizeof(g_scan_rows));

    g_scan.lcd = lcd;
    g_scan.palettes = palette565;

    g_scan.active_frame = 0u;
    g_scan.pending_frame = 1u;
    g_scan.phase_count = MC_FD_SCAN_DEFAULT_PHASES;
    g_scan.requested_phase_count = MC_FD_SCAN_DEFAULT_PHASES;

    initial_palette = scan_clamp_palette(*palette_index);

    memcpy(
        g_scan_frame[g_scan.active_frame],
        (const void *)source_index8,
        MC_FD_SRC_BYTES);

    g_scan.active_palette = initial_palette;
    g_scan.pending_palette = initial_palette;
    g_scan.pending_ready = 0;

    mask = 1u << lcd->dma_chan;

    dma_hw->ints1 = mask;

    dma_channel_set_irq1_enabled(
        lcd->dma_chan,
        true);

    irq_set_exclusive_handler(
        DMA_IRQ_1,
        scan_dma_irq1);

    irq_set_enabled(
        DMA_IRQ_1,
        true);

    g_scan.running = 1;

    scan_launch_next();

    return 1;
}

void mc_fd_st7796s_scanout_stop(void)
{
    uint32_t mask;

    if (!g_scan.lcd)
    {
        memset(&g_scan, 0, sizeof(g_scan));
        return;
    }

    g_scan.running = 0;

    mask = 1u << g_scan.lcd->dma_chan;

    dma_channel_set_irq1_enabled(
        g_scan.lcd->dma_chan,
        false);

    irq_set_enabled(
        DMA_IRQ_1,
        false);

    if (g_scan.lcd->dma_active)
        dma_channel_abort(g_scan.lcd->dma_chan);

    dma_hw->ints1 = mask;

    mr_pico_ili9341_flush_wait(
        NULL,
        g_scan.lcd);

    g_scan.lcd = NULL;
}

int mc_fd_st7796s_scanout_set_phases(unsigned int phases)
{
    int was_running;

    if (!scan_valid_phases(phases))
        return 0;

    was_running = g_scan.running ? 1 : 0;

    if (was_running)
        irq_set_enabled(DMA_IRQ_1, false);

    g_scan.requested_phase_count = phases;
    __compiler_memory_barrier();

    if (!was_running)
        g_scan.phase_count = phases;

    if (was_running)
        irq_set_enabled(DMA_IRQ_1, true);

    return 1;
}

unsigned int mc_fd_st7796s_scanout_get_phases(void)
{
    return g_scan.phase_count;
}

unsigned int mc_fd_st7796s_scanout_get_requested_phases(void)
{
    return g_scan.requested_phase_count;
}

int mc_fd_st7796s_scanout_running(void)
{
    return g_scan.running ? 1 : 0;
}

unsigned long mc_fd_st7796s_scanout_blocks(void)
{
    return g_scan.blocks_completed;
}

unsigned long mc_fd_st7796s_scanout_phases(void)
{
    return g_scan.phases_completed;
}

unsigned long mc_fd_st7796s_scanout_cycles(void)
{
    return g_scan.cycles_completed;
}

unsigned long mc_fd_st7796s_scanout_published_frames(void)
{
    return g_scan.published_frames;
}

unsigned long mc_fd_st7796s_scanout_latched_frames(void)
{
    return g_scan.latched_frames;
}

unsigned long mc_fd_st7796s_scanout_replaced_pending_frames(void)
{
    return g_scan.replaced_pending_frames;
}

unsigned int mc_fd_st7796s_scanout_phase_hz10(void)
{
    return g_scan.phase_hz10;
}

unsigned int mc_fd_st7796s_scanout_cycle_hz10(void)
{
    return g_scan.cycle_hz10;
}

uint32_t mc_fd_st7796s_scanout_last_service_us(void)
{
    return g_scan.last_service_us;
}

uint32_t mc_fd_st7796s_scanout_max_service_us(void)
{
    return g_scan.max_service_us;
}

uint32_t mc_fd_st7796s_scanout_last_publish_us(void)
{
    return g_scan.last_publish_us;
}

uint32_t mc_fd_st7796s_scanout_max_publish_us(void)
{
    return g_scan.max_publish_us;
}
