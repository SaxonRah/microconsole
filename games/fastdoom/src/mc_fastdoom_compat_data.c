/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Small pieces of the original DOS platform data that remain part of
 * FastDoom's portable configuration semantics.
 *
 * m_misc.c stores key bindings as legacy PC scan codes in the CFG file and
 * translates those codes through scantokey[] when loading defaults. Raylib
 * input itself does not consume this table, but the config loader still does.
 *
 * Several navigation-key constants used by the original table are local to
 * i_ibm.c rather than doomdef.h. Keep the exact original FastDoom values here
 * instead of pulling the DOS input implementation back into the portable
 * target.
 */
#include "doomtype.h"
#include "doomdef.h"

#define FD_KEY_LSHIFT 0xfe
#define FD_KEY_INS    (0x80 + 0x52)
#define FD_KEY_DEL    (0x80 + 0x53)
#define FD_KEY_PGUP   (0x80 + 0x49)
#define FD_KEY_PGDN   (0x80 + 0x51)
#define FD_KEY_HOME   (0x80 + 0x47)
#define FD_KEY_END    (0x80 + 0x4f)

byte scantokey[128] =
{
    /* 00 */ 0, 27, '1', '2', '3', '4', '5', '6',
    /* 08 */ '7', '8', '9', '0', '-', '=', KEY_BACKSPACE, 9,
    /* 10 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    /* 18 */ 'o', 'p', '[', ']', 13, KEY_RCTRL, 'a', 's',
    /* 20 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /* 28 */ 39, '`', FD_KEY_LSHIFT, 92, 'z', 'x', 'c', 'v',
    /* 30 */ 'b', 'n', 'm', ',', '.', '/', KEY_RSHIFT, '*',
    /* 38 */ KEY_RALT, ' ', 0, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
    /* 40 */ KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, 0, 0, FD_KEY_HOME,
    /* 48 */ KEY_UPARROW, FD_KEY_PGUP, '-', KEY_LEFTARROW, '5', KEY_RIGHTARROW, '+', FD_KEY_END,
    /* 50 */ KEY_DOWNARROW, FD_KEY_PGDN, FD_KEY_INS, FD_KEY_DEL, 0, 0, 0, KEY_F11,
    /* 58 */ KEY_F12, 0, 0, 0, 0, 0, 0, 0,
    /* 60 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 68 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 70 */ 0, 0, 0, 0, 0, 0, 0, 0,
    /* 78 */ 0, 0, 0, 0, 0, 0, 0, 0
};
