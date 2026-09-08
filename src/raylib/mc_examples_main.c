/* Desktop frontend for the console-era examples.
 *
 * Deliberately the same shape as src/raylib/main.c: MicroRender rasterizes a
 * full 320x240 RGB565 frame, alternating 8-row groups are uploaded to a
 * texture, and MicroWave fills an AudioStream around the rendering work. The
 * only structural difference is that the workload is selectable, so the same
 * loop drives nine different programs.
 *
 * Two things this frontend has to get right that the stress demo does not:
 *
 * The audio clock does not restart. g_audio_frame keeps counting across an
 * example switch and is handed to the new example's init(), because a mixer
 * frame is an absolute position and the block being filled when the user
 * presses a key is already in flight. Resetting it to zero would make every
 * switch a discontinuity.
 *
 * The simulation runs at a fixed 60 Hz while rendering runs uncapped. The
 * stress demo ties one tick to one frame on purpose -- that is benchmark
 * policy -- but these examples are animations with an intended speed, and a
 * fast machine should not play them fast. The accumulator below is the same
 * argument mr_timestep.h makes.
 */

#include "gfx.h"
#include "mc_example.h"
#include "mr_demo_input.h"
#include "raylib.h"
#include "snd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MC_W MC_EX_W
#define MC_H MC_EX_H
#define MC_LACE_H 8
#define MC_SCALE 3
#define MC_AUDIO_RATE 32000
#define MC_AUDIO_BLOCK 512
#define MC_VOLUME_STEP 5
#define MC_TICK_HZ 60

static gfx_renderer_t g_renderer;
static gfx_color_t g_render[MC_W * MC_H];
static gfx_color_t g_present[MC_W * MC_H];

static snd_mixer_t g_mixer;
static snd_sample_t g_audio[MC_AUDIO_BLOCK];
static float g_float[MC_AUDIO_BLOCK];
static AudioStream g_stream;
static long g_audio_frame;

static const mc_example_t *g_example;
static int g_index;
static int g_overlay = 1;

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
                             g_example ? g_example->mix : NULL, NULL,
                             SND_RENDER_SKIP_SILENT);
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

static void select_example(int index) {
    int count = mc_example_count();
    if (count <= 0) return;
    while (index < 0) index += count;
    index %= count;
    g_index = index;
    g_example = mc_example_at(index);
    /* The audio clock is deliberately not rewound: the example is told where
       the mixer already is and schedules from there. */
    if (g_example && g_example->init)
        g_example->init(MC_W, MC_H, &g_mixer, g_audio_frame);
    printf("\n[%d/%d] %s\n  %s\n  video: %s\n  audio: %s\n",
           index + 1, count, g_example->id, g_example->system,
           g_example->technique, g_example->audio);
    if (g_example->notes) {
        int i;
        for (i = 0; g_example->notes[i]; ++i)
            printf("    - %s\n", g_example->notes[i]);
    }
}

static void gather_input(mr_demo_input_t *in) {
    memset(in, 0, sizeof(*in));
    if (IsKeyDown(KEY_LEFT))  in->dx -= 1;
    if (IsKeyDown(KEY_RIGHT)) in->dx += 1;
    if (IsKeyDown(KEY_UP))    in->dy -= 1;
    if (IsKeyDown(KEY_DOWN))  in->dy += 1;
    if (IsKeyPressed(KEY_Z))  in->buttons |= MR_DEMO_INPUT_ACTION;
    if (IsKeyPressed(KEY_TAB)) in->buttons |= MR_DEMO_INPUT_DEBUG;
}

static int parse_args(int argc, char **argv, int *out_volume, int *out_index) {
    int i;
    *out_volume = 100;
    *out_index = 0;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--volume") == 0 && i + 1 < argc) {
            int v = atoi(argv[++i]);
            *out_volume = v < 0 ? 0 : (v > 100 ? 100 : v);
        } else if (strcmp(argv[i], "--example") == 0 && i + 1 < argc) {
            int found = mc_example_find(argv[++i]);
            if (found < 0) {
                printf("unknown example: %s\n", argv[i]);
                return -2;
            }
            *out_index = found;
        } else if (strcmp(argv[i], "--list") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            int k;
            printf("usage: %s [--example ID] [--volume 0..100]\n\n", argv[0]);
            for (k = 0; k < mc_example_count(); ++k) {
                const mc_example_t *e = mc_example_at(k);
                printf("  %-16s %s\n", e->id, e->system);
            }
            return -1;
        } else {
            printf("unknown argument: %s\n", argv[i]);
            return -2;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    Image image;
    Texture2D texture;
    Rectangle src = {0, 0, MC_W, MC_H};
    Rectangle dst = {0, 0, MC_W * MC_SCALE, MC_H * MC_SCALE};
    unsigned long frame = 0;
    double last_time;
    double accumulator = 0.0;
    const double tick_dt = 1.0 / (double)MC_TICK_HZ;
    int volume_pct = 100;
    int start_index = 0;
    int muted = 0;
    int rc;

    rc = parse_args(argc, argv, &volume_pct, &start_index);
    if (rc == -1) return 0;
    if (rc < 0) return 1;

    memset(g_present, 0, sizeof(g_present));
    gfx_init(&g_renderer, MC_W, MC_H, g_render, MC_H, noop_flush, NULL);

    InitWindow(MC_W * MC_SCALE, MC_H * MC_SCALE,
               "MicroConsole - console era examples");
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
    snd_set_master_volume_now(&g_mixer, snd_vol_from_percent(volume_pct));
    snd_set_volume_ramp(&g_mixer, MC_AUDIO_RATE / 50); /* 20 ms */
    PlayAudioStream(g_stream);

    select_example(start_index);
    printf("\nControls: [ ] switch example, 1-9 jump, TAB overlay,\n"
           "          arrows interact, Z toggle, SPACE sfx, -/+ volume, M mute\n");

    last_time = GetTime();

    while (!WindowShouldClose()) {
        double now = GetTime();
        double dt = now - last_time;
        int steps = 0;
        mr_demo_input_t input;

        last_time = now;
        if (dt > 0.25) dt = 0.25; /* never try to catch up more than this */
        accumulator += dt;

        audio_service();

        gather_input(&input);
        while (accumulator >= tick_dt && steps < 8) {
            if (g_example && g_example->tick)
                g_example->tick(&input);
            /* Edge-triggered buttons are consumed by the first step, exactly
               as mr_demo_input.h asks. */
            input.buttons &= (uint16_t)~MR_DEMO_INPUT_EDGE_MASK;
            accumulator -= tick_dt;
            ++steps;
        }

        gfx_begin_tile(&g_renderer, 0, MC_H);
        if (g_example && g_example->render)
            g_example->render(&g_renderer);
        present_lace(texture, frame);
        ++frame;
        audio_service();

        if (IsKeyPressed(KEY_RIGHT_BRACKET)) select_example(g_index + 1);
        if (IsKeyPressed(KEY_LEFT_BRACKET))  select_example(g_index - 1);
        {
            int k;
            for (k = 0; k < 9 && k < mc_example_count(); ++k)
                if (IsKeyPressed(KEY_ONE + k)) select_example(k);
        }
        if (IsKeyPressed(KEY_TAB)) g_overlay = !g_overlay;

        if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) {
            volume_pct -= MC_VOLUME_STEP;
            if (volume_pct < 0) volume_pct = 0;
            muted = 0;
            snd_set_master_volume(&g_mixer, snd_vol_from_percent(volume_pct));
        }
        if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) {
            volume_pct += MC_VOLUME_STEP;
            if (volume_pct > 100) volume_pct = 100;
            muted = 0;
            snd_set_master_volume(&g_mixer, snd_vol_from_percent(volume_pct));
        }
        if (IsKeyPressed(KEY_M)) {
            muted = !muted;
            snd_set_master_volume(
                &g_mixer,
                muted ? SND_VOL_SILENT : snd_vol_from_percent(volume_pct));
        }
        if (IsKeyPressed(KEY_SPACE) && g_example && g_example->sfx)
            g_example->sfx(&g_mixer, g_audio_frame + MC_AUDIO_BLOCK);

        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexturePro(texture, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        if (g_overlay && g_example) {
            int line = 0;
            DrawRectangle(0, 0, MC_W * MC_SCALE, 64, (Color){0, 0, 0, 170});
            DrawText(TextFormat("[%d/%d] %s", g_index + 1, mc_example_count(),
                                g_example->system),
                     8, 6 + line++ * 14, 12, RAYWHITE);
            DrawText(g_example->technique, 8, 6 + line++ * 14, 10,
                     (Color){180, 210, 255, 255});
            DrawText(g_example->audio, 8, 6 + line++ * 14, 10,
                     (Color){255, 225, 160, 255});
            DrawText(TextFormat("[ ] switch   TAB hide   SPACE sfx   "
                                "VOL %d%%%s",
                                volume_pct, muted ? " MUTE" : ""),
                     8, 6 + line * 14, 10, (Color){170, 170, 180, 255});
        }
        EndDrawing();
    }

    UnloadAudioStream(g_stream);
    CloseAudioDevice();
    UnloadTexture(texture);
    CloseWindow();
    return 0;
}
