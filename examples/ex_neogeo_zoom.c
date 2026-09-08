/* SNK Neo Geo MVS/AES, 1990 -- LSPC video + YM2610
 *
 * Display: shrink tables.
 *
 *   The Neo Geo has no background layer worth the name. It has 381 sprites,
 *   each 16 pixels wide and up to 32 tiles -- 512 pixels -- tall, and it draws
 *   backgrounds by standing them next to each other. When a sprite's "chain"
 *   bit is set it takes its position from its neighbour, so a wall of sprites
 *   scrolls as one object. Everything on screen is a sprite, which is why the
 *   hardware could afford to make sprites so capable.
 *
 *   What it spent that capability on is scaling, in hardware, per sprite, free.
 *
 *   Vertical shrink is an eight-bit value, and it does not resample anything.
 *   The chip has a table that maps each of the 256 shrink levels to which
 *   source lines survive: at full size all of them, at half size every other
 *   one, and at the extremes a handful. Horizontal shrink is the same idea in
 *   four bits, choosing which of the sprite's sixteen columns are drawn.
 *
 *   That is why Neo Geo scaling looks the way it does. It is not filtered and
 *   it is not smooth -- lines and columns simply vanish, and as the shrink
 *   value crosses a threshold a whole row disappears at once. On a fighting
 *   game camera pulling back from two characters, that reads as weight. On a
 *   sprite scaled to a quarter it reads as a sprite with holes in it, which is
 *   why artists drew for the sizes they knew would be used.
 *
 *   The important part is the cost: nothing. No matrix, no division per
 *   scanline, no second buffer. The line-select table is ROM and the sprite
 *   list already had two spare fields. Mode 7 bought one transformed
 *   background with the whole PPU; this buys 381 independently scaled objects
 *   with a lookup.
 *
 *   Colour: 65,536 available, 4,096 on screen, 16 per sprite. Nobody else in
 *   1990 was close, and it is the other half of why Neo Geo sprites did not
 *   need scaling to be smooth -- there was enough colour in them to survive a
 *   dropped line.
 *
 * Audio: the YM2610 -- three sound systems in one package.
 *
 *   ADPCM-A: six channels of 4-bit ADPCM at a fixed 18.5 kHz, straight out of
 *   ROM, with volume and nothing else. No pitch control at all -- a channel
 *   plays its sample at the one rate it has. That is exactly right for drums
 *   and voice samples, which is what all six were always used for, and the
 *   fixed rate is why Neo Geo percussion has such a consistent texture across
 *   the whole library.
 *
 *   ADPCM-B: one channel, same 4-bit scheme, but with a programmable rate. One
 *   pitched sample voice, usually spent on a bass or the announcer.
 *
 *   SSG: three square-wave channels inherited from the AY-3-8910, and this is
 *   the part worth the example. The AY has one envelope generator shared by
 *   all three channels, and it is not an ADSR -- it is a 16-step ramp with
 *   three bits deciding whether it rises or falls, whether it holds at the end
 *   or repeats, and whether it alternates direction. Set it to repeat and it
 *   becomes a periodic waveform. Set its period short enough and it is no
 *   longer an envelope at all: it is an oscillator in the audio range, running
 *   underneath the square-wave channel and producing a buzzy sawtooth that the
 *   chip has no other way to make. Chip musicians called it the buzzer, and it
 *   is doing the bass line below.
 */

#include "mc_ex_chip.h"
#include "mc_ex_gfx.h"
#include "mc_example.h"
#include "mr_strbuf.h"

#include <string.h>

#define NG_W 320
#define NG_H 224
#define NG_Y0 8
#define NG_SPRITE_W 16
#define NG_SPRITE_TILES 8 /* 8 tiles of 16 lines = a 128-pixel-tall sprite */
#define NG_SPRITE_H (NG_SPRITE_TILES * 16)
#define NG_DRONES 12

static struct {
  unsigned long frame;
  int scroll;
  int show_debug;

  /* The hero sprite's shrink registers. */
  int xshrink; /* 0..15  */
  int yshrink; /* 0..255 */
  int hero_x, hero_y;

  /* The shrink tables. On hardware these are ROM inside the chip; here they
     are built once, which is the same thing with the lid off. */
  uint16_t xtable[16];       /* bit n set: source column n survives */
  uint8_t ytable[256][16];   /* per shrink level, which of 16 lines survive */

  uint8_t art[NG_SPRITE_H][NG_SPRITE_W];
  uint8_t drone_art[16][16];
  gfx_color_t pal[8][16];

  struct {
    int x, y, xs, ys, pal;
  } drone[NG_DRONES];
} ng;

/* 16 bits per channel gives 65,536 colours; a sprite gets sixteen of them. The
   'dark bit' that halves every channel is left out here -- it is a fifth low
   bit shared across R, G and B, and it exists because SNK wanted a shade
   between two adjacent levels without spending another bit per channel. */
static gfx_color_t ng_color(int r, int g, int b) {
  return mc_rgb_quant(GFX_RGB565(r, g, b), 5, 5, 5);
}

static void ng_build_palettes(void) {
  int i;
  /* hero */
  static const int hero[16][3] = {
      {0, 0, 0},      {24, 18, 30},    {60, 40, 46},    {110, 66, 60},
      {170, 110, 86}, {224, 168, 130}, {250, 214, 180}, {40, 60, 150},
      {70, 100, 210}, {120, 160, 250}, {180, 30, 40},   {230, 70, 60},
      {250, 150, 90}, {250, 230, 120}, {255, 255, 255}, {12, 12, 16}};
  for (i = 0; i < 16; ++i)
    ng.pal[0][i] = ng_color(hero[i][0], hero[i][1], hero[i][2]);
  /* drones, three tinted variants */
  for (i = 0; i < 16; ++i) {
    ng.pal[1][i] = ng_color(i * 16, 60 + i * 8, 200 - i * 6);
    ng.pal[2][i] = ng_color(200 - i * 5, i * 15, 60 + i * 10);
    ng.pal[3][i] = ng_color(60 + i * 10, 200 - i * 6, i * 14);
  }
  /* background sprite wall */
  for (i = 0; i < 16; ++i)
    ng.pal[4][i] = ng_color(20 + i * 9, 24 + i * 8, 40 + i * 11);
  ng.pal[4][0] = ng_color(0, 0, 0);
  for (i = 0; i < 16; ++i)
    ng.pal[5][i] = ng_color(60 + i * 6, 40 + i * 5, 30 + i * 4);
}

/* The shrink tables.

   Vertical: for each of the 256 levels, mark which of a tile's sixteen lines
   are drawn, distributed as evenly as the arithmetic allows. Level 255 keeps
   all sixteen; level 127 keeps eight; level 15 keeps one. Distribution is the
   whole design -- keeping the first N lines would make a scaled sprite look
   sliced, while spreading them keeps its proportions. */
static void ng_build_tables(void) {
  int s, i;
  for (s = 0; s < 16; ++s) {
    uint16_t mask = 0u;
    int keep = s + 1;
    for (i = 0; i < 16; ++i)
      if (((i * keep) / 16) != (((i + 1) * keep) / 16))
        mask |= (uint16_t)(1u << i);
    ng.xtable[s] = mask;
  }
  for (s = 0; s < 256; ++s) {
    int keep = ((s + 1) * 16) / 256;
    if (keep < 1)
      keep = 1;
    for (i = 0; i < 16; ++i)
      ng.ytable[s][i] =
          (uint8_t)((((i * keep) / 16) != (((i + 1) * keep) / 16)) ? 1u : 0u);
  }
}

/* A 16x128 sprite: one chain of eight tiles. Drawn tall on purpose, because
   the tall-thin sprite is the unit this hardware thinks in. */
static void ng_build_art(void) {
  int x, y;
  for (y = 0; y < NG_SPRITE_H; ++y) {
    for (x = 0; x < NG_SPRITE_W; ++x) {
      int v = 0;
      int cx = x - 8;
      if (y < 24) { /* head */
        int cy = y - 12;
        int d = cx * cx + cy * cy;
        if (d < 30)
          v = (cy < -2) ? 3 : ((x < 8) ? 5 : 4);
        if (d < 30 && cy > -2 && cy < 2 && (x == 5 || x == 10))
          v = 15;
      } else if (y < 40) { /* shoulders */
        if (cx > -7 && cx < 7)
          v = (y < 28) ? 8 : 7;
      } else if (y < 80) { /* torso and arms */
        if (cx > -6 && cx < 6)
          v = ((y & 7) < 4) ? 8 : 7;
        if ((cx <= -6 && cx > -8) || (cx >= 6 && cx < 8))
          v = (y < 62) ? 4 : 6;
      } else if (y < 112) { /* legs */
        if ((cx > -6 && cx < -1) || (cx > 1 && cx < 6))
          v = ((y & 7) < 4) ? 11 : 10;
      } else { /* boots */
        if (cx > -7 && cx < 7)
          v = 1;
        if (y > 120 && cx > -7 && cx < 7)
          v = 2;
      }
      ng.art[y][x] = (uint8_t)v;
    }
  }
  for (y = 0; y < 16; ++y) {
    for (x = 0; x < 16; ++x) {
      int cx = x - 8, cy = y - 8;
      int d = cx * cx + cy * cy;
      ng.drone_art[y][x] =
          (uint8_t)((d > 56) ? 0 : ((d < 8) ? 14 : (15 - d / 5)));
    }
  }
}

/* ---------------------------------------------------------------- */
/* YM2610                                                            */
/* ---------------------------------------------------------------- */

#define NG_ADPCM_A 6
#define NG_PCM_LEN 4000

/* Yamaha's ADPCM step ladder. Modelled on the IMA table the YM2610's decoder
   is a close relative of: the step size follows the signal, so a loud passage
   gets coarse quantization and a quiet one gets fine. Four bits per sample,
   half the ROM of 8-bit PCM, and the artefacts land where the signal is
   already loud enough to hide them. */
static const int16_t ng_step[49] = {
    16,   17,   19,   21,   23,   25,   28,   31,   34,   37,   41,   45,  50,
    55,   60,   66,   73,   80,   88,   97,   107,  118,  130,  143,  157, 173,
    190,  209,  230,  253,  279,  307,  337,  371,  408,  449,  494,  544, 598,
    658,  724,  796,  876,  963,  1060, 1166, 1282, 1411, 1552};
static const int8_t ng_index_adj[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

typedef struct ng_adpcm {
  const uint8_t *data; /* two 4-bit samples per byte */
  long nibbles;
  long cursor;
  /* long, not int: the predictor is a full-scale 16-bit sample and the decode
     step adds to it before clamping, so it needs headroom past 32767. On a
     16-bit-int target the clamp below would be comparing a value against a
     bound it can never reach, which Open Watcom says out loud as W124. */
  long predictor;
  int index;
  int vol; /* 0..31, the only control ADPCM-A has */
  uint32_t acc;
  uint32_t step; /* 16.16 source samples per output frame */
  int playing;
  int32_t last;
} ng_adpcm_t;

static struct {
  int rate;
  long frame;
  long row_acc;
  long row_period;
  int row;

  uint8_t rom[3][NG_PCM_LEN / 2]; /* three baked ADPCM samples */
  long rom_nibbles[3];
  ng_adpcm_t a[NG_ADPCM_A];
  ng_adpcm_t b; /* the one pitched channel */

  /* SSG. Three squares and one envelope generator between them. */
  mc_osc_t ssg[3];
  int ssg_vol[3];      /* 0..15, or -1 meaning "use the envelope" */
  int ssg_out[3];

  mc_osc_t env_osc;    /* the envelope generator's own period */
  int env_step;        /* 0..15 */
  int env_dir;
  int env_hold;
  int env_shape;       /* the three shape bits, as one number */
  int env_level;
} ym;

/* Encode a PCM buffer to 4-bit ADPCM in place, so the playback path decodes
   real compressed data rather than pretending to. The encoder is the decoder
   run backwards, which is the property that makes ADPCM cheap at both ends. */
static void ng_encode_adpcm(const int16_t *src, long frames, uint8_t *dst) {
  long i;
  long predictor = 0;
  int index = 0;
  for (i = 0; i < frames; ++i) {
    long diff = (long)src[i] - predictor;
    long step = ng_step[index];
    int code = 0;
    long delta;
    if (diff < 0) {
      code = 8;
      diff = -diff;
    }
    if (diff >= step) {
      code |= 4;
      diff -= step;
    }
    if (diff >= step / 2) {
      code |= 2;
      diff -= step / 2;
    }
    if (diff >= step / 4)
      code |= 1;

    delta = step >> 3;
    if (code & 4)
      delta += step;
    if (code & 2)
      delta += step >> 1;
    if (code & 1)
      delta += step >> 2;
    predictor += (code & 8) ? -delta : delta;
    if (predictor > 32767)
      predictor = 32767;
    if (predictor < -32768)
      predictor = -32768;
    index += ng_index_adj[code & 7];
    if (index < 0)
      index = 0;
    if (index > 48)
      index = 48;

    if (i & 1)
      dst[i >> 1] = (uint8_t)(dst[i >> 1] | ((unsigned)code << 4));
    else
      dst[i >> 1] = (uint8_t)code;
  }
}

static int32_t ng_adpcm_decode(ng_adpcm_t *c) {
  int code;
  long step, delta;
  if (!c->data || c->cursor >= c->nibbles) {
    c->playing = 0;
    return 0;
  }
  code = (int)((c->cursor & 1) ? (c->data[c->cursor >> 1] >> 4)
                               : (c->data[c->cursor >> 1] & 0x0Fu));
  ++c->cursor;
  step = ng_step[c->index];
  delta = step >> 3;
  if (code & 4)
    delta += step;
  if (code & 2)
    delta += step >> 1;
  if (code & 1)
    delta += step >> 2;
  c->predictor += (code & 8) ? -delta : delta;
  if (c->predictor > 32767)
    c->predictor = 32767;
  if (c->predictor < -32768)
    c->predictor = -32768;
  c->index += ng_index_adj[code & 7];
  if (c->index < 0)
    c->index = 0;
  if (c->index > 48)
    c->index = 48;
  return (int32_t)c->predictor;
}

static int32_t ng_adpcm_next(ng_adpcm_t *c) {
  if (!c->playing)
    return 0;
  c->acc += c->step;
  while (c->acc >= 0x10000u) {
    c->acc -= 0x10000u;
    c->last = ng_adpcm_decode(c);
    if (!c->playing)
      return 0;
  }
  return ((int32_t)c->last * (int32_t)c->vol) / 31;
}

static void ng_adpcm_start(ng_adpcm_t *c, const uint8_t *data, long nibbles,
                           int src_rate, int vol) {
  c->data = data;
  c->nibbles = nibbles;
  c->cursor = 0;
  c->predictor = 0;
  c->index = 0;
  c->vol = vol;
  c->acc = 0u;
  c->last = 0;
  c->playing = 1;
  c->step = (uint32_t)(((int64_t)src_rate << 16) / (int64_t)ym.rate);
}

/* Bake three drum samples as PCM, then compress them, so what plays back has
   actually been through the codec. */
static void ng_bake_rom(void) {
  static int16_t pcm[NG_PCM_LEN];
  long i;
  int32_t s;
  for (s = 0; s < 3; ++s) {
    uint32_t rng = 0x1000u + (uint32_t)s * 7919u;
    int32_t amp = 32767;
    mc_osc_t body;
    mc_lp1_t lp;
    lp.z = 0;
    mc_osc_reset(&body);
    mc_osc_set_hz(&body, 18500,
                  ((int32_t)((s == 0 ? 120 : (s == 1 ? 220 : 900))) << 8));
    for (i = 0; i < NG_PCM_LEN; ++i) {
      int32_t noise = (int32_t)(mc_rand(&rng) & 0xFFFFu) - 32768;
      int32_t v;
      MC_OSC_ADVANCE(&body);
      if (s == 0)
        v = (mc_wave_sine(body.phase) * 7 / 8) + (noise / 12);
      else if (s == 1)
        v = (mc_wave_sine(body.phase) / 3) +
            (mc_lp1(&lp, noise, 24000) * 2 / 3);
      else
        v = mc_lp1(&lp, noise, 40000) * 2 / 3;
      pcm[i] = (int16_t)(((long)snd_clip_sample(v) * amp) >> 15);
      amp -= amp >> (s == 0 ? 9 : (s == 1 ? 8 : 6));
      if (s == 0)
        body.step -= body.step >> 9;
    }
    ng_encode_adpcm(pcm, NG_PCM_LEN, ym.rom[s]);
    ym.rom_nibbles[s] = NG_PCM_LEN;
  }
}

/* The AY envelope generator. Three bits: continue, attack, alternate, plus
   hold. The eight combinations give a ramp down, a ramp up, a sawtooth, a
   triangle, and a few that fire once and stop. Run the period fast enough and
   the "envelope" is a waveform. */
static void ng_env_tick(void) {
  if (ym.env_hold)
    return;
  ym.env_step += ym.env_dir;
  if (ym.env_step > 15 || ym.env_step < 0) {
    switch (ym.env_shape) {
    case 0: /* \___ : fall once, then silence */
      ym.env_step = 0;
      ym.env_hold = 1;
      break;
    case 1: /* \\\\ : sawtooth, repeating fall */
      ym.env_step = 15;
      break;
    case 2: /* \/\/ : triangle */
      ym.env_dir = -ym.env_dir;
      ym.env_step += ym.env_dir * 2;
      break;
    case 3: /* //// : repeating rise */
      ym.env_step = 0;
      break;
    default: /* /--- : rise once, then hold at full */
      ym.env_step = 15;
      ym.env_hold = 1;
      break;
    }
  }
  if (ym.env_step < 0)
    ym.env_step = 0;
  if (ym.env_step > 15)
    ym.env_step = 15;
  ym.env_level = ym.env_step;
}

static void ng_env_set(int shape, int hz_q8) {
  ym.env_shape = shape;
  ym.env_hold = 0;
  ym.env_dir = (shape == 3 || shape >= 4) ? 1 : -1;
  ym.env_step = (ym.env_dir > 0) ? 0 : 15;
  /* The generator walks its sixteen steps across one cycle of this
     oscillator, so the oscillator's frequency *is* the waveform's frequency
     when the envelope is being used as one. */
  mc_osc_set_hz(&ym.env_osc, ym.rate, (int32_t)hz_q8);
}

static const uint8_t ng_lead[16] = {81, 0, 84, 0, 88, 0, 86, 0,
                                    84, 0, 81, 0, 79, 0, 0,  0};
static const uint8_t ng_harm[16] = {69, 0, 72, 0, 76, 0, 74, 0,
                                    72, 0, 69, 0, 67, 0, 0,  0};
static const uint8_t ng_buzz[16] = {33, 0, 0, 40, 0, 33, 0, 0,
                                    31, 0, 0, 38, 0, 31, 0, 0};
static const uint8_t ng_kick[16] = {1, 0, 0, 0, 0, 0, 1, 0,
                                    1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t ng_snare[16] = {0, 0, 0, 0, 1, 0, 0, 0,
                                     0, 0, 0, 0, 1, 0, 1, 0};
static const uint8_t ng_hat[16] = {1, 0, 1, 1, 0, 1, 1, 0,
                                   1, 0, 1, 1, 0, 1, 1, 1};

static void ng_audio_init(const snd_mixer_t *m) {
  int i;
  memset(&ym, 0, sizeof(ym));
  ym.rate = m ? m->rate : SND_DEFAULT_RATE;
  ym.row_period = mc_frames_per_tick(m, 60) * 6;
  ng_bake_rom();
  for (i = 0; i < 3; ++i)
    ym.ssg_vol[i] = 0;
  ng_env_set(1, 55 << 8);
}

static void ng_row(void) {
  int r = ym.row & 15;
  int n;

  n = ng_lead[r];
  if (n) {
    mc_osc_set_hz(&ym.ssg[0], ym.rate, ((int32_t)(snd_note_hz(n)) << 8));
    ym.ssg_vol[0] = 11;
  } else if (ym.ssg_vol[0] > 0)
    --ym.ssg_vol[0];

  n = ng_harm[r];
  if (n) {
    mc_osc_set_hz(&ym.ssg[1], ym.rate, ((int32_t)(snd_note_hz(n)) << 8));
    ym.ssg_vol[1] = 7;
  } else if (ym.ssg_vol[1] > 0)
    --ym.ssg_vol[1];

  n = ng_buzz[r];
  if (n) {
    /* Channel 3 takes its level from the envelope generator instead of its own
       volume register -- that is what -1 means here -- and the generator is
       set to a repeating fall at the note's own frequency. The square wave
       underneath it is silenced, so what is heard is the envelope: a sawtooth
       the chip otherwise cannot produce. */
    ym.ssg_vol[2] = -1;
    ng_env_set(1, snd_note_hz(n) << 8);
  }

  if (ng_kick[r])
    ng_adpcm_start(&ym.a[0], ym.rom[0], ym.rom_nibbles[0], 18500, 30);
  if (ng_snare[r])
    ng_adpcm_start(&ym.a[1], ym.rom[1], ym.rom_nibbles[1], 18500, 26);
  if (ng_hat[r])
    ng_adpcm_start(&ym.a[2], ym.rom[2], ym.rom_nibbles[2], 18500, 14);

  ++ym.row;
  if (ym.row >= 16)
    ym.row = 0;
}

static void ng_mix(snd_mixer_t *m, void *user) {
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

    if (--ym.row_acc <= 0) {
      ym.row_acc = ym.row_period;
      ng_row();
    }

    /* The envelope generator runs on its own clock, independent of every
       channel using it. */
    {
      /* Sixteen steps per cycle: tick whenever the top four bits of the phase
         change, which happens exactly sixteen times per wrap. */
      uint32_t before = ym.env_osc.phase;
      MC_OSC_ADVANCE(&ym.env_osc);
      if ((before >> 28) != (ym.env_osc.phase >> 28))
        ng_env_tick();
    }

    for (k = 0; k < 3; ++k) {
      int level;
      int32_t s;
      if (ym.ssg_vol[k] == 0)
        continue;
      MC_OSC_ADVANCE(&ym.ssg[k]);
      s = mc_wave_pulse(ym.ssg[k].phase, 32768u);
      level = (ym.ssg_vol[k] < 0) ? ym.env_level : ym.ssg_vol[k];
      /* The AY's volume ladder is logarithmic across sixteen steps, which is
         also the ladder the envelope generator walks -- so the buzz waveform
         is exponentially shaped, not linear. */
      acc += mc_psg_attenuate(s / 3, 15 - level);
    }

    /* Six ADPCM-A channels, all at the same fixed rate, volume and nothing
       else. */
    for (k = 0; k < NG_ADPCM_A; ++k)
      acc += ng_adpcm_next(&ym.a[k]) / 2;
    acc += ng_adpcm_next(&ym.b) / 2;

    mc_out(m, i, snd_clip_sample(acc), 128);
    ++ym.frame;
  }
}

static void ng_sfx(const snd_mixer_t *m, long at_frame) {
  (void)m;
  (void)at_frame;
  /* ADPCM-B: the one channel with a rate register, so the same ROM data can
     be played at any pitch. */
  ng_adpcm_start(&ym.b, ym.rom[1], ym.rom_nibbles[1], 9000, 31);
}

/* ---------------------------------------------------------------- */
/* simulation                                                        */
/* ---------------------------------------------------------------- */

static void ng_init(int screen_w, int screen_h, const snd_mixer_t *m,
                    long start_frame) {
  int i;
  uint32_t rng = 0x9E0u;
  (void)screen_w;
  (void)screen_h;
  (void)start_frame;
  memset(&ng, 0, sizeof(ng));
  ng.show_debug = 1;
  ng.xshrink = 15;
  ng.yshrink = 255;
  ng.hero_x = 150;
  ng.hero_y = NG_Y0 + 60;
  ng_build_palettes();
  ng_build_tables();
  ng_build_art();
  for (i = 0; i < NG_DRONES; ++i) {
    ng.drone[i].x = 20 + i * 24;
    ng.drone[i].y = NG_Y0 + 20;
    ng.drone[i].xs = (i * 15) / (NG_DRONES - 1);
    ng.drone[i].ys = (i * 255) / (NG_DRONES - 1);
    ng.drone[i].pal = 1 + (int)(mc_rand(&rng) % 3u);
  }
  ng_audio_init(m);
}

static void ng_tick(const mr_demo_input_t *input) {
  /* int32_t throughout: t is a Q15 quantity and t * 215 reaches seven
     million, so every one of these products needs the wider type. The
     sprite-height term is worse than it looks -- 128 * 256 is 32768, which
     misses a 16-bit int by exactly one. */
  int32_t t;
  ++ng.frame;
  ng.scroll += 2;

  /* The camera pull-back: one register pair animated, no other cost. */
  t = (mc_sin((int)ng.frame * 3) + 32767) / 2; /* 0..32767 */
  ng.yshrink = (int)(40 + (t * 215) / 32767);
  ng.xshrink = (int)(4 + (t * 11) / 32767);
  ng.hero_y =
      NG_Y0 + 200 - (int)(((long)NG_SPRITE_H * (long)(ng.yshrink + 1)) / 256L);

  if (input) {
    if (input->dx)
      ng.hero_x += input->dx * 3;
    if ((input->buttons & MR_DEMO_INPUT_DEBUG) != 0u)
      ng.show_debug = !ng.show_debug;
  }
}

/* ---------------------------------------------------------------- */
/* render                                                            */
/* ---------------------------------------------------------------- */

/* Draw one shrunk sprite. The two loops below are the hardware: walk the
   source lines, consult the table, and emit the ones that survive. Nothing is
   averaged and nothing is interpolated -- a line is either fetched or it is
   not, which is what makes this free. */
static void ng_draw_sprite(gfx_renderer_t *r, int y, int x0, int tw,
                           const uint8_t *art, int art_w, int art_h, int sx,
                           int sy, int xs, int ys, int pal) {
  gfx_color_t *row = mc_row(r, y, &x0, &tw);
  int src_y = -1;
  int out_y = 0;
  int i, j;
  uint16_t xmask;
  if (!row)
    return;

  /* Find which source line lands on this output line. */
  for (i = 0; i < art_h; ++i) {
    if (!ng.ytable[ys & 255][i & 15])
      continue;
    if (sy + out_y == y) {
      src_y = i;
      break;
    }
    ++out_y;
  }
  if (src_y < 0)
    return;

  xmask = ng.xtable[xs & 15];
  {
    int out_x = 0;
    for (j = 0; j < art_w; ++j) {
      int px;
      int index;
      if (((xmask >> (j & 15)) & 1u) == 0u)
        continue;
      px = sx + out_x;
      ++out_x;
      if (px < x0 || px >= x0 + tw)
        continue;
      index = art[src_y * art_w + j];
      if (index == 0)
        continue; /* colour 0 of a sprite palette is transparent */
      row[px - x0] = ng.pal[pal][index];
    }
  }
}

static void ng_render(gfx_renderer_t *r) {
  int y, y0, y1;
  if (!r)
    return;
  mc_rows(r, &y0, &y1);

  for (y = y0; y < y1; ++y) {
    int x0, tw, x;
    gfx_color_t *row = mc_row(r, y, &x0, &tw);
    int ly = y - NG_Y0;
    if (!row)
      continue;
    for (x = 0; x < tw; ++x)
      row[x] = GFX_RGB565_BLACK;
    if (ly < 0 || ly >= NG_H)
      continue;

    /* The "background", which is a wall of chained 16-wide sprites. Each strip
       takes its position from its neighbour, so scrolling it is one write. */
    for (x = 0; x < tw; ++x) {
      int sx = x0 + x;
      int wx = (sx + ng.scroll) & 511;
      int strip = wx >> 4;
      int fx = wx & 15;
      int index;
      if (ly > 176) {
        index = 3 + (((sx >> 3) ^ ((ly - 176) >> 2)) & 1) * 2;
        row[x] = ng.pal[5][index + ((ly - 176) >> 4)];
      } else {
        int h = 60 + ((strip * 37) % 90);
        if (ly > 176 - h) {
          index = (fx == 0 || fx == 15) ? 2 : (((fx & 3) == 1 && (ly & 7) < 3)
                                                   ? 13
                                                   : 6 + ((strip >> 1) & 3));
          row[x] = ng.pal[4][index];
        } else {
          row[x] = ng.pal[4][1 + (ly >> 5)];
        }
      }
    }
  }

  /* The shrink ladder: the same 16x16 sprite at twelve shrink levels, so the
     dropped columns and lines are visible as a progression rather than as an
     artefact. */
  {
    int i;
    for (i = 0; i < NG_DRONES; ++i)
      for (y = y0; y < y1; ++y)
        ng_draw_sprite(r, y, 0, MC_EX_W, &ng.drone_art[0][0], 16, 16,
                       ng.drone[i].x, ng.drone[i].y, ng.drone[i].xs,
                       ng.drone[i].ys, ng.drone[i].pal);
  }

  for (y = y0; y < y1; ++y)
    ng_draw_sprite(r, y, 0, MC_EX_W, &ng.art[0][0], NG_SPRITE_W, NG_SPRITE_H,
                   ng.hero_x, ng.hero_y, ng.xshrink, ng.yshrink, 0);

  mc_text_shadow(r, 6, 12, "NEO GEO - SCALING IS A LOOKUP, NOT A MATRIX",
                 ng_color(255, 240, 200), 1);
  if (ng.show_debug) {
    char buf[64];
    char *p = buf;
    const char *end = buf + sizeof(buf) - 1;
    p = mr_strbuf_str(p, end, "XSHRINK ");
    p = mr_strbuf_u32(p, end, (unsigned long)ng.xshrink);
    p = mr_strbuf_str(p, end, "/15  YSHRINK ");
    p = mr_strbuf_u32(p, end, (unsigned long)ng.yshrink);
    p = mr_strbuf_str(p, end, "/255");
    *p = '\0';
    mc_text_shadow(r, 6, 22, buf, ng_color(255, 220, 140), 1);
    mc_text_shadow(r, 6, MC_EX_H - 24,
                   "SSG CH3 LEVEL COMES FROM THE ENVELOPE GENERATOR",
                   ng_color(180, 220, 255), 1);
    mc_text_shadow(r, 6, MC_EX_H - 14,
                   "RUN FAST ENOUGH, AN ENVELOPE IS A WAVEFORM",
                   ng_color(180, 220, 255), 1);
  }
}

/* ---------------------------------------------------------------- */

static const char *const ng_notes[] = {
    "Everything is a sprite. The background is a wall of chained strips.",
    "Y shrink is 8 bits into a line-select table: lines vanish, not blur.",
    "X shrink is 4 bits choosing which of 16 columns survive.",
    "381 sprites can each scale independently, for the cost of a lookup.",
    "YM2610 ADPCM-A: six channels, 18.5 kHz fixed, volume and nothing else.",
    "SSG channel 3 takes its level from the shared envelope generator.",
    "Clock that generator into the audio range and it becomes the oscillator.",
    0};

static const mc_example_t ng_example = {
    "neogeo-zoom",
    "SNK Neo Geo MVS/AES (1990), LSPC + YM2610",
    "per-sprite shrink tables dropping lines and columns, chained sprite backgrounds",
    "4-bit ADPCM-A drums at a fixed rate, pitched ADPCM-B, AY envelope as a waveform",
    ng_notes,
    ng_init,
    ng_tick,
    ng_render,
    ng_mix,
    ng_sfx};

const mc_example_t *mc_example_neogeo_zoom(void) { return &ng_example; }
