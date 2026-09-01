/*
 * MicroConsole DOS frontend.
 *
 * MC_DOS_AUDIO=0 builds the graphics-only MCGFX parity executable;
 * MC_DOS_AUDIO=1 builds the combined MCDEMO executable. Both use the same
 * separately compiled MicroRender objects as MCREF so performance comparisons
 * are meaningful under Open Watcom's 16-bit large memory model.
 */
#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "mr_stress_test.h"
#include "dos_vga.h"

#ifndef MC_DOS_AUDIO
#define MC_DOS_AUDIO 1
#endif

#if MC_DOS_AUDIO
#include "mc_sb.h"
#endif

#define MC_W 320
#define MC_H 240
#define MC_TILE_H 16
#define MC_DEFAULT_SPRITES 1024
#define MC_DEFAULT_FRAMES 2100ul

static gfx_renderer_t g_renderer;
static gfx_color_t g_tile[MC_W * MC_TILE_H];
static mr_stress_test_t g_stress;

static void draw_stress(gfx_renderer_t GFX_PTR *r, void GFX_PTR *user) {
    mr_stress_render(r, (mr_stress_test_t GFX_PTR *)user);
}

static unsigned long parse_ulong_arg(int argc, char **argv, int *i,
                                     unsigned long fallback) {
    char *endp;
    unsigned long value;
    if (*i + 1 >= argc) return fallback;
    ++(*i);
    endp = NULL;
    value = strtoul(argv[*i], &endp, 10);
    if (!endp || endp == argv[*i] || *endp != '\0') return fallback;
    return value;
}

static void print_usage(void) {
    printf("MicroConsole DOS: MicroRender stress + MicroWave Sound Blaster\n");
    printf("Usage: MCDEMO [/sprites N] [/frames N] [/noaudio]\n");
    printf("               [/nohud] [/notri] [/nostats] [/statsrate N]\n");
    printf("\n");
    printf("Benchmark pairs:\n");
    printf("  MCDEMO /sprites 1024 /frames 2100 /noaudio\n");
    printf("  MCDEMO /sprites 1024 /frames 2100\n");
}

int main(int argc, char **argv) {
    mr_stress_config_t cfg;
    unsigned long frames;
    unsigned long frame_limit;
    unsigned long start_tick;
    unsigned long fps_tick;
    unsigned long fps_frame;
    unsigned long start_us;
    unsigned long end_us;
    unsigned long elapsed_us;
    unsigned long fps10;
    unsigned long avg_fps10;
    unsigned long audio_frames;
    int audio_enabled;
    int audio_ok;
    int i;

    frame_limit = MC_DEFAULT_FRAMES;
    audio_enabled = MC_DOS_AUDIO ? 1 : 0;

    mr_stress_config_defaults(&cfg, MC_W, MC_H);
    cfg.sprite_count = MC_DEFAULT_SPRITES;
    cfg.stats_sample_rate = 8;
    cfg.features = MR_STRESS_FEATURE_DEFAULT;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "/?") == 0 || strcmp(argv[i], "-?") == 0 ||
            strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "/sprites") == 0 ||
                   strcmp(argv[i], "-sprites") == 0) {
            unsigned long n;
            n = parse_ulong_arg(argc, argv, &i,
                                (unsigned long)cfg.sprite_count);
            if (n < 1ul) n = 1ul;
            if (n > (unsigned long)MR_STRESS_MAX_SPRITES)
                n = (unsigned long)MR_STRESS_MAX_SPRITES;
            cfg.sprite_count = (int)n;
        } else if (strcmp(argv[i], "/frames") == 0 ||
                   strcmp(argv[i], "-frames") == 0) {
            frame_limit = parse_ulong_arg(argc, argv, &i, frame_limit);
        } else if (strcmp(argv[i], "/noaudio") == 0 ||
                   strcmp(argv[i], "-noaudio") == 0) {
            audio_enabled = 0;
        } else if (strcmp(argv[i], "/nohud") == 0 ||
                   strcmp(argv[i], "-nohud") == 0) {
            cfg.features &= ~MR_STRESS_FEATURE_HUD;
        } else if (strcmp(argv[i], "/notri") == 0 ||
                   strcmp(argv[i], "-notri") == 0) {
            cfg.features &= ~MR_STRESS_FEATURE_TRIANGLES;
        } else if (strcmp(argv[i], "/nostats") == 0 ||
                   strcmp(argv[i], "-nostats") == 0) {
            cfg.features &= ~MR_STRESS_FEATURE_STATS;
        } else if (strcmp(argv[i], "/statsrate") == 0 ||
                   strcmp(argv[i], "-statsrate") == 0) {
            unsigned long n;
            n = parse_ulong_arg(argc, argv, &i,
                                (unsigned long)cfg.stats_sample_rate);
            if (n < 1ul) n = 1ul;
            if (n > 32767ul) n = 32767ul;
            cfg.stats_sample_rate = (int)n;
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            /* Keep the original positional frame-count form working. */
            frame_limit = strtoul(argv[i], NULL, 10);
        }
    }

    gfx_init(&g_renderer, MC_W, MC_H, g_tile, MC_TILE_H,
             dos_vga_flush_tile, 0);
    mr_stress_init(&g_stress, &cfg);

    printf("MicroConsole DOS: MicroRender stress + MicroWave Sound Blaster\n");
    printf("sprites=%d tile=%d frames=%lu audio=%s\n",
           cfg.sprite_count, MC_TILE_H, frame_limit,
           audio_enabled ? "on" : "off");
    printf("Press ESC during the run to stop early.\n");

    audio_ok = 0;
#if MC_DOS_AUDIO
    if (audio_enabled) {
        audio_ok = mc_sb_init();
        if (!audio_ok) {
            printf("WARNING: Sound Blaster init failed; graphics will still run.\n");
        }
    }
#else
    (void)audio_enabled;
#endif

    dos_vga_enter();

    /* Match MicroRender's standalone DOS benchmark timing policy.  The PIT
       microsecond timer is accurate but requires port I/O, so keep it out of
       the hot frame loop.  BIOS ticks are cheap enough for HUD FPS updates. */
    start_us = dos_vga_micros();
    start_tick = dos_vga_ticks();
    fps_tick = start_tick;
    fps_frame = 0ul;
    fps10 = 0ul;
    avg_fps10 = 0ul;
    frames = 0ul;
    mr_stress_set_fps10(&g_stress, fps10, avg_fps10);

    while (frame_limit == 0ul || frames < frame_limit) {
        unsigned long now_tick;
        unsigned long dt;

        if (kbhit()) {
            int ch;
            ch = getch();
            if (ch == 27) break;
        }

        /* One DMA-position poll per graphics frame.  At the expected renderer
           rate this checks the 46 ms Sound Blaster half-buffer several times
           before it changes, without doing redundant ISA I/O twice per frame. */
#if MC_DOS_AUDIO
        if (audio_ok) mc_sb_service();
#endif

        mr_stress_tick(&g_stress);
        gfx_render_tiled_no_clear(&g_renderer, draw_stress, &g_stress);
        ++frames;

        now_tick = dos_vga_ticks();
        dt = now_tick - fps_tick;
        if (dt >= 9ul) {
            unsigned long df;
            unsigned long total_dt;
            df = frames - fps_frame;
            fps10 = (df * 182ul) / dt;
            total_dt = now_tick - start_tick;
            if (total_dt != 0ul)
                avg_fps10 = (frames * 182ul) / total_dt;
            mr_stress_set_fps10(&g_stress, fps10, avg_fps10);
            fps_tick = now_tick;
            fps_frame = frames;
        }
    }

    end_us = dos_vga_micros();
    elapsed_us = end_us - start_us;
#if MC_DOS_AUDIO
    audio_frames = audio_ok ? mc_sb_frames() : 0ul;
#else
    audio_frames = 0ul;
#endif

    dos_vga_leave();
#if MC_DOS_AUDIO
    if (audio_ok) mc_sb_shutdown();
#endif

    printf("done: frames=%lu elapsed=%.3f s avg=%.2f fps audio_frames=%lu\n",
           frames,
           (double)elapsed_us / 1000000.0,
           elapsed_us ? ((double)frames * 1000000.0) / (double)elapsed_us : 0.0,
           audio_frames);
    return 0;
}
