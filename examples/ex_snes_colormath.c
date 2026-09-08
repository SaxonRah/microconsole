/* Super Famicom / SNES, 1990 -- S-PPU colour math, windows, mosaic + S-DSP
 *
 * Display: two screens, and arithmetic between them.
 *
 *   Mode 7 is the SNES trick everyone names. Colour math is the one that is
 *   actually in every game on the system.
 *
 *   Each background layer and the sprite layer is assigned to the *main
 *   screen*, the *subscreen*, or neither. The PPU renders both, and then --
 *   per pixel -- adds or subtracts the subscreen from the main screen, with an
 *   optional halving. That is the whole of transparency on the machine: no
 *   alpha channel, no per-pixel coverage, just add, subtract, and divide by
 *   two, at 21 MHz.
 *
 *   It buys a great deal. Main plus half-sub is a translucent overlay. Main
 *   minus sub is a shadow that darkens what is under it rather than replacing
 *   it. Point the subscreen at the fixed colour register instead of a layer
 *   and you get a tint, a fade, or a fog with no second layer spent on it. All
 *   of those are here.
 *
 *   The reason this is not simply "the Mega Drive with blending" is the
 *   windows. Two window ranges per layer, each a left and right edge, combined
 *   with OR, AND, XOR or XNOR -- and, because the edges are just registers,
 *   HDMA can rewrite them every scanline. A per-line pair of edges is an
 *   arbitrary vertical shape, so the region where colour math applies can be a
 *   circle, an iris wipe, a beam of light, or a character-shaped hole. The
 *   spotlight below is one HDMA table of left/right edges and nothing else.
 *
 *   Mosaic is the third register and the cheapest effect on the console: it
 *   makes a layer sample at 1 to 16 pixel granularity. Not a filter, not a
 *   downscale -- the PPU simply holds the last fetched pixel for N columns and
 *   N rows. It is per-layer, so a mosaicking foreground over a sharp
 *   background costs nothing, and it is why almost every SNES death and
 *   teleport animation is a pixelation.
 *
 * Audio: the S-DSP's pitch modulation and its shared noise source.
 *
 *   The Mode 7 example covers what the DSP does to every sample: Gaussian
 *   resampling and the echo unit. This one covers the two features a composer
 *   had to ask for.
 *
 *   Pitch modulation is the strange one. Setting a bit in PMON makes voice
 *   N's *output* modulate voice N+1's pitch, sample by sample. Voice N is
 *   usually silenced and exists only to bend its neighbour. With a slow
 *   modulator that is vibrato; with a modulator in the audio range it is
 *   frequency modulation between two sample-playing voices, which is not what
 *   anyone designed a sample chip to do. It cost two of the eight voices and
 *   produced the growls, warps and detuned basses that date a soundtrack to
 *   this machine instantly.
 *
 *   The noise source is shared: one generator at a rate chosen from a table,
 *   and any voice can be switched to read it instead of its own sample. The
 *   voice keeps its envelope, its volume and its echo send, so a hi-hat costs
 *   no sample memory at all -- which mattered, because the whole chip had
 *   64 KB and the samples had to live in it alongside the echo buffer.
 */

#include "mc_ex_chip.h"
#include "mc_ex_gfx.h"
#include "mc_example.h"
#include "mr_strbuf.h"

#include <string.h>

#define CM_W 256
#define CM_H 224
#define CM_X0 ((MC_EX_W - CM_W) / 2)
#define CM_Y0 8

/* Colour math operations, in the order the register encodes them. */
#define CM_OP_ADD 0
#define CM_OP_ADD_HALF 1
#define CM_OP_SUB 2
#define CM_OP_SUB_HALF 3

static struct {
  unsigned long frame;
  int scroll;
  int op;
  int mosaic;      /* 1..16, the MOSAIC register plus one */
  int use_fixed;   /* subscreen is the fixed colour rather than a layer */
  int show_debug;

  /* The HDMA window table: one left and right edge per scanline. */
  int16_t win_l[MC_EX_H];
  int16_t win_r[MC_EX_H];
  /* HDMA on the fixed colour register, giving a per-line tint. */
  gfx_color_t fixed[MC_EX_H];
} cm;

/* ---------------------------------------------------------------- */
/* the two screens, generated per pixel                              */
/* ---------------------------------------------------------------- */

/* Main screen: a stone chamber with pillars and a floor. Written as a function
   of world coordinates so that mosaic can sample it at block granularity
   without needing a framebuffer -- which is also, near enough, what the PPU
   does when it holds a fetched pixel across a block. */
static gfx_color_t cm_main(int x, int y) {
  int wx = x + cm.scroll;
  int col = ((wx / 48) & 7);
  int in_pillar = ((wx % 48) < 16) && y > 40 && y < 170;

  if (y < 40) {
    /* ceiling */
    int v = 40 + ((wx ^ y) & 7) * 3;
    return GFX_RGB565(v, v - 8, v + 10);
  }
  if (y > 170) {
    /* floor, checkered and receding */
    int b = ((wx >> 4) ^ ((y - 170) >> 3)) & 1;
    int shade = 90 - (y - 170) / 3;
    return b ? GFX_RGB565(shade, shade - 20, shade - 34)
             : GFX_RGB565(shade - 24, shade - 40, shade - 50);
  }
  if (in_pillar) {
    int f = (wx % 48);
    int v = 70 + (f < 3 ? 60 : (f > 12 ? -20 : 0)) + ((y & 3) == 0 ? 12 : 0);
    if (v < 0)
      v = 0;
    if (v > 255)
      v = 255;
    return GFX_RGB565(v, v - 10 < 0 ? 0 : v - 10, v - 30 < 0 ? 0 : v - 30);
  }
  /* back wall */
  {
    int v = 26 + ((wx >> 3) & 1) * 6 + ((y >> 3) & 1) * 4 + col;
    return GFX_RGB565(v, v + 2, v + 14);
  }
}

/* Subscreen: a separate layer, here a drifting body of luminous fog. In a real
   game this would be BG2 with its own tile map and its own scroll registers;
   the point is that it is a full second image, not a mask. */
static gfx_color_t cm_sub(int x, int y) {
  int a = (mc_sin((x * 3 + (int)cm.frame * 4) & 1023) * 40) >> 15;
  int b = (mc_cos((y * 5 - (int)cm.frame * 3) & 1023) * 40) >> 15;
  int v = 80 + a + b;
  if (v < 0)
    v = 0;
  if (v > 255)
    v = 255;
  return GFX_RGB565(v / 3, v / 2, v);
}

/* ---------------------------------------------------------------- */
/* HDMA tables                                                       */
/* ---------------------------------------------------------------- */

static void cm_build_hdma(void) {
  int y;
  int cx = CM_X0 + 128 + ((mc_sin((int)cm.frame * 6) * 70) >> 15);
  int cy = CM_Y0 + 112 + ((mc_cos((int)cm.frame * 4) * 40) >> 15);
  int radius = 52 + ((mc_sin((int)cm.frame * 9) * 16) >> 15);

  for (y = 0; y < MC_EX_H; ++y) {
    /* A circle expressed as one left and one right edge per scanline, which is
       exactly the shape a window register pair can hold. Anything convex is
       free; anything else needs two windows and a combination rule. */
    int dy = y - cy;
    int half = 0;
    if (dy > -radius && dy < radius) {
      int q = radius * radius - dy * dy;
      /* integer square root, small and exact enough */
      int lo = 0, hi = radius;
      while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (mid * mid <= q)
          lo = mid;
        else
          hi = mid - 1;
      }
      half = lo;
    }
    cm.win_l[y] = (int16_t)(half ? cx - half : 0);
    cm.win_r[y] = (int16_t)(half ? cx + half : 0);

    /* HDMA on the fixed colour: a warm-to-cool tint down the frame. One
       register, 224 writes, no layer spent. */
    {
      int t = (y * 255) / MC_EX_H;
      cm.fixed[y] = GFX_RGB565(120 - t / 3, 60 + t / 4, 40 + t / 2);
    }
  }
}

/* ---------------------------------------------------------------- */
/* S-DSP: pitch modulation and the shared noise source                */
/* ---------------------------------------------------------------- */

#define CM_VOICES 8
#define CM_SAMPLE_LEN 1600

typedef struct cm_voice {
  const int16_t *data;
  uint32_t len;
  uint32_t loop;
  uint32_t pos;
  uint32_t step;      /* base pitch, before modulation */
  int pitch_mod;      /* PMON: take the previous voice's output as a bend */
  int use_noise;      /* NON: read the shared noise source instead */
  int16_t gain;
  snd_env_t env;
  long start;
  int active;
  int silent;         /* a modulator voice: heard by its neighbour only */
  int last_out;
} cm_voice_t;

static struct {
  int rate;
  long frame;
  long row_acc;
  long row_period;
  int row;

  int16_t growl[CM_SAMPLE_LEN];
  int16_t bell[CM_SAMPLE_LEN];
  int16_t lfo_sample[CM_SAMPLE_LEN];

  cm_voice_t v[CM_VOICES];

  /* One noise generator for the whole chip, at a rate from a fixed table. */
  mc_noise_t noise;
} sd;

static void cm_bake_growl(int16_t *dst, uint32_t len, int rate) {
  uint32_t i;
  mc_osc_t o;
  mc_svf_t f;
  mc_svf_reset(&f);
  mc_osc_reset(&o);
  mc_osc_set_hz(&o, rate, ((int32_t)(110) << 8));
  for (i = 0; i < len; ++i) {
    int32_t v;
    MC_OSC_ADVANCE(&o);
    v = mc_wave_saw(o.phase);
    v = mc_svf(&f, v, mc_svf_cutoff(rate, 900), 22000, MC_SVF_LOW);
    dst[i] = (int16_t)(v * 3 / 4);
  }
}

static void cm_bake_bell(int16_t *dst, uint32_t len, int rate) {
  uint32_t i;
  mc_osc_t a, b;
  int32_t amp = 32767;
  mc_osc_reset(&a);
  mc_osc_reset(&b);
  mc_osc_set_hz(&a, rate, ((int32_t)(440) << 8));
  mc_osc_set_hz(&b, rate, ((int32_t)(1109) << 8)); /* an inharmonic partial */
  for (i = 0; i < len; ++i) {
    int32_t v;
    MC_OSC_ADVANCE(&a);
    MC_OSC_ADVANCE(&b);
    v = (mc_wave_sine(a.phase) * 2 / 3) + (mc_wave_sine(b.phase) / 3);
    dst[i] = (int16_t)(((long)v * amp) >> 15);
    amp -= amp >> 10;
  }
}

/* The modulator voice's sample: a slow sine. It is never mixed into the
   output -- it exists so that the voice after it has something to be bent
   by. Two of eight voices, spent on one instrument. */
static void cm_bake_lfo(int16_t *dst, uint32_t len) {
  uint32_t i;
  for (i = 0; i < len; ++i)
    dst[i] = (int16_t)mc_wave_sine((uint32_t)i * (0xFFFFFFFFu / len));
}

static void cm_audio_init(const snd_mixer_t *m) {
  memset(&sd, 0, sizeof(sd));
  sd.rate = m ? m->rate : SND_DEFAULT_RATE;
  sd.row_period = mc_frames_per_tick(m, 60) * 7;
  cm_bake_growl(sd.growl, CM_SAMPLE_LEN, sd.rate);
  cm_bake_bell(sd.bell, CM_SAMPLE_LEN, sd.rate);
  cm_bake_lfo(sd.lfo_sample, CM_SAMPLE_LEN);
  mc_noise_init(&sd.noise, MC_LFSR_NES_LONG, 0x5EEDu);
  mc_noise_set_hz(&sd.noise, sd.rate, ((int32_t)(16000) << 8));
}

static void cm_key_on(int slot, const int16_t *data, uint32_t len,
                      uint32_t loop, int hz, int base_hz, int16_t gain,
                      int silent, int pitch_mod, int use_noise, long a, long d,
                      int16_t s, long rl) {
  cm_voice_t *v;
  if (slot < 0 || slot >= CM_VOICES)
    return;
  v = &sd.v[slot];
  v->data = data;
  v->len = len;
  v->loop = loop;
  v->pos = 0;
  v->step = (uint32_t)(((int64_t)hz << 16) / (base_hz > 0 ? base_hz : 1));
  v->gain = gain;
  v->silent = silent;
  v->pitch_mod = pitch_mod;
  v->use_noise = use_noise;
  v->start = sd.frame;
  v->active = 1;
  v->last_out = 0;
  snd_env_init(&v->env, a, d, s, rl);
}

static const uint8_t cm_bass[16] = {33, 0, 0, 40, 0, 33, 0, 0,
                                    31, 0, 0, 38, 0, 31, 0, 0};
static const uint8_t cm_bell[16] = {69, 0, 76, 0, 0, 72, 0, 0,
                                    67, 0, 74, 0, 0, 71, 0, 0};
static const uint8_t cm_hat[16] = {1, 0, 1, 1, 0, 1, 0, 1,
                                   1, 0, 1, 1, 0, 1, 1, 1};

static void cm_row(void) {
  int r = cm.frame ? (sd.row & 15) : 0;
  int n;

  n = cm_bass[r];
  if (n) {
    /* Voice 0 is the modulator and is never heard; voice 1 reads its output as
       a pitch bend. The modulator's own frequency decides whether this is
       vibrato or FM -- here it is deliberately in between, which is where the
       growl lives. */
    cm_key_on(0, sd.lfo_sample, CM_SAMPLE_LEN, 0, 40 + (r * 3), 20,
              (int16_t)(SND_GAIN_UNITY), 1, 0, 0, 5, 8000,
              (int16_t)(SND_GAIN_UNITY), 2000);
    cm_key_on(1, sd.growl, CM_SAMPLE_LEN, 0, snd_note_hz(n), 110,
              (int16_t)(SND_GAIN_UNITY / 2), 0, 1, 0, 20, 5000,
              (int16_t)(SND_GAIN_UNITY / 2), 3000);
  }

  n = cm_bell[r];
  if (n)
    cm_key_on(3, sd.bell, CM_SAMPLE_LEN, 0, snd_note_hz(n), 440,
              (int16_t)(SND_GAIN_UNITY / 3), 0, 0, 0, 10, 6000,
              (int16_t)(SND_GAIN_UNITY / 6), 5000);

  if (cm_hat[r])
    /* A voice with no sample: NON points it at the shared noise generator,
       and the envelope and volume still apply. Zero bytes of sample RAM. */
    cm_key_on(5, 0, 0, 0, 1, 1, (int16_t)(SND_GAIN_UNITY / 5), 0, 0, 1, 2,
              900, 0, 300);

  ++sd.row;
  if (sd.row >= 16)
    sd.row = 0;
}

static void cm_mix(snd_mixer_t *m, void *user) {
  int i, frames, k;
  (void)user;
  if (!m)
    return;
  frames = m->block_frames;
  if (frames <= 0)
    return;
  snd_touch_block(m);

  for (i = 0; i < frames; ++i) {
    int32_t acc = 0;
    int prev_out = 0;

    if (--sd.row_acc <= 0) {
      sd.row_acc = sd.row_period;
      cm_row();
    }

    /* Voices are evaluated in order, because pitch modulation reads the voice
       immediately before. That ordering is a hardware constraint, not a
       convenience: PMON can only ever chain N-1 into N. */
    for (k = 0; k < CM_VOICES; ++k) {
      cm_voice_t *v = &sd.v[k];
      int32_t s;
      int16_t level;
      if (!v->active) {
        prev_out = 0;
        continue;
      }
      level = snd_env_level(&v->env, sd.frame - v->start, -1);

      if (v->use_noise) {
        s = mc_noise_next(&sd.noise) / 2;
      } else if (v->data && v->len) {
        uint32_t idx = (v->pos >> 16) % v->len;
        s = v->data[idx];
      } else {
        s = 0;
      }
      s = (int32_t)(((long)s * v->gain) >> 8);
      s = (int32_t)(((long)s * level) >> 8);

      {
        uint32_t step = v->step;
        if (v->pitch_mod) {
          /* The bend is proportional to the previous voice's current output,
             applied to this voice's step every single sample. */
          long bend = ((long)prev_out * (long)step) >> 16;
          long ns = (long)step + bend;
          if (ns < 256)
            ns = 256;
          step = (uint32_t)ns;
        }
        v->pos += step;
      }
      if (!v->use_noise && v->len && (v->pos >> 16) >= v->len) {
        if (v->loop)
          v->pos = v->loop << 16;
        else
          v->active = 0;
      }
      if (level <= 0 && (sd.frame - v->start) > 300)
        v->active = 0;

      v->last_out = s;
      prev_out = s;
      if (!v->silent)
        acc += s;
    }

    mc_out(m, i, snd_clip_sample(acc), 128);
    ++sd.frame;
  }
}

static void cm_sfx(const snd_mixer_t *m, long at_frame) {
  (void)m;
  (void)at_frame;
  cm_key_on(3, sd.bell, CM_SAMPLE_LEN, 0, 1760, 440,
            (int16_t)(SND_GAIN_UNITY * 3 / 4), 0, 0, 0, 4, 4000,
            (int16_t)(SND_GAIN_UNITY / 8), 6000);
}

/* ---------------------------------------------------------------- */
/* simulation                                                        */
/* ---------------------------------------------------------------- */

static void cm_init(int screen_w, int screen_h, const snd_mixer_t *m,
                    long start_frame) {
  (void)screen_w;
  (void)screen_h;
  (void)start_frame;
  memset(&cm, 0, sizeof(cm));
  cm.op = CM_OP_ADD_HALF;
  cm.mosaic = 1;
  cm.show_debug = 1;
  cm_build_hdma();
  cm_audio_init(m);
}

static void cm_tick(const mr_demo_input_t *input) {
  int phase;
  ++cm.frame;
  cm.scroll += 1;

  /* Walk the four colour-math operations, a couple of seconds each, so all of
     them are seen rather than described. */
  cm.op = (int)((cm.frame / 150ul) & 3ul);
  cm.use_fixed = (int)(((cm.frame / 150ul) >> 2) & 1ul);

  /* A mosaic sweep: 1 to 16 and back, which is the whole of every SNES
     pixelate transition. */
  phase = (int)(cm.frame % 300ul);
  if (phase < 100)
    cm.mosaic = 1 + phase * 15 / 100;
  else if (phase < 140)
    cm.mosaic = 16;
  else if (phase < 240)
    cm.mosaic = 16 - (phase - 140) * 15 / 100;
  else
    cm.mosaic = 1;

  if (input) {
    if (input->dx)
      cm.scroll += input->dx * 3;
    if ((input->buttons & MR_DEMO_INPUT_DEBUG) != 0u)
      cm.show_debug = !cm.show_debug;
  }
  cm_build_hdma();
}

/* ---------------------------------------------------------------- */
/* render                                                            */
/* ---------------------------------------------------------------- */

static gfx_color_t cm_apply(gfx_color_t main_c, gfx_color_t sub_c, int op) {
  switch (op) {
  case CM_OP_ADD:
    return mc_rgb_add(main_c, sub_c);
  case CM_OP_SUB:
    return mc_rgb_sub(main_c, sub_c);
  case CM_OP_SUB_HALF:
    return mc_rgb_sub(main_c, mc_rgb_scale(sub_c, 1, 2));
  case CM_OP_ADD_HALF:
  default:
    /* Add then halve. The halving is a separate register bit, and it is what
       makes this read as translucency rather than as a blowout: without it
       two bright layers saturate to white immediately. */
    return mc_rgb_half(main_c, sub_c);
  }
}

static const char *cm_op_name(int op) {
  switch (op) {
  case CM_OP_ADD:
    return "MAIN + SUB";
  case CM_OP_SUB:
    return "MAIN - SUB";
  case CM_OP_SUB_HALF:
    return "MAIN - SUB/2";
  default:
    return "(MAIN + SUB)/2";
  }
}

static void cm_render(gfx_renderer_t *r) {
  int y, y0, y1;
  if (!r)
    return;
  mc_rows(r, &y0, &y1);

  for (y = y0; y < y1; ++y) {
    int x0, tw, x;
    gfx_color_t *row = mc_row(r, y, &x0, &tw);
    int ly = y - CM_Y0;
    int wl, wr;
    if (!row)
      continue;
    for (x = 0; x < tw; ++x)
      row[x] = GFX_RGB565_BLACK;
    if (ly < 0 || ly >= CM_H)
      continue;

    wl = cm.win_l[y];
    wr = cm.win_r[y];

    for (x = 0; x < CM_W; ++x) {
      int sx = CM_X0 + x;
      int mx, my;
      gfx_color_t main_c, sub_c;
      if (sx < x0 || sx >= x0 + tw)
        continue;

      /* Mosaic. The PPU holds the pixel it fetched at the top-left of each
         block for the whole block, in both axes. Sampling the source at the
         block origin is the same operation. */
      mx = x - (x % cm.mosaic);
      my = ly - (ly % cm.mosaic);
      main_c = cm_main(mx, my);

      /* Inside the window, colour math runs. Outside it, the main screen goes
         out untouched -- the subscreen is simply not consulted. */
      if (wr > wl && sx >= wl && sx < wr) {
        sub_c = cm.use_fixed ? cm.fixed[y] : cm_sub(x, ly);
        row[sx - x0] = cm_apply(main_c, sub_c, cm.op);
      } else {
        row[sx - x0] = main_c;
      }
    }

    /* Draw the window edges themselves so the HDMA table is visible as a
       shape rather than merely as an effect. */
    if (cm.show_debug && wr > wl) {
      if (wl >= x0 && wl < x0 + tw)
        row[wl - x0] = GFX_RGB565(255, 230, 90);
      if (wr - 1 >= x0 && wr - 1 < x0 + tw)
        row[wr - 1 - x0] = GFX_RGB565(255, 230, 90);
    }
  }

  mc_text_shadow(r, 6, 12, "SNES COLOUR MATH - TWO SCREENS, ONE ADD",
                 GFX_RGB565(235, 240, 250), 1);
  if (cm.show_debug) {
    char buf[64];
    char *p = buf;
    const char *end = buf + sizeof(buf) - 1;
    p = mr_strbuf_str(p, end, cm_op_name(cm.op));
    p = mr_strbuf_str(p, end, cm.use_fixed ? "  SUB=FIXED COLOUR" : "  SUB=BG2");
    *p = '\0';
    mc_text_shadow(r, 6, 22, buf, GFX_RGB565(255, 230, 140), 1);

    p = buf;
    p = mr_strbuf_str(p, end, "MOSAIC ");
    p = mr_strbuf_u32(p, end, (unsigned long)cm.mosaic);
    p = mr_strbuf_str(p, end, "x   WINDOW: HDMA EDGES PER LINE");
    *p = '\0';
    mc_text_shadow(r, 6, MC_EX_H - 14, buf, GFX_RGB565(200, 220, 255), 1);
    mc_text_shadow(r, 6, MC_EX_H - 24,
                   "PMON: VOICE 0 IS SILENT AND BENDS VOICE 1",
                   GFX_RGB565(200, 220, 255), 1);
  }
}

/* ---------------------------------------------------------------- */

static const char *const cm_notes[] = {
    "Layers go to a main screen or a subscreen; the PPU adds or subtracts.",
    "Add-and-halve is translucency. Subtract is a shadow that darkens.",
    "Point the subscreen at the fixed colour for a tint with no layer spent.",
    "Windows are left/right edges per layer -- HDMA makes them any shape.",
    "Mosaic holds the fetched pixel for N columns and rows, per layer.",
    "S-DSP PMON: voice N's output modulates voice N+1's pitch per sample.",
    "NON points a voice at the shared noise source: a hat with no sample.",
    0};

static const mc_example_t cm_example = {
    "snes-colormath",
    "Super Famicom / SNES (1990), S-PPU colour math + S-DSP",
    "main/subscreen add and subtract, HDMA window shapes, per-layer mosaic",
    "pitch modulation between voices, shared noise source, ADSR envelopes",
    cm_notes,
    cm_init,
    cm_tick,
    cm_render,
    cm_mix,
    cm_sfx};

const mc_example_t *mc_example_snes_colormath(void) { return &cm_example; }
