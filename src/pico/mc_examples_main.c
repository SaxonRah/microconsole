/*
 * Pico 2 frontend for the console-era MicroConsole examples.
 *
 * Unlike the desktop frontend, this target deliberately does not reserve a
 * 320x240 framebuffer.  The nine examples are already required to render
 * correctly into arbitrary tiles for the DOS frontend, so the Pico reuses that
 * property and keeps two small row-band buffers instead.  While LCD DMA sends
 * one band, core 0 renders the next into the other buffer.
 *
 * That removes the existing Pico stress frontend's two 153,600-byte RGB565
 * framebuffers from the memory budget and leaves the RP2350 SRAM available for
 * the state of all nine examples.  No PSRAM is required by this frontend.
 *
 * USB serial commands:
 *
 *   PING
 *   LIST
 *   EXAMPLE snes-mode7
 *   NEXT
 *   PREV
 *   VOL 0..100
 *   SFX
 *   ACTION
 *   DEBUG
 *   KEY LEFT DOWN
 *   KEY LEFT UP
 *   KEY RIGHT DOWN
 *   KEY RIGHT UP
 *   KEY UP DOWN
 *   KEY UP UP
 *   KEY DOWN DOWN
 *   KEY DOWN UP
 *   HELP
 *
 * The Pico Plus 2 user switch also advances to the next example.
 */

#include "gfx.h"
#include "mc_example.h"
#include "mr_demo_input.h"
#include "mr_pico_ili9341.h"
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
#ifndef MC_AUDIO_VOLUME
#define MC_AUDIO_VOLUME 100
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

#define MC_W MC_EX_W
#define MC_H MC_EX_H
#define MC_AUDIO_PIO pio1
#define MC_AUDIO_SM 0
#define MC_TICK_US 16667ull
#define MC_TILE_H 16

#define MC_KEY_LEFT  (1u << 0)
#define MC_KEY_RIGHT (1u << 1)
#define MC_KEY_UP    (1u << 2)
#define MC_KEY_DOWN  (1u << 3)

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
static gfx_color_t g_tile[2][MC_W * MC_TILE_H];

static snd_mixer_t g_mixer;
static snd_sample_t g_audio_mix[2][MC_AUDIO_BLOCK];
static uint32_t g_audio_i2s[2][MC_AUDIO_BLOCK];
static int g_audio_dma = -1;
static volatile int g_audio_active;
static volatile int g_audio_ready[2];
static volatile int g_audio_refill = -1;
static volatile unsigned long g_audio_underruns;
static long g_audio_frame;

static const mc_example_t *g_example;
static int g_example_index;

static unsigned g_held_keys;
static uint16_t g_edge_buttons;

static char g_cmd[96];
static unsigned g_cmd_n;

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
    (void)r;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)pixels;
    (void)user;
}

static void print_selected(void) {
    if (!g_example)
        return;
    printf("MWPICO1 selected=%s index=%d count=%d system=%s\n",
           g_example->id, g_example_index, mc_example_count(),
           g_example->system);
    fflush(stdout);
}

static void select_example(int index) {
    int count = mc_example_count();
    const mc_example_t *next;

    if (count <= 0)
        return;
    while (index < 0)
        index += count;
    index %= count;

    next = mc_example_at(index);
    if (!next)
        return;

    if (next->init)
        next->init(MC_W, MC_H, &g_mixer, g_audio_frame);

    g_example = next;
    g_example_index = index;
    print_selected();
}

static int parse_volume_command(const char *cmd) {
    int v = 0;
    const char *p = cmd;

    if (strncmp(p, "VOL", 3) != 0)
        return -1;
    p += 3;
    while (*p == ' ')
        ++p;
    if (*p < '0' || *p > '9')
        return -1;

    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 100)
            return 100;
        ++p;
    }
    return v;
}

static void list_examples(void) {
    int i;
    int count = mc_example_count();

    printf("MWPICO1 list-begin count=%d\n", count);
    for (i = 0; i < count; ++i) {
        const mc_example_t *e = mc_example_at(i);
        if (e)
            printf("MWPICO1 example=%s system=%s\n", e->id, e->system);
    }
    printf("MWPICO1 list-end\n");
    fflush(stdout);
}

static void set_key(const char *name, int down) {
    unsigned bit = 0u;

    if (strcmp(name, "LEFT") == 0)
        bit = MC_KEY_LEFT;
    else if (strcmp(name, "RIGHT") == 0)
        bit = MC_KEY_RIGHT;
    else if (strcmp(name, "UP") == 0)
        bit = MC_KEY_UP;
    else if (strcmp(name, "DOWN") == 0)
        bit = MC_KEY_DOWN;

    if (bit == 0u) {
        printf("MWPICO1 error=unknown-key\n");
        fflush(stdout);
        return;
    }

    if (down)
        g_held_keys |= bit;
    else
        g_held_keys &= ~bit;

    printf("MWPICO1 key=%s state=%s\n", name, down ? "down" : "up");
    fflush(stdout);
}

static void handle_command(const char *cmd) {
    int v;

    if (strcmp(cmd, "PING") == 0) {
        printf("MWPICO1 mode=examples device=%s rate=%d vol=%d cur=%d "
               "example=%s underrun=%lu\n",
               device_name(), MC_AUDIO_RATE,
               snd_vol_to_percent(snd_master_volume(&g_mixer)),
               snd_vol_to_percent(snd_master_volume_current(&g_mixer)),
               g_example ? g_example->id : "(none)", g_audio_underruns);
        fflush(stdout);
        return;
    }

    if (strcmp(cmd, "LIST") == 0) {
        list_examples();
        return;
    }

    if (strncmp(cmd, "EXAMPLE ", 8) == 0) {
        int found = mc_example_find(cmd + 8);
        if (found < 0) {
            printf("MWPICO1 error=unknown-example id=%s\n", cmd + 8);
            fflush(stdout);
        } else {
            select_example(found);
        }
        return;
    }

    if (strcmp(cmd, "NEXT") == 0) {
        select_example(g_example_index + 1);
        return;
    }

    if (strcmp(cmd, "PREV") == 0) {
        select_example(g_example_index - 1);
        return;
    }

    v = parse_volume_command(cmd);
    if (v >= 0) {
        snd_set_master_volume(&g_mixer, snd_vol_from_percent(v));
        printf("MWPICO1 vol=%d cur=%d\n", v,
               snd_vol_to_percent(snd_master_volume_current(&g_mixer)));
        fflush(stdout);
        return;
    }

    if (strcmp(cmd, "SFX") == 0) {
        if (g_example && g_example->sfx)
            g_example->sfx(&g_mixer, g_audio_frame + MC_AUDIO_BLOCK);
        printf("MWPICO1 sfx=%s\n", g_example ? g_example->id : "(none)");
        fflush(stdout);
        return;
    }

    if (strcmp(cmd, "ACTION") == 0) {
        g_edge_buttons |= MR_DEMO_INPUT_ACTION;
        printf("MWPICO1 action=queued\n");
        fflush(stdout);
        return;
    }

    if (strcmp(cmd, "DEBUG") == 0) {
        g_edge_buttons |= MR_DEMO_INPUT_DEBUG;
        printf("MWPICO1 debug=queued\n");
        fflush(stdout);
        return;
    }

    if (strncmp(cmd, "KEY ", 4) == 0) {
        char key[16];
        char state[16];
        if (sscanf(cmd + 4, "%15s %15s", key, state) == 2) {
            if (strcmp(state, "DOWN") == 0) {
                set_key(key, 1);
                return;
            }
            if (strcmp(state, "UP") == 0) {
                set_key(key, 0);
                return;
            }
        }
        printf("MWPICO1 error=key-syntax\n");
        fflush(stdout);
        return;
    }

    if (strcmp(cmd, "HELP") == 0) {
        printf("MWPICO1 commands=PING,LIST,EXAMPLE,NEXT,PREV,VOL,SFX,"
               "ACTION,DEBUG,KEY,HELP\n");
        fflush(stdout);
        return;
    }

    printf("MWPICO1 error=unknown-command cmd=%s\n", cmd);
    fflush(stdout);
}

static void serial_service(void) {
    int ch;

    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (ch == '\r' || ch == '\n') {
            if (g_cmd_n != 0u) {
                g_cmd[g_cmd_n] = '\0';
                handle_command(g_cmd);
                g_cmd_n = 0u;
            }
        } else if (g_cmd_n + 1u < sizeof(g_cmd)) {
            g_cmd[g_cmd_n++] = (char)ch;
        } else {
            g_cmd_n = 0u;
        }
    }
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
                         g_example ? g_example->mix : NULL, NULL,
                         SND_RENDER_SKIP_SILENT);
    g_audio_frame += MC_AUDIO_BLOCK;

    for (i = 0; i < MC_AUDIO_BLOCK; ++i) {
        int16_t s = sample_s16(g_audio_mix[index][i]);
        g_audio_i2s[index][i] =
            ((uint32_t)(uint16_t)s << 16) | (uint16_t)s;
    }

    __dmb();
    g_audio_ready[index] = 1;
}

static void audio_start_dma(int index) {
    dma_channel_set_read_addr((uint)g_audio_dma, g_audio_i2s[index], false);
    dma_channel_set_trans_count((uint)g_audio_dma, MC_AUDIO_BLOCK, true);
}

static void audio_dma_irq(void) {
    int done;
    int next;

    dma_hw->ints0 = 1u << (unsigned)g_audio_dma;
    done = g_audio_active;
    next = done ^ 1;

    if (g_audio_ready[next]) {
        g_audio_ready[next] = 0;
        g_audio_active = next;
        audio_start_dma(next);
        g_audio_refill = done;
    } else {
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
    snd_set_master_volume_now(&g_mixer,
                              snd_vol_from_percent(MC_AUDIO_VOLUME));
    snd_set_volume_ramp(&g_mixer, MC_AUDIO_RATE / 50);

    g_audio_frame = 0;
    g_audio_ready[0] = 0;
    g_audio_ready[1] = 0;

    select_example(0);

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

    dma_hw->inte0 = 0u;
    dma_hw->inte1 = 0u;
    dma_hw->abort = (uint32_t)~0u;
    while (dma_hw->abort)
        tight_loop_contents();
    dma_hw->ints0 = (uint32_t)~0u;
    dma_hw->ints1 = (uint32_t)~0u;
}

static void configure_peripheral_clock(void) {
    uint32_t sys_hz = clock_get_hz(clk_sys);

    if (sys_hz != 0u) {
        clock_configure(clk_peri, 0,
                        CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                        sys_hz, sys_hz);
        sleep_ms(2);
    }
}

static void build_input(mr_demo_input_t *input) {
    memset(input, 0, sizeof(*input));

    if (g_held_keys & MC_KEY_LEFT)
        input->dx -= 1;
    if (g_held_keys & MC_KEY_RIGHT)
        input->dx += 1;
    if (g_held_keys & MC_KEY_UP)
        input->dy -= 1;
    if (g_held_keys & MC_KEY_DOWN)
        input->dy += 1;

    input->buttons = g_edge_buttons;
}

static void present_lace(int phase) {
    int y;
    int buffer_index = 0;
    int dma_started = 0;

    for (y = phase * MC_TILE_H;
         y < MC_H;
         y += MC_TILE_H * 2) {
        int h = MC_TILE_H;
        gfx_color_t *buffer;

        if (y + h > MC_H)
            h = MC_H - y;

        buffer = g_tile[buffer_index];
        g_renderer.tile = buffer;
        gfx_begin_tile(&g_renderer, y, h);

        if (g_example && g_example->render)
            g_example->render(&g_renderer);

        /*
         * Rendering the next band happens before this wait on the following
         * iteration, so CPU rasterization overlaps the previous LCD DMA.
         */
        if (dma_started)
            mr_pico_ili9341_flush_wait(&g_renderer, &g_lcd);

        mr_pico_ili9341_flush_begin(&g_renderer, 0, y, MC_W, h,
                                    buffer, &g_lcd);
        dma_started = 1;
        buffer_index ^= 1;

        /* Audio refill is CPU work and can also overlap the LCD DMA. */
        audio_service();
    }

    if (dma_started)
        mr_pico_ili9341_flush_wait(&g_renderer, &g_lcd);
}

static void user_button_init(void) {
#ifdef PIMORONI_PICO_PLUS2_USER_SW_PIN
    gpio_init(PIMORONI_PICO_PLUS2_USER_SW_PIN);
    gpio_set_dir(PIMORONI_PICO_PLUS2_USER_SW_PIN, GPIO_IN);
    gpio_pull_up(PIMORONI_PICO_PLUS2_USER_SW_PIN);
#endif
}

static void user_button_service(void) {
#ifdef PIMORONI_PICO_PLUS2_USER_SW_PIN
    static int previous = 1;
    static uint64_t last_press_us = 0;
    int now = gpio_get(PIMORONI_PICO_PLUS2_USER_SW_PIN);
    uint64_t t = time_us_64();

    if (previous && !now && t - last_press_us > 180000ull) {
        select_example(g_example_index + 1);
        last_press_us = t;
    }
    previous = now;
#endif
}

int main(void) {
    uint64_t last_us;
    uint64_t tick_accum = 0;
    uint64_t stats_us;
    unsigned long frame = 0;
    unsigned long stats_frame = 0;

    recover_warm_boot();

    if (!set_sys_clock_khz(MC_SYS_KHZ, false))
        panic("MicroConsole: could not set requested system clock");

    configure_peripheral_clock();
    stdio_init_all();

    mr_pico_ili9341_init(&g_lcd);
    mr_pico_ili9341_panel_init(&g_lcd);
    mr_pico_ili9341_fill_screen(&g_lcd, GFX_RGB565_BLACK, MC_W, MC_H);

    gfx_init(&g_renderer, MC_W, MC_H, g_tile[0], MC_TILE_H,
             noop_flush, NULL);

    user_button_init();
    audio_init();

    printf("MicroConsole Pico examples: count=%d tile=%d sys=%lu spi=%u "
           "audio=%s %dHz block=%d volume=%d%%\n",
           mc_example_count(), MC_TILE_H,
           (unsigned long)clock_get_hz(clk_sys), (unsigned)g_lcd.spi_baud_hz,
           device_name(), MC_AUDIO_RATE, MC_AUDIO_BLOCK,
           snd_vol_to_percent(snd_master_volume(&g_mixer)));
    printf("MWPICO1 ready mode=examples; commands: HELP\n");
    fflush(stdout);

    last_us = time_us_64();
    stats_us = last_us;

    for (;;) {
        uint64_t now = time_us_64();
        uint64_t dt = now - last_us;
        int steps = 0;
        mr_demo_input_t input;

        last_us = now;
        if (dt > 250000ull)
            dt = 250000ull;
        tick_accum += dt;

        serial_service();
        user_button_service();
        audio_service();

        build_input(&input);
        while (tick_accum >= MC_TICK_US && steps < 8) {
            if (g_example && g_example->tick)
                g_example->tick(&input);

            input.buttons &= (uint16_t)~MR_DEMO_INPUT_EDGE_MASK;
            g_edge_buttons = 0u;
            tick_accum -= MC_TICK_US;
            ++steps;
        }

        /*
         * Ensure the very first frame has initialized simulation state, just
         * like the DOS frontend.
         */
        if (frame == 0ul && steps == 0) {
            if (g_example && g_example->tick)
                g_example->tick(&input);
            g_edge_buttons = 0u;
        }

        present_lace((int)(frame & 1ul));
        ++frame;

        serial_service();
        user_button_service();
        audio_service();

        now = time_us_64();
        if (now - stats_us >= 1000000ull) {
            uint64_t elapsed = now - stats_us;
            unsigned long df = frame - stats_frame;
            unsigned long fps10 =
                elapsed
                    ? (unsigned long)((uint64_t)df * 10000000ull / elapsed)
                    : 0ul;

            printf("MWPICO1 status example=%s frame=%lu fps=%lu.%lu "
                   "audio=%ld vol=%d cur=%d underrun=%lu\n",
                   g_example ? g_example->id : "(none)",
                   frame, fps10 / 10ul, fps10 % 10ul, g_audio_frame,
                   snd_vol_to_percent(snd_master_volume(&g_mixer)),
                   snd_vol_to_percent(
                       snd_master_volume_current(&g_mixer)),
                   g_audio_underruns);
            fflush(stdout);

            stats_us = now;
            stats_frame = frame;
        }
    }
}
