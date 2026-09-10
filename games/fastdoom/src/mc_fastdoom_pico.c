/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FastDoom -> MicroConsole Pico 2 platform boundary.
 *
 * Milestone scope:
 *   - real RP2350 system/stdio startup
 *   - proven 6 MiB FastDoom Z_Zone in Pico Plus 2 PSRAM
 *   - real 320x200 INDEX8 -> RGB565 ILI9341 presentation
 *   - real MicroWave Pico audio transport servicing
 *   - serial diagnostics / temporary key injection
 *
 * SD/FatFS is intentionally NOT implemented here. The first real target is
 * meant to compile and link the complete game before storage is introduced.
 */

#include "gfx.h"
#include "mr_pico_ili9341.h"
#include "snd.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/psram.h"
#include "hardware/spi.h"
#include "hardware/watchdog.h"
#include "hardware/regs/clocks.h"
#include "hardware/regs/qmi.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/time.h"

#ifdef true
#undef true
#endif
#ifdef false
#undef false
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d_event.h"
#include "d_main.h"
#include "doomdef.h"
#include "doomstat.h"
#include "g_game.h"
#include "i_debug.h"
#include "i_gamma.h"
#include "i_ibm.h"
#include "i_system.h"
#include "m_misc.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "mc_fastdoom_audio.h"
#include "mc_fastdoom_audio_pico.h"
#include "mc_fastdoom_pico_wad.h"

/* Implemented by mc_fastdoom_pico_fs.c. */
int mc_fd_pico_fs_ready(void);
int mc_fd_pico_fs_sd_driver_ok(void);
int mc_fd_pico_fs_mount_result(void);
int mc_fd_pico_fs_last_result(void);
unsigned int mc_fd_pico_fs_mount_attempts(void);
const char *mc_fd_pico_fs_last_path(void);
int mc_fd_pico_fs_probe_wad(void);
int mc_fd_pico_fs_raw_probe(void);
int mc_fd_pico_fs_raw_dstatus(void);
int mc_fd_pico_fs_raw_card_type(void);
int mc_fd_pico_fs_raw_miso(void);

#ifndef MC_SYS_KHZ
#define MC_SYS_KHZ 300000
#endif

#ifndef MC_FD_PERI_KHZ
#define MC_FD_PERI_KHZ 300000
#endif

#define MC_FD_SAFE_SYS_KHZ 300000u
#define MC_FD_FLASH_MAX_HZ 88000000u

#ifndef MC_FD_PSRAM_ZONE_BYTES
#define MC_FD_PSRAM_ZONE_BYTES 6291456u
#endif

#define MC_FD_WIDTH       320
#define MC_FD_HEIGHT      200
#define MC_FD_LCD_HEIGHT  240
#define MC_FD_LCD_Y       ((MC_FD_LCD_HEIGHT - MC_FD_HEIGHT) / 2)
#define MC_FD_PALETTES    14
#define MC_FD_LCD_CHUNK_H  8
#define MC_FD_FRAME_US    (1000000u / TICRATE)

_Static_assert(
    (MC_FD_PSRAM_ZONE_BYTES % sizeof(uint32_t)) == 0u,
    "FastDoom PSRAM zone must be uint32_t aligned");

volatile unsigned int ticcount_hr = 0;
volatile unsigned int ticcount = 0;
unsigned int fps = 0;

unsigned int hasCPUID = 0;
unsigned int hasFPU = 0;
unsigned int hasMMX = 0;

int updatestate = I_NOUPDATE;
byte *pcscreen = 0;
byte *destscreen = 0;
byte *destview = 0;
unsigned short *currentscreen = 0;

volatile int TS_InInterrupt = 0;

static byte __uninitialized_psram("fastdoom_zone")
    mc_fd_zone[MC_FD_PSRAM_ZONE_BYTES];

/*
 * Two small SRAM staging buffers replace the old full 320x200 RGB565 PSRAM
 * presentation surface.  While SPI DMA transmits one 8-row chunk, Core 0
 * converts the next chunk into the other buffer.
 */
static gfx_color_t
    mc_fd_lcd_rows[2][MC_FD_WIDTH * MC_FD_LCD_CHUNK_H];

static uint16_t mc_fd_palette565[MC_FD_PALETTES][256];

static mr_pico_ili9341_t mc_fd_lcd = {
    .spi = MR_LCD_SPI,
    .dma_chan = 0u,
    .pin_miso = MR_LCD_PIN_MISO,
    .pin_cs = MR_LCD_PIN_CS,
    .pin_sck = MR_LCD_PIN_SCK,
    .pin_mosi = MR_LCD_PIN_MOSI,
    .pin_rst = MR_LCD_PIN_RST,
    .pin_dc = MR_LCD_PIN_DC,
    .spi_baud_hz = MR_LCD_SPI_BAUD,
    .x_offset = 0,
    .y_offset = 0,
    .dma_active = 0u,
    .spi_format_bits = 0u,
    .dma_cfg16 = {0}};

static int mc_fd_graphics_ready;
static int mc_fd_panel_ready;
static int mc_fd_palette;
static int mc_fd_fatal;
static int mc_fd_fatal_line = -1;
static int mc_fd_psram_config_rc;
static int mc_fd_psram_reinit_rc;
static int mc_fd_clock_fallback;

static repeating_timer_t mc_fd_tic_timer;
static int mc_fd_tic_timer_active;

static uint32_t mc_fd_last_frame_us;
static uint32_t mc_fd_max_frame_us;
static uint32_t mc_fd_last_convert_us;
static uint32_t mc_fd_last_lcd_us;

/*
 * Time spent in Doom between completed presentations.  This captures game
 * tics + R_RenderPlayerView() + HUD/menu work that happens before the next
 * I_FinishUpdate(), which the old frame_us metric did not include.
 */
static uint64_t mc_fd_last_finish_return_us;
static uint64_t mc_fd_last_finish_entry_us;
static uint32_t mc_fd_last_render_us;
static uint32_t mc_fd_max_render_us;
static uint32_t mc_fd_last_period_us;
static uint32_t mc_fd_max_period_us;
static uint64_t mc_fd_next_frame_us;
static uint64_t mc_fd_fps_start_us;
static unsigned int mc_fd_fps_frames;

static char mc_fd_cmd[64];
static unsigned int mc_fd_cmd_n;

static uint16_t mc_fd_rgb565(unsigned int r, unsigned int g, unsigned int b)
{
    unsigned int r5 = (r * 31u + 127u) / 255u;
    unsigned int g6 = (g * 63u + 127u) / 255u;
    unsigned int b5 = (b * 31u + 127u) / 255u;

    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

static void __no_inline_not_in_flash_func(mc_fd_set_flash_timings)(
    uint32_t sys_khz)
{
    uint32_t clock_hz = sys_khz * 1000u;
    uint32_t divisor;
    uint32_t rxdelay;

    divisor =
        (clock_hz + MC_FD_FLASH_MAX_HZ -
         (MC_FD_FLASH_MAX_HZ >> 4) - 1u) /
        MC_FD_FLASH_MAX_HZ;

    if (divisor < 1u)
        divisor = 1u;
    if (divisor == 1u && clock_hz >= 166000000u)
        divisor = 2u;

    rxdelay = divisor;
    if ((clock_hz / divisor) > 100000000u &&
        clock_hz >= 166000000u)
        ++rxdelay;

    qmi_hw->m[0].timing =
        0x60007000u |
        (rxdelay << QMI_M0_TIMING_RXDELAY_LSB) |
        (divisor << QMI_M0_TIMING_CLKDIV_LSB);
}

static void mc_fd_prepare_fast_clock(void)
{
    if ((uint32_t)MC_SYS_KHZ <= 252000u)
        return;

    vreg_disable_voltage_limit();

    if ((uint32_t)MC_SYS_KHZ >= 378000u)
        vreg_set_voltage(VREG_VOLTAGE_1_60);
    else
        vreg_set_voltage(VREG_VOLTAGE_1_50);

    sleep_ms(100);
    mc_fd_set_flash_timings((uint32_t)MC_SYS_KHZ);
}

static void mc_fd_configure_peripheral_clock(void)
{
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t peri_hz = (uint32_t)MC_FD_PERI_KHZ * 1000u;

    if (sys_hz == 0u)
        return;

    if (peri_hz == 0u || peri_hz > sys_hz)
        peri_hz = sys_hz;

    /*
     * Keep peripherals at the proven 300 MHz clock while the M33 cores run at
     * 378 MHz. RP2350 clk_peri has a fractional divider, so this restores the
     * exact 75 MHz SPI0 baud that we had at the old 300 MHz system clock.
     */
    if (!clock_configure(clk_peri, 0,
                         CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                         sys_hz, peri_hz))
    {
        (void)clock_configure(clk_peri, 0,
                              CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                              sys_hz, sys_hz);
    }

    sleep_ms(2);
}

static void mc_fd_recover_warm_boot(void)
{
    multicore_reset_core1();

    irq_set_enabled(DMA_IRQ_0, false);
    irq_set_enabled(DMA_IRQ_1, false);

    dma_hw->inte0 = 0u;
    dma_hw->inte1 = 0u;
    dma_hw->abort = (uint32_t)~0u;
    while (dma_hw->abort)
        tight_loop_contents();

    dma_hw->ints0 = (uint32_t)~0u;
    dma_hw->ints1 = (uint32_t)~0u;
}

void MC_FastDoomPicoBoot(void)
{
    mc_fd_recover_warm_boot();

    mc_fd_clock_fallback = 0;
    mc_fd_prepare_fast_clock();

    if (!set_sys_clock_khz(MC_SYS_KHZ, false))
    {
        mc_fd_set_flash_timings(MC_FD_SAFE_SYS_KHZ);
        if (!set_sys_clock_khz(MC_FD_SAFE_SYS_KHZ, false))
            panic("FastDoom Pico: requested and fallback clocks were rejected");
        mc_fd_clock_fallback = 1;
    }

    /*
     * hardware_psram is initialized by the SDK runtime before main(), using
     * the system clock that existed at that time.  FastDoom then changes
     * clk_sys to 300 MHz.  QMI PSRAM timing is expressed in system-clock
     * cycles, so recalculate and reapply it after the clock change before
     * touching the Doom zone.
     */
    mc_fd_psram_config_rc = psram_configure_params(
        PICO_DEFAULT_PSRAM_MAX_FREQ,
        PICO_DEFAULT_PSRAM_MAX_SELECT,
        PICO_DEFAULT_PSRAM_MIN_DESELECT);

    if (mc_fd_psram_config_rc == PICO_OK)
        mc_fd_psram_reinit_rc = psram_reinitialize();
    else
        mc_fd_psram_reinit_rc = PICO_ERROR_PRECONDITION_NOT_MET;

    mc_fd_configure_peripheral_clock();
    stdio_init_all();
    sleep_ms(250);

    printf("MCFDOOM1 boot sys=%lu clock_fallback=%d psram=%lu zone=%u "
           "zone_start=%p zone_end=%p psram_cfg=%d psram_reinit=%d\n",
           (unsigned long)clock_get_hz(clk_sys),
           mc_fd_clock_fallback,
           (unsigned long)psram_get_size(),
           (unsigned int)MC_FD_PSRAM_ZONE_BYTES,
           (void *)&mc_fd_zone[0],
           (void *)&mc_fd_zone[MC_FD_PSRAM_ZONE_BYTES - 1u],
           mc_fd_psram_config_rc,
           mc_fd_psram_reinit_rc);

    if (mc_fd_psram_config_rc != PICO_OK ||
        mc_fd_psram_reinit_rc != PICO_OK ||
        !psram_is_available() ||
        psram_get_size() < (size_t)MC_FD_PSRAM_ZONE_BYTES ||
        !psram_check_address((void *)&mc_fd_zone[0]) ||
        !psram_check_address(
            (void *)&mc_fd_zone[MC_FD_PSRAM_ZONE_BYTES - 1u]))
    {
        printf("MCFDOOM1 fatal=psram-zone-invalid\n");
        fflush(stdout);
        for (;;)
            tight_loop_contents();
    }

    /*
     * Bring the physical LCD out of its white power-on state before Doom needs
     * any WAD data.  If startup later fails in the filesystem path, I_Error()
     * changes this to a solid red diagnostic screen.
     */
    mr_pico_ili9341_init(&mc_fd_lcd);
    mr_pico_ili9341_panel_init(&mc_fd_lcd);
    mr_pico_ili9341_fill_screen(
        &mc_fd_lcd, GFX_RGB565_BLACK, MC_FD_WIDTH, MC_FD_LCD_HEIGHT);
    mc_fd_panel_ready = 1;

    printf("MCFDOOM1 psram=ok lcd=black sd=probing\n");
    fflush(stdout);
}

static void __not_in_flash_func(mc_fd_convert_lcd_rows)(
    gfx_color_t *dst,
    int first_y,
    int rows)
{
    const byte *src;
    const uint16_t *pal;
    unsigned int count;
    unsigned int i;

    src = &backbuffer[first_y * MC_FD_WIDTH];
    pal = mc_fd_palette565[mc_fd_palette];
    count = (unsigned int)(rows * MC_FD_WIDTH);

    for (i = 0u; i < count; ++i)
        dst[i] = pal[src[i]];
}

static void mc_fd_post_key(int type, int key)
{
    event_t ev;

    ev.type = (byte)type;
    ev.data1 = key;
    ev.data2 = 0;
    D_PostEvent(&ev);
}

static int mc_fd_key_from_name(const char *name)
{
    if (strcmp(name, "UP") == 0)
        return KEY_UPARROW;
    if (strcmp(name, "DOWN") == 0)
        return KEY_DOWNARROW;
    if (strcmp(name, "LEFT") == 0)
        return KEY_LEFTARROW;
    if (strcmp(name, "RIGHT") == 0)
        return KEY_RIGHTARROW;
    if (strcmp(name, "FIRE") == 0)
        return KEY_RCTRL;
    if (strcmp(name, "USE") == 0)
        return ' ';
    if (strcmp(name, "ESC") == 0 || strcmp(name, "ESCAPE") == 0)
        return KEY_ESCAPE;
    if (strcmp(name, "ENTER") == 0)
        return KEY_ENTER;

    return 0;
}

static int mc_fd_parse_volume(const char *cmd)
{
    int value = 0;
    const char *p = cmd;

    if (!((p[0] == 'V' || p[0] == 'v') &&
          (p[1] == 'O' || p[1] == 'o') &&
          (p[2] == 'L' || p[2] == 'l')))
        return -1;

    p += 3;
    while (*p == ' ')
        ++p;

    if (*p < '0' || *p > '9')
        return -1;

    while (*p >= '0' && *p <= '9')
    {
        value = value * 10 + (*p - '0');
        if (value > 100)
            value = 100;
        ++p;
    }

    return value;
}

static int mc_fd_cmd_equal_ci(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    if (!a || !b)
        return 0;

    while (*a && *b)
    {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;

        if (ca >= 'a' && ca <= 'z')
            ca = (unsigned char)(ca - ('a' - 'A'));
        if (cb >= 'a' && cb <= 'z')
            cb = (unsigned char)(cb - ('a' - 'A'));

        if (ca != cb)
            return 0;
    }

    return *a == '\0' && *b == '\0';
}

static int mc_fd_cmd_prefix_ci(const char *text, const char *prefix)
{
    unsigned char a;
    unsigned char b;

    if (!text || !prefix)
        return 0;

    while (*prefix)
    {
        if (!*text)
            return 0;

        a = (unsigned char)*text++;
        b = (unsigned char)*prefix++;

        if (a >= 'a' && a <= 'z')
            a = (unsigned char)(a - ('a' - 'A'));
        if (b >= 'a' && b <= 'z')
            b = (unsigned char)(b - ('a' - 'A'));

        if (a != b)
            return 0;
    }

    return 1;
}

static void mc_fd_handle_command(const char *cmd)
{
    int volume;

    if (mc_fd_cmd_equal_ci(cmd, "PING"))
    {
        /*
         * Keep liveness probes deliberately short.  The host uses PING while
         * detecting a freshly reset target, so it should not perturb a 15-30
         * FPS game with hundreds of bytes of printf traffic.
         */
        printf("MCFDOOM1 state=%s audio=%d core1=%d fs=%d volume=%d\n",
               mc_fd_fatal ? "fatal" : "running",
               mc_fd_audio_transport_ready(),
               mc_fd_audio_pico_core1_running(),
               mc_fd_pico_fs_ready(),
               snd_vol_to_percent(mc_fd_audio_master_volume()));
        fflush(stdout);
        return;
    }

    if (mc_fd_cmd_equal_ci(cmd, "WAD"))
    {
        mc_fd_wad_boot_t wad;

        (void)mc_fd_pico_wad_current(&wad);

        printf("MCFDOOM1 wad_current=%s type=%s base=%s state=%c\n",
               wad.selected,
               wad.mode == MC_FD_WAD_MODE_PWAD ? "PWAD" : "IWAD",
               wad.base,
               wad.state);
        fflush(stdout);
        return;
    }

    if (mc_fd_cmd_equal_ci(cmd, "WADS"))
    {
        int count = mc_fd_pico_wad_list();

        printf("MCFDOOM1 wads_done=%d\n", count);
        fflush(stdout);
        return;
    }

    if (mc_fd_cmd_prefix_ci(cmd, "WAD "))
    {
        mc_fd_wad_boot_t wad;
        int selected = mc_fd_pico_wad_select(cmd + 4, &wad);

        if (selected <= 0)
        {
            printf("MCFDOOM1 wad_select=%s ok=0 reason=%s\n",
                   cmd + 4,
                   selected < 0 ? "config-write" : "not-found-or-not-wad");
            fflush(stdout);
            return;
        }

        printf("MCFDOOM1 wad_select=%s ok=1 type=%s base=%s reboot=1\n",
               wad.selected,
               wad.mode == MC_FD_WAD_MODE_PWAD ? "PWAD" : "IWAD",
               wad.base);
        fflush(stdout);

        /*
         * Give USB CDC enough time to move the acknowledgement before the
         * watchdog performs a clean application reboot.
         */
        sleep_ms(150);
        watchdog_reboot(0, 0, 10);

        for (;;)
            tight_loop_contents();
    }

    if (mc_fd_cmd_equal_ci(cmd, "MUSIC"))
    {
        mc_fd_audio_music_diag_t m;

        mc_fd_audio_get_music_diag(&m);

        printf("MCFDOOM1 music name=%s handle=%d active=%d backend_pause=%d "
               "doom_pause=%d genmidi=%d volume=%d gain=%d "
               "audio_frame=%ld music_frame=%ld start=%ld next=%ld "
               "cursor=%u score_end=%u ticks=%u loops=%lu finished=%d error=%d "
               "emitted=%lu notes=%lu voices_started=%lu voices_released=%lu "
               "active_voices=%d pending=%d stolen=%lu secondary_drop=%lu "
               "dropped=%lu regwrites=%lu "
               "opl_peak=%d opl_peak_max=%d filtered_peak=%d filtered_peak_max=%d "
               "post_shift=%d post_clips=%lu\n",
               m.name,
               m.music_handle,
               m.backend_active,
               m.backend_paused,
               m.doom_paused,
               m.genmidi_ready,
               m.music_volume,
               m.output_gain,
               m.audio_frame,
               m.music_frame,
               m.player_start_frame,
               m.player_next_frame,
               m.player_cursor,
               m.player_score_end,
               m.ticks_in_loop,
               m.loops_completed,
               m.player_finished,
               m.player_error,
               m.messages_emitted,
               m.midi_notes_started,
               m.opl_voices_started,
               m.opl_voices_released,
               m.active_voices,
               m.pending_events,
               m.voices_stolen,
               m.secondary_voices_dropped,
               m.dropped_events,
               m.register_writes,
               m.opl_peak,
               m.opl_peak_max,
               m.filtered_peak,
               m.filtered_peak_max,
               m.opl_post_gain_shift,
               m.opl_post_gain_clips);
        fflush(stdout);
        return;
    }

    if (mc_fd_cmd_prefix_ci(cmd, "TRACK "))
    {
        const char *name = cmd + 6;
        int ok = mc_fd_audio_debug_change_music(name);

        printf("MCFDOOM1 track=%s ok=%d\n", name, ok);
        fflush(stdout);
        return;
    }

    if (mc_fd_cmd_equal_ci(cmd, "STACK"))
    {
        unsigned int total = mc_fd_audio_pico_core1_stack_bytes();
        unsigned int used = mc_fd_audio_pico_core1_stack_used_bytes();

        printf("MCFDOOM1 stack total=%u used=%u free=%u\n",
               total,
               used,
               used <= total ? total - used : 0u);
        fflush(stdout);
        return;
    }

    if (mc_fd_cmd_equal_ci(cmd, "STAT"))
    {
        printf("MCFDOOM1 stat state=%s fatal_line=%d "
               "sys=%lu peri=%lu spi=%lu clock_fallback=%d psram=%lu zone=%u "
               "audio=%d core1=%d stack=%u ctrl_to=%lu ctrl_pause=%d "
               "block=%u deadline_us=%u ring=%u/%u ring_target=%u "
               "ring_low=%u ring_high=%u "
               "underrun=%lu refills=%lu "
               "audio_us=%lu audio_min_us=%lu audio_avg_us=%lu audio_max_us=%lu "
               "volume=%d fps=%u period_us=%lu max_period_us=%lu "
               "render_us=%lu max_render_us=%lu "
               "present_us=%lu max_present_us=%lu "
               "convert_us=%lu lcd_block_us=%lu "
               "fs=%d sd_driver=%d mount_fr=%d last_fr=%d "
               "attempts=%u path=%s i2s_data=%d "
               "psram_cfg=%d psram_reinit=%d\n",
               mc_fd_fatal ? "fatal" : "running",
               mc_fd_fatal_line,
               (unsigned long)clock_get_hz(clk_sys),
               (unsigned long)clock_get_hz(clk_peri),
               (unsigned long)spi_get_baudrate(MR_LCD_SPI),
               mc_fd_clock_fallback,
               (unsigned long)psram_get_size(),
               (unsigned int)MC_FD_PSRAM_ZONE_BYTES,
               mc_fd_audio_transport_ready(),
               mc_fd_audio_pico_core1_running(),
               mc_fd_audio_pico_core1_stack_bytes(),
               mc_fd_audio_pico_control_timeouts(),
               mc_fd_audio_pico_control_paused(),
               mc_fd_audio_pico_block_frames(),
               mc_fd_audio_pico_period_us(),
               mc_fd_audio_pico_ring_ready(),
               mc_fd_audio_pico_ring_count(),
               mc_fd_audio_pico_ring_target(),
               mc_fd_audio_pico_ring_low_water(),
               mc_fd_audio_pico_ring_high_water(),
               mc_fd_audio_pico_underruns(),
               mc_fd_audio_pico_refills(),
               (unsigned long)mc_fd_audio_pico_last_refill_us(),
               (unsigned long)mc_fd_audio_pico_min_refill_us(),
               (unsigned long)mc_fd_audio_pico_avg_refill_us(),
               (unsigned long)mc_fd_audio_pico_max_refill_us(),
               snd_vol_to_percent(mc_fd_audio_master_volume()),
               fps,
               (unsigned long)mc_fd_last_period_us,
               (unsigned long)mc_fd_max_period_us,
               (unsigned long)mc_fd_last_render_us,
               (unsigned long)mc_fd_max_render_us,
               (unsigned long)mc_fd_last_frame_us,
               (unsigned long)mc_fd_max_frame_us,
               (unsigned long)mc_fd_last_convert_us,
               (unsigned long)mc_fd_last_lcd_us,
               mc_fd_pico_fs_ready(),
               mc_fd_pico_fs_sd_driver_ok(),
               mc_fd_pico_fs_mount_result(),
               mc_fd_pico_fs_last_result(),
               mc_fd_pico_fs_mount_attempts(),
               mc_fd_pico_fs_last_path(),
               MC_I2S_DATA,
               mc_fd_psram_config_rc,
               mc_fd_psram_reinit_rc);
        fflush(stdout);
        return;
    }

    if (mc_fd_cmd_equal_ci(cmd, "FS"))
    {
        int wad_ok = mc_fd_pico_fs_probe_wad();

        printf("MCFDOOM1 fs_probe=%d fs=%d sd_driver=%d "
               "mount_fr=%d last_fr=%d attempts=%u path=%s\n",
               wad_ok,
               mc_fd_pico_fs_ready(),
               mc_fd_pico_fs_sd_driver_ok(),
               mc_fd_pico_fs_mount_result(),
               mc_fd_pico_fs_last_result(),
               mc_fd_pico_fs_mount_attempts(),
               mc_fd_pico_fs_last_path());
        fflush(stdout);
        return;
    }

    if (mc_fd_cmd_equal_ci(cmd, "SDRAW"))
    {
        int raw = mc_fd_pico_fs_raw_probe();

        printf("MCFDOOM1 sdraw=%d dstatus=%d card_type=%d miso=%d "
               "pins=miso:%d,cs:%d,sck:%d,mosi:%d\n",
               raw,
               mc_fd_pico_fs_raw_dstatus(),
               mc_fd_pico_fs_raw_card_type(),
               mc_fd_pico_fs_raw_miso(),
               MC_SD_MISO, MC_SD_CS, MC_SD_SCK, MC_SD_MOSI);
        fflush(stdout);
        return;
    }

    volume = mc_fd_parse_volume(cmd);
    if (volume >= 0)
    {
        mc_fd_audio_set_master_volume(snd_vol_from_percent(volume));
        printf("MCFDOOM1 volume=%d\n", volume);
        fflush(stdout);
        return;
    }

    if (strncmp(cmd, "KEY ", 4) == 0)
    {
        char name[16];
        char state[16];

        if (sscanf(cmd + 4, "%15s %15s", name, state) == 2)
        {
            int key = mc_fd_key_from_name(name);

            if (key != 0)
            {
                if (strcmp(state, "DOWN") == 0)
                    mc_fd_post_key(ev_keydown, key);
                else if (strcmp(state, "UP") == 0)
                    mc_fd_post_key(ev_keyup, key);
                else
                    key = 0;

                if (key != 0)
                {
                    printf("MCFDOOM1 key=%s state=%s\n", name, state);
                    fflush(stdout);
                    return;
                }
            }
        }

        printf("MCFDOOM1 error=key-syntax\n");
        fflush(stdout);
        return;
    }

    printf("MCFDOOM1 error=unknown-command cmd=%s\n", cmd);
    fflush(stdout);
}

static void mc_fd_serial_service(void)
{
    int ch;

    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
    {
        if (ch == '\r' || ch == '\n')
        {
            if (mc_fd_cmd_n != 0u)
            {
                mc_fd_cmd[mc_fd_cmd_n] = '\0';
                mc_fd_handle_command(mc_fd_cmd);
                mc_fd_cmd_n = 0u;
            }
        }
        else if (mc_fd_cmd_n + 1u < sizeof(mc_fd_cmd))
        {
            mc_fd_cmd[mc_fd_cmd_n++] = (char)ch;
        }
        else
        {
            mc_fd_cmd_n = 0u;
        }
    }
}

static bool mc_fd_tic_timer_callback(struct repeating_timer *timer)
{
    (void)timer;

    /*
     * Match FastDoom's normal 35 Hz DOS timer semantics.  Gameplay time must
     * advance independently of rendering; otherwise a 25 FPS frame rate makes
     * Doom itself run at 25/35 speed.
     */
    ++ticcount;
    ticcount_hr = ticcount << 4;
    return true;
}

void I_Init(void)
{
    /*
     * Functional bring-up is complete: restore FastDoom's normal capped
     * adaptive scheduler.  A hardware timer owns ticcount at 35 Hz while the
     * LCD is free to present fewer frames when a scene exceeds 28.57 ms.
     */
    singletics = false;
    uncappedFPS = false;
    highResTimer = false;
    waitVsync = false;

    ticcount = 0u;
    ticcount_hr = 0u;
    I_StartupTimer();

    mc_fd_next_frame_us = time_us_64();
    mc_fd_fps_start_us = mc_fd_next_frame_us;
    mc_fd_fps_frames = 0u;
    mc_fd_last_frame_us = 0u;
    mc_fd_max_frame_us = 0u;
    mc_fd_last_convert_us = 0u;
    mc_fd_last_lcd_us = 0u;
    mc_fd_last_finish_return_us = 0u;
    mc_fd_last_finish_entry_us = 0u;
    mc_fd_last_render_us = 0u;
    mc_fd_max_render_us = 0u;
    mc_fd_last_period_us = 0u;
    mc_fd_max_period_us = 0u;

    printf("MCFDOOM1 init timing=35hz-realtime adaptive=1\n");
    fflush(stdout);
}

byte *I_ZoneBase(int *size)
{
    unsigned int bytes = MC_FD_PSRAM_ZONE_BYTES;

    if (limitram > 0u)
    {
        unsigned int requested = limitram * 1024u;
        if (requested < bytes)
            bytes = requested;
    }

    if (size != NULL)
        *size = (int)bytes;

    printf("MCFDOOM1 zone=%u start=%p end=%p\n",
           bytes,
           (void *)&mc_fd_zone[0],
           (void *)&mc_fd_zone[bytes - 1u]);
    fflush(stdout);

    return mc_fd_zone;
}

void I_InitGraphics(void)
{
    byte *playpal;

    if (mc_fd_graphics_ready)
        return;

    if (!mc_fd_panel_ready)
    {
        mr_pico_ili9341_init(&mc_fd_lcd);
        mr_pico_ili9341_panel_init(&mc_fd_lcd);
        mc_fd_panel_ready = 1;
    }

    mr_pico_ili9341_fill_screen(
        &mc_fd_lcd, GFX_RGB565_BLACK, MC_FD_WIDTH, MC_FD_LCD_HEIGHT);

    memset(backbuffer, 0, sizeof(backbuffer));

    pcscreen = backbuffer;
    destscreen = backbuffer;
    destview = backbuffer;
    currentscreen = (unsigned short *)backbuffer;

    I_SetGamma(usegamma);
    playpal = W_CacheLumpName("PLAYPAL", PU_CACHE);
    I_ProcessPalette(playpal);
    I_SetPalette(0);

    mc_fd_graphics_ready = 1;

    printf("MCFDOOM1 graphics=ili9341 320x200 y=%d spi=%u\n",
           MC_FD_LCD_Y,
           (unsigned int)mc_fd_lcd.spi_baud_hz);
    fflush(stdout);
}

void I_ShutdownGraphics(void)
{
    if (!mc_fd_graphics_ready)
        return;

    mr_pico_ili9341_flush_wait(NULL, &mc_fd_lcd);
    mc_fd_graphics_ready = 0;
}

void I_StartTic(void)
{
    static int wad_boot_confirmed;

    if (!wad_boot_confirmed)
    {
        if (mc_fd_pico_wad_confirm_boot())
            wad_boot_confirmed = 1;
    }

    mc_fd_serial_service();
    mc_fd_audio_transport_service();
}

void I_ProcessPalette(byte *palette)
{
    int p;
    int i;

    if (palette == NULL)
        return;

    for (p = 0; p < MC_FD_PALETTES; ++p)
    {
        for (i = 0; i < 256; ++i)
        {
            int base = p * 768 + i * 3;
            unsigned int r6 = gammatable[palette[base + 0]];
            unsigned int g6 = gammatable[palette[base + 1]];
            unsigned int b6 = gammatable[palette[base + 2]];

            unsigned int r = (r6 * 255u + 31u) / 63u;
            unsigned int g = (g6 * 255u + 31u) / 63u;
            unsigned int b = (b6 * 255u + 31u) / 63u;

            mc_fd_palette565[p][i] = mc_fd_rgb565(r, g, b);
        }
    }
}

void I_SetPalette(int numpalette)
{
    if (numpalette < 0)
        numpalette = 0;
    if (numpalette >= MC_FD_PALETTES)
        numpalette = MC_FD_PALETTES - 1;

    mc_fd_palette = numpalette;
}

void I_FinishUpdate(void)
{
    int y;
    int chunk_index;
    uint64_t frame_begin;
    uint64_t lcd_end;
    uint64_t now;
    uint64_t elapsed;
    uint64_t convert_total = 0u;
    uint64_t lcd_block_total = 0u;

    if (!mc_fd_graphics_ready)
        return;

    frame_begin = time_us_64();

    if (mc_fd_last_finish_entry_us != 0u)
    {
        uint64_t period = frame_begin - mc_fd_last_finish_entry_us;
        mc_fd_last_period_us = (uint32_t)period;
        if (mc_fd_last_period_us > mc_fd_max_period_us)
            mc_fd_max_period_us = mc_fd_last_period_us;
    }
    mc_fd_last_finish_entry_us = frame_begin;

    if (mc_fd_last_finish_return_us != 0u)
    {
        uint64_t render = frame_begin - mc_fd_last_finish_return_us;
        mc_fd_last_render_us = (uint32_t)render;
        if (mc_fd_last_render_us > mc_fd_max_render_us)
            mc_fd_max_render_us = mc_fd_last_render_us;
    }

    mc_fd_audio_transport_service();

    /*
     * Pipeline INDEX8 -> RGB565 conversion with SPI DMA:
     *
     *   convert rows 0..7 into SRAM buffer A
     *   DMA A while converting rows 8..15 into buffer B
     *   DMA B while converting the next rows back into A
     *   ...
     *
     * The old path converted the whole frame into a 128 KiB PSRAM surface and
     * then DMA-read that surface back through QMI.  This removes both copies
     * from PSRAM and lets conversion overlap the wire transfer.
     */
    chunk_index = 0;
    for (y = 0; y < MC_FD_HEIGHT; y += MC_FD_LCD_CHUNK_H)
    {
        int rows = MC_FD_LCD_CHUNK_H;
        gfx_color_t *dst;
        uint64_t t0;
        uint64_t t1;

        if (y + rows > MC_FD_HEIGHT)
            rows = MC_FD_HEIGHT - y;

        dst = mc_fd_lcd_rows[chunk_index & 1];

        t0 = time_us_64();
        mc_fd_convert_lcd_rows(dst, y, rows);
        t1 = time_us_64();
        convert_total += t1 - t0;

        t0 = time_us_64();
        mr_pico_ili9341_flush_begin(
            NULL,
            0,
            MC_FD_LCD_Y + y,
            MC_FD_WIDTH,
            rows,
            dst,
            &mc_fd_lcd);
        t1 = time_us_64();
        lcd_block_total += t1 - t0;

        ++chunk_index;
    }

    {
        uint64_t t0 = time_us_64();
        uint64_t t1;

        mr_pico_ili9341_flush_wait(NULL, &mc_fd_lcd);
        t1 = time_us_64();
        lcd_block_total += t1 - t0;
        lcd_end = t1;
    }

    mc_fd_audio_transport_service();

    mc_fd_last_convert_us = (uint32_t)convert_total;
    mc_fd_last_lcd_us = (uint32_t)lcd_block_total;
    elapsed = lcd_end - frame_begin;
    mc_fd_last_frame_us = (uint32_t)elapsed;
    if (mc_fd_last_frame_us > mc_fd_max_frame_us)
        mc_fd_max_frame_us = mc_fd_last_frame_us;

    ++mc_fd_fps_frames;
    updatestate = I_NOUPDATE;

    mc_fd_next_frame_us += MC_FD_FRAME_US;
    now = time_us_64();

    if (mc_fd_next_frame_us > now)
        sleep_until(from_us_since_boot(mc_fd_next_frame_us));
    else if (now - mc_fd_next_frame_us > (uint64_t)MC_FD_FRAME_US * 4u)
        mc_fd_next_frame_us = now;

    now = time_us_64();
    {
        uint64_t fps_elapsed = now - mc_fd_fps_start_us;
        if (fps_elapsed >= 1000000ull)
        {
            fps = (unsigned int)(
                ((uint64_t)mc_fd_fps_frames * 1000000ull) / fps_elapsed);
            mc_fd_fps_frames = 0u;
            mc_fd_fps_start_us = now;
        }
    }

    mc_fd_last_finish_return_us = now;
}

void I_CalculateFPS(void)
{
    /*
     * FPS is maintained unconditionally by I_FinishUpdate() so PING can report
     * it even with showFPS=0.  Keep this symbol for FastDoom's normal callsite.
     */
}

void I_WaitSingleVBL(void)
{
    sleep_us(1000);
}

void I_WaitCGA(void) {}
void I_DisableCGABlink(void) {}
void I_DisableMDABlink(void) {}

byte *I_AllocLow(int length)
{
    if (length <= 0)
        return NULL;

    return (byte *)calloc(1u, (size_t)length);
}

void *I_DosMemAlloc(unsigned long size)
{
    return calloc(1u, (size_t)size);
}

int I_GetCPUModel(void)
{
    /*
     * FastDoom uses the selected CPU model to choose renderer entry points.
     * The portable C aliases implement the baseline path, so keep the same
     * semantic choice as the validated desktop port.
     */
    return 486;
}

void I_GetCPU(void)
{
    hasCPUID = 0u;
    hasFPU = 0u;
    hasMMX = 0u;
}

void I_StartupTimer(void)
{
    if (mc_fd_tic_timer_active)
        return;

    /*
     * Negative delay means each callback is scheduled relative to the previous
     * target time, minimizing accumulated drift.  28571 us is the closest
     * integer-microsecond period to Doom's 35 Hz clock.
     */
    if (!add_repeating_timer_us(
            -(int64_t)MC_FD_FRAME_US,
            mc_fd_tic_timer_callback,
            NULL,
            &mc_fd_tic_timer))
    {
        panic("FastDoom Pico: unable to start 35 Hz timer");
    }

    mc_fd_tic_timer_active = 1;
}

void I_ShutdownTimer(void)
{
    if (!mc_fd_tic_timer_active)
        return;

    cancel_repeating_timer(&mc_fd_tic_timer);
    mc_fd_tic_timer_active = 0;
}

void I_SetHrTimerEnabled(boolean enabled)
{
    /*
     * The Pico target is currently capped at 35 FPS, so the 560 Hz
     * interpolation timer is intentionally not enabled yet.
     */
    (void)enabled;
}

void I_TimerISR(task *task)
{
    (void)task;
}

void I_TimerMS(task *task)
{
    (void)task;
}

void I_LoopSong(int handle)
{
    (void)handle;
}

void I_ResumeSong(int handle)
{
    (void)handle;
}

void I_Quit(void)
{
    I_ShutdownTimer();
    mc_fd_audio_transport_stop();
    I_ShutdownGraphics();

    printf("MCFDOOM1 quit\n");
    fflush(stdout);

    for (;;)
    {
        mc_fd_serial_service();
        sleep_ms(1);
    }
}

void I_Error(int line, ...)
{
    va_list ap;

    mc_fd_fatal = 1;
    mc_fd_fatal_line = line;

    if (mc_fd_panel_ready)
    {
        mr_pico_ili9341_fill_screen(
            &mc_fd_lcd, (gfx_color_t)0xF800u,
            MC_FD_WIDTH, MC_FD_LCD_HEIGHT);
    }

    fprintf(stderr, "\nMCFDOOM1 fatal line=%d", line);

    /*
     * FastDoom's human-readable PROG.TXT is not available until the SD layer
     * exists. Do not call I_LoadTextProgram() here; that would hide the useful
     * numeric failure behind another failed filesystem access.
     */
    va_start(ap, line);
    va_end(ap);

    fprintf(stderr, "\n");
    fflush(stderr);

    /*
     * Stay alive on USB so the bring-up target can always answer PING and
     * report exactly where startup stopped.
     */
    for (;;)
    {
        mc_fd_serial_service();
        mc_fd_audio_transport_service();
        sleep_ms(1);
    }
}

void I_Printf(const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
}

void I_Clear(void) {}
void I_DebugInit(void) {}
void I_DebugShutdown(void) {}

debugsymbol_t *I_LookupSymbol(int addr)
{
    (void)addr;
    return NULL;
}

const char *I_LookupSymbolName(void *addr)
{
    (void)addr;
    return "<pico>";
}

void I_Backtrace(const char *msg, ...)
{
    va_list ap;

    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);
}
