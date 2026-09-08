/* Super Famicom / SNES, 1990 -- S-PPU Mode 7 + S-DSP
 *
 * Display: one background, four signed 8.8 numbers, and a new set of them
 * every scanline.
 *
 *   Mode 7 gives up almost everything to buy one thing. There is a single
 *   background layer, its tile map is 128x128, its tiles are 8-bit colour, and
 *   there are no other layers to speak of. In exchange, the address the PPU
 *   fetches from is not (x + scroll) but a full affine transform:
 *
 *       u = A*(x - X0) + B*(y - Y0) + X0
 *       v = C*(x - X0) + D*(y - Y0) + Y0
 *
 *   A, B, C and D are 8.8 signed fixed point. Set them to a rotation matrix
 *   and the background rotates about (X0, Y0). Scale them and it zooms. That
 *   alone is a nice trick and it is not what Mode 7 is remembered for.
 *
 *   What it is remembered for is HDMA rewriting those four registers between
 *   every scanline. If the scale factor is made proportional to 1/(y -
 *   horizon), each line samples the texture at the rate a plane at that
 *   distance would project to -- and the flat rotating map becomes a ground
 *   plane running to a horizon. There is no 3D hardware anywhere in the
 *   machine. There is a matrix that changes 224 times a frame.
 *
 *   So the loop below computes a new du/dx and dv/dx per scanline and then
 *   walks the row adding them. That is the PPU's inner loop, and it is why
 *   the effect costs the same whether the camera is still or moving.
 *
 *   Two details that are easy to leave out and shouldn't be. The tile map
 *   wraps by default -- 128x128 tiles of 8 pixels is 1024x1024, and past that
 *   the field repeats, which is why so many Mode 7 skies are a flat colour
 *   sitting above a floor that goes on forever. And the horizon needs help:
 *   at the vanishing point a single output pixel covers thousands of texture
 *   pixels, so it aliases violently. Games hid it with a colour gradient
 *   fading the floor into the sky, which is the fog here.
 *
 * Audio: the S-DSP -- interpolation and echo.
 *
 *   The SNES sound chip plays compressed samples, and two things it does to
 *   them on the way out are more responsible for "the SNES sound" than any
 *   composer's choices.
 *
 *   First, resampling is done with a four-point Gaussian kernel, not by
 *   picking the nearest sample. That is a genuine lowpass: it rolls off the
 *   top of everything, and it rolls off more the further a sample is pitched
 *   down. It is why SNES instruments sound soft and slightly muffled even when
 *   the source was bright, and why pitching a sample down two octaves on this
 *   chip sounds mellow rather than crunchy -- which is the exact opposite of
 *   what the same operation does on an Amiga.
 *
 *   Second, the echo unit. A delay line in the sound chip's own RAM, with a
 *   feedback term and -- the part people forget -- an eight-tap FIR filter in
 *   the loop. Because the filter is inside the feedback path, each repeat is
 *   filtered again, so echoes do not merely get quieter, they get duller. Set
 *   the taps to a lowpass and the tail dissolves; set them to something
 *   peaky and the echo rings. Almost every SNES soundtrack has this on,
 *   usually subtly, and it is doing most of the work of making eight mono
 *   voices sound like a room.
 */

#include "mc_ex_chip.h"
#include "mc_ex_gfx.h"
#include "mc_example.h"
#include "mr_strbuf.h"

#include <string.h>

/* ---------------------------------------------------------------- */
/* Mode 7 field                                                      */
/* ---------------------------------------------------------------- */

/* The PPU's Mode 7 field is 128x128 tiles -- 1024x1024 pixels. This is a
   single flat 8-bit surface standing in for it, and its size is the one thing
   in this example that has to know what target it is on: Open Watcom's DOS
   large model has a 16-bit int, so 256*256 is not merely a big array, it is an
   array dimension that does not fit in the type used to express it. 128x128 is
   16 KiB and sits comfortably inside a segment. The field simply repeats twice
   as often, which is the same wrapping behaviour the real hardware has. */
#ifndef M7_TEX
#if defined(GFX_INT_IS_16BIT) && GFX_INT_IS_16BIT
#define M7_TEX 128
#else
#define M7_TEX 256
#endif
#endif
#define M7_MASK (M7_TEX - 1)
#define M7_HORIZON 96 /* screen row the vanishing point sits on */

static struct {
  unsigned long frame;
  int angle;      /* 0..1023 */
  int cam_x, cam_y;
  int speed;
  int show_debug;
  int fog;
  uint8_t tex[M7_TEX * M7_TEX];
  gfx_color_t pal[256];
} m7;

/* An 8-bit indexed texture, which is what Mode 7 tiles actually are: one byte
   per pixel straight into CGRAM, no sub-palettes and no attribute grid. It is
   the only mode on the machine with a flat 256-colour space, and it is one of
   the things it spent its layers on. */
static void m7_build_texture(void) {
  int x, y;
  uint32_t rng = 0x9E3779B9u;
  for (y = 0; y < M7_TEX; ++y) {
    for (x = 0; x < M7_TEX; ++x) {
      int u = x, v = y;
      int c;
      /* A closed circuit: distance from a rounded rectangle path. */
      int dx = u - M7_TEX / 2;
      int dy = v - M7_TEX / 2;
      int adx = dx < 0 ? -dx : dx;
      int ady = dy < 0 ? -dy : dy;
      int ring = (adx > ady) ? adx : ady;
      int mix = (adx + ady) / 2;
      int d = (ring * 3 + mix) / 4;

      if (d > M7_TEX / 4 - 2 && d < M7_TEX / 4 + 32) {
        /* Road surface, with a dashed centre line and kerbs at the edges. */
        if (d < M7_TEX / 4 + 2 || d > M7_TEX / 4 + 28)
          c = ((u + v) & 8) ? 3 : 4; /* red/white kerb */
        else if (d > M7_TEX / 4 + 13 && d < M7_TEX / 4 + 17 && (((u + v) >> 3) & 1))
          c = 5; /* dashes */
        else
          c = 1 + (int)((mc_rand(&rng) & 1u));
      } else if (d <= M7_TEX / 4 - 2) {
        c = 8 + (int)((unsigned)((u >> 4) ^ (v >> 4)) & 1u); /* infield */
      } else {
        c = 6 + (int)((unsigned)((u >> 3) ^ (v >> 3)) & 1u); /* grass */
      }
      /* Start/finish line. */
      if (v > M7_TEX / 2 - 8 && v < M7_TEX / 2 + 8 &&
          d > M7_TEX / 4 - 2 && d < M7_TEX / 4 + 32 && u > M7_TEX / 2)
        c = ((u ^ v) & 4) ? 4 : 0;
      m7.tex[y * M7_TEX + x] = (uint8_t)c;
    }
  }
}

static void m7_build_palette(void) {
  int i;
  memset(m7.pal, 0, sizeof(m7.pal));
  m7.pal[0] = GFX_RGB565(20, 20, 24);
  m7.pal[1] = GFX_RGB565(64, 64, 72);
  m7.pal[2] = GFX_RGB565(78, 78, 86);
  m7.pal[3] = GFX_RGB565(200, 40, 40);
  m7.pal[4] = GFX_RGB565(235, 235, 235);
  m7.pal[5] = GFX_RGB565(230, 220, 90);
  m7.pal[6] = GFX_RGB565(38, 110, 44);
  m7.pal[7] = GFX_RGB565(30, 92, 38);
  m7.pal[8] = GFX_RGB565(120, 96, 60);
  m7.pal[9] = GFX_RGB565(104, 84, 52);
  for (i = 10; i < 256; ++i)
    m7.pal[i] = GFX_RGB565(i, i / 2, 255 - i);
}

/* ---------------------------------------------------------------- */
/* S-DSP                                                             */
/* ---------------------------------------------------------------- */

#define SDSP_VOICES 8
#define SDSP_SAMPLES 3
#define SDSP_SAMPLE_LEN 2048
#define SDSP_ECHO_MAX 8192 /* the DSP's echo buffer lives in its own RAM */

typedef struct sdsp_voice {
  const int16_t *data;
  uint32_t len;
  uint32_t loop;
  uint32_t pos;   /* 16.16 into the sample */
  uint32_t step;
  int16_t gain;   /* 8.8 */
  snd_env_t env;  /* MicroWave's envelope: the S-DSP's ADSR has the same shape */
  long start;
  long release;
  int echo_send;  /* EON: whether this voice feeds the echo bus */
  int active;
} sdsp_voice_t;

static struct {
  int rate;
  long frame;
  long row_acc;
  long row_period;
  int row;

  int16_t sample[SDSP_SAMPLES][SDSP_SAMPLE_LEN];
  uint32_t sample_len[SDSP_SAMPLES];
  uint32_t sample_loop[SDSP_SAMPLES];

  sdsp_voice_t voice[SDSP_VOICES];
  int next_voice;

  /* Echo unit. */
  int16_t echo_buf[SDSP_ECHO_MAX];
  long echo_len;  /* EDL: 16 ms per step on real hardware */
  long echo_pos;
  int echo_fb;    /* EFB, 8.8 */
  int echo_vol;   /* EVOL, 8.8 */
  int fir[8];     /* the eight FIR taps, 8.8, summing to about unity */
  int16_t fir_hist[8];
  int fir_pos;

  /* Gaussian interpolation table, one quarter of the kernel. */
  int16_t gauss[257];
} dsp;

/* The real chip has a 512-entry table baked in ROM. Its shape is close enough
   to a raised cosine that generating one keeps this readable without changing
   what it teaches: a four-point kernel that is a genuine lowpass, applied on
   every sample fetch whether the pitch needs resampling or not. */
static void sdsp_build_gauss(void) {
  int i;
  long total = 0;
  for (i = 0; i <= 256; ++i) {
    /* Half a raised cosine across the quarter: rises from zero at the far tap
       to its peak at the near one. */
    dsp.gauss[i] = (int16_t)((mc_cos(i / 2) + 32767) / 2 / 64);
    total += dsp.gauss[i];
  }
  /* Normalize so the four taps a fetch uses sum to 2048, matching the shift
     the fetch divides by. An unnormalized kernel would change the volume with
     the fractional position, which is audible as a buzz at the sample rate. */
  if (total > 0) {
    for (i = 0; i <= 256; ++i)
      dsp.gauss[i] = (int16_t)(((long)dsp.gauss[i] * 512L * 257L) / total);
  }
}

/* Four-tap Gaussian fetch at a 16.16 position. This is the whole reason a
   SNES sample sounds like a SNES sample. */
static int32_t sdsp_gauss_fetch(const int16_t *data, uint32_t len,
                                uint32_t pos) {
  uint32_t i = pos >> 16;
  int f = (int)((pos >> 8) & 0xFFu); /* 8 bits of fraction, as on hardware */
  long g0, g1, g2, g3;
  long out;
  uint32_t i0, i1, i2, i3;
  if (len == 0)
    return 0;
  i0 = (i + len - 1) % len;
  i1 = i % len;
  i2 = (i + 1) % len;
  i3 = (i + 2) % len;
  g0 = dsp.gauss[255 - f];
  g1 = dsp.gauss[511 - f > 256 ? 256 : 511 - f];
  g2 = dsp.gauss[256 + f > 256 ? 256 : 256 + f];
  g3 = dsp.gauss[f];
  out = (long)data[i0] * g0 + (long)data[i1] * g1 + (long)data[i2] * g2 +
        (long)data[i3] * g3;
  return snd_clip_sample(out >> 11);
}

/* Karplus-Strong: a burst of noise round a delay line with a two-tap average
   in it. Two lines of arithmetic that produce a plucked string, which is
   exactly the sort of thing a sample-based console got asked for and exactly
   the sort of thing that would not fit in ROM as raw audio. */
static void sdsp_bake_pluck(int16_t *dst, uint32_t len, int period) {
  uint32_t i;
  uint32_t rng = 0x1BADC0DEu;
  for (i = 0; i < (uint32_t)period && i < len; ++i)
    dst[i] = (int16_t)(((int32_t)(mc_rand(&rng) & 0xFFFFu) - 32768) / 2);
  for (i = (uint32_t)period; i < len; ++i) {
    int32_t a = dst[i - (uint32_t)period];
    int32_t b = dst[i - (uint32_t)period + 1];
    /* The two-tap average and the 249/256 loss term are the whole filter.
       Both operands are full-scale samples, so the multiply has to be 32-bit
       or the string decays into noise on a 16-bit target. */
    dst[i] = (int16_t)(((a + b) / 2) * 249 / 256);
  }
}

static void sdsp_bake_marimba(int16_t *dst, uint32_t len, int hz, int rate) {
  uint32_t i;
  mc_osc_t o1, o2;
  int32_t amp = 32767;
  mc_osc_reset(&o1);
  mc_osc_reset(&o2);
  mc_osc_set_hz(&o1, rate, ((int32_t)(hz) << 8));
  mc_osc_set_hz(&o2, rate, ((int32_t)((hz * 4)) << 8)); /* the bar's 4th partial */
  for (i = 0; i < len; ++i) {
    int32_t v;
    MC_OSC_ADVANCE(&o1);
    MC_OSC_ADVANCE(&o2);
    v = ((mc_wave_sine(o1.phase) * 3) >> 2) + (mc_wave_sine(o2.phase) >> 2);
    dst[i] = (int16_t)(((long)v * amp) >> 15);
    amp -= amp >> 9;
  }
}

static void sdsp_bake_snare(int16_t *dst, uint32_t len, int rate) {
  uint32_t i;
  uint32_t rng = 0xF00Du;
  int32_t amp = 32767;
  mc_lp1_t lp;
  mc_osc_t body;
  lp.z = 0;
  mc_osc_reset(&body);
  mc_osc_set_hz(&body, rate, ((int32_t)(190) << 8));
  for (i = 0; i < len; ++i) {
    int32_t noise = (int32_t)(mc_rand(&rng) & 0xFFFFu) - 32768;
    int32_t v;
    MC_OSC_ADVANCE(&body);
    v = mc_lp1(&lp, noise, 26000) + (mc_wave_sine(body.phase) >> 2);
    dst[i] = (int16_t)(((long)snd_clip_sample(v) * amp) >> 15);
    amp -= amp >> 7;
  }
}

static void sdsp_init(const snd_mixer_t *m) {
  int i;
  memset(&dsp, 0, sizeof(dsp));
  dsp.rate = m ? m->rate : SND_DEFAULT_RATE;
  dsp.row_period = mc_frames_per_tick(m, 60) * 7;
  sdsp_build_gauss();

  dsp.sample_len[0] = SDSP_SAMPLE_LEN;
  dsp.sample_loop[0] = 200;
  sdsp_bake_pluck(dsp.sample[0], dsp.sample_len[0], 96);
  dsp.sample_len[1] = SDSP_SAMPLE_LEN;
  dsp.sample_loop[1] = 0;
  sdsp_bake_marimba(dsp.sample[1], dsp.sample_len[1], 220, dsp.rate);
  dsp.sample_len[2] = 1200;
  dsp.sample_loop[2] = 0;
  sdsp_bake_snare(dsp.sample[2], dsp.sample_len[2], dsp.rate);

  /* Echo. About 190 ms, moderate feedback, and a gently falling FIR so each
     repeat is duller than the last. */
  /* (long), because rate * 190 is 4.2 million at 22 kHz. On a 16-bit int this
     wraps to a nonsense delay length and the echo turns into a buzz. */
  dsp.echo_len = ((long)dsp.rate * 190L) / 1000L;
  if (dsp.echo_len > SDSP_ECHO_MAX)
    dsp.echo_len = SDSP_ECHO_MAX;
  dsp.echo_fb = 150;
  dsp.echo_vol = 96;
  dsp.fir[0] = 48;
  dsp.fir[1] = 40;
  dsp.fir[2] = 34;
  dsp.fir[3] = 30;
  dsp.fir[4] = 26;
  dsp.fir[5] = 22;
  dsp.fir[6] = 18;
  dsp.fir[7] = 14;

  for (i = 0; i < SDSP_VOICES; ++i)
    dsp.voice[i].active = 0;
}

static void sdsp_key_on(int sample, int note, int base_note, int16_t gain,
                        int echo_send, long attack, long decay, int16_t sustain,
                        long release) {
  sdsp_voice_t *v = &dsp.voice[dsp.next_voice];
  int hz = snd_note_hz(note);
  int base_hz = snd_note_hz(base_note);
  dsp.next_voice = (dsp.next_voice + 1) % SDSP_VOICES;
  v->data = dsp.sample[sample];
  v->len = dsp.sample_len[sample];
  v->loop = dsp.sample_loop[sample];
  /* Pitch is a ratio against the sample's own rate. Nothing here is a
     bandlimited resample -- the Gaussian kernel is the only filter in the
     path, which is exactly the point. */
  v->step = (uint32_t)(((int64_t)hz << 16) / (base_hz > 0 ? base_hz : 1));
  v->pos = 0;
  v->gain = gain;
  v->start = dsp.frame;
  v->release = -1;
  v->echo_send = echo_send;
  v->active = 1;
  snd_env_init(&v->env, attack, decay, sustain, release);
}

static const uint8_t m7_lead[32] = {76, 0,  0,  79, 0,  81, 0,  0,
                                    83, 0,  81, 0,  79, 0,  76, 0,
                                    74, 0,  0,  76, 0,  79, 0,  0,
                                    81, 0,  79, 0,  76, 0,  0,  0};
static const uint8_t m7_bass[32] = {40, 0, 0, 47, 0, 40, 0, 0,
                                    45, 0, 0, 52, 0, 45, 0, 0,
                                    38, 0, 0, 45, 0, 38, 0, 0,
                                    43, 0, 0, 50, 0, 43, 0, 0};
static const uint8_t m7_perc[32] = {1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0,
                                    0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0,
                                    0, 1, 1, 0, 0, 0, 1, 0, 1, 1};

static void sdsp_row(void) {
  int r = dsp.row & 31;
  int n = m7_lead[r];
  if (n)
    sdsp_key_on(1, n, 69, (int16_t)(SND_GAIN_UNITY * 3 / 4), 1, 60, 4000,
                (int16_t)(SND_GAIN_UNITY / 2), 6000);
  n = m7_bass[r];
  if (n)
    sdsp_key_on(0, n, 45, (int16_t)(SND_GAIN_UNITY * 2 / 3), 0, 40, 5000,
                (int16_t)(SND_GAIN_UNITY / 2), 4000);
  if (m7_perc[r])
    sdsp_key_on(2, 69, 69, (int16_t)(SND_GAIN_UNITY * 2 / 3), 1, 2, 1500, 0, 400);
  ++dsp.row;
  if (dsp.row >= 32)
    dsp.row = 0;
}

static void m7_mix(snd_mixer_t *m, void *user) {
  int i, frames, v;
  (void)user;
  if (!m)
    return;
  frames = m->block_frames;
  if (frames <= 0)
    return;
  snd_touch_block(m);

  for (i = 0; i < frames; ++i) {
    int32_t dry = 0;
    int32_t send = 0;
    int32_t echoed;
    int32_t filtered;

    if (--dsp.row_acc <= 0) {
      dsp.row_acc = dsp.row_period;
      sdsp_row();
    }

    for (v = 0; v < SDSP_VOICES; ++v) {
      sdsp_voice_t *vo = &dsp.voice[v];
      int32_t s;
      int16_t level;
      if (!vo->active)
        continue;
      s = sdsp_gauss_fetch(vo->data, vo->len, vo->pos);
      level = snd_env_level(&vo->env, dsp.frame - vo->start,
                            vo->release < 0 ? -1 : dsp.frame - vo->release);
      s = (int32_t)(((long)s * vo->gain) >> 8);
      s = (int32_t)(((long)s * level) >> 8);
      dry += s;
      if (vo->echo_send)
        send += s;
      vo->pos += vo->step;
      if ((vo->pos >> 16) >= vo->len) {
        if (vo->loop)
          vo->pos = (vo->loop << 16) + (vo->pos - (vo->len << 16));
        else
          vo->active = 0;
      }
      if (level <= 0 && (dsp.frame - vo->start) > 200)
        vo->active = 0;
    }

    /* Read the delay line, run it through the eight-tap FIR, and write the
       new input plus the filtered feedback back in. The filter being inside
       the loop is the whole difference between a delay and an echo unit:
       every repeat is filtered again, so the tail loses its top end as it
       fades rather than just getting quieter. */
    {
      long acc = 0;
      int t;
      dsp.fir_hist[dsp.fir_pos & 7] = dsp.echo_buf[dsp.echo_pos];
      for (t = 0; t < 8; ++t) {
        int idx = (dsp.fir_pos - t) & 7;
        acc += (long)dsp.fir_hist[idx] * (long)dsp.fir[t];
      }
      dsp.fir_pos = (dsp.fir_pos + 1) & 7;
      filtered = (int32_t)(acc >> 8);
    }
    echoed = filtered;

    {
      long w = (long)send + (((long)echoed * dsp.echo_fb) >> 8);
      dsp.echo_buf[dsp.echo_pos] = (int16_t)snd_clip_sample(w);
      ++dsp.echo_pos;
      if (dsp.echo_pos >= dsp.echo_len)
        dsp.echo_pos = 0;
    }

    mc_out(m, i,
           snd_clip_sample((long)dry + (((long)echoed * dsp.echo_vol) >> 8)),
           128);
    ++dsp.frame;
  }
}

static void m7_sfx(const snd_mixer_t *m, long at_frame) {
  (void)m;
  (void)at_frame;
  sdsp_key_on(0, 88, 45, (int16_t)(SND_GAIN_UNITY / 2), 1, 5, 2000,
              (int16_t)(SND_GAIN_UNITY / 8), 8000);
}

/* ---------------------------------------------------------------- */
/* simulation and render                                             */
/* ---------------------------------------------------------------- */

static void m7_init(int screen_w, int screen_h, const snd_mixer_t *m,
                    long start_frame) {
  (void)screen_w;
  (void)screen_h;
  (void)start_frame;
  memset(&m7, 0, sizeof(m7));
  m7.cam_x = (M7_TEX / 2) << 8;
  m7.cam_y = (M7_TEX * 3 / 4) << 8;
  m7.speed = 140;
  m7.show_debug = 1;
  m7.fog = 1;
  m7_build_texture();
  m7_build_palette();
  sdsp_init(m);
}

static void m7_tick(const mr_demo_input_t *input) {
  ++m7.frame;
  m7.angle = (m7.angle + 2) & 1023;
  if (input) {
    if (input->dx)
      m7.angle = (m7.angle + input->dx * 4) & 1023;
    if ((input->buttons & MR_DEMO_INPUT_DEBUG) != 0u)
      m7.show_debug = !m7.show_debug;
    if ((input->buttons & MR_DEMO_INPUT_ACTION) != 0u)
      m7.fog = !m7.fog;
  }
  /* Drive forward along the current heading. */
  m7.cam_x += (mc_sin(m7.angle) / 128) * m7.speed / 256;
  m7.cam_y -= (mc_cos(m7.angle) / 128) * m7.speed / 256;
}

static void m7_render(gfx_renderer_t *r) {
  int y, y0, y1;
  int sin_a = mc_sin(m7.angle);
  int cos_a = mc_cos(m7.angle);
  gfx_color_t sky_top = GFX_RGB565(20, 34, 90);
  gfx_color_t sky_bottom = GFX_RGB565(150, 170, 220);
  if (!r)
    return;
  mc_rows(r, &y0, &y1);

  for (y = y0; y < y1; ++y) {
    int x0, tw, x;
    gfx_color_t *row = mc_row(r, y, &x0, &tw);
    long scale, du, dv, u, v;
    int dist;
    if (!row)
      continue;

    if (y < M7_HORIZON) {
      /* Above the horizon there is no background: Mode 7's one layer is the
         floor, so the sky is a colour gradient written per line. Games that
         wanted a real sky up here used a second mode entirely and switched
         mid-frame. */
      gfx_color_t c =
          mc_rgb_lerp(sky_top, sky_bottom, (int)(((long)y * 256L) / M7_HORIZON));
      for (x = 0; x < tw; ++x)
        row[x] = c;
      continue;
    }

    /* The per-scanline matrix. dist is how far in front of the camera this
       row lands; the projection is a division, and it is the only one -- the
       row itself is two adds per pixel. */
    dist = y - M7_HORIZON + 1;
    scale = (200L * 256L) / dist; /* 16.8 texels per screen pixel */

    /* Scale the heading vector once, and shift before multiplying by
       anything else.

       Written the obvious way -- (sin_a * scale * 90) >> 15 -- this overflows
       a 32-bit long: sin_a reaches 32767 and scale reaches 51200 near the
       horizon, so their product is already 1.6e9 and the 90 puts it past
       1e11. That is silently fine on a host where long is 64-bit and wrong
       on both DOS and the RP2350, which is exactly the kind of difference
       the cross-target diff exists to catch. Shifting first keeps every
       intermediate inside 32 bits. */
    du = ((long)cos_a * scale) >> 15;
    dv = ((long)sin_a * scale) >> 15;

    /* Start half a screen to the left of the camera, transformed. */
    u = (long)m7.cam_x - (dv * 90L) / 256L * 256L - du * (long)(MC_EX_W / 2);
    v = (long)m7.cam_y + (du * 90L) / 256L * 256L - dv * (long)(MC_EX_W / 2);
    /* Move the sample point forward along the heading by the row's distance,
       which is what puts the camera behind the near edge of the field. */
    u += (dv * 100L) >> 5;
    v -= (du * 100L) >> 5;

    for (x = 0; x < tw; ++x) {
      int sx = x0 + x;
      int tu, tv;
      long uu = u + du * sx;
      long vv = v + dv * sx;
      tu = (int)((uu >> 8) & M7_MASK);
      tv = (int)((vv >> 8) & M7_MASK);
      row[x] = m7.pal[m7.tex[tv * M7_TEX + tu]];
    }

    /* Distance fog. Without it the horizon aliases into noise, because one
       pixel up there covers more texels than the texture has. Every Mode 7
       game had some version of this, usually as a per-line colour-math
       fixed-colour blend. */
    if (m7.fog) {
      int t = 220 - (dist * 220) / 40;
      if (t > 0) {
        for (x = 0; x < tw; ++x)
          row[x] = mc_rgb_lerp(row[x], sky_bottom, t);
      }
    }
  }

  mc_text_shadow(r, 8, 8, "SNES MODE 7 - ONE LAYER, A NEW MATRIX PER LINE",
                 GFX_RGB565(230, 235, 245), 1);
  if (m7.show_debug) {
    char buf[64];
    char *p = buf;
    const char *end = buf + sizeof(buf) - 1;
    p = mr_strbuf_str(p, end, "ANGLE ");
    p = mr_strbuf_u32(p, end, (unsigned long)m7.angle);
    p = mr_strbuf_str(p, end, "  HDMA ROWS ");
    p = mr_strbuf_u32(p, end, (unsigned long)(MC_EX_H - M7_HORIZON));
    *p = '\0';
    mc_text_shadow(r, 8, 18, buf, GFX_RGB565(200, 210, 230), 1);
    mc_text_shadow(r, 8, MC_EX_H - 22,
                   "ECHO: 8-TAP FIR INSIDE THE FEEDBACK LOOP",
                   GFX_RGB565(230, 225, 160), 1);
    mc_text_shadow(r, 8, MC_EX_H - 12,
                   "RESAMPLING: 4-POINT GAUSSIAN, ALWAYS ON",
                   GFX_RGB565(230, 225, 160), 1);
  }
}

/* ---------------------------------------------------------------- */

static const char *const m7_notes[] = {
    "One background layer, 8-bit colour, transformed by a 2x2 matrix.",
    "HDMA rewrites the matrix every scanline: 1/distance makes a floor.",
    "There is no 3D hardware. There is a division per row.",
    "Fog at the horizon hides aliasing one pixel covers 1000 texels wide.",
    "S-DSP resamples with a 4-point Gaussian: the softness is the chip.",
    "Echo runs an 8-tap FIR inside the feedback loop, so repeats get duller.",
    0};

static const mc_example_t m7_example = {
    "snes-mode7",
    "Super Famicom / SNES (1990), S-PPU Mode 7 + S-DSP",
    "per-scanline affine matrices, 1/distance projection, horizon fog",
    "4-point Gaussian resampling, echo unit with an 8-tap FIR in the feedback path",
    m7_notes,
    m7_init,
    m7_tick,
    m7_render,
    m7_mix,
    m7_sfx};

const mc_example_t *mc_example_snes_mode7(void) { return &m7_example; }
