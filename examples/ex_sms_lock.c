/* Sega Master System, 1985 -- VDP 315-5124 + SN76489
 *
 * Display: the scroll lock bits.
 *
 *   The VDP scrolls one background plane, and it scrolls all of it. A game
 *   that wants a status bar has the same problem the NES has and reaches for a
 *   different answer: instead of rewriting a register mid-frame, the Master
 *   System puts two bits in register 0 that carve fixed regions out of the
 *   scrolling plane permanently.
 *
 *     Bit 6, horizontal scroll lock: the top two rows -- sixteen lines -- ignore
 *     the horizontal scroll register. A score panel that stays put costs one
 *     bit and no CPU time at all.
 *
 *     Bit 7, vertical scroll lock: the rightmost eight columns -- sixty-four
 *     pixels -- ignore vertical scroll. A side panel for lives and rings, for
 *     free, on the axis the NES could not do without a second nametable.
 *
 *   Both are on here, which is why the top strip refuses to move sideways and
 *   the right strip refuses to move up.
 *
 *   Bit 5 is the third one worth having: blank the leftmost eight pixels and
 *   fill them with the backdrop colour. When a plane scrolls horizontally, the
 *   column entering from the left is only partly written -- the CPU updates
 *   the name table a column at a time and the beam does not wait. Masking eight
 *   pixels hides the seam. Nearly every scrolling Master System game has this
 *   on, and the visible cost is that the playfield is 248 pixels wide, not 256.
 *
 *   Two more period constraints are reproduced:
 *
 *   The palette is two banks of sixteen entries drawn from 64 colours -- two
 *   bits per channel, and that is the entire colour space of the machine. Every
 *   colour on screen here is quantized to RGB222, so the gradients step where
 *   the hardware would step.
 *
 *   Sprite zoom (register 1, bit 0) doubles every sprite to 16x16. Not some
 *   sprites: every sprite, because it is one bit for the whole chip. And the
 *   eight-per-scanline limit counts *unzoomed* entries, so turning zoom on does
 *   not buy any more of them -- it just makes the ones you have bigger, and
 *   makes the overflow flag fire sooner because they overlap more lines.
 *
 * Audio: SN76489, three square channels and a noise channel.
 *
 *   The trick this example is built around is the one every Sega 8-bit
 *   soundtrack used and almost nobody notices: the noise channel's shift
 *   register can be clocked from tone generator 3 instead of from one of its
 *   three fixed rates, and it can be switched from white noise to *periodic*
 *   noise, where a single bit walks a 15-stage register and produces one pulse
 *   every fifteen shifts.
 *
 *   Periodic noise clocked by a tone generator is therefore a pitched
 *   waveform -- a buzzy square an octave and a bit below whatever tone 3 is
 *   set to. Silence tone 3's own output, sweep its divider downward, and the
 *   result is a bass drum with a pitch envelope on a chip that has neither
 *   drums nor envelopes. That is what channel 4 is doing below.
 *
 *   The other absence worth hearing: there is no envelope generator at all.
 *   Volume is a 4-bit attenuator, 2 dB a step, sixteen steps to silence, and
 *   the only thing that changes it is the program writing to it once a frame.
 *   Every fade on this machine is a staircase with at most sixty steps a
 *   second, and it sounds like one.
 *
 *   Chords are faked the same way: three channels cannot play a triad and a
 *   bass line, so the lead channel cycles through the chord tones one per
 *   frame. At 60 Hz that reads as a chord with a fast tremolo rather than as
 *   an arpeggio, which is the whole reason the technique survived.
 */

#include "mc_ex_chip.h"
#include "mc_ex_gfx.h"
#include "mc_example.h"
#include "mr_strbuf.h"

#include <string.h>

/* ---------------------------------------------------------------- */
/* geometry: a 256x192 machine shown inside a 320x240 frame           */
/* ---------------------------------------------------------------- */

#define SMS_X0 32
#define SMS_Y0 24
#define SMS_W 256
#define SMS_H 192
#define SMS_LOCK_ROWS 16 /* top two rows: horizontal scroll lock  */
#define SMS_LOCK_COLS 64 /* right eight columns: vertical lock    */
#define SMS_BLANK_COL 8  /* leftmost eight pixels: backdrop       */
#define SMS_MAP_W 32
#define SMS_MAP_H 28
#define SMS_TILES 16
#define SMS_SPRITES 20
#define SMS_MAX_PER_LINE 8

/* 64 colours, two bits per channel. Not a palette so much as the complete set
   of things this machine can display. */
static gfx_color_t sms_rgb222[64];

static struct {
  unsigned long frame;
  int scroll_x;
  int scroll_y;
  int zoom;    /* register 1 bit 0: doubles every sprite, or none */
  int blank_left;
  int show_debug;
  int overflow; /* the VDP's sprite overflow flag for the last frame */

  uint8_t tiles[SMS_TILES][8 * 8]; /* 4bpp indices */
  uint8_t map[SMS_MAP_H][SMS_MAP_W];
  uint8_t map_pal[SMS_MAP_H][SMS_MAP_W]; /* one bit: which 16-colour bank */
  uint8_t pal[2][16];                    /* indices into sms_rgb222 */

  struct {
    int x, y, tile, vx, vy;
  } spr[SMS_SPRITES];
  uint8_t line_list[MC_EX_H][SMS_MAX_PER_LINE];
  uint8_t line_n[MC_EX_H];
} sms;

static void sms_build_rgb222(void) {
  int i;
  for (i = 0; i < 64; ++i) {
    int r = (i & 3) * 85;
    int g = ((i >> 2) & 3) * 85;
    int b = ((i >> 4) & 3) * 85;
    sms_rgb222[i] = GFX_RGB565(r, g, b);
  }
}

#define SMS_C(r, g, b) ((uint8_t)((r) | ((g) << 2) | ((b) << 4)))

static void sms_build_palettes(void) {
  static const uint8_t bank0[16] = {
      SMS_C(1, 1, 3), SMS_C(0, 0, 1), SMS_C(0, 1, 2), SMS_C(1, 2, 3),
      SMS_C(2, 3, 3), SMS_C(3, 3, 3), SMS_C(0, 2, 0), SMS_C(1, 3, 1),
      SMS_C(2, 3, 1), SMS_C(2, 1, 0), SMS_C(3, 2, 0), SMS_C(3, 3, 1),
      SMS_C(1, 0, 0), SMS_C(2, 0, 1), SMS_C(3, 1, 2), SMS_C(0, 0, 0)};
  static const uint8_t bank1[16] = {
      SMS_C(1, 1, 3), SMS_C(0, 0, 2), SMS_C(1, 0, 2), SMS_C(2, 1, 3),
      SMS_C(3, 2, 3), SMS_C(3, 3, 3), SMS_C(0, 1, 2), SMS_C(1, 2, 3),
      SMS_C(2, 2, 3), SMS_C(3, 1, 1), SMS_C(3, 2, 2), SMS_C(3, 3, 2),
      SMS_C(2, 0, 0), SMS_C(3, 0, 0), SMS_C(3, 2, 1), SMS_C(0, 0, 0)};
  memcpy(sms.pal[0], bank0, sizeof(bank0));
  memcpy(sms.pal[1], bank1, sizeof(bank1));
}

static void sms_build_tiles(void) {
  int t, x, y;
  for (t = 0; t < SMS_TILES; ++t) {
    for (y = 0; y < 8; ++y) {
      for (x = 0; x < 8; ++x) {
        int v = 0;
        switch (t) {
        case 0:
          v = 0; /* sky */
          break;
        case 1: /* checker sky detail */
          v = (((x ^ y) & 4) != 0) ? 0 : 2;
          break;
        case 2: /* cloud */
          v = ((x - 4) * (x - 4) + (y - 4) * (y - 4) < 15) ? 5 : 0;
          break;
        case 3: /* grass top */
          v = (y == 0) ? 7 : ((y < 3) ? 6 : ((((x * 3 + y) & 5) < 2) ? 8 : 6));
          break;
        case 4: /* dirt */
          v = (((x * 5 + y * 3) & 7) < 2) ? 10 : 9;
          break;
        case 5: /* brick */
          v = ((y & 3) == 0 || ((x + ((y >> 2) & 1) * 4) & 7) == 0) ? 12 : 13;
          break;
        case 6: /* panel plate */
          v = (x == 0 || y == 0) ? 4 : ((x == 7 || y == 7) ? 1 : 3);
          break;
        case 7: /* panel fill */
          v = 1;
          break;
        case 8: /* ring / token */
          v = (((x - 4) * (x - 4)) * 2 + (y - 4) * (y - 4) < 12) ? 11 : 0;
          break;
        case 9: /* stripe */
          v = (((x + y) & 7) < 4) ? 14 : 13;
          break;
        case 10: /* checker floor */
          v = ((((x >> 2) ^ (y >> 2)) & 1) != 0) ? 3 : 2;
          break;
        case 11: /* water */
          v = (((x * 3 + y * 2) & 7) < 3) ? 3 : 2;
          break;
        default:
          v = (t + x + y) & 15;
          break;
        }
        sms.tiles[t][y * 8 + x] = (uint8_t)v;
      }
    }
  }
}

static void sms_build_map(void) {
  int x, y;
  uint32_t rng = 0x5A17u;
  /* One continuous world, with strong structure on both axes so that both
     locks are visible for what they are. The locked regions are not special
     map data -- they read the same name table as everything else, just with
     one scroll component forced to zero. Building a "panel" into the map here
     would be a different effect entirely, and a wrong one: the vertical lock
     does not stop horizontal scrolling, so a panel drawn into those columns
     would slide off the side. */
  for (y = 0; y < SMS_MAP_H; ++y) {
    for (x = 0; x < SMS_MAP_W; ++x) {
      int t;
      if (y >= 22)
        t = ((x >> 1) & 1) ? 5 : 9; /* banded bedrock: reads vertical motion */
      else if (y >= 19)
        t = 4;
      else if (y == 18)
        t = 3;
      else if ((x % 6) == 0 && y >= 6)
        t = 6; /* pillars: read horizontal motion */
      else if ((x % 6) == 0)
        t = 7;
      else if (y == 12 || y == 5)
        t = 10; /* strata: read vertical motion */
      else if ((mc_rand(&rng) & 31u) < 2u)
        t = 2;
      else if ((mc_rand(&rng) & 31u) < 3u)
        t = 1;
      else
        t = 0;
      sms.map[y][x] = (uint8_t)t;
      /* Bank select is per name-table entry, so two sixteen-colour palettes
         can coexist on one screen -- but only one of them per 8x8 tile. */
      sms.map_pal[y][x] = (uint8_t)((y >= 19 && ((x >> 2) & 1)) ? 1 : 0);
    }
  }
}

/* ---------------------------------------------------------------- */
/* SN76489                                                           */
/* ---------------------------------------------------------------- */

/* The PSG's own clock: 3.579545 MHz divided by 16. Every pitch on the machine
   is this number divided by a 10-bit integer and then by two, which is why the
   top octave has audible gaps between adjacent semitones and the bottom
   octave simply does not exist. */
#define SN_CLOCK 223722

typedef struct sn_tone {
  int divider; /* 10 bits: 1..1023 */
  int atten;   /* 0 loudest, 15 silent */
  uint32_t acc;
  uint32_t step;
  int32_t out;
} sn_tone_t;

static struct {
  int rate;
  sn_tone_t tone[3];

  /* Noise channel. `from_tone3` is the bit that clocks it from tone generator
     3's divider instead of one of the three fixed rates; `periodic` swaps the
     white-noise tap for the single-bit one. Together they turn the noise
     channel into a tuned instrument. */
  uint32_t noise_lfsr;
  uint32_t noise_acc;
  uint32_t noise_step;
  int noise_atten;
  int noise_periodic;
  int noise_from_tone3;
  int32_t noise_out;

  long tick_acc;
  long tick_period; /* the 60 Hz vblank: the only clock the driver has */
  int tick;

  long row_acc;
  long row_period;
  int row;

  /* Software state the driver owns, because the chip owns none of it. */
  int arp_notes[3];
  int arp_index;
  int lead_atten;
  int bass_atten;
  int drum_divider;
} sn;

static void sn_tone_set(sn_tone_t *t, int divider) {
  if (divider < 1)
    divider = 1;
  if (divider > 1023)
    divider = 1023;
  t->divider = divider;
  /* Two divider ticks per cycle: the output flips on each one. */
  t->step = (uint32_t)(((int64_t)SN_CLOCK << 16) /
                       ((int64_t)sn.rate * 2LL * (int64_t)divider));
}

/* Hz to the nearest 10-bit divider. The rounding here is the whole reason a
   Master System melody drifts sharp as it climbs. */
static int sn_divider_for_hz(int hz) {
  int d;
  if (hz <= 0)
    return 1023;
  d = (SN_CLOCK / 2 + hz / 2) / hz;
  if (d < 1)
    d = 1;
  if (d > 1023)
    d = 1023;
  return d;
}

static int32_t sn_tone_next(sn_tone_t *t) {
  t->acc += t->step;
  while (t->acc >= 0x10000u) {
    t->acc -= 0x10000u;
    t->out = -t->out;
  }
  return mc_psg_attenuate((int32_t)t->out * 32767 / 4, t->atten);
}

static int32_t sn_noise_next(void) {
  uint32_t step = sn.noise_from_tone3 ? sn.tone[2].step : sn.noise_step;
  sn.noise_acc += step;
  while (sn.noise_acc >= 0x10000u) {
    sn.noise_acc -= 0x10000u;
    if (sn.noise_periodic) {
      /* Periodic mode: no XOR at all. One bit walks the fifteen stages and
         comes back around, so the output is a narrow pulse at 1/15 of the
         shift rate -- a pitched buzz, not noise. */
      uint32_t bit = sn.noise_lfsr & 1u;
      sn.noise_lfsr = (sn.noise_lfsr >> 1) | (bit << 14);
    } else {
      sn.noise_lfsr = mc_lfsr_step(sn.noise_lfsr, MC_LFSR_SN76489);
    }
    sn.noise_out = (sn.noise_lfsr & 1u) ? 1 : -1;
  }
  return mc_psg_attenuate((int32_t)sn.noise_out * 32767 / 4, sn.noise_atten);
}

/* Four bars of a lead, a bass, and a chord the lead cannot play. */
static const uint8_t sms_lead[32] = {76, 0,  0,  79, 0,  0,  83, 0,
                                     81, 0,  0,  79, 0,  0,  76, 0,
                                     74, 0,  0,  77, 0,  0,  81, 0,
                                     79, 0,  0,  76, 0,  0,  0,  0};
static const uint8_t sms_bass[32] = {45, 0, 45, 0, 52, 0, 45, 0,
                                     43, 0, 43, 0, 50, 0, 43, 0,
                                     41, 0, 41, 0, 48, 0, 41, 0,
                                     45, 0, 45, 0, 52, 0, 52, 0};
/* Chord roots the arpeggio walks: a minor triad per bar. */
static const uint8_t sms_chord[4][3] = {
    {69, 72, 76}, {67, 71, 74}, {65, 69, 72}, {64, 67, 71}};
/* 1 = tuned periodic-noise kick, 2 = white-noise snare, 3 = hat */
static const uint8_t sms_drum[32] = {1, 0, 0, 3, 2, 0, 3, 0, 1, 0, 1,
                                     3, 2, 0, 3, 3, 1, 0, 0, 3, 2, 0,
                                     3, 0, 1, 0, 1, 3, 2, 3, 3, 3};

static void sn_init(const snd_mixer_t *m) {
  int i;
  memset(&sn, 0, sizeof(sn));
  sn.rate = m ? m->rate : SND_DEFAULT_RATE;
  sn.tick_period = mc_frames_per_tick(m, 60);
  sn.row_period = mc_frames_per_tick(m, 60) * 6;
  sn.noise_lfsr = 0x8000u;
  for (i = 0; i < 3; ++i) {
    sn.tone[i].out = 1;
    sn.tone[i].atten = 15;
    sn_tone_set(&sn.tone[i], 400);
  }
  sn.noise_atten = 15;
  sn.noise_out = 1;
  sn.noise_step = (uint32_t)(((int64_t)SN_CLOCK << 16) / ((int64_t)sn.rate * 32LL));
  sn.lead_atten = 15;
  sn.bass_atten = 15;
  sn.arp_notes[0] = 69;
  sn.arp_notes[1] = 72;
  sn.arp_notes[2] = 76;
  sn.drum_divider = 200;
}

/* Once per vertical blank. Everything time-varying about this chip happens
   here, because there is nowhere else for it to happen. */
static void sn_vblank(void) {
  /* Arpeggio: one chord tone per frame on channel 1. Sixty changes a second is
     fast enough that the ear fuses them into a chord. */
  sn.arp_index = (sn.arp_index + 1) % 3;
  sn_tone_set(&sn.tone[0], sn_divider_for_hz(snd_note_hz(sn.arp_notes[sn.arp_index])));

  /* Software envelopes: one attenuation step per frame, 2 dB each. */
  if (sn.lead_atten < 15)
    ++sn.lead_atten;
  if (sn.bass_atten < 15)
    sn.bass_atten += ((sn.tick & 1) != 0) ? 1 : 0;
  sn.tone[1].atten = sn.bass_atten;
  sn.tone[0].atten = sn.lead_atten;

  /* The kick's pitch envelope, applied to tone generator 3's divider while
     tone 3 itself stays silent. The noise channel is listening to it. */
  if (sn.noise_atten < 15 && sn.noise_periodic) {
    sn.drum_divider += 24;
    sn_tone_set(&sn.tone[2], sn.drum_divider);
    sn.noise_atten += 1;
  } else if (sn.noise_atten < 15) {
    sn.noise_atten += 2;
  }

  ++sn.tick;
}

static void sn_row(void) {
  int r = sn.row & 31;
  int n = sms_lead[r];
  int i;

  if (n) {
    for (i = 0; i < 3; ++i)
      sn.arp_notes[i] = n - 12 + (int)sms_chord[(r >> 3) & 3][i] % 12;
    sn.arp_notes[0] = n;
    sn.arp_notes[1] = n + 3;
    sn.arp_notes[2] = n + 7;
    sn.lead_atten = 2;
  }
  n = sms_bass[r];
  if (n) {
    sn_tone_set(&sn.tone[1], sn_divider_for_hz(snd_note_hz(n)));
    sn.bass_atten = 3;
  }

  switch (sms_drum[r]) {
  case 1: /* kick: periodic noise clocked by a swept tone 3 */
    sn.noise_periodic = 1;
    sn.noise_from_tone3 = 1;
    sn.drum_divider = 150;
    sn_tone_set(&sn.tone[2], sn.drum_divider);
    sn.tone[2].atten = 15; /* tone 3 is the clock, not a voice */
    sn.noise_atten = 1;
    break;
  case 2: /* snare: white noise at a fixed rate */
    sn.noise_periodic = 0;
    sn.noise_from_tone3 = 0;
    sn.noise_step =
        (uint32_t)(((int64_t)SN_CLOCK << 16) / ((int64_t)sn.rate * 32LL));
    sn.noise_atten = 2;
    break;
  case 3: /* hat: white noise, fastest rate, short */
    sn.noise_periodic = 0;
    sn.noise_from_tone3 = 0;
    sn.noise_step =
        (uint32_t)(((int64_t)SN_CLOCK << 16) / ((int64_t)sn.rate * 8LL));
    sn.noise_atten = 8;
    break;
  default:
    break;
  }

  ++sn.row;
  if (sn.row >= 32)
    sn.row = 0;
}

static void sms_mix(snd_mixer_t *m, void *user) {
  int i, frames;
  (void)user;
  if (!m)
    return;
  frames = m->block_frames;
  if (frames <= 0)
    return;
  snd_touch_block(m);
  for (i = 0; i < frames; ++i) {
    int32_t acc;
    if (--sn.tick_acc <= 0) {
      sn.tick_acc = sn.tick_period;
      sn_vblank();
    }
    if (--sn.row_acc <= 0) {
      sn.row_acc = sn.row_period;
      sn_row();
    }
    acc = sn_tone_next(&sn.tone[0]) + sn_tone_next(&sn.tone[1]) +
          sn_tone_next(&sn.tone[2]) + sn_noise_next();
    mc_out(m, i, snd_clip_sample(acc), 128);
  }
}

static void sms_sfx(const snd_mixer_t *m, long at_frame) {
  (void)m;
  (void)at_frame;
  /* A ring pickup: periodic noise high and short, which is the same trick as
     the kick pointed the other way up the register. */
  sn.noise_periodic = 1;
  sn.noise_from_tone3 = 1;
  sn.drum_divider = 24;
  sn_tone_set(&sn.tone[2], sn.drum_divider);
  sn.tone[2].atten = 15;
  sn.noise_atten = 0;
}

/* ---------------------------------------------------------------- */
/* simulation                                                        */
/* ---------------------------------------------------------------- */

static void sms_evaluate_sprites(void) {
  int y;
  sms.overflow = 0;
  for (y = 0; y < MC_EX_H; ++y) {
    int n = 0;
    int i;
    int h = sms.zoom ? 16 : 8;
    for (i = 0; i < SMS_SPRITES; ++i) {
      int fy = y - sms.spr[i].y;
      if (fy < 0 || fy >= h)
        continue;
      if (n >= SMS_MAX_PER_LINE) {
        /* The VDP sets the overflow flag and stops fetching. Unlike the NES,
           Sega's documentation says out loud that the ninth sprite is
           dropped, so games budgeted for it rather than flickering. */
        sms.overflow = 1;
        continue;
      }
      sms.line_list[y][n++] = (uint8_t)i;
    }
    sms.line_n[y] = (uint8_t)n;
  }
}

static void sms_init(int screen_w, int screen_h, const snd_mixer_t *m,
                     long start_frame) {
  int i;
  uint32_t rng = 0xBEEF17u;
  (void)screen_w;
  (void)screen_h;
  (void)start_frame;
  memset(&sms, 0, sizeof(sms));
  sms.zoom = 1;
  sms.blank_left = 1;
  sms.show_debug = 1;
  sms_build_rgb222();
  sms_build_palettes();
  sms_build_tiles();
  sms_build_map();
  for (i = 0; i < SMS_SPRITES; ++i) {
    sms.spr[i].x = SMS_X0 + 16 + (int)(mc_rand(&rng) % 200u);
    sms.spr[i].y = SMS_Y0 + 60 + (int)(mc_rand(&rng) % 40u);
    sms.spr[i].tile = 8;
    sms.spr[i].vx = ((int)(mc_rand(&rng) % 3u)) - 1;
    sms.spr[i].vy = ((mc_rand(&rng) & 1u) != 0u) ? 1 : -1;
  }
  sms_evaluate_sprites();
  sn_init(m);
}

static void sms_tick(const mr_demo_input_t *input) {
  int i;
  ++sms.frame;
  sms.scroll_x += 1;
  sms.scroll_y = 40 + ((mc_sin((int)sms.frame * 4) * 38) >> 15);

  if (input) {
    if (input->dx)
      sms.scroll_x += input->dx * 2;
    if ((input->buttons & MR_DEMO_INPUT_ACTION) != 0u)
      sms.zoom = !sms.zoom;
    if ((input->buttons & MR_DEMO_INPUT_DEBUG) != 0u)
      sms.show_debug = !sms.show_debug;
  }

  for (i = 0; i < SMS_SPRITES; ++i) {
    sms.spr[i].x += sms.spr[i].vx;
    sms.spr[i].y += sms.spr[i].vy;
    if (sms.spr[i].y < SMS_Y0 + 56 || sms.spr[i].y > SMS_Y0 + 104)
      sms.spr[i].vy = -sms.spr[i].vy;
    if (sms.spr[i].x < SMS_X0)
      sms.spr[i].x += SMS_W;
    if (sms.spr[i].x > SMS_X0 + SMS_W)
      sms.spr[i].x -= SMS_W;
  }
  sms_evaluate_sprites();
}

/* ---------------------------------------------------------------- */
/* render                                                            */
/* ---------------------------------------------------------------- */

static gfx_color_t sms_pixel(int bank, int index) {
  return sms_rgb222[sms.pal[bank & 1][index & 15] & 63];
}

static void sms_render(gfx_renderer_t *r) {
  int y, y0, y1;
  gfx_color_t backdrop;
  if (!r)
    return;
  mc_rows(r, &y0, &y1);
  backdrop = sms_pixel(0, 15);

  for (y = y0; y < y1; ++y) {
    int x0, tw, sx;
    gfx_color_t *row = mc_row(r, y, &x0, &tw);
    int vy = y - SMS_Y0;
    int hscroll;
    if (!row)
      continue;

    /* Paint the whole row with the backdrop first. The area outside the
       256x192 active display is not "nothing": the VDP drives the border with
       the backdrop colour, and a game that changes that register mid-frame
       gets a coloured border for free. It also means render() writes every
       pixel it is given, which the DOS frontend requires -- it drives the
       renderer through gfx_render_tiled_no_clear() and reuses one band buffer,
       so an untouched pixel shows whatever the previous band left there. */
    {
      int i;
      for (i = 0; i < tw; ++i)
        row[i] = backdrop;
    }
    if (vy < 0 || vy >= SMS_H)
      continue;

    /* Register 0 bit 6. The top sixteen lines do not see the horizontal
       scroll register, so the panel drawn there stays where it was put. */
    hscroll = (vy < SMS_LOCK_ROWS) ? 0 : sms.scroll_x;

    for (sx = 0; sx < SMS_W; ++sx) {
      int screen_x = SMS_X0 + sx;
      int wx, wy, tx, ty, tile, bank, index;
      gfx_color_t c;
      if (screen_x < x0 || screen_x >= x0 + tw)
        continue;

      /* Register 0 bit 5: the leftmost eight pixels are replaced with the
         backdrop colour, hiding the partly-written column that is scrolling
         in behind them. */
      if (sms.blank_left && sx < SMS_BLANK_COL) {
        row[screen_x - x0] = backdrop;
        continue;
      }

      /* Register 0 bit 7. The rightmost sixty-four pixels ignore vertical
         scroll, which is how a side panel survives a plane that is moving
         underneath it. */
      wx = (sx + hscroll) & (SMS_MAP_W * 8 - 1);
      wy = (sx >= SMS_W - SMS_LOCK_COLS) ? vy
                                         : ((vy + sms.scroll_y) &
                                            (SMS_MAP_H * 8 - 1));

      tx = (wx >> 3) % SMS_MAP_W;
      ty = (wy >> 3) % SMS_MAP_H;
      tile = sms.map[ty][tx];
      bank = sms.map_pal[ty][tx];
      index = sms.tiles[tile][(wy & 7) * 8 + (wx & 7)];
      c = sms_pixel(bank, index);
      row[screen_x - x0] = c;
    }

    /* Sprites. Zoom is a single chip-wide bit, so either all of these are
       16x16 or none of them are. */
    {
      int k;
      int scale = sms.zoom ? 2 : 1;
      for (k = 0; k < (int)sms.line_n[y]; ++k) {
        int i = sms.line_list[y][k];
        int fy = (y - sms.spr[i].y) / scale;
        int px;
        for (px = 0; px < 8 * scale; ++px) {
          int screen_x = sms.spr[i].x + px;
          int index;
          if (screen_x < x0 || screen_x >= x0 + tw)
            continue;
          if (screen_x < SMS_X0 || screen_x >= SMS_X0 + SMS_W)
            continue;
          if (sms.blank_left && screen_x - SMS_X0 < SMS_BLANK_COL)
            continue;
          index = sms.tiles[sms.spr[i].tile][fy * 8 + px / scale];
          if (index == 0)
            continue; /* colour 0 of a sprite palette is transparent */
          row[screen_x - x0] = sms_pixel(1, index);
        }
      }
    }
  }

  /* Captions, drawn inside the locked regions so it is obvious which parts of
     the plane are refusing to move. */
  gfx_draw_text5x7(r, SMS_X0 + 12, SMS_Y0 + 4, "H-LOCK: TOP 2 ROWS",
                   sms_pixel(0, 5), 1);
  gfx_draw_text5x7(r, SMS_X0 + SMS_W - SMS_LOCK_COLS + 2, SMS_Y0 + 26,
                   "V-LOCK:", sms_pixel(0, 5), 1);
  gfx_draw_text5x7(r, SMS_X0 + SMS_W - SMS_LOCK_COLS + 2, SMS_Y0 + 36,
                   "RIGHT 8", sms_pixel(0, 5), 1);
  gfx_draw_vline(r, SMS_X0 + SMS_W - SMS_LOCK_COLS, SMS_Y0, SMS_H,
                 sms_pixel(0, 11));
  gfx_draw_hline(r, SMS_X0, SMS_Y0 + SMS_LOCK_ROWS, SMS_W, sms_pixel(0, 11));
  mc_text_shadow(r, 4, 6, "SEGA MASTER SYSTEM - 256x192 IN RGB222",
                 GFX_RGB565(200, 200, 210), 1);

  if (sms.show_debug) {
    char buf[64];
    char *p = buf;
    const char *end = buf + sizeof(buf) - 1;
    p = mr_strbuf_str(p, end, sms.zoom ? "ZOOM ON  " : "ZOOM OFF ");
    p = mr_strbuf_str(p, end, sms.overflow ? "OVERFLOW SET" : "overflow clear");
    *p = '\0';
    mc_text_shadow(r, 4, SMS_Y0 + SMS_H + 4, buf, GFX_RGB565(230, 220, 120), 1);
    mc_text_shadow(r, 4, SMS_Y0 + SMS_H + 14,
                   "LEFT 8 PX BLANKED TO HIDE THE SCROLL SEAM",
                   GFX_RGB565(160, 160, 180), 1);
  }
}

/* ---------------------------------------------------------------- */

static const char *const sms_notes[] = {
    "Register 0 bit 6: top two rows ignore horizontal scroll.",
    "Register 0 bit 7: right eight columns ignore vertical scroll.",
    "Register 0 bit 5: leftmost 8 px blanked, hiding the scroll-in seam.",
    "Sprite zoom is one bit for the whole chip: all 16x16 or none.",
    "64 colours exist. Two banks of 16 are on screen.",
    "SN76489: periodic noise clocked by tone 3 = a tuned bass drum.",
    "No envelope generator: every fade is 4-bit steps at 60 Hz.",
    0};

static const mc_example_t sms_example = {
    "sms-lock",
    "Sega Master System (1985), VDP 315-5124 + SN76489",
    "scroll-lock regions, left-column blanking, chip-wide sprite zoom, RGB222",
    "periodic noise clocked from tone 3, 60 Hz software envelopes, arpeggio chords",
    sms_notes,
    sms_init,
    sms_tick,
    sms_render,
    sms_mix,
    sms_sfx};

const mc_example_t *mc_example_sms_lock(void) { return &sms_example; }
