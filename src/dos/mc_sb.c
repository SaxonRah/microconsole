#include "mc_sb.h"
#include "mw_music_demo.h"
#include "snd.h"

#include <conio.h>
#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MC_SB_RATE 11025
#define MC_SB_BLOCK 512
#define MC_SB_DMA_BYTES (MC_SB_BLOCK * 2)

static unsigned g_base = 0x220u;
static unsigned g_irq = 7u;
static unsigned g_dma = 1u;
static unsigned char *g_dma_raw;
static unsigned char *g_dma_buf;
static unsigned long g_dma_phys;
static int g_half_playing;
static long g_frame;
static snd_sample_t g_block[MC_SB_BLOCK];
static snd_mixer_t g_mixer;
static mw_demo_t g_demo;

static unsigned long far_to_phys(void __far *p) {
    return ((unsigned long)FP_SEG(p) << 4) + (unsigned long)FP_OFF(p);
}

static int dma_alloc(void) {
    unsigned long phys, page_end;
    g_dma_raw = (unsigned char *)malloc(MC_SB_DMA_BYTES * 2 + 16);
    if (!g_dma_raw) return 0;
    phys = far_to_phys((void __far *)g_dma_raw);
    page_end = (phys & ~0xFFFFuL) + 0x10000uL;
    if (phys + MC_SB_DMA_BYTES <= page_end) {
        g_dma_buf = g_dma_raw; g_dma_phys = phys;
    } else {
        unsigned long skip = page_end - phys;
        g_dma_buf = g_dma_raw + skip; g_dma_phys = phys + skip;
    }
    if (g_dma_phys >= 0x100000uL) {
        free(g_dma_raw); g_dma_raw = 0; return 0;
    }
    memset(g_dma_buf, 0x80, MC_SB_DMA_BYTES);
    return 1;
}

#define DSP_RESET (g_base + 0x6u)
#define DSP_READ (g_base + 0xAu)
#define DSP_WRITE (g_base + 0xCu)
#define DSP_READ_STATUS (g_base + 0xEu)

static void dsp_write(unsigned char v) {
    int spin = 0;
    while ((inp(DSP_WRITE) & 0x80u) != 0u && ++spin <= 30000) {}
    if (spin <= 30000) outp(DSP_WRITE, v);
}

static int dsp_reset(void) {
    int i, spin;
    outp(DSP_RESET, 1);
    for (i = 0; i < 100; ++i) (void)inp(DSP_RESET);
    outp(DSP_RESET, 0);
    for (spin = 0; spin < 10000; ++spin)
        if ((inp(DSP_READ_STATUS) & 0x80u) && inp(DSP_READ) == 0xAAu) return 1;
    return 0;
}

static void parse_blaster(void) {
    const char *env = getenv("BLASTER");
    const char *p;
    if (!env) return;
    for (p = env; *p; ++p) {
        if (*p == 'A' || *p == 'a') g_base = (unsigned)strtoul(p + 1, NULL, 16);
        else if (*p == 'I' || *p == 'i') g_irq = (unsigned)strtoul(p + 1, NULL, 10);
        else if (*p == 'D' || *p == 'd') g_dma = (unsigned)strtoul(p + 1, NULL, 10);
    }
}

static void dma_program(void) {
    static const unsigned char page_port[4] = {0x87u, 0x83u, 0x81u, 0x82u};
    unsigned ch = g_dma & 3u;
    unsigned long len = MC_SB_DMA_BYTES - 1uL;
    outp(0x0Au, (int)(0x04u | ch));
    outp(0x0Cu, 0);
    outp(0x0Bu, (int)(0x58u | ch));
    outp((int)(ch << 1), (int)(g_dma_phys & 0xFFu));
    outp((int)(ch << 1), (int)((g_dma_phys >> 8) & 0xFFu));
    outp((int)page_port[ch], (int)((g_dma_phys >> 16) & 0xFFu));
    outp((int)((ch << 1) + 1u), (int)(len & 0xFFu));
    outp((int)((ch << 1) + 1u), (int)((len >> 8) & 0xFFu));
    outp(0x0Au, (int)ch);
}

static void dsp_start(void) {
    dsp_write(0xD1u);
    dsp_write(0x40u);
    dsp_write((unsigned char)((256u - (1000000u / MC_SB_RATE)) & 0xFFu));
    dsp_write(0x48u);
    dsp_write((unsigned char)((MC_SB_BLOCK - 1) & 0xFF));
    dsp_write((unsigned char)(((MC_SB_BLOCK - 1) >> 8) & 0xFF));
    dsp_write(0x1Cu);
}

static void dsp_stop(void) {
    dsp_write(0xD0u); dsp_write(0xDAu); dsp_write(0xD3u);
}

static void drain(snd_mixer_t *m, long frame, int frames,
                  const snd_sample_t *samples, void *user) {
    unsigned char *dst = g_dma_buf + (g_half_playing ? 0 : MC_SB_BLOCK);
    (void)m; (void)frame; (void)user;
    if (samples) snd_pack_u8(samples, dst, (long)frames);
    else memset(dst, 0x80, (size_t)frames);
}

static void render_next(void) {
    snd_render_one_block(&g_mixer, g_frame, MC_SB_BLOCK,
                         mw_demo_mix, &g_demo, SND_RENDER_SKIP_SILENT);
    g_frame += MC_SB_BLOCK;
}

int mc_sb_init(void) {
    parse_blaster();
    if (!dma_alloc()) return 0;
    if (!dsp_reset()) { free(g_dma_raw); g_dma_raw = 0; return 0; }
    snd_init(&g_mixer, MC_SB_RATE, 1, g_block, MC_SB_BLOCK, drain, NULL);
    snd_set_master_gain(&g_mixer, SND_GAIN_UNITY);
    mw_demo_init(&g_demo, &g_mixer, 0, 1);
    g_half_playing = 0; g_frame = 0;
    render_next();
    g_half_playing = 1;
    render_next();
    dma_program();
    dsp_start();
    return 1;
}

void mc_sb_service(void) {
    unsigned ch = g_dma & 3u;
    unsigned lo, hi;
    unsigned long remaining;
    int half;
    if (!g_dma_buf) return;
    outp(0x0Cu, 0);
    lo = (unsigned)inp((int)((ch << 1) + 1u));
    hi = (unsigned)inp((int)((ch << 1) + 1u));
    remaining = ((unsigned long)hi << 8) | lo;
    half = (remaining >= (unsigned long)MC_SB_BLOCK) ? 0 : 1;
    if (half != g_half_playing) {
        g_half_playing = half;
        render_next();
    }
}

void mc_sb_shutdown(void) {
    if (g_dma_buf) dsp_stop();
    if (g_dma_raw) free(g_dma_raw);
    g_dma_raw = 0; g_dma_buf = 0;
}

unsigned long mc_sb_frames(void) { return (unsigned long)g_frame; }
