/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Portable C counterparts for FastDoom's linear backbuffer NASM routines.
 *
 * r_main.c keeps choosing the normal FastDoom entry points; this file supplies
 * those symbols without changing the renderer's higher-level behavior.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "doomdef.h"
#include "doomstat.h"
#include "fastmath.h"
#include "r_defs.h"
#include "r_draw.h"
#include "r_main.h"
#include "r_state.h"
#include "v_video.h"

#ifndef MC_FD_RENDER_VALIDATE
#if defined(MC_FASTDOOM_PICO)
#define MC_FD_RENDER_VALIDATE 0
#else
#define MC_FD_RENDER_VALIDATE 1
#endif
#endif

#if defined(MC_FASTDOOM_PICO) && defined(__GNUC__)
#define MC_FD_RENDER_HOT(name) \
    __attribute__((noinline, section(".time_critical." #name))) name
#else
#define MC_FD_RENDER_HOT(name) name
#endif

#if MC_FD_RENDER_VALIDATE
static void mc_renderer_abort(const char *what, int a, int b, int c, int d)
{
    fprintf(stderr,
            "\nMicroConsole FastDoom renderer invariant failed: %s "
            "(%d, %d, %d, %d)\n",
            what, a, b, c, d);
    fflush(stderr);
    abort();
}

/*
 * Every column renderer needs valid destination geometry, but not every
 * FastDoom column renderer consumes dc_source/dc_colormap:
 *
 *   textured columns -> dc_source + dc_colormap
 *   flat columns     -> dc_color only
 *   fuzz columns     -> framebuffer + global colormaps
 *
 * Keeping those contracts separate is important.  The previous shared
 * validator rejected legal flat/fuzz calls whenever the higher-level renderer
 * intentionally left dc_source or dc_colormap unset.
 */
static void mc_validate_column_geometry(int pixel_width)
{
    int xoff;

    if (dc_yh < dc_yl)
        return;

    if (dc_x < 0 || dc_x >= SCREENWIDTH ||
        dc_yl < 0 || dc_yh >= SCREENHEIGHT)
        mc_renderer_abort("column coordinates",
                          dc_x, dc_yl, dc_yh, pixel_width);

    xoff = columnofs[dc_x];
    if (xoff < 0 || xoff + pixel_width > SCREENWIDTH)
        mc_renderer_abort("column destination",
                          dc_x, xoff, pixel_width, SCREENWIDTH);

    if (ylookup[dc_yl] == NULL || ylookup[dc_yh] == NULL)
        mc_renderer_abort("column ylookup",
                          dc_x, dc_yl, dc_yh, pixel_width);
}

static void mc_validate_textured_column(int pixel_width)
{
    mc_validate_column_geometry(pixel_width);

    if (dc_yh < dc_yl)
        return;

    if (dc_source == NULL || dc_colormap == NULL)
        mc_renderer_abort("textured column source/colormap",
                          dc_x,
                          dc_yl,
                          dc_source != NULL,
                          dc_colormap != NULL);
}

static void mc_validate_span(int pixel_width)
{
    int xoff;
    int pixels;

    if (ds_x2 < ds_x1)
        return;

    if (ds_y < 0 || ds_y >= SCREENHEIGHT ||
        ds_x1 < 0 || ds_x1 >= SCREENWIDTH ||
        ds_x2 < 0 || ds_x2 >= SCREENWIDTH)
        mc_renderer_abort("span coordinates",
                          ds_y, ds_x1, ds_x2, pixel_width);

    xoff = columnofs[ds_x1];
    pixels = (ds_x2 - ds_x1 + 1) * pixel_width;
    if (xoff < 0 || xoff + pixels > SCREENWIDTH)
        mc_renderer_abort("span destination",
                          ds_y, xoff, pixels, SCREENWIDTH);

    if (ylookup[ds_y] == NULL)
        mc_renderer_abort("span ylookup",
                          ds_y, ds_x1, ds_x2, pixel_width);

    if (ds_source == NULL || ds_colormap == NULL)
        mc_renderer_abort("span source/colormap",
                          ds_y,
                          ds_x1,
                          ds_source != NULL,
                          ds_colormap != NULL);
}

#endif

static void mc_draw_column_scaled(int pixel_width)
{
    int count;
    byte *dest;
    uint32_t frac;
    uint32_t fracstep;

    if (dc_yh < dc_yl)
        return;

#if MC_FD_RENDER_VALIDATE
    mc_validate_textured_column(pixel_width);
#endif
    count = dc_yh - dc_yl + 1;
    dest = ylookup[dc_yl] + columnofs[dc_x];

    frac = (uint32_t)(
        (int64_t)dc_texturemid +
        (int64_t)(dc_yl - centery) * (int64_t)dc_iscale);
    fracstep = (uint32_t)dc_iscale;

    while (count-- > 0)
    {
        byte color = dc_colormap[dc_source[(frac >> FRACBITS) & 127u]];
        int k;

        for (k = 0; k < pixel_width; ++k)
            dest[k] = color;

        dest += SCREENWIDTH;
        frac += fracstep;
    }
}

static void mc_draw_column_flat(int pixel_width)
{
    int count;
    byte *dest;
    byte color;

    if (dc_yh < dc_yl)
        return;

    /*
     * Flat renderers intentionally do not consume dc_source or dc_colormap.
     * They render the caller-provided flat dc_color.
     */
#if MC_FD_RENDER_VALIDATE
    mc_validate_column_geometry(pixel_width);
#endif
    count = dc_yh - dc_yl + 1;
    dest = ylookup[dc_yl] + columnofs[dc_x];
    color = dc_color;

    while (count-- > 0)
    {
        int k;

        for (k = 0; k < pixel_width; ++k)
            dest[k] = color;

        dest += SCREENWIDTH;
    }
}

static unsigned mc_span_spot(uint32_t position)
{
    /*
     * FastDoom packs two 6.10 texture coordinates into ds_frac/ds_step.
     * This is the same extraction used by its C text-mode span path.
     */
    unsigned y = (position >> 4) & 0x0fc0u;
    unsigned x = position >> 26;
    return x | y;
}

static void mc_draw_span(int pixel_width)
{
    int x;
    byte *dest;
    uint32_t position;
    uint32_t step;

    if (ds_x2 < ds_x1)
        return;

#if MC_FD_RENDER_VALIDATE
    mc_validate_span(pixel_width);
#endif
    dest = ylookup[ds_y] + columnofs[ds_x1];
    position = (uint32_t)ds_frac;
    step = (uint32_t)ds_step;

    for (x = ds_x1; x <= ds_x2; ++x)
    {
        byte color = ds_colormap[ds_source[mc_span_spot(position)]];
        int k;

        for (k = 0; k < pixel_width; ++k)
            dest[k] = color;

        dest += pixel_width;
        position += step;
    }
}

static void mc_draw_fuzz(int pixel_width, int flat)
{
    static const signed char fuzz_dir[50] = {
         1,-1, 1,-1, 1, 1,-1, 1, 1,-1,
         1, 1, 1,-1, 1, 1, 1,-1,-1,-1,
        -1, 1,-1,-1, 1,-1,-1, 1, 1, 1,
         1,-1, 1, 1,-1,-1,-1,-1,-1, 1,
        -1,-1, 1, 1,-1, 1, 1,-1, 1, 1
    };
    static unsigned fuzz_pos;
    int y;

    if (dc_yh < dc_yl)
        return;

    /*
     * Fuzz rendering is framebuffer based.  Like Doom's original fuzz path,
     * it does not read dc_source or dc_colormap.
     */
#if MC_FD_RENDER_VALIDATE
    mc_validate_column_geometry(pixel_width);

    if (colormaps == NULL)
        mc_renderer_abort("fuzz colormaps",
                          dc_x, dc_yl, dc_yh, pixel_width);
#endif

    for (y = dc_yl; y <= dc_yh; ++y)
    {
        byte *dest = ylookup[y] + columnofs[dc_x];
        int sample_y = y + fuzz_dir[fuzz_pos];
        byte source_color;
        byte color;
        int k;

        ++fuzz_pos;
        if (fuzz_pos == 50u)
            fuzz_pos = 0u;

        if (sample_y < 0)
            sample_y = 0;
        else if (sample_y >= SCREENHEIGHT)
            sample_y = SCREENHEIGHT - 1;

        source_color = ylookup[sample_y][columnofs[dc_x]];

        if (flat)
        {
            /*
             * FastDoom's flat fuzz variants trade fidelity for speed. Keep
             * that contract: darken the existing pixel without a texture read.
             */
            color = colormaps[(6 * 256) + source_color];
        }
        else
        {
            color = colormaps[(6 * 256) + source_color];
        }

        for (k = 0; k < pixel_width; ++k)
            dest[k] = color;
    }
}

/*
 * Full-detail Pico hot paths.
 *
 * These are intentionally separate from the generic width-2/4 helpers so the
 * common 320x200 renderer has no pixel-width loop/branch in its inner loops.
 * The higher-level FastDoom renderer and all texture/lighting semantics remain
 * unchanged.
 */
static void MC_FD_RENDER_HOT(mc_draw_column_scaled_1)(void)
{
    int count;
    byte *dest;
    const byte *source;
    const byte *map;
    uint32_t frac;
    uint32_t fracstep;

    if (dc_yh < dc_yl)
        return;

#if MC_FD_RENDER_VALIDATE
    mc_validate_textured_column(1);
#endif

    count = dc_yh - dc_yl;
    dest = ylookup[dc_yl] + columnofs[dc_x];
    source = dc_source;
    map = dc_colormap;
    frac = (uint32_t)(
        (int64_t)dc_texturemid +
        (int64_t)(dc_yl - centery) * (int64_t)dc_iscale);
    fracstep = (uint32_t)dc_iscale;

    do
    {
        *dest = map[source[(frac >> FRACBITS) & 127u]];
        dest += SCREENWIDTH;
        frac += fracstep;
    } while (count--);
}

static void MC_FD_RENDER_HOT(mc_draw_column_flat_1)(void)
{
    int count;
    byte *dest;
    byte color;

    if (dc_yh < dc_yl)
        return;

#if MC_FD_RENDER_VALIDATE
    mc_validate_column_geometry(1);
#endif

    count = dc_yh - dc_yl;
    dest = ylookup[dc_yl] + columnofs[dc_x];
    color = dc_color;

    do
    {
        *dest = color;
        dest += SCREENWIDTH;
    } while (count--);
}

static void MC_FD_RENDER_HOT(mc_draw_span_1)(void)
{
    int count;
    byte *dest;
    const byte *source;
    const byte *map;
    uint32_t position;
    uint32_t step;

    if (ds_x2 < ds_x1)
        return;

#if MC_FD_RENDER_VALIDATE
    mc_validate_span(1);
#endif

    count = ds_x2 - ds_x1;
    dest = ylookup[ds_y] + columnofs[ds_x1];
    source = ds_source;
    map = ds_colormap;
    position = (uint32_t)ds_frac;
    step = (uint32_t)ds_step;

    do
    {
        unsigned spot =
            (position >> 26) | ((position >> 4) & 0x0fc0u);
        *dest++ = map[source[spot]];
        position += step;
    } while (count--);
}

static void MC_FD_RENDER_HOT(mc_draw_fuzz_1)(void)
{
    static const signed char fuzz_dir[50] = {
         1,-1, 1,-1, 1, 1,-1, 1, 1,-1,
         1, 1, 1,-1, 1, 1, 1,-1,-1,-1,
        -1, 1,-1,-1, 1,-1,-1, 1, 1, 1,
         1,-1, 1, 1,-1,-1,-1,-1,-1, 1,
        -1,-1, 1, 1,-1, 1, 1,-1, 1, 1
    };
    static unsigned fuzz_pos;
    int y;
    int xoff;

    if (dc_yh < dc_yl)
        return;

#if MC_FD_RENDER_VALIDATE
    mc_validate_column_geometry(1);
    if (colormaps == NULL)
        mc_renderer_abort("fuzz colormaps", dc_x, dc_yl, dc_yh, 1);
#endif

    xoff = columnofs[dc_x];

    for (y = dc_yl; y <= dc_yh; ++y)
    {
        byte *dest = ylookup[y] + xoff;
        int sample_y = y + fuzz_dir[fuzz_pos];

        ++fuzz_pos;
        if (fuzz_pos == 50u)
            fuzz_pos = 0u;

        if (sample_y < 0)
            sample_y = 0;
        else if (sample_y >= SCREENHEIGHT)
            sample_y = SCREENHEIGHT - 1;

        *dest = colormaps[(6 * 256) + ylookup[sample_y][xoff]];
    }
}

/* Normal wall/sprite columns. */
void R_DrawColumnBackbuffer(void)              { mc_draw_column_scaled_1(); }
void R_DrawColumnLowBackbuffer(void)           { mc_draw_column_scaled(2); }
void R_DrawColumnPotatoBackbuffer(void)        { mc_draw_column_scaled(4); }

void R_DrawColumnBackbufferFastLEA(void)       { mc_draw_column_scaled_1(); }
void R_DrawColumnLowBackbufferFastLEA(void)    { mc_draw_column_scaled(2); }
void R_DrawColumnPotatoBackbufferFastLEA(void) { mc_draw_column_scaled(4); }

void R_DrawColumnBackbufferRoll(void)          { mc_draw_column_scaled_1(); }
void R_DrawColumnBackbufferMMX(void)           { mc_draw_column_scaled_1(); }

/* FastDoom's full-screen/direct specializations are semantic aliases here. */
void R_DrawColumnBackbufferDirect(void)        { mc_draw_column_scaled_1(); }
void R_DrawColumnLowBackbufferDirect(void)     { mc_draw_column_scaled(2); }
void R_DrawColumnPotatoBackbufferDirect(void)  { mc_draw_column_scaled(4); }

void R_DrawColumnBackbufferSkyFullDirect(void)
{
    mc_draw_column_scaled_1();
}

void R_DrawColumnLowBackbufferSkyFullDirect(void)
{
    mc_draw_column_scaled(2);
}

void R_DrawColumnPotatoBackbufferSkyFullDirect(void)
{
    mc_draw_column_scaled(4);
}

/* Flat wall/sprite columns. */
void R_DrawColumnBackbufferFlat(void)          { mc_draw_column_flat_1(); }
void R_DrawColumnLowBackbufferFlat(void)       { mc_draw_column_flat(2); }
void R_DrawColumnPotatoBackbufferFlat(void)    { mc_draw_column_flat(4); }

/* Floor/ceiling spans. */
void R_DrawSpanBackbuffer(void)                { mc_draw_span_1(); }
void R_DrawSpanLowBackbuffer(void)             { mc_draw_span(2); }
void R_DrawSpanPotatoBackbuffer(void)          { mc_draw_span(4); }

void R_DrawSpanBackbufferRoll(void)            { mc_draw_span_1(); }
void R_DrawSpanBackbufferMMX(void)             { mc_draw_span_1(); }
void R_DrawSpanBackbufferPentium(void)         { mc_draw_span_1(); }
void R_DrawSpanLowBackbufferPentium(void)      { mc_draw_span(2); }
void R_DrawSpanPotatoBackbufferPentium(void)   { mc_draw_span(4); }

/* Spectre/fuzz paths that are NASM in the original linear renderer. */
void R_DrawFuzzColumnBackbuffer(void)           { mc_draw_fuzz_1(); }
void R_DrawFuzzColumnLowBackbuffer(void)        { mc_draw_fuzz(2, 0); }
void R_DrawFuzzColumnPotatoBackbuffer(void)     { mc_draw_fuzz(4, 0); }

void R_DrawFuzzColumnFlatBackbuffer(void)       { mc_draw_fuzz_1(); }
void R_DrawFuzzColumnFlatLowBackbuffer(void)    { mc_draw_fuzz(2, 1); }
void R_DrawFuzzColumnFlatPotatoBackbuffer(void) { mc_draw_fuzz(4, 1); }

/*
 * The DOS versions patch constants into generated/unrolled machine code.
 * Portable C reads the live globals directly, so these become no-ops.
 */
void R_PatchLinearHigh(void) {}
void R_PatchLinearLow(void) {}
void R_PatchLinearPotato(void) {}

void R_PatchCenteryLinearHighKN(void) {}
void R_PatchCenteryLinearLowKN(void) {}

void R_PatchCenteryLinearDirect(void) {}
void R_PatchCenteryLinearLowDirect(void) {}
void R_PatchCenteryLinearPotatoDirect(void) {}

void R_PatchColumnofsHighPentium(void) {}
void R_PatchColumnofsLowPentium(void) {}
void R_PatchColumnofsPotatoPentium(void) {}

void R_PatchFuzzColumnLinearHigh(void) {}
void R_PatchFuzzColumnLinearLow(void) {}
void R_PatchFuzzColumnLinearPotato(void) {}
