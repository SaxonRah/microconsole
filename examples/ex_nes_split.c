/* Nintendo Famicom / NES, 1983 -- Ricoh 2C02 PPU + 2A03 APU
 *
 * Display: the mid-frame raster split.
 *
 *   The 2C02 has exactly one horizontal scroll register and it is read at the
 *   start of every scanline. That is the whole trick. A game that wants a
 *   fixed status bar over a scrolling world does not need two background
 *   layers, it needs to know when the beam has cleared the status bar and to
 *   write a different scroll value before the next line starts. Games learned
 *   when by parking sprite 0 on the last row of the bar and polling the
 *   sprite-0 hit flag; later mappers put a counter on the cartridge and raised
 *   an IRQ instead, which is what made three and four splits per frame normal.
 *
 *   So this example keeps one scroll value per scanline and nothing else. The
 *   status bar holds still, the playfield scrolls, and the water band below it
 *   scrolls faster with a sine offset applied every eight lines -- roughly what
 *   an MMC3 IRQ could afford. There is no layer system anywhere in here.
 *
 *   Two more constraints are reproduced because leaving them out would make
 *   this look like a 16-bit machine wearing a small palette:
 *
 *   Attribute clash. Colour is not per-pixel or even per-tile. The nametable
 *   picks an 8x8 pattern; a separate attribute table picks one 4-colour
 *   subpalette per *16x16* block. Two 8x8 tiles that share an attribute square
 *   cannot disagree about colour, which is why NES artists aligned everything
 *   to a 16-pixel grid and why the seams are visible here when you look for
 *   them.
 *
 *   Eight sprites per scanline. The PPU's secondary OAM holds eight entries.
 *   The ninth sprite on a line is simply not drawn. Games hid this by rotating
 *   the order they wrote OAM in each frame, which turns a permanent
 *   disappearance into a flicker -- and flicker, unlike absence, reads as
 *   motion. The rotation is in here and the drop count is on screen.
 *
 * Audio: the 2A03's five channels and its 240 Hz frame sequencer.
 *
 *   Two pulse channels with four selectable duties, a sweep unit that slides
 *   pitch in hardware, a triangle that is really a 4-bit staircase, a noise
 *   channel whose shift register has a second, shorter tap that makes it
 *   tonal instead of hissy, and DPCM: a 1-bit delta stream driving a 7-bit
 *   counter.
 *
 *   The frame sequencer is the part worth reproducing carefully. It ticks at
 *   240 Hz regardless of what the music is doing; quarter-frames clock the
 *   volume envelopes and half-frames clock the sweep. Envelopes on this chip
 *   therefore move in 4 ms steps, and that granularity is audible -- it is why
 *   NES decays sound like a decay rather than like a fade.
 */

#include "mc_ex_chip.h"
#include "mc_ex_gfx.h"
#include "mc_example.h"
#include "mr_strbuf.h"

#include <string.h>

/* ---------------------------------------------------------------- */
/* the 2C02 master palette                                           */
/* ---------------------------------------------------------------- */

/* 64 entries, of which 8 are duplicates of black. These are fixed in silicon:
   a game chooses 4 subpalettes of 4 from this list and has no other colours
   available anywhere on the machine. */
#define P(r, g, b) GFX_RGB565(0x##r, 0x##g, 0x##b)
static const gfx_color_t nes_master[64] = {
    P(54, 54, 54), P(00, 1E, 74), P(08, 10, 90), P(30, 00, 88), P(44, 00, 64),
    P(5C, 00, 30), P(54, 04, 00), P(3C, 18, 00), P(20, 2A, 00), P(08, 3A, 00),
    P(00, 40, 00), P(00, 3C, 00), P(00, 32, 3C), P(00, 00, 00), P(00, 00, 00),
    P(00, 00, 00), P(98, 96, 98), P(08, 4C, C4), P(30, 32, EC), P(5C, 1E, E4),
    P(88, 14, B0), P(A0, 14, 64), P(98, 22, 20), P(78, 3C, 00), P(54, 5A, 00),
    P(28, 72, 00), P(08, 7C, 00), P(00, 76, 28), P(00, 66, 78), P(00, 00, 00),
    P(00, 00, 00), P(00, 00, 00), P(EC, EE, EC), P(4C, 9A, EC), P(78, 7C, EC),
    P(B0, 62, EC), P(E4, 54, EC), P(EC, 58, B4), P(EC, 6A, 64), P(D4, 88, 20),
    P(A0, AA, 00), P(74, C4, 00), P(4C, D0, 20), P(38, CC, 6C), P(38, B4, CC),
    P(3C, 3C, 3C), P(00, 00, 00), P(00, 00, 00), P(EC, EE, EC), P(A8, CC, EC),
    P(BC, BC, EC), P(D4, B2, EC), P(EC, AE, EC), P(EC, AE, D4), P(EC, B4, B0),
    P(E4, C4, 90), P(CC, D2, 78), P(B4, DE, 78), P(A8, E2, 90), P(98, E2, B4),
    P(A0, D6, E4), P(A0, A2, A0), P(00, 00, 00), P(00, 00, 00)};
#undef P

/* ---------------------------------------------------------------- */
/* PPU-side state                                                    */
/* ---------------------------------------------------------------- */

#define NES_PAT_COUNT 16
#define NES_NT_W 64
#define NES_NT_H 30
#define NES_SPRITES 32
#define NES_BAR_H 32
#define NES_WATER_Y 176
#define NES_MAX_PER_LINE 8

typedef struct nes_sprite {
  int x;
  int y;
  int tile;
  int pal;
  int vx;
  int vy;
} nes_sprite_t;

static struct {
  int w, h;
  unsigned long frame;

  uint8_t pattern[NES_PAT_COUNT][8 * 8]; /* 2 bits per pixel, stored as 0..3 */
  uint8_t nametable[NES_NT_W * NES_NT_H];
  uint8_t attrib[(NES_NT_W / 2) * (NES_NT_H / 2 + 1)];
  uint8_t bg_pal[4][4];  /* indices into nes_master */
  uint8_t spr_pal[4][4]; /* entry 0 is transparent */

  int scroll_main; /* whole-pixel camera for the playfield split */
  int scroll_water;
  int wave_phase;

  nes_sprite_t sprites[NES_SPRITES];
  int oam_rotate;   /* which sprite the PPU starts evaluating from */
  int dropped_last; /* sprites lost to the 8-per-line limit this frame */
  /* Secondary OAM: the eight entries the PPU keeps for each scanline. Filled
     once per frame in tick(), never during render().

     That split is not tidiness. render() has to produce the same picture
     whether it is handed one full-height tile or fifteen 16-row bands, so it
     must not accumulate anything a later band can see -- and a drop counter
     built while drawing is exactly that. The capture harness compares the two
     rendering shapes and will say so. */
  uint8_t line_list[MC_EX_H][NES_MAX_PER_LINE];
  uint8_t line_n[MC_EX_H];
  int show_debug;
} nes;

/* Pattern tiles are drawn as 2-bit index art, exactly the format the PPU
   reads: two bitplanes per tile. Painted here rather than loaded, so this
   example needs no asset pipeline -- the same argument mr_game_demo.c makes. */
static void nes_build_patterns(void) {
  int t, x, y;
  for (t = 0; t < NES_PAT_COUNT; ++t) {
    for (y = 0; y < 8; ++y) {
      for (x = 0; x < 8; ++x) {
        int v = 0;
        switch (t) {
        case 0:
          v = 0;
          break;
        case 1: /* brick */
          v = ((y & 3) == 3 || ((x + ((y >> 2) & 1) * 4) & 7) == 0) ? 1 : 2;
          break;
        case 2: /* brick top highlight */
          v = (y == 0) ? 3 : (((y & 3) == 3) ? 1 : 2);
          break;
        case 3: /* grass */
          v = (((x * 5 + y * 3) & 7) < 2) ? 3 : 2;
          break;
        case 4: /* stone block */
          v = (x == 0 || y == 0) ? 3 : ((x == 7 || y == 7) ? 1 : 2);
          break;
        case 5: /* question block */
          v = (x == 0 || y == 0 || x == 7 || y == 7)
                  ? 1
                  : (((x >= 3 && x <= 4 && y >= 2 && y <= 3) ||
                      (x == 4 && y == 4) || (x == 4 && y == 6))
                         ? 3
                         : 2);
          break;
        case 6: /* cloud / bush */
          v = ((x - 4) * (x - 4) + (y - 4) * (y - 4) < 14) ? 3 : 0;
          break;
        case 7: /* water surface */
          v = (y < 2) ? 3 : (((x + y) & 3) == 0 ? 2 : 1);
          break;
        case 8: /* water body */
          v = (((x * 3 + y) & 7) < 2) ? 2 : 1;
          break;
        case 9: /* pipe left */
          v = (x < 2) ? 3 : ((x > 5) ? 1 : 2);
          break;
        case 10: /* pipe right */
          v = (x > 5) ? 1 : ((x < 2) ? 2 : 3);
          break;
        case 11: /* hud panel */
          v = 0;
          break;
        case 12: /* coin: a tall ellipse with a darker rim */
          {
            int d = ((x - 4) * (x - 4)) * 3 + (y - 4) * (y - 4);
            v = (d < 10) ? 3 : ((d < 22) ? 2 : 0);
          }
          break;
        case 13: /* player body */
          v = (x >= 2 && x <= 5) ? ((y < 3) ? 2 : 3) : 0;
          break;
        case 14: /* player legs */
          v = ((x == 2 || x == 5) && y > 1) ? 1 : ((x >= 2 && x <= 5) ? 3 : 0);
          break;
        default: /* enemy: domed body, two eyes, feet */
          if (y < 1 || x < 1 || x > 6)
            v = 0;
          else if (y == 1 && (x < 2 || x > 5))
            v = 0;
          else if (y == 7)
            v = (x == 1 || x == 2 || x == 5 || x == 6) ? 3 : 0;
          else if (y == 4 && (x == 2 || x == 5))
            v = 3;
          else
            v = (y < 3) ? 2 : 1;
          break;
        }
        nes.pattern[t][y * 8 + x] = (uint8_t)v;
      }
    }
  }
}

static void nes_build_map(void) {
  int x, y;
  uint32_t rng = 0x1234ABCDu;
  memset(nes.nametable, 0, sizeof(nes.nametable));
  memset(nes.attrib, 0, sizeof(nes.attrib));

  for (y = 0; y < NES_NT_H; ++y) {
    for (x = 0; x < NES_NT_W; ++x) {
      int t = 0;
      int py = y * 8;
      if (py >= NES_WATER_Y)
        t = (py < NES_WATER_Y + 8) ? 7 : 8;
      else if (y >= 20)
        t = ((x + y) & 1) ? 1 : 2; /* brick */
      else if (y == 19)
        t = 3; /* grass cap */
      else if ((x % 13) == 5 && y == 15)
        t = 5; /* question block */
      else if ((x % 17) == 9 && y >= 16 && y < 19)
        t = (y == 16) ? 9 : 10; /* pipe */
      else if ((mc_rand(&rng) & 63u) < 4u && y > 3 && y < 12)
        t = 6; /* cloud */
      nes.nametable[y * NES_NT_W + x] = (uint8_t)t;
    }
  }

  /* One attribute byte per 16x16 pixels, and this is where the clash lives.
     The ground below is deliberately given alternating subpalettes across x so
     the colour change lands on a 16-pixel boundary that has nothing to do with
     the brickwork. Every NES artist knew this grid was there and drew to it;
     the ones who did not got exactly the seams visible here. */
  for (y = 0; y < NES_NT_H / 2 + 1; ++y) {
    for (x = 0; x < NES_NT_W / 2; ++x) {
      int pal;
      int py = y * 16;
      if (py >= NES_WATER_Y)
        pal = 3;
      else if (py >= 152)
        pal = ((x & 1) != 0) ? 1 : 2;
      else
        pal = 0;
      nes.attrib[y * (NES_NT_W / 2) + x] = (uint8_t)pal;
    }
  }
}

static void nes_build_palettes(void) {
  /* Four background subpalettes of four entries. Entry 0 is not per-palette on
     real hardware: it is one universal backdrop colour shared by all of them,
     and that is reproduced here -- every subpalette starts 0x22, the sky. A
     game gets three free colours per attribute square and no more. */
  static const uint8_t bg[4][4] = {{0x22, 0x21, 0x11, 0x30},  /* sky, cloud   */
                                   {0x22, 0x16, 0x27, 0x18},  /* brick warm   */
                                   {0x22, 0x1A, 0x2A, 0x30},  /* brick green  */
                                   {0x22, 0x11, 0x21, 0x31}}; /* water        */
  static const uint8_t sp[4][4] = {{0x0F, 0x27, 0x28, 0x30},
                                   {0x0F, 0x16, 0x27, 0x36},
                                   {0x0F, 0x11, 0x21, 0x30},
                                   {0x0F, 0x06, 0x16, 0x26}};
  memcpy(nes.bg_pal, bg, sizeof(bg));
  memcpy(nes.spr_pal, sp, sizeof(sp));
}

/* ---------------------------------------------------------------- */
/* 2A03                                                              */
/* ---------------------------------------------------------------- */

#define NES_DMC_BYTES 512

typedef struct nes_pulse {
  mc_osc_t osc;
  int32_t hz_q8;
  int duty;   /* 0..3 */
  int vol;    /* 0..15, the envelope's current level */
  int decay;  /* frames of envelope per step, 0 = hold */
  int decay_acc;
  int sweep;  /* signed hz_q8 added per half-frame; the sweep unit */
  int gate;
} nes_pulse_t;

static struct {
  int rate;
  long frame;      /* absolute mixer frame the chip has advanced to */
  long seq_acc;    /* frames until the next 240 Hz frame-sequencer tick */
  long seq_period;
  int seq_step;    /* 0..3 */

  long row_acc;    /* frames until the next pattern row */
  long row_period;
  int row;

  nes_pulse_t p1, p2;

  mc_osc_t tri;
  int tri_gate;
  int tri_hold; /* the linear counter: the triangle's only gate */

  mc_noise_t noise;
  int noise_vol;
  int noise_decay_acc;
  int noise_short;

  uint8_t dmc[NES_DMC_BYTES]; /* the delta bitstream */
  long dmc_bit;               /* current bit index, -1 when idle */
  uint32_t dmc_step; /* 16.16 bit-clock ticks per output frame */
  uint32_t dmc_acc;
  int dmc_level; /* 7-bit counter, 0..127 */
} apu;

/* Bake a DPCM bitstream. The 2A03 has no sample RAM: it reads one bit at a
   time and moves a 7-bit counter up or down by two. Encoding a target
   waveform into that stream is therefore the whole codec, and the
   characteristic DPCM grit is the counter failing to keep up with the slope.
   A kick drum is chosen because a slow-moving low sine is exactly the signal
   this scheme can almost represent. */
static void nes_bake_dmc(void) {
  int i;
  int level = 64;
  int32_t amp = 60;
  uint32_t phase = 0u;
  uint32_t step = (uint32_t)(((uint64_t)60 << 32) / 4200u); /* 60 Hz at 4.2 kHz */
  memset(apu.dmc, 0, sizeof(apu.dmc));
  for (i = 0; i < NES_DMC_BYTES * 8; ++i) {
    int target;
    int bit;
    target = 64 + ((mc_wave_sine(phase) * amp) >> 15);
    phase += step;
    step = step - (step >> 9); /* pitch drops as it decays, as a kick does */
    if (amp > 0)
      amp -= 1;
    bit = (target > level) ? 1 : 0;
    level += bit ? 2 : -2;
    if (level < 0)
      level = 0;
    if (level > 127)
      level = 127;
    if (bit)
      apu.dmc[i >> 3] |= (uint8_t)(1u << (i & 7));
  }
}

/* Music. Two pulse channels carrying a lead and a counter-melody, triangle on
   bass, noise on percussion, DPCM on the kick. 32 rows, four patterns, the
   shape any NES driver used. 0 means "no event", so rests are cheap. */
#define R 0
static const uint8_t nes_lead[4][32] = {
    {76, 0, 0, 0, 79, 0, 76, 0, 72, 0, 0, 0, 74, 0, 0, 0,
     76, 0, 0, 0, 81, 0, 79, 0, 76, 0, 0, 0, 0,  0, 0, 0},
    {74, 0, 0, 0, 77, 0, 74, 0, 71, 0, 0, 0, 72, 0, 0, 0,
     74, 0, 0, 0, 79, 0, 77, 0, 74, 0, 0, 0, 0,  0, 0, 0},
    {76, 0, 79, 0, 84, 0, 83, 0, 81, 0, 79, 0, 76, 0, 74, 0,
     72, 0, 74, 0, 76, 0, 0,  0, 0,  0, 0,  0, 0,  0, 0,  0},
    {81, 0, 0, 0, 79, 0, 0, 0, 76, 0, 0, 0, 72, 0, 0, 0,
     69, 0, 0, 0, 72, 0, 0, 0, 76, 0, 0, 0, 79, 0, 0, 0}};
static const uint8_t nes_harm[4][32] = {
    {64, 0, 0, 0, 67, 0, 64, 0, 60, 0, 0, 0, 62, 0, 0, 0,
     64, 0, 0, 0, 69, 0, 67, 0, 64, 0, 0, 0, 0,  0, 0, 0},
    {62, 0, 0, 0, 65, 0, 62, 0, 59, 0, 0, 0, 60, 0, 0, 0,
     62, 0, 0, 0, 67, 0, 65, 0, 62, 0, 0, 0, 0,  0, 0, 0},
    {R, 0, 0, 0, 72, 0, 0, 0, 71, 0, 0, 0, 69, 0, 0, 0,
     R, 0, 0, 0, 67, 0, 0, 0, 64, 0, 0, 0, 0,  0, 0, 0},
    {69, 0, 0, 0, 67, 0, 0, 0, 64, 0, 0, 0, 60, 0, 0, 0,
     57, 0, 0, 0, 60, 0, 0, 0, 64, 0, 0, 0, 67, 0, 0, 0}};
static const uint8_t nes_bass[4][32] = {
    {40, 0, 0, 0, 40, 0, 47, 0, 45, 0, 0, 0, 45, 0, 52, 0,
     40, 0, 0, 0, 40, 0, 47, 0, 45, 0, 0, 0, 45, 0, 0,  0},
    {38, 0, 0, 0, 38, 0, 45, 0, 43, 0, 0, 0, 43, 0, 50, 0,
     38, 0, 0, 0, 38, 0, 45, 0, 43, 0, 0, 0, 43, 0, 0,  0},
    {36, 0, 0, 0, 43, 0, 36, 0, 41, 0, 0, 0, 48, 0, 41, 0,
     40, 0, 0, 0, 47, 0, 40, 0, 45, 0, 0, 0, 0,  0, 0,  0},
    {45, 0, 45, 0, 43, 0, 43, 0, 40, 0, 40, 0, 36, 0, 36, 0,
     33, 0, 33, 0, 36, 0, 36, 0, 40, 0, 40, 0, 43, 0, 43, 0}};
/* 1 = hat (short/tonal noise), 2 = snare (long noise), 3 = kick (DPCM) */
static const uint8_t nes_drum[32] = {3, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1,
                                     0, 2, 0, 1, 1, 3, 0, 1, 0, 2, 0,
                                     1, 0, 3, 3, 1, 0, 2, 0, 1, 1};
#undef R

static void nes_pulse_note(nes_pulse_t *p, int note, int duty, int vol,
                           int decay, int sweep) {
  p->hz_q8 = ((int32_t)(snd_note_hz(note)) << 8);
  p->duty = duty;
  p->vol = vol;
  p->decay = decay;
  p->decay_acc = 0;
  p->sweep = sweep;
  p->gate = 1;
}

static void nes_row_advance(void) {
  int pat = (apu.row >> 5) & 3;
  int r = apu.row & 31;
  int n;

  n = nes_lead[pat][r];
  if (n)
    nes_pulse_note(&apu.p1, n, (r & 8) ? 2 : 1, 12, 3,
                   (pat == 2 && (r & 15) == 0) ? 40 : 0);
  n = nes_harm[pat][r];
  if (n)
    nes_pulse_note(&apu.p2, n, 0, 8, 4, 0);
  n = nes_bass[pat][r];
  if (n) {
    mc_osc_set_hz(&apu.tri, apu.rate, ((int32_t)(snd_note_hz(n)) << 8));
    apu.tri_gate = 1;
    apu.tri_hold = 14; /* linear counter, in 240 Hz ticks */
  }

  switch (nes_drum[r]) {
  case 1:
    apu.noise_short = 1;
    apu.noise.kind = MC_LFSR_NES_SHORT;
    mc_noise_set_hz(&apu.noise, apu.rate, ((int32_t)(9000) << 8));
    apu.noise_vol = 5;
    break;
  case 2:
    apu.noise_short = 0;
    apu.noise.kind = MC_LFSR_NES_LONG;
    mc_noise_set_hz(&apu.noise, apu.rate, ((int32_t)(14000) << 8));
    apu.noise_vol = 11;
    break;
  case 3:
    apu.dmc_bit = 0;
    apu.dmc_level = 64;
    break;
  default:
    break;
  }

  ++apu.row;
  if (apu.row >= 128)
    apu.row = 0;
}

/* The frame sequencer. 240 Hz, four steps. Quarter frames clock the volume
   envelopes; half frames (steps 1 and 3) clock the sweep unit and the
   triangle's linear counter. Everything the chip does over time happens on
   this grid and nowhere else, which is why NES envelopes have that stepped
   quality no matter how the music is written. */
static void nes_frame_tick(void) {
  nes_pulse_t *p;
  int i;
  int half = (apu.seq_step == 1 || apu.seq_step == 3);

  for (i = 0; i < 2; ++i) {
    p = i ? &apu.p2 : &apu.p1;
    if (p->decay > 0 && p->vol > 0) {
      if (++p->decay_acc >= p->decay) {
        p->decay_acc = 0;
        --p->vol;
      }
    }
    if (half && p->sweep) {
      p->hz_q8 += p->sweep;
      if (p->hz_q8 > ((int32_t)(4000) << 8))
        p->hz_q8 = ((int32_t)(4000) << 8);
      if (p->hz_q8 < ((int32_t)(20) << 8))
        p->gate = 0;
    }
    mc_osc_set_hz(&p->osc, apu.rate, p->hz_q8);
  }

  if (apu.noise_vol > 0 && ++apu.noise_decay_acc >= 2) {
    apu.noise_decay_acc = 0;
    --apu.noise_vol;
  }

  if (half && apu.tri_hold > 0) {
    apu.tri_hold -= 2;
    if (apu.tri_hold <= 0)
      apu.tri_gate = 0;
  }

  apu.seq_step = (apu.seq_step + 1) & 3;
}

static void nes_apu_init(const snd_mixer_t *m, long start_frame) {
  memset(&apu, 0, sizeof(apu));
  apu.rate = m ? m->rate : SND_DEFAULT_RATE;
  apu.frame = start_frame;
  apu.seq_period = mc_frames_per_tick(m, 240);
  apu.row_period = mc_frames_per_tick(m, 60) * 5; /* speed 5, ~12 rows/s */
  apu.seq_acc = 0;
  apu.row_acc = 0;
  apu.dmc_bit = -1;
  apu.dmc_level = 64;
  mc_noise_init(&apu.noise, MC_LFSR_NES_LONG, 0x4001u);
  mc_noise_set_hz(&apu.noise, apu.rate, ((int32_t)(14000) << 8));
  /* The DPCM bit clock is its own counter, not an oscillator phase: it wants
     "how many bits have elapsed", which is a 16.16 accumulator. */
  apu.dmc_step = (uint32_t)(((int64_t)4200 << 16) / (int64_t)apu.rate);
  nes_bake_dmc();
}

static const uint16_t nes_duty_table[4] = {8192u, 16384u, 32768u, 49152u};

static void nes_mix(snd_mixer_t *m, void *user) {
  int i;
  int frames;
  (void)user;
  if (!m)
    return;
  frames = m->block_frames;
  if (frames <= 0)
    return;
  snd_touch_block(m);

  for (i = 0; i < frames; ++i) {
    int32_t acc = 0;
    int32_t v;

    if (--apu.seq_acc <= 0) {
      apu.seq_acc = apu.seq_period;
      nes_frame_tick();
    }
    if (--apu.row_acc <= 0) {
      apu.row_acc = apu.row_period;
      nes_row_advance();
    }

    /* Two pulses. Volume is 4-bit, so the loudest a channel gets is 15/15 and
       the quietest audible step is 1/15 -- a coarse ladder that the frame
       sequencer walks down. */
    if (apu.p1.gate) {
      MC_OSC_ADVANCE(&apu.p1.osc);
      v = mc_wave_pulse(apu.p1.osc.phase, nes_duty_table[apu.p1.duty]);
      acc += (v * apu.p1.vol) / 15 / 6;
    }
    if (apu.p2.gate) {
      MC_OSC_ADVANCE(&apu.p2.osc);
      v = mc_wave_pulse(apu.p2.osc.phase, nes_duty_table[apu.p2.duty]);
      acc += (v * apu.p2.vol) / 15 / 6;
    }

    /* Triangle: 16 steps, and no volume register at all. It is either on at
       full scale or off, which is why NES basslines never swell. */
    if (apu.tri_gate) {
      MC_OSC_ADVANCE(&apu.tri);
      acc += mc_wave_tri_steps(apu.tri.phase, 16) / 5;
    }

    if (apu.noise_vol > 0)
      acc += (mc_noise_next(&apu.noise) * apu.noise_vol) / 15 / 7;
    else
      (void)mc_noise_next(&apu.noise);

    /* DPCM. One bit per clock moves the 7-bit counter by two; the counter is
       the output. Nothing filters it, which is the entire character. */
    if (apu.dmc_bit >= 0) {
      apu.dmc_acc += apu.dmc_step;
      while (apu.dmc_acc >= 0x10000u) {
        long byte_index;
        apu.dmc_acc -= 0x10000u;
        byte_index = apu.dmc_bit >> 3;
        if (byte_index >= NES_DMC_BYTES) {
          apu.dmc_bit = -1;
          break;
        }
        if ((apu.dmc[byte_index] >> (apu.dmc_bit & 7)) & 1u)
          apu.dmc_level += 2;
        else
          apu.dmc_level -= 2;
        if (apu.dmc_level < 0)
          apu.dmc_level = 0;
        if (apu.dmc_level > 127)
          apu.dmc_level = 127;
        ++apu.dmc_bit;
      }
      if (apu.dmc_bit >= 0)
        acc += (apu.dmc_level - 64) * 300;
    }

    mc_out(m, i, snd_clip_sample(acc), 128);
  }
  apu.frame += frames;
}

static void nes_sfx(const snd_mixer_t *m, long at_frame) {
  (void)m;
  (void)at_frame;
  /* A rising sweep on pulse 1: the sound the sweep unit exists to make. */
  nes_pulse_note(&apu.p1, 72, 0, 15, 0, 900);
}

/* ---------------------------------------------------------------- */
/* simulation                                                        */
/* ---------------------------------------------------------------- */

static void nes_evaluate_oam(void);

static void nes_init(int screen_w, int screen_h, const snd_mixer_t *m,
                     long start_frame) {
  int i;
  uint32_t rng = 0xC0FFEEu;
  memset(&nes, 0, sizeof(nes));
  nes.w = screen_w;
  nes.h = screen_h;
  nes.show_debug = 1;
  nes_build_patterns();
  nes_build_map();
  nes_build_palettes();

  for (i = 0; i < NES_SPRITES; ++i) {
    nes.sprites[i].x = (int)(mc_rand(&rng) % 312u);
    /* Deliberately packed into a 24-pixel band. Eight-pixel-tall sprites in a
       band that shallow put well over eight of them on the same scanline,
       which is the only way to see what the limit does. A game that never
       exceeds it never flickers. */
    nes.sprites[i].y = 104 + (int)(mc_rand(&rng) % 24u);
    nes.sprites[i].tile = ((mc_rand(&rng) & 1u) != 0u) ? 12 : 15;
    nes.sprites[i].pal = (int)(mc_rand(&rng) & 3u);
    nes.sprites[i].vx = ((int)(mc_rand(&rng) % 3u)) - 1;
    nes.sprites[i].vy = ((mc_rand(&rng) & 1u) != 0u) ? 1 : -1;
  }
  nes_evaluate_oam();
  nes_apu_init(m, start_frame);
}

static void nes_tick(const mr_demo_input_t *input) {
  int i;
  ++nes.frame;
  nes.scroll_main += 1;
  nes.scroll_water += 2;
  nes.wave_phase = (nes.wave_phase + 7) & 1023;
  nes.oam_rotate = (nes.oam_rotate + 1) % NES_SPRITES;

  if (input) {
    if (input->dx)
      nes.scroll_main += input->dx * 2;
    if ((input->buttons & MR_DEMO_INPUT_DEBUG) != 0u)
      nes.show_debug = !nes.show_debug;
  }

  for (i = 0; i < NES_SPRITES; ++i) {
    nes_sprite_t *s = &nes.sprites[i];
    s->x += s->vx;
    s->y += s->vy;
    if (s->y < 100 || s->y > 132)
      s->vy = -s->vy;
    if (s->x < -8)
      s->x += 336;
    if (s->x > 328)
      s->x -= 336;
  }

  nes_evaluate_oam();
}

/* Sprite evaluation, once per frame, the way the PPU does it: walk OAM in
   order for each scanline and stop after eight entries. The ninth sprite on a
   line is simply not fetched.

   The starting index rotates every frame, so the sprite that loses its slot is
   a different one each time. That is the whole flicker technique: it does not
   raise the limit, it spreads the loss around until absence reads as motion
   rather than as a missing object. */
static void nes_evaluate_oam(void) {
  int y;
  nes.dropped_last = 0;
  for (y = 0; y < MC_EX_H; ++y) {
    int n = 0;
    int k;
    if (y >= NES_BAR_H) {
      for (k = 0; k < NES_SPRITES; ++k) {
        int i = (k + nes.oam_rotate) % NES_SPRITES;
        int fy = y - nes.sprites[i].y;
        if (fy < 0 || fy >= 8)
          continue;
        if (n >= NES_MAX_PER_LINE) {
          ++nes.dropped_last;
          continue;
        }
        nes.line_list[y][n++] = (uint8_t)i;
      }
    }
    nes.line_n[y] = (uint8_t)n;
  }
}

/* ---------------------------------------------------------------- */
/* render                                                            */
/* ---------------------------------------------------------------- */

/* One scroll value per scanline. This function is the raster split: every
   value it returns is one write to $2005 that a real game would perform in
   hblank, and the three regions below are the three writes a status-bar game
   plus an MMC3 water effect would actually issue. */
static int nes_scroll_for_line(int y) {
  if (y < NES_BAR_H)
    return 0; /* the sprite-0 split: the bar does not move */
  if (y < NES_WATER_Y)
    return nes.scroll_main;
  /* Below the second split, faster scroll plus a per-8-line offset. Eight
     lines because that is about how often a cartridge IRQ could realistically
     be re-armed and serviced without eating the frame. */
  return nes.scroll_water +
         ((mc_sin(nes.wave_phase + ((y >> 3) << 6)) * 10) >> 15);
}

static void nes_render_background(gfx_renderer_t *r, int y0, int y1) {
  int y;
  for (y = y0; y < y1; ++y) {
    int x0, tw, x;
    gfx_color_t *row = mc_row(r, y, &x0, &tw);
    int scroll = nes_scroll_for_line(y);
    int world_y = (y < NES_BAR_H) ? y : y;
    int ty = (world_y >> 3) % NES_NT_H;
    int fy = world_y & 7;
    int arow = (world_y >> 4);
    if (!row)
      continue;
    if (arow >= NES_NT_H / 2 + 1)
      arow = NES_NT_H / 2;

    if (y < NES_BAR_H) {
      /* The status bar is a flat backdrop colour, which is what the universal
         palette entry 0 is for. */
      int i;
      for (i = 0; i < tw; ++i)
        row[i] = nes_master[0x0F];
      continue;
    }

    for (x = 0; x < tw; ++x) {
      int wx = (x0 + x + scroll) & (NES_NT_W * 8 - 1);
      int tx = wx >> 3;
      int fx = wx & 7;
      int tile = nes.nametable[ty * NES_NT_W + tx];
      int pal = nes.attrib[arow * (NES_NT_W / 2) + (tx >> 1)];
      int idx = nes.pattern[tile][fy * 8 + fx];
      row[x] = nes_master[nes.bg_pal[pal][idx]];
    }
  }
}

/* Draw the eight entries secondary OAM already holds for each line. Purely a
   consumer: no evaluation, no counting, no state that outlives the row. */
static void nes_render_sprites(gfx_renderer_t *r, int y0, int y1) {
  int y;
  for (y = y0; y < y1; ++y) {
    int x0, tw;
    gfx_color_t *row = mc_row(r, y, &x0, &tw);
    int k;
    if (!row)
      continue;
    for (k = 0; k < (int)nes.line_n[y]; ++k) {
      const nes_sprite_t *s = &nes.sprites[nes.line_list[y][k]];
      int fy = y - s->y;
      int x;
      for (x = 0; x < 8; ++x) {
        int sx = s->x + x;
        int idx;
        if (sx < x0 || sx >= x0 + tw)
          continue;
        idx = nes.pattern[s->tile][fy * 8 + x];
        if (idx == 0)
          continue; /* entry 0 of a sprite palette is transparent */
        row[sx - x0] = nes_master[nes.spr_pal[s->pal][idx]];
      }
    }
  }
}

static void nes_render(gfx_renderer_t *r) {
  int y0, y1;
  char buf[48];
  if (!r)
    return;
  mc_rows(r, &y0, &y1);
  nes_render_background(r, y0, y1);
  nes_render_sprites(r, y0, y1);

  /* The status bar contents. Drawn after the split so it is unmistakably on
     the non-scrolling side of it. */
  gfx_draw_text5x7(r, 8, 6, "MARIO", nes_master[0x30], 1);
  gfx_draw_text5x7(r, 8, 16, "000450", nes_master[0x30], 1);
  gfx_draw_text5x7(r, 96, 16, "WORLD 1-1", nes_master[0x30], 1);
  gfx_draw_text5x7(r, 200, 16, "TIME 300", nes_master[0x30], 1);
  gfx_draw_text5x7(r, 8, 26, "SPLIT 1: SPRITE 0 HIT", nes_master[0x27], 1);

  /* The second split is marked so the boundary is visible rather than merely
     effective. */
  gfx_draw_hline(r, 0, NES_WATER_Y - 1, MC_EX_W, nes_master[0x27]);
  gfx_draw_text5x7(r, 8, NES_WATER_Y + 4, "SPLIT 2: MAPPER IRQ", nes_master[0x30],
                   1);

  if (nes.show_debug) {
    char *p = buf;
    const char *end = buf + sizeof(buf) - 1;
    p = mr_strbuf_str(p, end, "OAM DROP ");
    p = mr_strbuf_u32(p, end, (unsigned long)nes.dropped_last);
    p = mr_strbuf_str(p, end, "  LIMIT 8/LINE");
    *p = '\0';
    mc_text_shadow(r, 8, 44, buf, nes_master[0x36], 1);

    /* Draw the attribute grid over the ground so the 16-pixel colour cells are
       not merely inferable from the seams. */
    {
      int gx;
      for (gx = 0; gx < MC_EX_W; gx += 16)
        gfx_draw_vline(r, gx, 152, NES_WATER_Y - 152, nes_master[0x00]);
      gfx_draw_hline(r, 0, 160, MC_EX_W, nes_master[0x00]);
      mc_text_shadow(r, 8, 140, "ATTRIBUTE CELLS 16x16", nes_master[0x30], 1);
    }
  }
}

/* ---------------------------------------------------------------- */

static const char *const nes_notes[] = {
    "One horizontal scroll register, rewritten between scanlines.",
    "Split 1 is sprite-0 hit; split 2 is a cartridge IRQ.",
    "Colour is one 4-entry subpalette per 16x16 block: attribute clash.",
    "Only 8 sprites per scanline fit; the rest flicker.",
    "2A03: duty morph, hardware sweep, 4-bit triangle, DPCM kick.",
    0};

static const mc_example_t nes_example = {
    "nes-split",
    "Nintendo Famicom / NES (1983), 2C02 PPU + 2A03 APU",
    "mid-frame raster splits, 16x16 attribute clash, 8-sprite line limit",
    "240 Hz frame sequencer, sweep unit, 16-step triangle, 1-bit DPCM",
    nes_notes,
    nes_init,
    nes_tick,
    nes_render,
    nes_mix,
    nes_sfx};

const mc_example_t *mc_example_nes_split(void) { return &nes_example; }
