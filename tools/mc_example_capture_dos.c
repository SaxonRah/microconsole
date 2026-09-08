/* DOS-side capture, for comparing what a 16-bit build produces against what
 * the host produces.
 *
 * The host harness (tools/mc_example_capture.c) proves an example renders the
 * same picture under a full-height tile and under 16-row bands. It cannot
 * prove anything about a 16-bit int, because the machine it runs on does not
 * have one. That is the whole risk of this port: Open Watcom's DOS targets
 * use a 16-bit int even in large model, and an expression that overflows one
 * does not warn, it just quietly produces a different number.
 *
 * So this renders the same deterministic frames on DOS, writes them out, and
 * lets the host diff them. A single wrapped multiply anywhere in an example
 * shows up as changed pixels. Nothing else finds that class of bug.
 *
 * Two deliberate differences from the host tool:
 *
 * It never holds a whole frame. A 320x240 RGB565 buffer is 150 KiB, which is
 * both larger than a real-mode segment and most of the memory this program
 * has. gfx_render_tiled_no_clear() delivers bands top to bottom in order, so
 * the PPM is streamed straight out of the flush callback -- which is exactly
 * how the real DOS frontend presents, and means this exercises the same path.
 *
 * Audio is written as raw signed 16-bit rather than WAV, because the host has
 * the header-writing code already and DOS does not need to grow any.
 *
 *   MCEXCAP <example-id> <frames> <shot-frame>
 */

#include "gfx.h"
#include "mc_example.h"
#include "mr_demo_input.h"
#include "snd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP_W MC_EX_W
#define CAP_H MC_EX_H
#define CAP_TILE_H 16
#define CAP_BLOCK 256

static gfx_color_t g_band[CAP_W * CAP_TILE_H];
static snd_sample_t g_block[CAP_BLOCK];
static gfx_renderer_t g_renderer;
static snd_mixer_t g_mixer;
static const mc_example_t *g_example;
static FILE *g_ppm;
static FILE *g_raw;

static void cap_flush(gfx_renderer_t GFX_PTR *r, int x, int y, int w, int h,
                      const gfx_color_t GFX_PTR *pixels, void GFX_PTR *user) {
  int row, col;
  (void)r;
  (void)x;
  (void)y;
  (void)user;
  if (!g_ppm)
    return;
  /* Bands arrive in order, so this is the whole file, written as it is made. */
  for (row = 0; row < h; ++row) {
    const gfx_color_t GFX_PTR *src = pixels + (long)row * (long)w;
    for (col = 0; col < w; ++col) {
      unsigned c = (unsigned)src[col];
      unsigned r5 = (c >> 11) & 0x1Fu;
      unsigned g6 = (c >> 5) & 0x3Fu;
      unsigned b5 = c & 0x1Fu;
      fputc((int)((r5 << 3) | (r5 >> 2)), g_ppm);
      fputc((int)((g6 << 2) | (g6 >> 4)), g_ppm);
      fputc((int)((b5 << 3) | (b5 >> 2)), g_ppm);
    }
  }
}

static void cap_draw_scene(gfx_renderer_t GFX_PTR *r, void GFX_PTR *user) {
  (void)user;
  if (g_example && g_example->render)
    g_example->render(r);
}

static void cap_drain(snd_mixer_t SND_PTR *m, long frame, int frames,
                      const snd_sample_t SND_PTR *samples, void SND_PTR *user) {
  long n = (long)frames * (long)m->channels;
  long i;
  (void)frame;
  (void)user;
  if (!g_raw)
    return;
  for (i = 0; i < n; ++i) {
    int v = samples ? (int)samples[i] : 0;
    fputc(v & 0xFF, g_raw);
    fputc((v >> 8) & 0xFF, g_raw);
  }
}

int main(int argc, char **argv) {
  mr_demo_input_t input;
  char path[64];
  long audio_frame = 0;
  long per_video;
  int frames, shot, f, idx;

  if (argc < 4) {
    printf("usage: MCEXCAP <example-id> <frames> <shot>\n");
    for (idx = 0; idx < mc_example_count(); ++idx)
      printf("  %s\n", mc_example_at(idx)->id);
    return 1;
  }

  idx = mc_example_find(argv[1]);
  if (idx < 0) {
    printf("unknown example: %s\n", argv[1]);
    return 1;
  }
  g_example = mc_example_at(idx);
  frames = atoi(argv[2]);
  shot = atoi(argv[3]);

  memset(&input, 0, sizeof(input));

  /* 22050 mono, which is what the DOS Sound Blaster path actually mixes at,
     rather than the host's 32000. The host tool is told the same rate for the
     comparison so the two agree sample for sample. */
  snd_init(&g_mixer, 22050, 1, g_block, CAP_BLOCK, cap_drain, 0);
  snd_set_master_volume_now(&g_mixer, SND_VOL_UNITY);
  snd_set_volume_ramp(&g_mixer, 0);
  per_video = 22050L / 60L;

  strcpy(path, "OUT.RAW"); /* 8.3, and the host knows which run it asked for */
  g_raw = fopen(path, "wb");

  g_example->init(CAP_W, CAP_H, &g_mixer, 0);

  for (f = 0; f < frames; ++f) {
    long want;

    if (g_example->tick)
      g_example->tick(&input);

    if (f == shot) {
      strcpy(path, "OUT.PPM");
      g_ppm = fopen(path, "wb");
      if (g_ppm)
        fprintf(g_ppm, "P6\n%d %d\n255\n", CAP_W, CAP_H);
    }

    gfx_init(&g_renderer, CAP_W, CAP_H, g_band, CAP_TILE_H, cap_flush, 0);
    gfx_render_tiled_no_clear(&g_renderer, cap_draw_scene, 0);

    if (g_ppm) {
      fclose(g_ppm);
      g_ppm = 0;
      printf("wrote %s.ppm at frame %d\n", argv[1], f);
    }

    want = per_video;
    while (want > 0) {
      int n = (want > CAP_BLOCK) ? CAP_BLOCK : (int)want;
      snd_render_one_block(&g_mixer, audio_frame, n, g_example->mix, 0, 0u);
      audio_frame += n;
      want -= n;
    }
  }

  if (g_raw)
    fclose(g_raw);
  printf("done: %ld audio frames\n", audio_frame);
  return 0;
}
