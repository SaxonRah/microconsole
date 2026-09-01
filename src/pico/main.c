#include "gfx.h"
#include "mr_pico_ili9341.h"
#include "mr_stress_test.h"
#include "mw_music_demo.h"
#include "snd.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/regs/clocks.h"
#include "mw_i2s.pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef MC_AUDIO_RATE
#define MC_AUDIO_RATE 32000
#endif
#ifndef MC_AUDIO_BLOCK
#define MC_AUDIO_BLOCK 1024
#endif
#ifndef MC_STRESS_SPRITES
#define MC_STRESS_SPRITES 1024
#endif
#ifndef MC_SYS_KHZ
#define MC_SYS_KHZ 300000
#endif
#ifndef MC_LACE_BLOCK_H
#define MC_LACE_BLOCK_H 8
#endif
#ifndef MC_I2S_BCLK
#define MC_I2S_BCLK 10
#endif
#ifndef MC_I2S_LRCLK
#define MC_I2S_LRCLK 11
#endif
#ifndef MC_I2S_DATA
#define MC_I2S_DATA 12
#endif
#ifndef MC_AUDIO_DEVICE
#define MC_AUDIO_DEVICE 1
#endif

#define MC_W 320
#define MC_H 240
#define MC_AUDIO_PIO pio1
#define MC_AUDIO_SM 0

#if MC_I2S_LRCLK != (MC_I2S_BCLK + 1)
#error "MC_I2S_LRCLK must equal MC_I2S_BCLK + 1"
#endif

static mr_pico_ili9341_t g_lcd = {
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
static gfx_renderer_t g_renderer;
static mr_stress_test_t g_stress;
static gfx_color_t g_frame_a[MC_W * MC_H];
static gfx_color_t g_frame_b[MC_W * MC_H];

static const gfx_color_t *volatile g_present_buffer;
static volatile int g_present_phase;
static int g_present_pending;

static snd_mixer_t g_mixer;
static mw_demo_t g_demo;
static snd_sample_t g_audio_mix[2][MC_AUDIO_BLOCK];
static uint32_t g_audio_i2s[2][MC_AUDIO_BLOCK];
static int g_audio_dma = -1;
static volatile int g_audio_active;
static volatile int g_audio_ready[2];
static volatile int g_audio_refill = -1;
static volatile unsigned long g_audio_underruns;
static long g_audio_frame;

static const char *device_name(void) {
#if MC_AUDIO_DEVICE == 1
    return "MAX98357A";
#elif MC_AUDIO_DEVICE == 2
    return "PCM5102A";
#elif MC_AUDIO_DEVICE == 3
    return "NS4168";
#else
    return "I2S";
#endif
}

static void noop_flush(gfx_renderer_t *r, int x, int y, int w, int h,
                       const gfx_color_t *pixels, void *user) {
    (void)r; (void)x; (void)y; (void)w; (void)h; (void)pixels; (void)user;
}

static void lace_send(const gfx_color_t *buffer, int phase) {
    int y;
    for (y = phase * MC_LACE_BLOCK_H; y < MC_H; y += MC_LACE_BLOCK_H * 2) {
        int h = MC_LACE_BLOCK_H;
        if (y + h > MC_H) h = MC_H - y;
        mr_pico_ili9341_flush(0, 0, y, MC_W, h,
                              buffer + y * MC_W, &g_lcd);
    }
}

static void core1_present(void) {
    for (;;) {
        (void)multicore_fifo_pop_blocking();
        __dmb();
        lace_send((const gfx_color_t *)g_present_buffer, g_present_phase);
        __dmb();
        multicore_fifo_push_blocking(1u);
    }
}

static void present_sync(void) {
    if (!g_present_pending) return;
    (void)multicore_fifo_pop_blocking();
    g_present_pending = 0;
}

static void present_async(const gfx_color_t *buffer, int phase) {
    present_sync();
    g_present_buffer = buffer;
    g_present_phase = phase;
    __dmb();
    multicore_fifo_push_blocking(1u);
    g_present_pending = 1;
}

static int16_t sample_s16(snd_sample_t s) {
#if SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_U8
    return (int16_t)(((int)s - 128) << 8);
#else
    return (int16_t)s;
#endif
}

static void audio_fill(int index) {
    int i;
    g_mixer.block = g_audio_mix[index];
    snd_render_one_block(&g_mixer, g_audio_frame, MC_AUDIO_BLOCK,
                         mw_demo_mix, &g_demo, 0u);
    g_audio_frame += MC_AUDIO_BLOCK;
    for (i = 0; i < MC_AUDIO_BLOCK; ++i) {
        int16_t s = sample_s16(g_audio_mix[index][i]);
        g_audio_i2s[index][i] = ((uint32_t)(uint16_t)s << 16) |
                                (uint16_t)s;
    }
    __dmb();
    g_audio_ready[index] = 1;
}

static void audio_start_dma(int index) {
    dma_channel_set_read_addr((uint)g_audio_dma, g_audio_i2s[index], false);
    dma_channel_set_trans_count((uint)g_audio_dma, MC_AUDIO_BLOCK, true);
}

static void audio_dma_irq(void) {
    int done, next;
    dma_hw->ints0 = 1u << (unsigned)g_audio_dma;
    done = g_audio_active;
    next = done ^ 1;
    if (g_audio_ready[next]) {
        g_audio_ready[next] = 0;
        g_audio_active = next;
        audio_start_dma(next);
        g_audio_refill = done;
    } else {
        /* Keep clocks continuous and make the failure audible rather than
           stopping I2S. Repeating one block is preferable to losing PLL lock. */
        ++g_audio_underruns;
        audio_start_dma(done);
    }
}

static void audio_service(void) {
    int index = g_audio_refill;
    if (index >= 0) {
        g_audio_refill = -1;
        audio_fill(index);
    }
}

static void audio_init(void) {
    uint offset;
    uint32_t sys_hz;
    uint32_t div256;
    dma_channel_config dc;

    gpio_set_function(MC_I2S_DATA, GPIO_FUNC_PIO1);
    gpio_set_function(MC_I2S_BCLK, GPIO_FUNC_PIO1);
    gpio_set_function(MC_I2S_LRCLK, GPIO_FUNC_PIO1);
    pio_sm_claim(MC_AUDIO_PIO, MC_AUDIO_SM);
    offset = pio_add_program(MC_AUDIO_PIO, &mw_i2s_program);
    mw_i2s_program_init(MC_AUDIO_PIO, MC_AUDIO_SM, offset,
                        MC_I2S_DATA, MC_I2S_BCLK);

    sys_hz = clock_get_hz(clk_sys);
    div256 = (uint32_t)((((uint64_t)sys_hz * 4u) + MC_AUDIO_RATE / 2u) /
                        (uint32_t)MC_AUDIO_RATE);
    pio_sm_set_clkdiv_int_frac(MC_AUDIO_PIO, MC_AUDIO_SM,
                               (uint16_t)(div256 >> 8u),
                               (uint8_t)(div256 & 0xffu));

    g_audio_dma = dma_claim_unused_channel(true);
    dc = dma_channel_get_default_config((uint)g_audio_dma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, DREQ_PIO1_TX0 + MC_AUDIO_SM);
    dma_channel_configure((uint)g_audio_dma, &dc,
                          &MC_AUDIO_PIO->txf[MC_AUDIO_SM], NULL, 0, false);

    snd_init(&g_mixer, MC_AUDIO_RATE, 1, g_audio_mix[0], MC_AUDIO_BLOCK,
             NULL, NULL);
    snd_set_master_gain(&g_mixer, SND_GAIN_UNITY);
    mw_demo_init(&g_demo, &g_mixer, 0, 1);
    g_audio_frame = 0;
    g_audio_ready[0] = g_audio_ready[1] = 0;
    audio_fill(0);
    audio_fill(1);

    irq_set_exclusive_handler(DMA_IRQ_0, audio_dma_irq);
    dma_channel_set_irq0_enabled((uint)g_audio_dma, true);
    irq_set_enabled(DMA_IRQ_0, true);
    pio_sm_set_enabled(MC_AUDIO_PIO, MC_AUDIO_SM, true);

    g_audio_active = 0;
    g_audio_ready[0] = 0;
    audio_start_dma(0);
}

static void recover_warm_boot(void) {
    multicore_reset_core1();

    /* SWD/picotool resets do not power-cycle the peripherals. Stop every old
       DMA request and clear IRQ routing/status before either backend claims a
       channel. This makes a warm flash behave like a cold BOOTSEL boot. */
    dma_hw->inte0 = 0u;
    dma_hw->inte1 = 0u;
    dma_hw->abort = (uint32_t)~0u;
    while (dma_hw->abort) tight_loop_contents();
    dma_hw->ints0 = (uint32_t)~0u;
    dma_hw->ints1 = (uint32_t)~0u;
}

static void configure_peripheral_clock(void) {
    uint32_t sys_hz = clock_get_hz(clk_sys);
    if (sys_hz != 0u) {
        /* SPI is clocked from clk_peri. After set_sys_clock_khz() clk_peri may
           still be 48 MHz, which caps SPI at about 24 MHz. MicroRender's
           working Pico stress frontend explicitly re-sources clk_peri from
           clk_sys; preserve that behavior in the combined firmware. */
        clock_configure(clk_peri, 0,
                        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                        sys_hz, sys_hz);
        sleep_ms(2);
    }
}

int main(void) {
    mr_stress_config_t cfg;
    unsigned long frame = 0;
    uint64_t start_us, last_us;
    unsigned long last_frame = 0;

    recover_warm_boot();
    if (!set_sys_clock_khz(MC_SYS_KHZ, false))
        panic("MicroConsole: could not set requested system clock");
    configure_peripheral_clock();
    stdio_init_all();

    mr_pico_ili9341_init(&g_lcd);
    mr_pico_ili9341_panel_init(&g_lcd);
    mr_pico_ili9341_fill_screen(&g_lcd, GFX_RGB565_BLACK, MC_W, MC_H);

    gfx_init(&g_renderer, MC_W, MC_H, g_frame_a, MC_H, noop_flush, NULL);
    mr_stress_config_defaults(&cfg, MC_W, MC_H);
    cfg.sprite_count = MC_STRESS_SPRITES;
    cfg.stats_sample_rate = 8;
    cfg.features = MR_STRESS_FEATURE_DEFAULT;
    mr_stress_init(&g_stress, &cfg);

    /* LCD claims its DMA channel first. Audio then claims another unused
       channel, so the two submodule backends cannot accidentally collide. */
    audio_init();
    multicore_launch_core1(core1_present);

    printf("MicroConsole Pico: stress=%d sprites lace=%d sys=%lu spi=%u; audio=%s %dHz block=%d BCLK=%d LRCLK=%d DATA=%d\n",
           MC_STRESS_SPRITES, MC_LACE_BLOCK_H,
           (unsigned long)clock_get_hz(clk_sys), (unsigned)g_lcd.spi_baud_hz,
           device_name(), MC_AUDIO_RATE, MC_AUDIO_BLOCK,
           MC_I2S_BCLK, MC_I2S_LRCLK, MC_I2S_DATA);

    start_us = time_us_64();
    last_us = start_us;
    for (;;) {
        gfx_color_t *buffer = (frame & 1ul) ? g_frame_b : g_frame_a;
        uint64_t now;

        audio_service();
        mr_stress_tick(&g_stress);
        g_renderer.tile = buffer;
        gfx_begin_tile(&g_renderer, 0, MC_H);
        mr_stress_render(&g_renderer, &g_stress);
        audio_service();

        present_async(buffer, (int)(frame & 1ul));
        ++frame;
        audio_service();

        now = time_us_64();
        if (now - last_us >= 1000000ull) {
            uint64_t dt = now - last_us;
            uint64_t total = now - start_us;
            unsigned long df = frame - last_frame;
            unsigned long fps10 = dt ? (unsigned long)((uint64_t)df * 10000000ull / dt) : 0ul;
            unsigned long avg10 = total ? (unsigned long)((uint64_t)frame * 10000000ull / total) : 0ul;
            mr_stress_set_fps10(&g_stress, fps10, avg10);
            printf("stress frame=%lu fps=%lu.%lu avg=%lu.%lu audio=%ld underrun=%lu\n",
                   frame, fps10 / 10ul, fps10 % 10ul,
                   avg10 / 10ul, avg10 % 10ul,
                   g_audio_frame, g_audio_underruns);
            last_us = now;
            last_frame = frame;
        }
    }
}
