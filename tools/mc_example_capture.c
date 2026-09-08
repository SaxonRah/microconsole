/* Headless capture and tile-parity harness for the era examples.
 *
 * Two jobs, and the second one is the reason this exists.
 *
 * Capture: run an example for N deterministic frames, write chosen frames as
 * PPM and the accompanying audio as a WAV. That gives every example a
 * reviewable artifact without a window, a GPU, or a sound card, which is what
 * lets them be checked on a build machine.
 *
 * Parity: render every captured frame twice -- once against a full-height
 * 320x240 tile, the way the Raylib and Pico frontends drive the renderer, and
 * once through gfx_render_tiled_no_clear() in 16-row bands, the way the DOS
 * frontend does -- and compare the two images pixel for pixel.
 *
 * That check is not decoration. A per-scanline effect is exactly the kind of
 * code that quietly assumes it can see the whole frame: read a pixel the tile
 * above already wrote, keep state across rows, or index the tile buffer from
 * absolute y. All three work perfectly on a full-height tile and produce
 * garbage in 16-row bands, and the DOS build is where that would be
 * discovered. Comparing the two here turns a target-specific rendering bug
 * into a failed assertion on any machine.
 *
 * Usage:
 *   mc_example_capture [--example ID|all] [--frames N] [--shot F]...
 *                      [--seconds S] [--outdir DIR] [--rate HZ] [--quiet]
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
#define CAP_MAX_SHOTS 16
#define CAP_BLOCK 512

static gfx_color_t g_full[CAP_W * CAP_H];
static gfx_color_t g_tiled[CAP_W * CAP_H];
static gfx_color_t g_band[CAP_W * CAP_TILE_H];
static snd_sample_t g_block[CAP_BLOCK * 2];
static gfx_renderer_t g_renderer;
static snd_mixer_t g_mixer;
static const mc_example_t *g_example;

static FILE *g_wav;
static unsigned long g_wav_samples;

/* The tiled pass writes each 16-row band into g_band and this copies it into
   the comparison image, which is precisely what a real tiled frontend's flush
   callback does. */
static void cap_flush(gfx_renderer_t *r, int x, int y, int w, int h,
                      const gfx_color_t *pixels, void *user) {
  int row;
  (void)r;
  (void)user;
  for (row = 0; row < h; ++row) {
    memcpy(g_tiled + (long)(y + row) * CAP_W + x, pixels + (long)row * w,
           (size_t)w * sizeof(gfx_color_t));
  }
}

static void cap_flush_none(gfx_renderer_t *r, int x, int y, int w, int h,
                           const gfx_color_t *pixels, void *user) {
  (void)r;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
  (void)pixels;
  (void)user;
}

static void cap_draw_scene(gfx_renderer_t *r, void *scene_user) {
  (void)scene_user;
  if (g_example && g_example->render)
    g_example->render(r);
}

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

static int cap_write_ppm(const char *path, const gfx_color_t *pixels) {
  FILE *f = fopen(path, "wb");
  long i;
  if (!f)
    return 0;
  fprintf(f, "P6\n%d %d\n255\n", CAP_W, CAP_H);
  for (i = 0; i < (long)CAP_W * CAP_H; ++i) {
    unsigned c = pixels[i];
    unsigned char rgb[3];
    unsigned r5 = (c >> 11) & 0x1Fu;
    unsigned g6 = (c >> 5) & 0x3Fu;
    unsigned b5 = c & 0x1Fu;
    /* Replicate high bits downward so 0x1F maps to 255 rather than 248. */
    rgb[0] = (unsigned char)((r5 << 3) | (r5 >> 2));
    rgb[1] = (unsigned char)((g6 << 2) | (g6 >> 4));
    rgb[2] = (unsigned char)((b5 << 3) | (b5 >> 2));
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  return 1;
}

static void cap_put32(FILE *f, unsigned long v) {
  fputc((int)(v & 0xFFu), f);
  fputc((int)((v >> 8) & 0xFFu), f);
  fputc((int)((v >> 16) & 0xFFu), f);
  fputc((int)((v >> 24) & 0xFFu), f);
}

static void cap_put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xFFu), f);
  fputc((int)((v >> 8) & 0xFFu), f);
}

static int cap_wav_open(const char *path, int rate, int channels) {
  g_wav = fopen(path, "wb");
  g_wav_samples = 0;
  if (!g_wav)
    return 0;
  fwrite("RIFF", 1, 4, g_wav);
  cap_put32(g_wav, 0); /* patched on close */
  fwrite("WAVEfmt ", 1, 8, g_wav);
  cap_put32(g_wav, 16);
  cap_put16(g_wav, 1);
  cap_put16(g_wav, (unsigned)channels);
  cap_put32(g_wav, (unsigned long)rate);
  cap_put32(g_wav, (unsigned long)rate * (unsigned long)channels * 2ul);
  cap_put16(g_wav, (unsigned)(channels * 2));
  cap_put16(g_wav, 16);
  fwrite("data", 1, 4, g_wav);
  cap_put32(g_wav, 0); /* patched on close */
  return 1;
}

static void cap_wav_close(void) {
  unsigned long data_bytes;
  if (!g_wav)
    return;
  data_bytes = g_wav_samples * 2ul;
  fseek(g_wav, 4, SEEK_SET);
  cap_put32(g_wav, 36ul + data_bytes);
  fseek(g_wav, 40, SEEK_SET);
  cap_put32(g_wav, data_bytes);
  fclose(g_wav);
  g_wav = 0;
}

/* The mixer's drain. A NULL sample pointer means "this many frames of
   silence", which SND_RENDER_SKIP_SILENT produces and a WAV still has to
   contain. */
static void cap_drain(snd_mixer_t *m, long frame, int frames,
                      const snd_sample_t *samples, void *user) {
  long n = (long)frames * (long)m->channels;
  long i;
  (void)frame;
  (void)user;
  if (!g_wav)
    return;
  for (i = 0; i < n; ++i) {
    int v = samples ? (int)samples[i] : 0;
    cap_put16(g_wav, (unsigned)(v & 0xFFFF));
  }
  g_wav_samples += (unsigned long)n;
}

/* ------------------------------------------------------------------ */

static int cap_run(const mc_example_t *ex, int frames, const int *shots,
                   int shot_count, int rate, const char *outdir, int quiet) {
  char path[512];
  mr_demo_input_t input;
  long audio_frame = 0;
  long frames_per_video = rate / 60;
  int f;
  int mismatches = 0;
  int shot_index = 0;

  g_example = ex;
  memset(&input, 0, sizeof(input));

  snd_init(&g_mixer, rate, 1, g_block, CAP_BLOCK, cap_drain, 0);
  snd_set_master_volume_now(&g_mixer, SND_VOL_UNITY);
  snd_set_volume_ramp(&g_mixer, 0);

  sprintf(path, "%s/%s.wav", outdir, ex->id);
  if (!cap_wav_open(path, rate, 1)) {
    fprintf(stderr, "cannot write %s -- does the output directory exist?\n",
            path);
    return -1;
  }

  ex->init(CAP_W, CAP_H, &g_mixer, 0);

  for (f = 0; f < frames; ++f) {
    int want_shot = (shot_index < shot_count && shots[shot_index] == f);

    if (ex->tick)
      ex->tick(&input);

    /* Full-height pass: one tile covering the whole screen. */
    gfx_init(&g_renderer, CAP_W, CAP_H, g_full, CAP_H, cap_flush_none, 0);
    gfx_begin_tile(&g_renderer, 0, CAP_H);
    memset(g_full, 0, sizeof(g_full));
    if (ex->render)
      ex->render(&g_renderer);

    if (want_shot) {
      /* Banded pass: 16-row tiles through the tiled renderer, exactly the
         shape of the DOS frontend, then compare. */
      long i;
      int diff = 0;
      memset(g_tiled, 0, sizeof(g_tiled));
      gfx_init(&g_renderer, CAP_W, CAP_H, g_band, CAP_TILE_H, cap_flush, 0);
      gfx_render_tiled_no_clear(&g_renderer, cap_draw_scene, 0);
      for (i = 0; i < (long)CAP_W * CAP_H; ++i) {
        if (g_full[i] != g_tiled[i]) {
          ++diff;
        }
      }
      if (diff) {
        ++mismatches;
        fprintf(stderr,
                "%s: frame %d differs between full-tile and 16-row tiled "
                "rendering in %d pixels\n",
                ex->id, f, diff);
        sprintf(path, "%s/%s_f%04d_tiled.ppm", outdir, ex->id, f);
        cap_write_ppm(path, g_tiled);
      }
      sprintf(path, "%s/%s_f%04d.ppm", outdir, ex->id, f);
      cap_write_ppm(path, g_full);
      if (!quiet)
        printf("  wrote %s\n", path);
      ++shot_index;
    }

    /* One video frame's worth of audio, so the WAV lines up with the frame
       numbers above. */
    {
      long want = frames_per_video;
      while (want > 0) {
        int n = (want > CAP_BLOCK) ? CAP_BLOCK : (int)want;
        snd_render_one_block(&g_mixer, audio_frame, n, ex->mix, 0,
                             SND_RENDER_SKIP_SILENT);
        audio_frame += n;
        want -= n;
      }
    }

    /* Fire the one-shot occasionally so the sfx path is exercised too. */
    if (ex->sfx && f > 0 && (f % 90) == 0)
      ex->sfx(&g_mixer, audio_frame);
  }

  cap_wav_close();
  if (!quiet)
    printf("  audio: %lu frames -> %s/%s.wav\n", (unsigned long)audio_frame,
           outdir, ex->id);
  return mismatches;
}

static void cap_usage(void) {
  int i;
  printf("usage: mc_example_capture [options]\n"
         "  --example ID   one example, or 'all' (default: all)\n"
         "  --frames N     frames to simulate (default 240)\n"
         "  --shot F       capture frame F; repeatable (default 60 and 180)\n"
         "  --seconds S    ignored; audio length follows --frames\n"
         "  --rate HZ      mix rate (default 32000)\n"
         "  --outdir DIR   where to write (default .)\n"
         "  --quiet        only report parity failures\n\n"
         "examples:\n");
  for (i = 0; i < mc_example_count(); ++i) {
    const mc_example_t *e = mc_example_at(i);
    printf("  %-18s %s\n", e->id, e->system);
  }
}

int main(int argc, char **argv) {
  const char *want = "all";
  const char *outdir = ".";
  int frames = 240;
  int rate = 32000;
  int quiet = 0;
  int shots[CAP_MAX_SHOTS];
  int shot_count = 0;
  int i;
  int parity_failures = 0;
  int io_failures = 0;

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--example") == 0 && i + 1 < argc)
      want = argv[++i];
    else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
      frames = atoi(argv[++i]);
    else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc)
      rate = atoi(argv[++i]);
    else if (strcmp(argv[i], "--outdir") == 0 && i + 1 < argc)
      outdir = argv[++i];
    else if (strcmp(argv[i], "--quiet") == 0)
      quiet = 1;
    else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
      ++i;
    else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {
      if (shot_count < CAP_MAX_SHOTS)
        shots[shot_count++] = atoi(argv[++i]);
      else
        ++i;
    } else {
      cap_usage();
      return strcmp(argv[i], "--help") == 0 ? 0 : 1;
    }
  }

  if (shot_count == 0) {
    shots[shot_count++] = 60;
    shots[shot_count++] = 180;
  }
  /* The parity comparison walks the shot list in order. */
  for (i = 1; i < shot_count; ++i) {
    int j = i;
    while (j > 0 && shots[j - 1] > shots[j]) {
      int t = shots[j - 1];
      shots[j - 1] = shots[j];
      shots[j] = t;
      --j;
    }
  }

  for (i = 0; i < mc_example_count(); ++i) {
    const mc_example_t *e = mc_example_at(i);
    int rc;
    if (strcmp(want, "all") != 0 && strcmp(want, e->id) != 0)
      continue;
    if (!quiet)
      printf("%s -- %s\n  %s\n  %s\n", e->id, e->system, e->technique,
             e->audio);
    rc = cap_run(e, frames, shots, shot_count, rate, outdir, quiet);
    if (rc < 0)
      ++io_failures;
    else if (rc > 0)
      ++parity_failures;
  }

  /* Kept separate so "the output directory does not exist" never reads as
     "the renderer produces different pixels in 16-row bands". */
  if (io_failures) {
    fprintf(stderr, "%d example(s) could not be written\n", io_failures);
    return 2;
  }
  if (parity_failures) {
    fprintf(stderr, "%d example(s) failed tile parity\n", parity_failures);
    return 1;
  }
  if (!quiet)
    printf("tile parity: all captured frames identical under 16-row bands\n");
  return 0;
}
