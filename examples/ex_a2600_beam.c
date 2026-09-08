/* Atari VCS / 2600, 1977 -- TIA
 *
 * Display: there is no display mode, because there is no display.
 *
 *   The TIA has no frame buffer and the machine has 128 bytes of RAM, which is
 *   not enough to hold one scanline of anything. What it has instead is a
 *   handful of registers that describe what the beam should be painting *right
 *   now*, and a program that rewrites them faster than the beam moves. The
 *   picture exists only while it is being drawn.
 *
 *   Every later machine in this set is a reaction to that. The NES added a
 *   nametable so the CPU would stop having to be there for every line; the
 *   Mega Drive added a scroll table so it would stop having to be there for
 *   every line *again*. The 2600 is where the idea that a program can change
 *   its mind between scanlines starts, and it is the reason none of these
 *   examples need MicroRender to know what a mode is.
 *
 *   What the registers actually offer:
 *
 *   Playfield. Twenty bits -- PF0's top nibble, then PF1 and PF2 as bytes --
 *   each covering four colour clocks. That is half a scanline at 8-screen-pixel
 *   granularity, and CTRLPF's reflect bit decides whether the right half is a
 *   copy or a mirror. Twenty bits is the entire horizontal resolution of the
 *   background, which is why 2600 landscapes are chunky and symmetrical.
 *
 *   Players. Two 8-bit sprites, each one bit per colour clock, stretchable to
 *   double and quadruple width, and NUSIZ can ask for two or three copies at
 *   fixed spacings. Rewriting a player's graphics register between lines is
 *   how a 2600 draws anything taller than eight pixels.
 *
 *   Horizontal position. There is no X register. A sprite is positioned by
 *   strobing RESP0 at the moment the beam passes the wanted column, which
 *   quantizes to fifteen colour clocks; HMOVE then nudges it by up to seven
 *   more. HMOVE also blanks the leftmost eight clocks of the line it runs on.
 *   That black notch down the left edge -- the HMOVE comb -- is on screen here
 *   because it was on screen in every game that moved anything.
 *
 *   Score mode. CTRLPF's score bit paints the left half of the playfield in
 *   player 0's colour and the right half in player 1's. It exists so that the
 *   two-digit score at the bottom of the screen can be drawn with playfield
 *   bits instead of spending sprites on it.
 *
 * Audio: two channels, five bits of pitch, and polynomial counters.
 *
 *   AUDF is a 5-bit divider off a 31.4 kHz clock, so a channel can produce
 *   exactly 32 pitches per waveform and nothing in between. Those pitches do
 *   not line up with equal temperament anywhere, and the error is large: a
 *   third of a semitone is a good outcome and a whole semitone is common. 2600
 *   composers did not tune, they chose which wrongness to live with.
 *
 *   That is the trick this example makes audible. The note table is built by
 *   searching all 32 dividers for the closest match to each wanted pitch, and
 *   the cents error of whatever it settled for is printed on screen while it
 *   plays.
 *
 *   AUDC selects the waveform, and the interesting settings are not tones at
 *   all: they are linear feedback shift registers of 4, 5 and 9 bits, used
 *   alone or clocking each other. The 4-bit poly is a 15-step buzz that reads
 *   as a rude square; the 9-bit poly is white noise; a 5-bit poly gating a
 *   div-6 tone is the engine rumble every driving game on the machine used.
 */

#include "mc_ex_chip.h"
#include "mc_ex_gfx.h"
#include "mc_example.h"
#include "mr_strbuf.h"

#include <string.h>

/* ---------------------------------------------------------------- */
/* the NTSC TIA palette: 16 hues x 8 luminances                       */
/* ---------------------------------------------------------------- */

/* Generated rather than tabulated. The TIA's colour burst phase-shifts by hue
   and the luminance ladder is linear, so a hue/luma pair reconstructs closely
   enough at RGB565 -- and 128 hand-entered constants would be 128 chances to
   mistype one. */
static gfx_color_t a26_pal[128];

static void a26_build_palette(void) {
  int hue, luma;
  for (hue = 0; hue < 16; ++hue) {
    for (luma = 0; luma < 8; ++luma) {
      int y = 24 + luma * 30; /* luminance ladder, 24..234 */
      int r, g, b;
      if (hue == 0) {
        r = g = b = y; /* hue 0 is greyscale on real hardware */
      } else {
        /* Chroma as a phase on the colour wheel, amplitude fixed by the
           encoder rather than by the luminance. */
        int angle = ((hue - 1) * 1024) / 15 + 100;
        int cr = (mc_cos(angle) * 68) >> 15;
        int cb = (mc_sin(angle) * 68) >> 15;
        r = y + cr + (cr >> 2);
        g = y - (cr >> 1) - (cb >> 2);
        b = y + cb + (cb >> 1);
      }
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
      a26_pal[hue * 8 + luma] = GFX_RGB565(r, g, b);
    }
  }
}

/* ---------------------------------------------------------------- */
/* TIA-side state                                                    */
/* ---------------------------------------------------------------- */

#define A26_TOP 24         /* first visible line; above it is vertical blank */
#define A26_LINES 192      /* what an NTSC 2600 kernel actually draws        */
#define A26_CLOCKS 160     /* colour clocks per visible line                 */
#define A26_PX 2           /* screen pixels per colour clock (160 * 2 = 320) */
#define A26_SCORE_TOP 168  /* where the score kernel takes over              */

typedef struct a26_line {
  /* Two sets of playfield bits, because the register set only has one.
     The playfield is latched twice per line -- once for each half -- so a
     program that rewrites PF0/PF1/PF2 in the gap between the two latches gets
     an asymmetric line out of symmetric hardware. It has about 20 machine
     cycles to do it in. Nearly every scoreboard on the system is this trick. */
  uint32_t pf_l;
  uint32_t pf_r;
  uint8_t bg;       /* COLUBK */
  uint8_t fg;       /* COLUPF */
  uint8_t reflect;  /* CTRLPF D0 */
  uint8_t score;    /* CTRLPF D1 */
  uint8_t hmove;    /* did this line strobe HMOVE?  */
  uint8_t p0gfx;    /* GRP0 for this line */
  uint8_t p1gfx;    /* GRP1 */
  uint8_t p0col;
  uint8_t p1col;
} a26_line_t;

static struct {
  unsigned long frame;
  int scroll;      /* the kernel's own idea of where the world is */
  int p0x, p1x;    /* resolved player positions, in colour clocks */
  int p0v, p1v;
  int p0y, p1y;
  int nusiz0;      /* 0 = one copy, 1 = two copies, 2 = three copies */
  int show_notes;
  a26_line_t line[MC_EX_H];
} vcs;

/* An 8x16 player bitmap, rewritten into GRP0 one row per scanline. Eight bits
   wide is not a stylistic choice on this machine, it is the width of the
   register. */
static const uint8_t a26_player[16] = {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x7E,
                                       0xFF, 0xFF, 0xDB, 0x99, 0x18, 0x3C,
                                       0x24, 0x24, 0x66, 0x42};
static const uint8_t a26_alien[16] = {0x00, 0x24, 0x18, 0x7E, 0xDB, 0xFF,
                                      0xFF, 0xA5, 0x24, 0x42, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00};

/* Two-digit score glyphs as playfield bits: four rows of five columns, which
   is all the 20-bit playfield can spare and all any 2600 game used. */
static const uint8_t a26_digit[10][5] = {
    {0x1F, 0x11, 0x11, 0x11, 0x1F}, {0x04, 0x0C, 0x04, 0x04, 0x0E},
    {0x1F, 0x01, 0x1F, 0x10, 0x1F}, {0x1F, 0x01, 0x0F, 0x01, 0x1F},
    {0x11, 0x11, 0x1F, 0x01, 0x01}, {0x1F, 0x10, 0x1F, 0x01, 0x1F},
    {0x1F, 0x10, 0x1F, 0x11, 0x1F}, {0x1F, 0x01, 0x02, 0x04, 0x04},
    {0x1F, 0x11, 0x1F, 0x11, 0x1F}, {0x1F, 0x11, 0x1F, 0x01, 0x1F}};

/* Build the whole frame's worth of register writes in one pass. On the real
   machine this loop *is* the program: the 6507 sits in it for the entire
   frame, and every iteration has to finish inside 76 cycles or the picture
   tears. Doing it in tick() rather than render() keeps the result identical
   whether the frontend hands us one tall tile or fifteen short ones. */
static void a26_build_kernel(void) {
  int y;
  for (y = 0; y < MC_EX_H; ++y) {
    a26_line_t *L = &vcs.line[y];
    int k = y - A26_TOP;
    memset(L, 0, sizeof(*L));
    if (k < 0 || k >= A26_LINES)
      continue;

    if (y >= A26_SCORE_TOP) {
      /* Score kernel. CTRLPF's score bit means the two halves take the player
         colours, so a symmetrical playfield reads as two independently
         coloured digits without spending a sprite. */
      int row = (y - A26_SCORE_TOP) / 4;
      int score = 1042 + (int)(vcs.frame / 30ul);
      L->bg = (uint8_t)(0 * 8 + 0);
      L->fg = (uint8_t)(0 * 8 + 7);
      L->score = 1;
      L->reflect = 0;
      L->p0col = (uint8_t)(4 * 8 + 6);
      L->p1col = (uint8_t)(12 * 8 + 6);
      if (row < 5) {
        /* Four digits: two latched into the left half, two written over the
           top for the right half. Each digit is five playfield bits, and a
           playfield bit is four colour clocks, so a score pixel is eight
           screen pixels wide -- which is why 2600 scores are chunkier than
           2600 sprites even though they cost less. */
        uint32_t d0 = a26_digit[(score / 1000) % 10][row];
        uint32_t d1 = a26_digit[(score / 100) % 10][row];
        uint32_t d2 = a26_digit[(score / 10) % 10][row];
        uint32_t d3 = a26_digit[score % 10][row];
        L->pf_l = ((d0 & 0x1Fu) << 14) | ((d1 & 0x1Fu) << 7);
        L->pf_r = ((d2 & 0x1Fu) << 14) | ((d3 & 0x1Fu) << 7);
      }
      continue;
    }

    /* The playfield terrain. Twenty bits, mirrored, so the cave is forced to
       be symmetrical: not a design decision, a register width. */
    {
      int depth = 3 + ((mc_sin((k * 5 + vcs.scroll * 3) & 1023) * 4) >> 15);
      int i;
      uint32_t bits = 0u;
      for (i = 0; i < 10; ++i)
        if (i < depth)
          bits |= 1u << i;
      /* A ledge that walks with the scroll, drawn into the middle bits. */
      if (((k + vcs.scroll) & 63) < 6)
        bits |= 0x0300u;
      L->pf_l = bits;
      L->pf_r = bits;
      L->reflect = 1;
    }

    /* One COLUBK write per scanline is all a rainbow costs, and it is the
       cheapest visual effect the machine has. Nothing else on the 2600 is
       free; this is. */
    L->bg = (uint8_t)(((((k + (int)vcs.frame) >> 3) & 15) * 8) + 2);
    L->fg = (uint8_t)((9 * 8) + 4 + (((k >> 4) & 1)));

    /* GRP0/GRP1 are rewritten every line. A player taller than eight pixels
       does not exist; a stack of eight-pixel writes does. */
    {
      int r0 = y - vcs.p0y;
      int r1 = y - vcs.p1y;
      L->p0gfx = (r0 >= 0 && r0 < 16) ? a26_player[r0] : 0u;
      L->p1gfx = (r1 >= 0 && r1 < 16) ? a26_alien[r1] : 0u;
      L->p0col = (uint8_t)(4 * 8 + 6);
      L->p1col = (uint8_t)(12 * 8 + 5);
    }

    /* HMOVE is strobed on the lines where a sprite actually needs its fine
       nudge. It costs the leftmost eight colour clocks of that line. */
    L->hmove = (uint8_t)(((k & 1) == 0) ? 1 : 0);
  }
}

/* ---------------------------------------------------------------- */
/* TIA audio                                                         */
/* ---------------------------------------------------------------- */

#define A26_AUDIO_CLOCK 31400 /* 3.579545 MHz / 114, near enough */

/* AUDC waveform selectors, using the settings that matter. */
#define A26_TONE_PURE 4    /* divide by 2: the closest thing to a square    */
#define A26_TONE_POLY4 1   /* 4-bit LFSR: a 15-step buzz                    */
#define A26_TONE_POLY9 8   /* 9-bit LFSR: white noise                       */
#define A26_TONE_DIV6 12   /* divide by 6: the low pure tone                */
#define A26_TONE_ENGINE 15 /* 5-bit poly clocking div 6: the rumble         */
#define A26_TONE_DIV31 6   /* divide by 31: sub-bass                        */

typedef struct a26_chan {
  int audf;  /* 0..31 */
  int audc;  /* waveform selector */
  int audv;  /* 0..15 */
  uint32_t acc;
  uint32_t step;   /* 16.16 divider ticks per output frame */
  uint32_t poly4;
  uint32_t poly5;
  uint32_t poly9;
  int div_count;
  int out;
} a26_chan_t;

static struct {
  int rate;
  long row_acc;
  long row_period;
  int row;
  a26_chan_t ch[2];
  int cents_error; /* of the last note channel 0 was asked to play */
  int last_note;
} tia;

/* The channel's divider ticks at 31.4 kHz / (AUDF+1). Everything downstream --
   poly counters and the div-by-6 and div-by-31 stages -- is clocked from that
   and nothing else, which is why the waveform selector changes the pitch as
   well as the timbre. */
static void a26_chan_set(a26_chan_t *c, int audf, int audc, int audv) {
  c->audf = audf & 31;
  c->audc = audc;
  c->audv = audv & 15;
  c->step = (uint32_t)(((int64_t)A26_AUDIO_CLOCK << 16) /
                       ((int64_t)tia.rate * (int64_t)(c->audf + 1)));
}

static void a26_chan_clock(a26_chan_t *c) {
  int bit5;
  /* 5-bit poly, taps 0 and 2. It runs on every channel clock because several
     AUDC settings use it to gate the others. */
  bit5 = (int)((c->poly5 ^ (c->poly5 >> 2)) & 1u);
  c->poly5 = ((c->poly5 >> 1) | ((uint32_t)bit5 << 4)) & 0x1Fu;

  switch (c->audc) {
  case A26_TONE_POLY4: {
    uint32_t b = (c->poly4 ^ (c->poly4 >> 1)) & 1u;
    c->poly4 = ((c->poly4 >> 1) | (b << 3)) & 0x0Fu;
    c->out = (c->poly4 & 1u) ? 1 : -1;
    break;
  }
  case A26_TONE_POLY9: {
    uint32_t b = (c->poly9 ^ (c->poly9 >> 4)) & 1u;
    c->poly9 = ((c->poly9 >> 1) | (b << 8)) & 0x1FFu;
    c->out = (c->poly9 & 1u) ? 1 : -1;
    break;
  }
  case A26_TONE_DIV6:
  case A26_TONE_DIV6 + 1:
    if (++c->div_count >= 3) {
      c->div_count = 0;
      c->out = -c->out;
    }
    break;
  case A26_TONE_DIV31:
    if (++c->div_count >= 15) {
      c->div_count = 0;
      c->out = -c->out;
    }
    break;
  case A26_TONE_ENGINE:
    /* The 5-bit poly gates a divide-by-6. Two irrational-looking periods
       beating against each other is the whole sound of a 2600 engine. */
    if (++c->div_count >= 3) {
      c->div_count = 0;
      if (bit5)
        c->out = -c->out;
    }
    break;
  case A26_TONE_PURE:
  default:
    c->out = -c->out;
    break;
  }
}

static int32_t a26_chan_next(a26_chan_t *c) {
  c->acc += c->step;
  while (c->acc >= 0x10000u) {
    c->acc -= 0x10000u;
    a26_chan_clock(c);
  }
  /* AUDV is 4 bits, linear. Sixteen steps of volume and no envelope
     generator anywhere: any fade on this machine is the program writing AUDV
     once per frame. */
  return ((int32_t)c->out * 32767 / 6) * (int32_t)c->audv / 15;
}

/* Search all 32 dividers for the closest match to a wanted frequency, and
   report how far off it lands. This is what writing music for the machine
   actually consisted of. */
static int a26_pick_audf(int want_hz, int audc, int *out_cents) {
  int divider = (audc == A26_TONE_PURE) ? 2
                : (audc == A26_TONE_DIV6 || audc == A26_TONE_ENGINE) ? 6
                : (audc == A26_TONE_DIV31)                           ? 31
                                                                     : 15;
  int best = 0;
  long best_err = 0x7FFFFFFFL;
  int f;
  int got = 0;
  for (f = 0; f < 32; ++f) {
    int hz = A26_AUDIO_CLOCK / (divider * (f + 1));
    long err = (long)hz - (long)want_hz;
    if (err < 0)
      err = -err;
    /* Compare in proportion, not in hertz: being 5 Hz out matters far more at
       110 Hz than at 1 kHz, and ranking by absolute error would always pick
       the bottom of the range. */
    err = err * 1000L / (want_hz > 0 ? want_hz : 1);
    if (err < best_err) {
      best_err = err;
      best = f;
      got = hz;
    }
  }
  if (out_cents) {
    /* cents ~ 1731 * ln(ratio); near unity ln(1+x) ~ x - x^2/2, which is
       plenty for an error this size and needs no logarithm. */
    long ratio_1000 = ((long)got * 1000L) / (want_hz > 0 ? want_hz : 1) - 1000L;
    *out_cents = (int)((ratio_1000 * 1731L) / 1000L);
  }
  return best;
}

/* Four bars. Deliberately written as MIDI notes and then mangled by the
   divider search, rather than written as divider values that happen to sound
   fine -- the point is to hear what the machine does to a tune. */
static const uint8_t a26_lead[32] = {69, 0,  72, 0,  76, 0,  72, 0,
                                     69, 0,  0,  0,  67, 0,  0,  0,
                                     65, 0,  69, 0,  72, 0,  69, 0,
                                     64, 0,  0,  0,  0,  0,  0,  0};
static const uint8_t a26_bass[32] = {33, 0, 0, 0, 40, 0, 0, 0, 33, 0, 0,
                                     0,  38, 0, 0, 0, 29, 0, 0, 0, 36, 0,
                                     0,  0,  31, 0, 0, 0, 0, 0, 0, 0};

static void a26_audio_init(const snd_mixer_t *m) {
  memset(&tia, 0, sizeof(tia));
  tia.rate = m ? m->rate : SND_DEFAULT_RATE;
  tia.row_period = mc_frames_per_tick(m, 60) * 6;
  tia.ch[0].poly4 = tia.ch[0].poly5 = tia.ch[0].poly9 = 1u;
  tia.ch[1].poly4 = tia.ch[1].poly5 = tia.ch[1].poly9 = 1u;
  tia.ch[0].out = 1;
  tia.ch[1].out = 1;
  a26_chan_set(&tia.ch[0], 15, A26_TONE_PURE, 0);
  a26_chan_set(&tia.ch[1], 20, A26_TONE_ENGINE, 0);
}

static void a26_row_advance(void) {
  int n = a26_lead[tia.row & 31];
  if (n) {
    int cents = 0;
    int audc = ((tia.row & 15) < 8) ? A26_TONE_PURE : A26_TONE_POLY4;
    int audf = a26_pick_audf(snd_note_hz(n), audc, &cents);
    a26_chan_set(&tia.ch[0], audf, audc, 12);
    tia.cents_error = cents;
    tia.last_note = n;
  } else if (tia.ch[0].audv > 0 && (tia.row & 1)) {
    /* The only envelope this machine has: the program writes AUDV again. */
    a26_chan_set(&tia.ch[0], tia.ch[0].audf, tia.ch[0].audc,
                 tia.ch[0].audv - 3);
  }

  n = a26_bass[tia.row & 31];
  if (n) {
    int audc = ((tia.row & 31) < 16) ? A26_TONE_DIV6 : A26_TONE_ENGINE;
    int audf = a26_pick_audf(snd_note_hz(n), audc, 0);
    a26_chan_set(&tia.ch[1], audf, audc, 10);
  }

  ++tia.row;
  if (tia.row >= 32)
    tia.row = 0;
}

static void a26_mix(snd_mixer_t *m, void *user) {
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
    if (--tia.row_acc <= 0) {
      tia.row_acc = tia.row_period;
      a26_row_advance();
    }
    /* Two channels summed with no mixer of any kind. The TIA's outputs are
       shorted together on the way to the modulator, so two loud channels do
       not get quieter -- exactly what this add does. */
    acc = a26_chan_next(&tia.ch[0]) + a26_chan_next(&tia.ch[1]);
    mc_out(m, i, snd_clip_sample(acc), 128);
  }
}

static void a26_sfx(const snd_mixer_t *m, long at_frame) {
  (void)m;
  (void)at_frame;
  /* The other thing the 9-bit poly was for: an explosion. */
  a26_chan_set(&tia.ch[1], 6, A26_TONE_POLY9, 15);
}

/* ---------------------------------------------------------------- */
/* simulation                                                        */
/* ---------------------------------------------------------------- */

static void a26_init(int screen_w, int screen_h, const snd_mixer_t *m,
                     long start_frame) {
  (void)screen_w;
  (void)screen_h;
  (void)start_frame;
  memset(&vcs, 0, sizeof(vcs));
  a26_build_palette();
  vcs.p0x = 40;
  vcs.p1x = 116;
  vcs.p0y = 90;
  vcs.p1y = 60;
  vcs.p0v = 1;
  vcs.p1v = -1;
  vcs.nusiz0 = 2;
  vcs.show_notes = 1;
  a26_build_kernel();
  a26_audio_init(m);
}

static void a26_tick(const mr_demo_input_t *input) {
  ++vcs.frame;
  vcs.scroll = (vcs.scroll + 1) & 1023;

  vcs.p0x += vcs.p0v;
  if (vcs.p0x < 8 || vcs.p0x > 120)
    vcs.p0v = -vcs.p0v;
  vcs.p1x += vcs.p1v;
  if (vcs.p1x < 96 || vcs.p1x > 146)
    vcs.p1v = -vcs.p1v;
  vcs.p0y = 100 + ((mc_sin((int)vcs.frame * 4) * 30) >> 15);
  vcs.p1y = 60 + ((mc_cos((int)vcs.frame * 7) * 24) >> 15);

  if (input) {
    if (input->dx)
      vcs.p0x += input->dx * 2;
    if ((input->buttons & MR_DEMO_INPUT_DEBUG) != 0u)
      vcs.show_notes = !vcs.show_notes;
  }

  a26_build_kernel();
}

/* ---------------------------------------------------------------- */
/* render                                                            */
/* ---------------------------------------------------------------- */

/* Paint one player into the row, honouring NUSIZ copies. Each graphics bit is
   one colour clock wide, and a colour clock is two screen pixels here. */
static void a26_draw_player(gfx_color_t *row, int x0, int tw, uint8_t gfx,
                            int clock_x, gfx_color_t color, int copies,
                            int spacing) {
  int copy, bit;
  if (!gfx)
    return;
  for (copy = 0; copy < copies; ++copy) {
    int base = clock_x + copy * spacing;
    for (bit = 0; bit < 8; ++bit) {
      int px, sx;
      if (((gfx >> (7 - bit)) & 1u) == 0u)
        continue;
      sx = (base + bit) * A26_PX;
      for (px = 0; px < A26_PX; ++px) {
        int s = sx + px;
        if (s >= x0 && s < x0 + tw)
          row[s - x0] = color;
      }
    }
  }
}

static void a26_render(gfx_renderer_t *r) {
  int y, y0, y1;
  if (!r)
    return;
  mc_rows(r, &y0, &y1);

  for (y = y0; y < y1; ++y) {
    int x0, tw, clock;
    const a26_line_t *L = &vcs.line[y];
    gfx_color_t *row = mc_row(r, y, &x0, &tw);
    gfx_color_t bg, fg;
    if (!row)
      continue;

    if (y < A26_TOP || y >= A26_TOP + A26_LINES) {
      int i;
      /* Vertical blank. The 2600 does not have one because the standard says
         so; it has one because the program is busy doing its own arithmetic
         and cannot afford to be painting at the same time. */
      for (i = 0; i < tw; ++i)
        row[i] = GFX_RGB565_BLACK;
      continue;
    }

    bg = a26_pal[L->bg & 127];
    fg = a26_pal[L->fg & 127];

    for (clock = 0; clock < A26_CLOCKS; ++clock) {
      int px;
      gfx_color_t c = bg;
      int half = (clock < 80) ? 0 : 1;
      int slot;
      /* Twenty playfield bits across half a line, four colour clocks each.
         Reflect mirrors the right half; without it the right half repeats the
         same bits, and the difference is why some 2600 backgrounds look like
         caves and others look like wallpaper. */
      if (!half)
        slot = clock >> 2;
      else
        slot = L->reflect ? (19 - ((clock - 80) >> 2)) : ((clock - 80) >> 2);
      if (((half ? L->pf_r : L->pf_l) >> slot) & 1u)
        c = L->score ? a26_pal[(half ? L->p1col : L->p0col) & 127] : fg;

      for (px = 0; px < A26_PX; ++px) {
        int s = clock * A26_PX + px;
        if (s >= x0 && s < x0 + tw)
          row[s - x0] = c;
      }
    }

    if (!L->score) {
      a26_draw_player(row, x0, tw, L->p1gfx, vcs.p1x,
                      a26_pal[L->p1col & 127], 1, 0);
      /* NUSIZ on player 0: three copies at close spacing, which is how one
         sprite register becomes a row of invaders. */
      a26_draw_player(row, x0, tw, L->p0gfx, vcs.p0x,
                      a26_pal[L->p0col & 127],
                      vcs.nusiz0 == 2 ? 3 : (vcs.nusiz0 == 1 ? 2 : 1), 16);
    }

    /* The HMOVE comb. Strobing HMOVE extends the horizontal blank by eight
       colour clocks, so the leftmost sixteen screen pixels of that line are
       simply not painted. Every game that moved a sprite had these black
       notches, and players stopped seeing them. */
    if (L->hmove) {
      int32_t s;
      for (s = 0; s < 8 * A26_PX; ++s)
        if (s >= x0 && s < x0 + tw)
          row[s - x0] = GFX_RGB565_BLACK;
    }
  }

  gfx_draw_text5x7(r, 8, 6, "ATARI 2600 - RACING THE BEAM", a26_pal[0 * 8 + 7],
                   1);
  gfx_draw_text5x7(r, 8, A26_TOP + 4, "PF: 20 BITS, MIRRORED",
                   a26_pal[0 * 8 + 6], 1);
  gfx_draw_text5x7(r, 8, A26_TOP + 14, "HMOVE COMB ->  LEFT EDGE",
                   a26_pal[0 * 8 + 6], 1);

  if (vcs.show_notes) {
    char buf[48];
    char *p = buf;
    const char *end = buf + sizeof(buf) - 1;
    p = mr_strbuf_str(p, end, "AUDF ");
    p = mr_strbuf_u32(p, end, (unsigned long)tia.ch[0].audf);
    p = mr_strbuf_str(p, end, "  OFF BY ");
    p = mr_strbuf_i32(p, end, (long)tia.cents_error);
    p = mr_strbuf_str(p, end, " CENTS");
    *p = '\0';
    mc_text_shadow(r, 8, A26_SCORE_TOP - 26, buf, a26_pal[0 * 8 + 7], 1);
    mc_text_shadow(r, 8, A26_SCORE_TOP - 16, "32 PITCHES PER WAVEFORM. PICK ONE.",
                   a26_pal[0 * 8 + 5], 1);
  }
}

/* ---------------------------------------------------------------- */

static const char *const a26_notes[] = {
    "No frame buffer: every scanline is register writes made in time.",
    "Playfield is 20 bits, mirrored -- the whole background resolution.",
    "GRP0/GRP1 are rewritten per line; NUSIZ gives three copies free.",
    "The black notch at the left edge is the HMOVE comb, not a bug.",
    "TIA: 32 pitches per waveform, so the tune is always slightly wrong.",
    "AUDC 1/8/15 are 4-, 9- and 5-bit polynomial counters, not tones.",
    0};

static const mc_example_t a26_example = {
    "a2600-beam",
    "Atari VCS / 2600 (1977), TIA",
    "no frame buffer: 20-bit mirrored playfield, per-line GRP writes, HMOVE comb",
    "TIA polynomial counters, 5-bit pitch divider, audible detuning in cents",
    a26_notes,
    a26_init,
    a26_tick,
    a26_render,
    a26_mix,
    a26_sfx};

const mc_example_t *mc_example_a2600_beam(void) { return &a26_example; }
