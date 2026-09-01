/*
 * Desktop reference frontend for the combined MicroRender + MicroWave load.
 *
 * MicroRender produces RGB565 into a full 320x240 buffer; alternating 8-row
 * groups are uploaded to a Raylib texture while MicroWave feeds AudioStream.
 */
#include "raylib.h"
#include "gfx.h"
#include "mr_stress_test.h"
#include "mw_music_demo.h"
#include "snd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MC_W 320
#define MC_H 240
#define MC_SPRITES 1024
#define MC_LACE_H 8
#define MC_SCALE 3
#define MC_AUDIO_RATE 32000
#define MC_AUDIO_BLOCK 512

static gfx_renderer_t g_renderer;
static mr_stress_test_t g_stress;
static gfx_color_t g_render[MC_W * MC_H];
static gfx_color_t g_present[MC_W * MC_H];

static snd_mixer_t g_mixer;
static mw_demo_t g_demo;
static snd_sample_t g_audio[MC_AUDIO_BLOCK];
static float g_float[MC_AUDIO_BLOCK];
static AudioStream g_stream;
static long g_audio_frame;

static void noop_flush(gfx_renderer_t *r, int x, int y, int w, int h,
                       const gfx_color_t *pixels, void *user) {
    (void)r; (void)x; (void)y; (void)w; (void)h; (void)pixels; (void)user;
}

static void audio_drain(snd_mixer_t *m, long frame, int frames,
                        const snd_sample_t *samples, void *user) {
    long n = (long)frames * (long)m->channels;
    (void)frame; (void)user;
    if (samples) {
        snd_pack_float(samples, g_float, n);
    } else {
        long i;
        for (i = 0; i < n; ++i) g_float[i] = 0.0f;
    }
    UpdateAudioStream(g_stream, g_float, frames);
}

static void audio_service(void) {
    while (IsAudioStreamProcessed(g_stream)) {
        snd_render_one_block(&g_mixer, g_audio_frame, MC_AUDIO_BLOCK,
                             mw_demo_mix, &g_demo, SND_RENDER_SKIP_SILENT);
        g_audio_frame += MC_AUDIO_BLOCK;
    }
}

static void present_lace(Texture2D texture, unsigned long frame) {
    int phase = (int)(frame & 1ul);
    int y;
    for (y = phase * MC_LACE_H; y < MC_H; y += MC_LACE_H * 2) {
        int h = MC_LACE_H;
        Rectangle rect;
        if (y + h > MC_H) h = MC_H - y;
        memcpy(g_present + y * MC_W, g_render + y * MC_W,
               (size_t)MC_W * (size_t)h * sizeof(gfx_color_t));
        rect.x = 0.0f; rect.y = (float)y;
        rect.width = (float)MC_W; rect.height = (float)h;
        UpdateTextureRec(texture, rect, g_present + y * MC_W);
    }
}

int main(void) {
    mr_stress_config_t cfg;
    Image image;
    Texture2D texture;
    Rectangle src = {0, 0, MC_W, MC_H};
    Rectangle dst = {0, 0, MC_W * MC_SCALE, MC_H * MC_SCALE};
    unsigned long frame = 0;
    double start;
    double last_stats;
    unsigned long last_frame = 0;

    memset(g_present, 0, sizeof(g_present));
    gfx_init(&g_renderer, MC_W, MC_H, g_render, MC_H, noop_flush, NULL);
    mr_stress_config_defaults(&cfg, MC_W, MC_H);
    cfg.sprite_count = MC_SPRITES;
    cfg.stats_sample_rate = 8;
    cfg.features = MR_STRESS_FEATURE_DEFAULT;
    mr_stress_init(&g_stress, &cfg);

    InitWindow(MC_W * MC_SCALE, MC_H * MC_SCALE,
               "MicroConsole Demo - MicroRender + MicroWave");
    image.data = g_present;
    image.width = MC_W; image.height = MC_H;
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R5G6B5;
    texture = LoadTextureFromImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);

    InitAudioDevice();
    SetAudioStreamBufferSizeDefault(MC_AUDIO_BLOCK);
    g_stream = LoadAudioStream(MC_AUDIO_RATE, 32, 1);
    snd_init(&g_mixer, MC_AUDIO_RATE, 1, g_audio, MC_AUDIO_BLOCK,
             audio_drain, NULL);
    snd_set_master_gain(&g_mixer, SND_GAIN_UNITY);
    mw_demo_init(&g_demo, &g_mixer, 0, 1);
    PlayAudioStream(g_stream);

    start = GetTime();
    last_stats = start;
    printf("MicroConsole Raylib: 320x240, 1024 sprites, lace=%d, audio=%d Hz\n",
           MC_LACE_H, MC_AUDIO_RATE);

    while (!WindowShouldClose()) {
        double now;
        unsigned long fps10;
        unsigned long avg10;

        audio_service();
        mr_stress_tick(&g_stress);
        gfx_begin_tile(&g_renderer, 0, MC_H);
        mr_stress_render(&g_renderer, &g_stress);
        present_lace(texture, frame);
        ++frame;
        audio_service();

        now = GetTime();
        avg10 = (now > start)
                    ? (unsigned long)((double)frame * 10.0 / (now - start))
                    : 0ul;
        fps10 = (now > last_stats)
                    ? (unsigned long)((double)(frame - last_frame) * 10.0 /
                                      (now - last_stats))
                    : 0ul;
        mr_stress_set_fps10(&g_stress, fps10, avg10);
        if (now - last_stats >= 1.0) {
            printf("stress frame=%lu fps=%lu.%lu avg=%lu.%lu audio=%ld\n",
                   frame, fps10 / 10ul, fps10 % 10ul,
                   avg10 / 10ul, avg10 % 10ul, g_audio_frame);
            last_stats = now;
            last_frame = frame;
        }

        if (IsKeyPressed(KEY_SPACE))
            mw_demo_trigger_sfx(&g_demo, &g_mixer, NULL,
                                g_audio_frame + MC_AUDIO_BLOCK);

        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexturePro(texture, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        DrawText("MicroRender stress + MicroWave audio", 8, 8, 10, RAYWHITE);
        DrawText("SPACE: synth SFX", 8, 22, 10, RAYWHITE);
        EndDrawing();
    }

    UnloadAudioStream(g_stream);
    CloseAudioDevice();
    UnloadTexture(texture);
    CloseWindow();
    return 0;
}
