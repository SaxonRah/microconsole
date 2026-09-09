/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * FastDoom -> MicroConsole Raylib platform boundary.
 *
 * The game renders into FastDoom's normal 320x200 INDEX8 backbuffer. This file
 * owns window/input/timing/palette presentation only.
 */
#include "raylib.h"

/*
 * FastDoom uses several KEY_* preprocessor names that Raylib also uses as enum
 * constants. Capture the Raylib values before including Doom headers.
 */
enum
{
    RLK_ESCAPE = KEY_ESCAPE,
    RLK_ENTER = KEY_ENTER,
    RLK_TAB = KEY_TAB,
    RLK_BACKSPACE = KEY_BACKSPACE,
    RLK_PAUSE = KEY_PAUSE,
    RLK_MINUS = KEY_MINUS,
    RLK_F1 = KEY_F1,
    RLK_F2 = KEY_F2,
    RLK_F3 = KEY_F3,
    RLK_F4 = KEY_F4,
    RLK_F5 = KEY_F5,
    RLK_F6 = KEY_F6,
    RLK_F7 = KEY_F7,
    RLK_F8 = KEY_F8,
    RLK_F9 = KEY_F9,
    RLK_F10 = KEY_F10,
    RLK_F11 = KEY_F11,
    RLK_F12 = KEY_F12
};

/* raylib includes <stdbool.h>; FastDoom defines enum constants named
 * true/false, so remove the C bool macros before Doom headers are parsed. */
#ifdef true
#undef true
#endif
#ifdef false
#undef false
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d_event.h"
#include "d_main.h"
#include "doomdef.h"
#include "doomstat.h"
#include "g_game.h"
#include "i_debug.h"
#include "i_file.h"
#include "i_gamma.h"
#include "i_ibm.h"
#include "i_system.h"
#include "m_misc.h"
#include "r_defs.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#define MC_FD_WIDTH  320
#define MC_FD_HEIGHT 200
#define MC_FD_PALETTES 14
#define MC_FD_ZONE_DEFAULT (64 * 1024 * 1024)

unsigned int ticcount_hr = 0;
unsigned int ticcount = 0;
unsigned int fps = 0;

unsigned int hasCPUID = 0;
unsigned int hasFPU = 0;
unsigned int hasMMX = 0;

int updatestate = I_NOUPDATE;
byte *pcscreen = 0;
byte *destscreen = 0;
byte *destview = 0;
unsigned short *currentscreen = 0;

volatile int TS_InInterrupt = 0;

extern int usemouse;
extern byte demorecording;
extern void MC_FastDoomInstallCrashHandler(void);

static Texture2D mc_texture;
static uint16_t mc_frame565[MC_FD_WIDTH * MC_FD_HEIGHT];
static uint16_t mc_palette565[MC_FD_PALETTES][256];
static int mc_palette = 0;
static int mc_graphics_ready = 0;
static int mc_quitting = 0;
static byte *mc_zone_memory = 0;

static uint16_t mc_rgb565(unsigned r, unsigned g, unsigned b)
{
    unsigned r5 = (r * 31u + 127u) / 255u;
    unsigned g6 = (g * 63u + 127u) / 255u;
    unsigned b5 = (b * 31u + 127u) / 255u;
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

static void mc_post_key(int type, int doom_key)
{
    event_t ev;
    ev.type = (byte)type;
    ev.data1 = doom_key;
    ev.data2 = 0;
    D_PostEvent(&ev);
}

static void mc_key_transition(int ray_key, int doom_key)
{
    if (IsKeyPressed(ray_key))
        mc_post_key(ev_keydown, doom_key);
    if (IsKeyReleased(ray_key))
        mc_post_key(ev_keyup, doom_key);
}

static void mc_post_keyboard(void)
{
    int i;

    /* Raw alphabet for menu/save-name input. */
    for (i = 0; i < 26; ++i)
        mc_key_transition(KEY_A + i, 'a' + i);

    /* Raw digits. */
    for (i = 0; i < 10; ++i)
        mc_key_transition(KEY_ZERO + i, '0' + i);

    mc_key_transition(KEY_SPACE, ' ');
    mc_key_transition(KEY_APOSTROPHE, '\'');
    mc_key_transition(KEY_COMMA, ',');
    mc_key_transition(KEY_PERIOD, '.');
    mc_key_transition(KEY_SLASH, '/');
    mc_key_transition(KEY_SEMICOLON, ';');
    mc_key_transition(KEY_LEFT_BRACKET, '[');
    mc_key_transition(KEY_RIGHT_BRACKET, ']');
    mc_key_transition(KEY_BACKSLASH, '\\');
    mc_key_transition(KEY_GRAVE, '`');
    mc_key_transition(RLK_MINUS, KEY_MINUS);
    mc_key_transition(KEY_EQUAL, KEY_EQUALS);

    /* Native Doom navigation keys. */
    mc_key_transition(KEY_UP, KEY_UPARROW);
    mc_key_transition(KEY_DOWN, KEY_DOWNARROW);
    mc_key_transition(KEY_LEFT, KEY_LEFTARROW);
    mc_key_transition(KEY_RIGHT, KEY_RIGHTARROW);

    mc_key_transition(RLK_ESCAPE, KEY_ESCAPE);
    mc_key_transition(RLK_ENTER, KEY_ENTER);
    mc_key_transition(RLK_TAB, KEY_TAB);
    mc_key_transition(RLK_BACKSPACE, KEY_BACKSPACE);
    mc_key_transition(RLK_PAUSE, KEY_PAUSE);

    mc_key_transition(RLK_F1, KEY_F1);
    mc_key_transition(RLK_F2, KEY_F2);
    mc_key_transition(RLK_F3, KEY_F3);
    mc_key_transition(RLK_F4, KEY_F4);
    mc_key_transition(RLK_F5, KEY_F5);
    mc_key_transition(RLK_F6, KEY_F6);
    mc_key_transition(RLK_F7, KEY_F7);
    mc_key_transition(RLK_F8, KEY_F8);
    mc_key_transition(RLK_F9, KEY_F9);
    mc_key_transition(RLK_F10, KEY_F10);
    mc_key_transition(RLK_F11, KEY_F11);
    mc_key_transition(RLK_F12, KEY_F12);

    mc_key_transition(KEY_LEFT_SHIFT, KEY_RSHIFT);
    mc_key_transition(KEY_RIGHT_SHIFT, KEY_RSHIFT);
    mc_key_transition(KEY_LEFT_CONTROL, KEY_RCTRL);
    mc_key_transition(KEY_RIGHT_CONTROL, KEY_RCTRL);
    mc_key_transition(KEY_LEFT_ALT, KEY_RALT);
    mc_key_transition(KEY_RIGHT_ALT, KEY_RALT);

    /*
     * MicroConsole convenience aliases. The raw letter event is still sent so
     * the keys remain useful for menu text entry.
     */
    mc_key_transition(KEY_W, KEY_UPARROW);
    mc_key_transition(KEY_S, KEY_DOWNARROW);
    mc_key_transition(KEY_A, KEY_LEFTARROW);
    mc_key_transition(KEY_D, KEY_RIGHTARROW);
    mc_key_transition(KEY_Z, KEY_RCTRL);
    mc_key_transition(KEY_E, ' ');
}

static void mc_post_mouse(void)
{
    event_t ev;
    Vector2 delta;
    int buttons = 0;

    if (!usemouse)
        return;

    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        buttons |= 1;
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
        buttons |= 2;
    if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))
        buttons |= 4;

    delta = GetMouseDelta();

    ev.type = ev_mouse;
    ev.data1 = buttons;
    ev.data2 = (int)(delta.x * 4.0f);
    D_PostEvent(&ev);
}

void I_Init(void)
{
    /*
     * First portability milestone: one game tic per presented frame. This
     * avoids emulating the DOS PIT/task scheduler while renderer/input are
     * being validated.
     */
    singletics = true;
    uncappedFPS = false;
    highResTimer = false;
    waitVsync = false;

    fprintf(stderr,
            "MicroConsole FastDoom: no-melt and no-attract-demo defaults are "
            "enabled for Raylib bring-up; use -melt / -enabledemo to retest them.\n");
    fflush(stderr);
}

void I_InitGraphics(void)
{
    Image image;
    byte *playpal;

    if (mc_graphics_ready)
        return;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(960, 600, "MicroConsole FastDoom - Raylib");
    SetExitKey(0);
    SetTargetFPS(TICRATE);

    image = GenImageColor(MC_FD_WIDTH, MC_FD_HEIGHT, BLACK);
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R5G6B5);
    mc_texture = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(mc_texture, TEXTURE_FILTER_POINT);

    if (usemouse)
        DisableCursor();

    memset(backbuffer, 0, sizeof(backbuffer));
    memset(mc_frame565, 0, sizeof(mc_frame565));

    pcscreen = backbuffer;
    destscreen = backbuffer;
    destview = backbuffer;
    currentscreen = (unsigned short *)backbuffer;

    I_SetGamma(usegamma);
    playpal = W_CacheLumpName("PLAYPAL", PU_CACHE);
    I_ProcessPalette(playpal);
    I_SetPalette(0);

    mc_graphics_ready = 1;
}

void I_ShutdownGraphics(void)
{
    if (!mc_graphics_ready)
        return;

    EnableCursor();
    UnloadTexture(mc_texture);
    CloseWindow();
    mc_graphics_ready = 0;
}

void I_StartTic(void)
{
    if (!mc_graphics_ready)
        return;

    if (WindowShouldClose())
    {
        fprintf(stderr, "MicroConsole FastDoom: Raylib requested window close.\n");
        fflush(stderr);
        I_Quit();
        return;
    }

    mc_post_keyboard();
    mc_post_mouse();
}

void I_ProcessPalette(byte *palette)
{
    int p;
    int i;

    for (p = 0; p < MC_FD_PALETTES; ++p)
    {
        for (i = 0; i < 256; ++i)
        {
            int base = p * 768 + i * 3;
            unsigned r6 = gammatable[palette[base + 0]];
            unsigned g6 = gammatable[palette[base + 1]];
            unsigned b6 = gammatable[palette[base + 2]];

            unsigned r = (r6 * 255u + 31u) / 63u;
            unsigned g = (g6 * 255u + 31u) / 63u;
            unsigned b = (b6 * 255u + 31u) / 63u;

            mc_palette565[p][i] = mc_rgb565(r, g, b);
        }
    }
}

void I_SetPalette(int numpalette)
{
    if (numpalette < 0)
        numpalette = 0;
    if (numpalette >= MC_FD_PALETTES)
        numpalette = MC_FD_PALETTES - 1;
    mc_palette = numpalette;
}

void I_FinishUpdate(void)
{
    int i;
    float sx;
    float sy;
    float scale;
    float out_w;
    float out_h;
    Rectangle src;
    Rectangle dst;
    Vector2 origin = {0.0f, 0.0f};

    if (!mc_graphics_ready)
        return;

    for (i = 0; i < MC_FD_WIDTH * MC_FD_HEIGHT; ++i)
        mc_frame565[i] = mc_palette565[mc_palette][backbuffer[i]];

    UpdateTexture(mc_texture, mc_frame565);

    sx = (float)GetScreenWidth() / (float)MC_FD_WIDTH;
    sy = (float)GetScreenHeight() / (float)MC_FD_HEIGHT;
    scale = (sx < sy) ? sx : sy;
    if (scale <= 0.0f)
        scale = 1.0f;

    out_w = MC_FD_WIDTH * scale;
    out_h = MC_FD_HEIGHT * scale;

    src.x = 0.0f;
    src.y = 0.0f;
    src.width = (float)MC_FD_WIDTH;
    src.height = (float)MC_FD_HEIGHT;

    dst.x = ((float)GetScreenWidth() - out_w) * 0.5f;
    dst.y = ((float)GetScreenHeight() - out_h) * 0.5f;
    dst.width = out_w;
    dst.height = out_h;

    BeginDrawing();
    ClearBackground(BLACK);
    DrawTexturePro(mc_texture, src, dst, origin, 0.0f, WHITE);
    EndDrawing();

    ++ticcount;
    ticcount_hr = ticcount << 4;
    updatestate = I_NOUPDATE;
}

void I_CalculateFPS(void)
{
    int current = mc_graphics_ready ? GetFPS() : 0;
    fps = current > 0 ? (unsigned int)current : 0u;
}

void I_WaitSingleVBL(void)
{
    /* EndDrawing/SetTargetFPS owns pacing in the Raylib target. */
}

void I_WaitCGA(void) {}
void I_DisableCGABlink(void) {}
void I_DisableMDABlink(void) {}

byte *I_ZoneBase(int *size)
{
    int bytes = MC_FD_ZONE_DEFAULT;

    if (limitram > 0)
        bytes = (int)limitram * 1024;
    else if (freeram > 0 && bytes > (int)freeram * 1024)
        bytes -= (int)freeram * 1024;

    if (bytes < 8 * 1024 * 1024)
        bytes = 8 * 1024 * 1024;

    mc_zone_memory = (byte *)malloc((size_t)bytes);
    if (!mc_zone_memory)
    {
        fprintf(stderr, "FastDoom: could not allocate %d bytes of zone memory\n", bytes);
        exit(1);
    }

    *size = bytes;
    printf("Zone memory: %d Kb\n", bytes >> 10);
    return mc_zone_memory;
}

byte *I_AllocLow(int length)
{
    byte *mem = (byte *)calloc(1u, (size_t)length);
    return mem;
}

void *I_DosMemAlloc(unsigned long size)
{
    return calloc(1u, (size_t)size);
}

int I_GetCPUModel(void)
{
    /*
     * Keep FastDoom on its baseline portable symbol selection. Specialized
     * x86 names are also aliased in mc_fastdoom_render.c, but 486 is the
     * least surprising semantic baseline.
     */
    return 486;
}

void I_GetCPU(void)
{
    MC_FastDoomInstallCrashHandler();
    hasCPUID = 0;
    hasFPU = 0;
    hasMMX = 0;
}

void I_StartupTimer(void) {}
void I_ShutdownTimer(void) {}
void I_SetHrTimerEnabled(boolean enabled)
{
    (void)enabled;
}
void I_TimerISR(task *task) { (void)task; }
void I_TimerMS(task *task) { (void)task; }

void I_LoopSong(int handle) { (void)handle; }
void I_ResumeSong(int handle) { (void)handle; }

void I_Quit(void)
{
    if (mc_quitting)
        exit(0);

    mc_quitting = 1;

    if (demorecording)
        G_CheckDemoStatus();

    M_SaveDefaults();
    I_ShutdownGraphics();
    exit(0);
}

void I_Error(int line, ...)
{
    va_list ap;
    const char *fmt;

    fmt = I_LoadTextProgram(line + 198);

    if (mc_graphics_ready)
        I_ShutdownGraphics();

    fprintf(stderr, "\nFastDoom fatal error");
    if (line >= 0)
        fprintf(stderr, " [%d]", line);
    fprintf(stderr, ": ");

    va_start(ap, line);
    if (fmt && fmt[0])
        vfprintf(stderr, fmt, ap);
    else
        fprintf(stderr, "no FastDoom error text was available");
    va_end(ap);

    fprintf(stderr, "\n");
    exit(1);
}

/* Debug API stubs/console fallback. */
void I_Printf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vprintf(format, ap);
    va_end(ap);
}

void I_Clear(void) {}
void I_DebugInit(void) {}
void I_DebugShutdown(void) {}

debugsymbol_t *I_LookupSymbol(int addr)
{
    (void)addr;
    return 0;
}

const char *I_LookupSymbolName(void *addr)
{
    (void)addr;
    return "<portable>";
}

void I_Backtrace(const char *msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);
}
