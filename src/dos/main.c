#include <conio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "mr_stress_test.h"
#include "dos_vga.h"
#include "mc_sb.h"

#define MC_W 320
#define MC_H 240
#define MC_TILE_H 16
#define MC_SPRITES 1024

static gfx_renderer_t g_renderer;
static gfx_color_t g_tile[MC_W * MC_TILE_H];
static mr_stress_test_t g_stress;

static void draw_stress(gfx_renderer_t GFX_PTR *r, void GFX_PTR *user) {
    mr_stress_render(r, (mr_stress_test_t GFX_PTR *)user);
}

int main(int argc, char **argv) {
    mr_stress_config_t cfg;
    unsigned long frames = 0;
    unsigned long start_us, last_us, last_frame;
    unsigned long limit = 2100ul;
    int audio_ok;

    if (argc > 1) limit = (unsigned long)strtoul(argv[1], NULL, 10);

    mr_stress_config_defaults(&cfg, MC_W, MC_H);
    cfg.sprite_count = MC_SPRITES;
    cfg.stats_sample_rate = 8;
    cfg.features = MR_STRESS_FEATURE_DEFAULT;
    gfx_init(&g_renderer, MC_W, MC_H, g_tile, MC_TILE_H,
             dos_vga_flush_tile, 0);
    mr_stress_init(&g_stress, &cfg);

    printf("MicroConsole DOS: MicroRender stress + MicroWave Sound Blaster\n");
    printf("sprites=%d tile=%d frames=%lu (0 = forever)\n", MC_SPRITES, MC_TILE_H, limit);
    audio_ok = mc_sb_init();
    if (!audio_ok) {
        printf("WARNING: Sound Blaster init failed; graphics will still run.\n");
    }

    dos_vga_enter();
    start_us = dos_vga_micros();
    last_us = start_us;
    last_frame = 0;

    while (limit == 0ul || frames < limit) {
        unsigned long now;
        if (kbhit()) {
            int ch = getch();
            if (ch == 27) break;
        }
        if (audio_ok) mc_sb_service();
        mr_stress_tick(&g_stress);
        gfx_render_tiled_no_clear(&g_renderer, draw_stress, &g_stress);
        ++frames;
        if (audio_ok) mc_sb_service();

        now = dos_vga_micros();
        if (now - last_us >= 1000000ul) {
            unsigned long dt = now - last_us;
            unsigned long df = frames - last_frame;
            unsigned long fps10 = dt ? (df * 10000000ul) / dt : 0ul;
            unsigned long total = now - start_us;
            unsigned long avg10 = total ? (frames * 10000000ul) / total : 0ul;
            mr_stress_set_fps10(&g_stress, fps10, avg10);
            last_us = now; last_frame = frames;
        }
    }

    dos_vga_leave();
    if (audio_ok) mc_sb_shutdown();
    printf("done: frames=%lu audio_frames=%lu\n", frames,
           audio_ok ? mc_sb_frames() : 0ul);
    return 0;
}
