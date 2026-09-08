/* NEC PC Engine / TurboGrafx-16, 1987 -- HuC6270 VDC + HuC6260 VCE + HuC6280 PSG
 *
 * Display: 512 colours, and a raster compare register.
 *
 *   The PC Engine is the awkward one in any list of generations. The CPU is an
 *   8-bit 6502 derivative. The video chip has a 512-colour palette, 64 sprites
 *   that can be 32 pixels wide and 64 tall, and a display that can run 512
 *   pixels across. Sega marketed against it as a 16-bit machine because
 *   arguing about the CPU was losing them the argument.
 *
 *   The colour system is the part worth building an example around. The VCE
 *   holds 512 palette entries of nine bits -- three per channel -- and there is
 *   no attribute grid: a background tile picks one of sixteen sixteen-colour
 *   palettes, and sprites pick from another sixteen. But palette RAM is not
 *   read-only during display. The raster compare register raises an interrupt
 *   at a chosen scanline, and a handler with a few spare cycles can rewrite
 *   colour entries before the beam reaches them.
 *
 *   So the sky here is not a gradient in tile data. It is *one* tile, drawn
 *   with one palette index, and the entry behind that index is rewritten on
 *   every scanline. Sixty bytes of VRAM produce a 200-step gradient. That is a
 *   fundamentally different trick from the Mega Drive's scroll table -- Sega
 *   changed where the plane was read from, NEC changed what its colours meant --
 *   and it is why PC Engine skies look the way they do.
 *
 *   RCR is doing two other jobs below: a scroll split at the horizon, and a
 *   second split for the foreground band.
 *
 *   The sprite limit is sixteen per scanline rather than eight, and a single
 *   sprite can be 32x64. One register pair puts a boss on screen that the NES
 *   would have needed thirty-two sprites and four scanline limits to attempt.
 *
 * Audio: the HuC6280's six wavetable channels.
 *
 *   The Game Boy has one wavetable channel and treats it as an oddity. The PC
 *   Engine has six, and treats it as the whole design: every channel is 32
 *   samples of 5-bit waveform that the program writes, and there are no
 *   dedicated pulse or triangle generators at all. Want a square? Write one
 *   into the table. Want something that is not any named waveform? Also write
 *   it. The chip has no opinion.
 *
 *   Two extras make it more than a six-voice Game Boy channel 3:
 *
 *   LFO. Channel 2 can be taken out of the mix and used to modulate channel
 *   1's frequency instead, with a selectable depth. At low rates that is
 *   vibrato; at audio rates it is frequency modulation, which means this chip
 *   could do a coarse FM voice a year before the Mega Drive shipped one. The
 *   lead below crosses that line deliberately -- the LFO rate ramps from a few
 *   hertz up into the audible range over a bar, and the timbre goes with it.
 *
 *   DDA. A channel can be switched from reading its wavetable to taking
 *   whatever the CPU last wrote, one sample at a time. That is a software DAC,
 *   and it is where PC Engine drums and speech came from -- at the cost of a
 *   channel and an interrupt, the same bargain the Mega Drive made for its
 *   channel 6.
 */

#include "mc_ex_chip.h"
#include "mc_ex_gfx.h"
#include "mc_example.h"
#include "mr_strbuf.h"

#include <string.h>

#define PCE_W 256
#define PCE_H 224
#define PCE_X0 ((MC_EX_W - PCE_W) / 2)
#define PCE_Y0 8
#define PCE_MAP_W 64
#define PCE_MAP_H 32
#define PCE_TILES 16
#define PCE_SPRITES 24
#define PCE_MAX_PER_LINE 16
#define PCE_HORIZON 96
#define PCE_FOREGROUND 168

static struct {
  unsigned long frame;
  int scroll_sky, scroll_mid, scroll_fore;
  int show_debug;
  int overflow;

  uint8_t tiles[PCE_TILES][8 * 8];
  uint8_t map[PCE_MAP_H][PCE_MAP_W];
  uint8_t map_pal[PCE_MAP_H][PCE_MAP_W];
  gfx_color_t pal[16][16];

  /* The per-scanline colour written into palette entry 0 of bank 0 before the
     beam reaches that line. On hardware this is an RCR interrupt handler and
     one store; here it is a table the renderer indexes, which is the same
     thing without pretending the CPU is real. */
  gfx_color_t sky_cram[MC_EX_H];

  struct {
    int x, y, w, h, tile, pal, vx, vy;
  } spr[PCE_SPRITES];
  uint8_t line_list[MC_EX_H][PCE_MAX_PER_LINE];
  uint8_t line_n[MC_EX_H];
} pce;

/* Nine bits: three per channel, 512 entries. Half again as many levels per
   channel as the Mega Drive's RGB333 has... which is the same. The difference
   is not the depth, it is that there are 512 of them addressable at once and
   no attribute grid deciding which sixteen a given 16x16 block may use. */
static gfx_color_t pce_color(int r, int g, int b) {
  return mc_rgb_quant(GFX_RGB565(r, g, b), 3, 3, 3);
}

static void pce_build_palettes(void) {
  int i;
  /* bank 0: sky. Entry 0 is the one RCR rewrites every line. */
  for (i = 0; i < 16; ++i)
    pce.pal[0][i] = pce_color(20 + i * 14, 30 + i * 13, 70 + i * 11);
  /* bank 1: distant city */
  pce.pal[1][0] = pce_color(0, 0, 0);
  for (i = 1; i < 16; ++i)
    pce.pal[1][i] = pce_color(30 + i * 5, 26 + i * 4, 60 + i * 8);
  /* bank 2: mid buildings */
  pce.pal[2][0] = pce_color(0, 0, 0);
  for (i = 1; i < 16; ++i)
    pce.pal[2][i] = pce_color(50 + i * 8, 40 + i * 6, 70 + i * 7);
  /* bank 3: foreground */
  pce.pal[3][0] = pce_color(0, 0, 0);
  for (i = 1; i < 16; ++i)
    pce.pal[3][i] = pce_color(20 + i * 4, 18 + i * 3, 30 + i * 5);
  /* bank 4: sprite colours, deliberately bright against all of the above */
  pce.pal[4][0] = pce_color(0, 0, 0);
  pce.pal[4][1] = pce_color(255, 90, 40);
  pce.pal[4][2] = pce_color(255, 170, 60);
  pce.pal[4][3] = pce_color(255, 240, 180);
  pce.pal[4][4] = pce_color(140, 30, 30);
  pce.pal[4][5] = pce_color(90, 200, 255);
  pce.pal[4][6] = pce_color(40, 110, 200);
  pce.pal[4][7] = pce_color(230, 230, 255);
  for (i = 8; i < 16; ++i)
    pce.pal[4][i] = pce_color(i * 16, 255 - i * 12, 200);
}

static void pce_build_tiles(void) {
  int t, x, y;
  for (t = 0; t < PCE_TILES; ++t) {
    for (y = 0; y < 8; ++y) {
      for (x = 0; x < 8; ++x) {
        int v = 0;
        switch (t) {
        case 0:
          v = 0; /* the sky: one index, whose colour changes per line */
          break;
        case 1: /* star */
          v = (x == 4 && y == 3) ? 15 : 0;
          break;
        case 2: /* distant tower body */
          v = ((x & 3) == 1 && (y & 3) == 1) ? 9 : 5;
          break;
        case 3: /* distant tower top */
          v = (y < 2) ? 3 : (((x & 3) == 1 && (y & 3) == 1) ? 9 : 5);
          break;
        case 4: /* mid building */
          v = (x == 0 || x == 7) ? 4 : (((x & 1) && (y & 3) < 2) ? 12 : 7);
          break;
        case 5: /* mid roof */
          v = (y < 3) ? 10 : 7;
          break;
        case 6: /* foreground girder */
          v = (x < 2 || x > 5) ? 6 : ((y & 3) ? 3 : 9);
          break;
        case 7: /* foreground plate */
          v = (((x + y) & 7) < 4) ? 4 : 5;
          break;
        case 8: /* road */
          v = ((y == 3 || y == 4) && ((x >> 1) & 1)) ? 12 : 2;
          break;
        default:
          v = (t + x + y) & 15;
          break;
        }
        pce.tiles[t][y * 8 + x] = (uint8_t)v;
      }
    }
  }
}

static void pce_build_map(void) {
  int x, y;
  uint32_t rng = 0x600Du;
  for (y = 0; y < PCE_MAP_H; ++y) {
    for (x = 0; x < PCE_MAP_W; ++x) {
      int t = 0, p = 0;
      int py = y * 8;
      if (py >= PCE_FOREGROUND + 16) {
        t = (py >= PCE_FOREGROUND + 40) ? 8 : 7;
        p = 3;
      } else if (py >= PCE_FOREGROUND) {
        t = ((x % 9) < 2) ? 6 : 7;
        p = 3;
      } else if (py >= PCE_HORIZON + 24) {
        int h = 4 + (int)(((unsigned)x * 7u) % 9u);
        t = (py >= PCE_FOREGROUND - h * 8) ? (((py / 8) == (PCE_FOREGROUND / 8 - h)) ? 5 : 4) : 0;
        p = t ? 2 : 0;
      } else if (py >= PCE_HORIZON) {
        int h = 2 + (int)(((unsigned)x * 13u) % 6u);
        t = (py >= PCE_HORIZON + 24 - h * 8) ? (((py / 8) == ((PCE_HORIZON + 24) / 8 - h)) ? 3 : 2) : 0;
        p = t ? 1 : 0;
      } else if ((mc_rand(&rng) & 63u) < 2u) {
        t = 1;
        p = 0;
      }
      pce.map[y][x] = (uint8_t)t;
      pce.map_pal[y][x] = (uint8_t)p;
    }
  }
}

/* The RCR handler. One palette store per scanline turns a single-index sky
   into a 200-band gradient, plus a slow colour cycle so the whole scene reads
   as dusk moving. */
static void pce_build_cram(void) {
  int y;
  int cycle = (int)(pce.frame & 1023ul);
  for (y = 0; y < MC_EX_H; ++y) {
    int ly = y - PCE_Y0;
    /* int32_t, not int: ly * 256 reaches 61440 and t * t reaches 65025, both
       of which are past the end of a 16-bit int. The DOS build produces a
       different sky without this and says nothing about it -- which is what
       the cross-target diff in tools/ is for. */
    int32_t t;
    if (ly < 0)
      ly = 0;
    t = ((int32_t)ly * 256) / (PCE_HORIZON + 24);
    if (t > 255)
      t = 255;
    {
      int32_t r = 12 + (t * 210) / 255 + ((mc_sin(cycle) * 24) >> 15);
      int32_t g = 10 + (t * 90) / 255 + ((mc_sin(cycle + 200) * 16) >> 15);
      int32_t b = 90 + (t * 60) / 255 - (t * t) / 700;
      if (r < 0)
        r = 0;
      if (g < 0)
        g = 0;
      if (b < 0)
        b = 0;
      if (r > 255)
        r = 255;
      if (g > 255)
        g = 255;
      if (b > 255)
        b = 255;
      pce.sky_cram[y] = pce_color((int)r, (int)g, (int)b);
    }
  }
}

/* ---------------------------------------------------------------- */
/* HuC6280 PSG                                                       */
/* ---------------------------------------------------------------- */

#define PCE_CH 6
#define PCE_DDA_LEN 2400

typedef struct pce_chan {
  uint8_t wave[32]; /* 5-bit samples: 0..31 */
  mc_osc_t osc;
  int32_t hz_q8;
  int vol;    /* 0..31, the chip's 5-bit channel volume */
  int on;
  int dda;    /* direct D/A: the table is ignored, the CPU writes samples */
  int noise;  /* channels 5 and 6 only */
  mc_noise_t ng;
} pce_chan_t;

static struct {
  int rate;
  long frame;
  long row_acc;
  long row_period;
  int row;

  pce_chan_t ch[PCE_CH];

  /* The LFO: channel 2 taken out of the mix and pointed at channel 1's
     frequency instead. Rate and depth are registers. */
  int lfo_on;
  int32_t lfo_hz_q8;
  int lfo_depth;
  mc_osc_t lfo;

  int8_t dda_sample[PCE_DDA_LEN];
  long dda_len;
  uint32_t dda_pos;
  uint32_t dda_step;
  int dda_playing;
} huc;

/* Five bits per entry, thirty-two entries. Writing one of these is what
   choosing an instrument means on this chip. */
static void pce_wave(uint8_t *dst, int shape) {
  int i;
  for (i = 0; i < 32; ++i) {
    int32_t v;
    switch (shape) {
    case 0: /* square, 50% -- because you wrote one, not because the chip has one */
      v = (i < 16) ? 31 : 0;
      break;
    case 1: /* narrow pulse: thinner than any fixed-duty chip offered */
      v = (i < 4) ? 31 : 0;
      break;
    case 2: /* sine */
      v = 16 + ((mc_wave_sine((uint32_t)i << 27) * 15) >> 15);
      break;
    case 3: /* saw */
      v = i;
      break;
    case 4: /* a formant-ish stack: two partials in one table */
      v = 16 + ((mc_wave_sine((uint32_t)i << 27) * 9) >> 15) +
          ((mc_wave_sine((uint32_t)i << 29) * 5) >> 15);
      break;
    default: /* organ: fundamental plus octave plus fifth */
      v = 16 + ((mc_wave_sine((uint32_t)i << 27) * 7) >> 15) +
          ((mc_wave_sine((uint32_t)i << 28) * 5) >> 15) +
          ((mc_wave_sine((uint32_t)(i * 3) << 27) * 3) >> 15);
      break;
    }
    if (v < 0)
      v = 0;
    if (v > 31)
      v = 31;
    dst[i] = (uint8_t)v;
  }
}

static void pce_bake_dda(void) {
  long i;
  uint32_t rng = 0x51EEDu;
  int32_t amp = 32767;
  mc_osc_t body;
  mc_lp1_t lp;
  lp.z = 0;
  mc_osc_reset(&body);
  mc_osc_set_hz(&body, 7000, ((int32_t)(170) << 8));
  huc.dda_len = PCE_DDA_LEN;
  for (i = 0; i < huc.dda_len; ++i) {
    int32_t noise = (int32_t)(mc_rand(&rng) & 0xFFFFu) - 32768;
    int32_t v;
    MC_OSC_ADVANCE(&body);
    v = (mc_wave_sine(body.phase) * 3 / 5) + (mc_lp1(&lp, noise, 22000) / 3);
    v = (int)(((long)snd_clip_sample(v) * amp) >> 15);
    huc.dda_sample[i] = (int8_t)(v >> 8);
    amp -= amp >> 8;
    body.step -= body.step >> 9;
  }
}

static const uint8_t pce_lead[32] = {81, 0,  0,  84, 0,  86, 0,  0,
                                     88, 0,  86, 0,  84, 0,  81, 0,
                                     79, 0,  0,  81, 0,  84, 0,  0,
                                     86, 0,  84, 0,  81, 0,  0,  0};
static const uint8_t pce_pad1[32] = {69, 0, 0, 0, 0, 0, 0, 0, 72, 0, 0,
                                     0,  0, 0, 0, 0, 67, 0, 0, 0, 0, 0,
                                     0,  0, 71, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t pce_pad2[32] = {64, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0,
                                     0,  0, 0, 0, 0, 62, 0, 0, 0, 0, 0,
                                     0,  0, 62, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t pce_bass[32] = {45, 0, 0, 45, 0, 52, 0, 0,
                                     48, 0, 0, 48, 0, 55, 0, 0,
                                     43, 0, 0, 43, 0, 50, 0, 0,
                                     47, 0, 0, 47, 0, 54, 0, 0};
static const uint8_t pce_drum[32] = {1, 0, 0, 0, 2, 0, 0, 1, 1, 0, 0,
                                     0, 2, 0, 2, 0, 1, 0, 0, 0, 2, 0,
                                     1, 0, 1, 0, 0, 1, 2, 0, 2, 2};

static void pce_audio_init(const snd_mixer_t *m) {
  int i;
  memset(&huc, 0, sizeof(huc));
  huc.rate = m ? m->rate : SND_DEFAULT_RATE;
  huc.row_period = mc_frames_per_tick(m, 60) * 6;
  for (i = 0; i < PCE_CH; ++i) {
    pce_wave(huc.ch[i].wave, i);
    huc.ch[i].vol = 0;
    mc_noise_init(&huc.ch[i].ng, MC_LFSR_NES_LONG, 0x2A5Fu + (uint32_t)i);
    mc_noise_set_hz(&huc.ch[i].ng, huc.rate, ((int32_t)(11000) << 8));
  }
  pce_wave(huc.ch[0].wave, 4);
  pce_wave(huc.ch[2].wave, 5);
  pce_wave(huc.ch[3].wave, 5);
  pce_wave(huc.ch[4].wave, 0);
  huc.lfo_on = 1;
  huc.lfo_hz_q8 = ((int32_t)(4) << 8);
  huc.lfo_depth = 6;
  pce_bake_dda();
  huc.dda_step = (uint32_t)(((int64_t)7000 << 16) / (int64_t)huc.rate);
  huc.ch[5].dda = 1;
}

static void pce_chan_note(pce_chan_t *c, int note, int vol) {
  c->hz_q8 = ((int32_t)(snd_note_hz(note)) << 8);
  mc_osc_set_hz(&c->osc, huc.rate, c->hz_q8);
  c->vol = vol;
  c->on = 1;
}

static void pce_row(void) {
  int r = huc.row & 31;
  int n;

  n = pce_lead[r];
  if (n)
    pce_chan_note(&huc.ch[0], n, 26);
  n = pce_pad1[r];
  if (n)
    pce_chan_note(&huc.ch[2], n, 14);
  n = pce_pad2[r];
  if (n)
    pce_chan_note(&huc.ch[3], n, 14);
  n = pce_bass[r];
  if (n)
    pce_chan_note(&huc.ch[4], n, 24);

  if (pce_drum[r]) {
    if (pce_drum[r] == 1) {
      huc.dda_pos = 0u;
      huc.dda_playing = 1;
      huc.ch[5].vol = 26;
    } else {
      /* The other percussion source: channels 5 and 6 can be switched to a
         noise generator instead of their wavetable. */
      huc.ch[5].dda = 0;
      huc.ch[5].noise = 1;
      huc.ch[5].vol = 16;
      mc_noise_set_hz(&huc.ch[5].ng, huc.rate, ((int32_t)(13000) << 8));
    }
  }

  /* Ramp the LFO from vibrato up into the audio range across each bar, which
     is the boundary between "modulation" and "frequency modulation" and is
     audible as the lead growing teeth. */
  huc.lfo_hz_q8 = ((int32_t)((4 + r * 22)) << 8);
  huc.lfo_depth = 5 + (r >> 2);

  /* Change the lead's wavetable at the bar line. Free: it is 32 stores. */
  if (r == 0)
    pce_wave(huc.ch[0].wave, 4);
  else if (r == 16)
    pce_wave(huc.ch[0].wave, 1);

  ++huc.row;
  if (huc.row >= 32)
    huc.row = 0;
}

static void pce_mix(snd_mixer_t *m, void *user) {
  int i, frames, c;
  (void)user;
  if (!m)
    return;
  frames = m->block_frames;
  if (frames <= 0)
    return;
  snd_touch_block(m);

  for (i = 0; i < frames; ++i) {
    int32_t acc = 0;
    int32_t lfo_out = 0;

    if (--huc.row_acc <= 0) {
      huc.row_acc = huc.row_period;
      pce_row();
    }

    /* Channel 2 as the LFO. It is not summed into the output -- it exists only
       to bend channel 1, which is exactly what the register does. */
    if (huc.lfo_on) {
      mc_osc_set_hz(&huc.lfo, huc.rate, huc.lfo_hz_q8);
      MC_OSC_ADVANCE(&huc.lfo);
      lfo_out = mc_wave_sine(huc.lfo.phase);
    }

    for (c = 0; c < PCE_CH; ++c) {
      pce_chan_t *ch = &huc.ch[c];
      int32_t s;
      if (c == 1 && huc.lfo_on)
        continue; /* channel 2 is the modulator, not a voice */
      if (!ch->on && !(c == 5 && huc.dda_playing))
        continue;

      if (c == 5 && ch->dda && huc.dda_playing) {
        long idx = (long)(huc.dda_pos >> 16);
        if (idx >= huc.dda_len) {
          huc.dda_playing = 0;
          continue;
        }
        /* Straight to the output: no wavetable lookup at all. */
        s = (int)huc.dda_sample[idx] * 200;
        huc.dda_pos += huc.dda_step;
      } else if (ch->noise) {
        s = mc_noise_next(&ch->ng) / 3;
      } else {
        uint32_t phase = ch->osc.phase;
        if (c == 0 && huc.lfo_on) {
          /* Frequency modulation by adding to the phase increment, which is
             what a frequency register being rewritten actually does. */
          int32_t bend = (int32_t)(((long)lfo_out * huc.lfo_depth) >> 9);
          mc_osc_set_hz(&ch->osc, huc.rate, ch->hz_q8 + bend * 8);
        }
        MC_OSC_ADVANCE(&ch->osc);
        phase = ch->osc.phase;
        /* Five bits, thirty-two entries, no interpolation. */
        s = ((int)ch->wave[(phase >> 27) & 31] - 16) * 1400;
      }
      acc += (s * ch->vol) / 31 / 3;
    }

    /* The chip's 5-bit channel volume steps in about 1.5 dB, so a decay is a
       staircase -- coarser than the Game Boy's 4-bit linear envelope in
       resolution but wider in range. Applied once a frame by the driver. */
    if ((huc.frame % (long)mc_frames_per_tick(m, 60)) == 0) {
      for (c = 0; c < PCE_CH; ++c)
        if (huc.ch[c].vol > 0 && (c == 0 || c == 4 || c == 5))
          --huc.ch[c].vol;
    }

    mc_out(m, i, snd_clip_sample(acc), 128);
    ++huc.frame;
  }
}

static void pce_sfx(const snd_mixer_t *m, long at_frame) {
  (void)m;
  (void)at_frame;
  huc.ch[5].dda = 1;
  huc.dda_pos = 0u;
  huc.dda_playing = 1;
  huc.ch[5].vol = 31;
}

/* ---------------------------------------------------------------- */
/* simulation                                                        */
/* ---------------------------------------------------------------- */

static void pce_evaluate_sprites(void) {
  int y;
  pce.overflow = 0;
  for (y = 0; y < MC_EX_H; ++y) {
    int n = 0;
    int i;
    for (i = 0; i < PCE_SPRITES; ++i) {
      int fy = y - pce.spr[i].y;
      if (fy < 0 || fy >= pce.spr[i].h)
        continue;
      if (n >= PCE_MAX_PER_LINE) {
        pce.overflow = 1;
        continue;
      }
      pce.line_list[y][n++] = (uint8_t)i;
    }
    pce.line_n[y] = (uint8_t)n;
  }
}

static void pce_init(int screen_w, int screen_h, const snd_mixer_t *m,
                     long start_frame) {
  int i;
  uint32_t rng = 0xACE17u;
  (void)screen_w;
  (void)screen_h;
  (void)start_frame;
  memset(&pce, 0, sizeof(pce));
  pce.show_debug = 1;
  pce_build_palettes();
  pce_build_tiles();
  pce_build_map();
  pce_build_cram();

  /* Sprite 0 is the reason this machine could put a boss on screen: one entry,
     32 pixels wide and 64 tall. Everything after it is a small drone. */
  pce.spr[0].x = PCE_X0 + 100;
  pce.spr[0].y = PCE_Y0 + 40;
  pce.spr[0].w = 32;
  pce.spr[0].h = 64;
  pce.spr[0].tile = 0;
  pce.spr[0].pal = 4;
  pce.spr[0].vx = 1;
  pce.spr[0].vy = 0;
  for (i = 1; i < PCE_SPRITES; ++i) {
    pce.spr[i].x = PCE_X0 + (int)(mc_rand(&rng) % 240u);
    pce.spr[i].y = PCE_Y0 + 100 + (int)(mc_rand(&rng) % 30u);
    pce.spr[i].w = 16;
    pce.spr[i].h = 16;
    pce.spr[i].tile = 1;
    pce.spr[i].pal = 4;
    pce.spr[i].vx = ((int)(mc_rand(&rng) % 3u)) - 1;
    pce.spr[i].vy = ((mc_rand(&rng) & 1u) != 0u) ? 1 : -1;
  }
  pce_evaluate_sprites();
  pce_audio_init(m);
}

static void pce_tick(const mr_demo_input_t *input) {
  int i;
  ++pce.frame;
  /* Three scroll values, changed at two RCR splits. */
  pce.scroll_sky += 1;
  pce.scroll_mid += 2;
  pce.scroll_fore += 5;

  if (input) {
    if (input->dx)
      pce.scroll_fore += input->dx * 4;
    if ((input->buttons & MR_DEMO_INPUT_DEBUG) != 0u)
      pce.show_debug = !pce.show_debug;
  }

  pce.spr[0].x += pce.spr[0].vx;
  if (pce.spr[0].x < PCE_X0 + 20 || pce.spr[0].x > PCE_X0 + 180)
    pce.spr[0].vx = -pce.spr[0].vx;
  pce.spr[0].y = PCE_Y0 + 40 + ((mc_sin((int)pce.frame * 5) * 12) >> 15);
  for (i = 1; i < PCE_SPRITES; ++i) {
    pce.spr[i].x += pce.spr[i].vx;
    pce.spr[i].y += pce.spr[i].vy;
    if (pce.spr[i].y < PCE_Y0 + 96 || pce.spr[i].y > PCE_Y0 + 132)
      pce.spr[i].vy = -pce.spr[i].vy;
    if (pce.spr[i].x < PCE_X0 - 16)
      pce.spr[i].x += PCE_W + 16;
    if (pce.spr[i].x > PCE_X0 + PCE_W)
      pce.spr[i].x -= PCE_W + 16;
  }
  pce_build_cram();
  pce_evaluate_sprites();
}

/* ---------------------------------------------------------------- */
/* render                                                            */
/* ---------------------------------------------------------------- */

static int pce_scroll_for_line(int ly) {
  /* Two raster compare splits. Above the horizon the sky drifts, between the
     horizon and the foreground band the city moves faster, and below the
     second split the girders move fastest. */
  if (ly < PCE_HORIZON)
    return pce.scroll_sky;
  if (ly < PCE_FOREGROUND)
    return pce.scroll_mid;
  return pce.scroll_fore;
}

static void pce_render(gfx_renderer_t *r) {
  int y, y0, y1;
  if (!r)
    return;
  mc_rows(r, &y0, &y1);

  for (y = y0; y < y1; ++y) {
    int x0, tw, x;
    gfx_color_t *row = mc_row(r, y, &x0, &tw);
    int ly = y - PCE_Y0;
    int scroll;
    gfx_color_t sky;
    if (!row)
      continue;
    for (x = 0; x < tw; ++x)
      row[x] = GFX_RGB565_BLACK;
    if (ly < 0 || ly >= PCE_H)
      continue;

    /* This is the RCR write. One palette entry, rewritten before the line is
       drawn -- and the sky tile below never changes. */
    sky = pce.sky_cram[y];
    scroll = pce_scroll_for_line(ly);

    for (x = 0; x < PCE_W; ++x) {
      int sx = PCE_X0 + x;
      int wx, wy, tile, bank, index;
      if (sx < x0 || sx >= x0 + tw)
        continue;
      wx = (x + scroll) & (PCE_MAP_W * 8 - 1);
      wy = ly & (PCE_MAP_H * 8 - 1);
      tile = pce.map[wy >> 3][wx >> 3];
      bank = pce.map_pal[wy >> 3][wx >> 3];
      index = pce.tiles[tile][(wy & 7) * 8 + (wx & 7)];
      /* Colour 0 of every background palette is the shared backdrop, so any
         transparent pixel in any tile shows the sky -- which is why the
         per-line CRAM write reaches the gaps between buildings as well as the
         empty sky above them. */
      row[sx - x0] = (index == 0) ? sky : pce.pal[bank][index];
    }

    {
      int k;
      for (k = 0; k < (int)pce.line_n[y]; ++k) {
        int i = pce.line_list[y][k];
        int fy = y - pce.spr[i].y;
        int px;
        for (px = 0; px < pce.spr[i].w; ++px) {
          int sx = pce.spr[i].x + px;
          int index;
          if (sx < x0 || sx >= x0 + tw)
            continue;
          if (sx < PCE_X0 || sx >= PCE_X0 + PCE_W)
            continue;
          if (pce.spr[i].w >= 32) {
            /* The big one: a 32x64 cell, drawn as a single entry rather than
               as a grid of small sprites glued together. */
            int cx = px - 16;
            int cy = fy - 32;
            int d = cx * cx + (cy * cy) / 3;
            if (d > 240)
              continue;
            index = (d < 40) ? 3 : ((d < 110) ? 2 : ((d < 190) ? 1 : 4));
            if (fy > 44 && ((px + fy) & 3) == 0)
              index = 5;
          } else {
            int cx = px - 8;
            int cy = fy - 8;
            int d = cx * cx + cy * cy;
            if (d > 44)
              continue;
            index = (d < 10) ? 7 : ((d < 26) ? 5 : 6);
          }
          row[sx - x0] = pce.pal[pce.spr[i].pal][index];
        }
      }
    }
  }

  mc_text_shadow(r, 6, 14, "PC ENGINE - THE SKY IS ONE PALETTE ENTRY",
                 GFX_RGB565(255, 240, 210), 1);
  mc_text_shadow(r, 6, 24, "REWRITTEN EVERY SCANLINE BY THE RCR IRQ",
                 GFX_RGB565(240, 210, 170), 1);
  if (pce.show_debug) {
    char buf[64];
    char *p = buf;
    const char *end = buf + sizeof(buf) - 1;
    p = mr_strbuf_str(p, end, "SPR/LINE LIMIT 16  ");
    p = mr_strbuf_str(p, end, pce.overflow ? "OVERFLOW" : "under");
    *p = '\0';
    mc_text_shadow(r, 6, PCE_Y0 + PCE_H - 2, buf, GFX_RGB565(200, 220, 255), 1);
    gfx_draw_hline(r, PCE_X0, PCE_Y0 + PCE_HORIZON, PCE_W,
                   GFX_RGB565(255, 200, 90));
    gfx_draw_hline(r, PCE_X0, PCE_Y0 + PCE_FOREGROUND, PCE_W,
                   GFX_RGB565(255, 200, 90));
    mc_text_shadow(r, PCE_X0 + 4, PCE_Y0 + PCE_HORIZON + 2, "RCR SPLIT 1",
                   GFX_RGB565(255, 220, 140), 1);
    mc_text_shadow(r, PCE_X0 + 4, PCE_Y0 + PCE_FOREGROUND + 2, "RCR SPLIT 2",
                   GFX_RGB565(255, 220, 140), 1);
  }
}

/* ---------------------------------------------------------------- */

static const char *const pce_notes[] = {
    "512 nine-bit colours, 16 background palettes, no attribute grid.",
    "The sky is one tile and one index; RCR rewrites its colour per line.",
    "Two more RCR splits change the scroll: three parallax bands, no layers.",
    "Sprites go to 32x64, and sixteen fit on a scanline instead of eight.",
    "HuC6280: six wavetable channels, 32 samples of 5 bits each.",
    "Channel 2 can leave the mix and become an LFO on channel 1.",
    "DDA mode turns a channel into a software DAC for drums.",
    0};

static const mc_example_t pce_example = {
    "pce-wavetable",
    "NEC PC Engine / TurboGrafx-16 (1987), HuC6270 + HuC6260 + HuC6280",
    "per-line CRAM rewriting via RCR, three scroll splits, 32x64 sprites",
    "six 5-bit wavetable channels, channel-2 LFO crossing into FM, DDA drums",
    pce_notes,
    pce_init,
    pce_tick,
    pce_render,
    pce_mix,
    pce_sfx};

const mc_example_t *mc_example_pce_wavetable(void) { return &pce_example; }
