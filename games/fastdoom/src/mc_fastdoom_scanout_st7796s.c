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
#define MC_FD_CONTENT_Y 10

/*
 * Progressive FastDoom no longer needs the old 8-row temporal-lace
 * granularity. Twenty rows gives 15 windows for the 300-row active image.
 *
 * Two RGB565 strips let Core 0 convert strip N+1 while DMA is shifting strip
 * N over SPI. The 10 black rows above and below the active image are written
 * once by the existing startup clear and never retransmitted.
 */
#define MC_FD_SCAN_BLOCK_H 20
#define MC_FD_SCAN_BLOCKS \
    ((MC_FD_CONTENT_H + MC_FD_SCAN_BLOCK_H - 1) / MC_FD_SCAN_BLOCK_H)

typedef struct mc_fd_st7796s_scanout
{
    mr_pico_ili9341_t *lcd;
    const volatile uint16_t *palettes;

    volatile int running;
    volatile int displaying;

    /*
     * active_frame is immutable while a physical frame is transmitted.
     * pending_frame receives the newest completed Doom frame.
     */
    volatile unsigned int active_frame;
    volatile unsigned int pending_frame;
    volatile int pending_ready;
    volatile int pending_writing;

    volatile int active_palette;
    volatile int pending_palette;

    unsigned int dma_block;
    unsigned int dma_buffer;

    unsigned int prepared_block;
    unsigned int prepared_buffer;
    int prepared_valid;

    unsigned long blocks_completed;
    unsigned long frames_started;
    unsigned long frames_completed;
    unsigned long published_frames;
    unsigned long replaced_pending_frames;

    uint32_t rate_start_ms;
    unsigned long rate_start_frames;
    volatile unsigned int frame_hz10;

    volatile uint32_t last_service_us;
    volatile uint32_t max_service_us;
    volatile uint32_t last_publish_us;
    volatile uint32_t max_publish_us;
} mc_fd_st7796s_scanout_t;

static mc_fd_st7796s_scanout_t g_scan;

/*
 * Two coherent 320x200 INDEX8 snapshots in PSRAM:
 *
 *   active  - display pipeline reads this only
 *   pending - Doom publishes the newest completed frame here
 *
 * 128 KB total. Keeping them in PSRAM is what made the coherent renderer fit
 * alongside FastDoom + MicroWave in the RP2350's 512 KB SRAM.
 */
static uint8_t __uninitialized_psram("fastdoom_scanout")
    g_scan_frame[2][MC_FD_SRC_BYTES];

/*
 * Two 480x20 RGB565 strips = 38,400 bytes of SRAM.
 *
 * DMA owns one while Core 0 prepares the other. This is intentionally SRAM:
 * the SPI DMA gets a fast local source and the QMI/PSRAM traffic is limited to
 * reading the small INDEX8 source rows during conversion.
 */
static gfx_color_t
    g_scan_rows[2][MC_FD_PANEL_W * MC_FD_SCAN_BLOCK_H];

static int scan_clamp_palette(int palette)
{
    if (palette < 0)
        palette = 0;
    if (palette > 13)
        palette = 13;
    return palette;
}

static int scan_block_rows(unsigned int block)
{
    int first = (int)(block * MC_FD_SCAN_BLOCK_H);
    int remain = MC_FD_CONTENT_H - first;

    if (remain <= 0)
        return 0;

    if (remain > MC_FD_SCAN_BLOCK_H)
        remain = MC_FD_SCAN_BLOCK_H;

    return remain;
}

static void __not_in_flash_func(scan_convert_block)(
    gfx_color_t *dst,
    unsigned int block)
{
    const uint8_t *frame;
    const volatile uint16_t *pal;
    unsigned int active;
    int palette;
    int rows;
    int oy;

    active = g_scan.active_frame & 1u;
    frame = g_scan_frame[active];

    palette = scan_clamp_palette(g_scan.active_palette);
    pal = g_scan.palettes + palette * 256;

    rows = scan_block_rows(block);

    for (oy = 0; oy < rows; ++oy)
    {
        int content_y =
            (int)(block * MC_FD_SCAN_BLOCK_H) + oy;
        int sy = (content_y * 2) / 3;
        const uint8_t *src;
        gfx_color_t *row;
        int sx;
        int dx;

        if (sy < 0)
            sy = 0;
        if (sy >= MC_FD_SRC_H)
            sy = MC_FD_SRC_H - 1;

        src = frame + sy * MC_FD_SRC_W;
        row = dst + oy * MC_FD_PANEL_W;

        /*
         * Exact 320 -> 480 nearest-neighbor expansion.
         * Every source pair a,b becomes physical pixels a,a,b.
         */
        dx = 0;

        for (sx = 0; sx < MC_FD_SRC_W; sx += 2)
        {
            gfx_color_t a =
                (gfx_color_t)pal[src[sx + 0]];
            gfx_color_t b =
                (gfx_color_t)pal[src[sx + 1]];

            row[dx++] = a;
            row[dx++] = a;
            row[dx++] = b;
        }
    }
}

static void scan_update_rate(void)
{
    uint32_t now;
    uint32_t delta_ms;

    now = to_ms_since_boot(get_absolute_time());

    if (g_scan.rate_start_ms == 0u)
    {
        g_scan.rate_start_ms = now;
        g_scan.rate_start_frames =
            g_scan.frames_completed;
        return;
    }

    delta_ms = now - g_scan.rate_start_ms;

    if (delta_ms >= 1000u)
    {
        unsigned long frames =
            g_scan.frames_completed -
            g_scan.rate_start_frames;

        g_scan.frame_hz10 =
            (unsigned int)(
                (frames * 10000ul) / delta_ms);

        g_scan.rate_start_ms = now;
        g_scan.rate_start_frames =
            g_scan.frames_completed;
    }
}

static void scan_launch_block(
    unsigned int block,
    unsigned int buffer)
{
    int rows;
    int y;

    rows = scan_block_rows(block);

    if (rows <= 0)
        return;

    y = MC_FD_CONTENT_Y +
        (int)(block * MC_FD_SCAN_BLOCK_H);

    mr_pico_ili9341_flush_begin(
        NULL,
        0,
        y,
        MC_FD_PANEL_W,
        rows,
        g_scan_rows[buffer & 1u],
        g_scan.lcd);

    g_scan.dma_block = block;
    g_scan.dma_buffer = buffer & 1u;
}

static void scan_prepare_block(
    unsigned int block,
    unsigned int buffer)
{
    if (block >= (unsigned int)MC_FD_SCAN_BLOCKS)
    {
        g_scan.prepared_valid = 0;
        return;
    }

    scan_convert_block(
        g_scan_rows[buffer & 1u],
        block);

    g_scan.prepared_block = block;
    g_scan.prepared_buffer = buffer & 1u;
    g_scan.prepared_valid = 1;
}

/*
 * Start one progressive physical frame from active_frame.
 *
 * Block 0 is prepared before DMA starts. Block 1 is then converted while the
 * first block is physically crossing SPI, establishing the steady-state
 * convert/DMA overlap.
 */
static void scan_start_active_frame(void)
{
    if (!g_scan.running ||
        !g_scan.lcd ||
        g_scan.displaying)
    {
        return;
    }

    g_scan.displaying = 1;
    ++g_scan.frames_started;

    scan_convert_block(
        g_scan_rows[0],
        0u);

    scan_launch_block(
        0u,
        0u);

    if (MC_FD_SCAN_BLOCKS > 1)
        scan_prepare_block(1u, 1u);
    else
        g_scan.prepared_valid = 0;
}

/*
 * Swap a completed pending snapshot into active state.
 *
 * Caller must prevent DMA IRQ 1 from changing the same state concurrently.
 */
static int scan_latch_pending(void)
{
    unsigned int old_active;

    if (!g_scan.pending_ready ||
        g_scan.pending_writing)
    {
        return 0;
    }

    old_active = g_scan.active_frame & 1u;

    g_scan.active_frame =
        g_scan.pending_frame & 1u;

    g_scan.active_palette =
        g_scan.pending_palette;

    g_scan.pending_frame = old_active;
    g_scan.pending_ready = 0;

    __compiler_memory_barrier();

    return 1;
}

static void __not_in_flash_func(scan_dma_irq1)(void)
{
    uint32_t mask;
    uint32_t begin_us;
    uint32_t elapsed_us;
    unsigned int completed_block;
    unsigned int completed_buffer;

    if (!g_scan.lcd)
        return;

    mask = 1u << g_scan.lcd->dma_chan;

    if ((dma_hw->ints1 & mask) == 0u)
        return;

    begin_us = time_us_32();

    dma_hw->ints1 = mask;

    /*
     * DMA transfer count may reach zero before the last SPI bit leaves the
     * shifter. Finish the strip before changing the LCD window or reusing its
     * SRAM buffer.
     */
    mr_pico_ili9341_flush_wait(
        NULL,
        g_scan.lcd);

    completed_block = g_scan.dma_block;
    completed_buffer = g_scan.dma_buffer;

    ++g_scan.blocks_completed;

    if (completed_block + 1u >=
        (unsigned int)MC_FD_SCAN_BLOCKS)
    {
        /*
         * The ST7796S retains GRAM. Once this progressive frame is complete,
         * stop all LCD traffic unless Doom has a newer coherent frame waiting.
         */
        g_scan.displaying = 0;
        g_scan.prepared_valid = 0;

        ++g_scan.frames_completed;
        scan_update_rate();

        if (scan_latch_pending())
            scan_start_active_frame();
    }
    else
    {
        unsigned int next_block;
        unsigned int free_buffer;

        /*
         * The next strip was prepared while the completed DMA was running.
         * Launch it first so SPI immediately resumes, then fill the now-free
         * old DMA buffer with the strip after that.
         */
        if (!g_scan.prepared_valid)
        {
            /* Defensive fallback; normal steady state never needs this. */
            scan_prepare_block(
                completed_block + 1u,
                completed_buffer ^ 1u);
        }

        next_block = g_scan.prepared_block;

        scan_launch_block(
            g_scan.prepared_block,
            g_scan.prepared_buffer);

        free_buffer = completed_buffer;

        scan_prepare_block(
            next_block + 1u,
            free_buffer);
    }

    elapsed_us =
        time_us_32() - begin_us;

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
    int start_now;

    if (!source_index8)
        return;

    begin_us = time_us_32();

    /*
     * Protect only the state handoff, not the 64 KB PSRAM memcpy.
     *
     * The previous implementation masked display IRQ 1 across the whole copy
     * (~4 ms in current measurements), which could leave SPI idle. Here the
     * current physical frame continues while Doom copies the next snapshot.
     */
    irq_set_enabled(DMA_IRQ_1, false);

    if (g_scan.pending_ready)
        ++g_scan.replaced_pending_frames;

    g_scan.pending_ready = 0;
    g_scan.pending_writing = 1;

    target = g_scan.pending_frame & 1u;

    irq_set_enabled(DMA_IRQ_1, true);

    memcpy(
        g_scan_frame[target],
        (const void *)source_index8,
        MC_FD_SRC_BYTES);

    irq_set_enabled(DMA_IRQ_1, false);

    g_scan.pending_palette =
        scan_clamp_palette(palette_index);

    g_scan.pending_writing = 0;
    g_scan.pending_ready = 1;

    ++g_scan.published_frames;

    /*
     * If the prior physical frame ended during the PSRAM copy, start this one
     * now. Otherwise DMA IRQ 1 will pick it up at the current frame boundary.
     */
    start_now =
        g_scan.running &&
        !g_scan.displaying;

    if (start_now && scan_latch_pending())
        scan_start_active_frame();

    irq_set_enabled(DMA_IRQ_1, true);

    elapsed_us =
        time_us_32() - begin_us;

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

    initial_palette =
        scan_clamp_palette(*palette_index);

    memcpy(
        g_scan_frame[g_scan.active_frame],
        (const void *)source_index8,
        MC_FD_SRC_BYTES);

    g_scan.active_palette = initial_palette;
    g_scan.pending_palette = initial_palette;

    mask = 1u << lcd->dma_chan;

    dma_hw->ints1 = mask;

    dma_channel_set_irq1_enabled(
        lcd->dma_chan,
        true);

    irq_set_exclusive_handler(
        DMA_IRQ_1,
        scan_dma_irq1);

    /*
     * Display service can do palette expansion work. Put it below the default
     * IRQ priority so the short MicroWave DMA IRQ 0 can preempt it if both hit
     * Core 0 at the same time.
     */
    irq_set_priority(
        DMA_IRQ_1,
        PICO_LOWEST_IRQ_PRIORITY);

    irq_set_enabled(
        DMA_IRQ_1,
        true);

    g_scan.running = 1;

    /*
     * Draw the initial complete frame once. Thereafter presentation is driven
     * only by mc_fd_st7796s_scanout_publish().
     */
    scan_start_active_frame();

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

    g_scan.displaying = 0;
    g_scan.lcd = NULL;
}

/*
 * Compatibility with the temporary SCAN command. The A/B test is complete:
 * production FastDoom presentation is deliberately progressive only.
 */
int mc_fd_st7796s_scanout_set_phases(unsigned int phases)
{
    return phases == 1u ? 1 : 0;
}

unsigned int mc_fd_st7796s_scanout_get_phases(void)
{
    return 1u;
}

unsigned int mc_fd_st7796s_scanout_get_requested_phases(void)
{
    return 1u;
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
    return g_scan.frames_completed;
}

unsigned long mc_fd_st7796s_scanout_cycles(void)
{
    return g_scan.frames_completed;
}

unsigned long mc_fd_st7796s_scanout_published_frames(void)
{
    return g_scan.published_frames;
}

unsigned long mc_fd_st7796s_scanout_latched_frames(void)
{
    return g_scan.frames_started;
}

unsigned long mc_fd_st7796s_scanout_replaced_pending_frames(void)
{
    return g_scan.replaced_pending_frames;
}

unsigned int mc_fd_st7796s_scanout_phase_hz10(void)
{
    return g_scan.frame_hz10;
}

unsigned int mc_fd_st7796s_scanout_cycle_hz10(void)
{
    return g_scan.frame_hz10;
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
