/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_COMPAT_DOS_H
#define MC_FASTDOOM_COMPAT_DOS_H

/*
 * Minimal DOS compatibility surface for the Raylib/Win32 bring-up.
 *
 * IMPORTANT: do not include <windows.h> here. This header is included by
 * FastDoom translation units after/beside Doom's own doomtype.h, whose
 * `boolean` type and MIN/MAX names collide with Windows SDK headers.
 *
 * The Win32 implementation of _dos_findfirst/_dos_findnext lives in its own
 * translation unit (mc_fastdoom_dos_win32.c), where Windows headers cannot
 * pollute FastDoom's type namespace.
 */

#include <stddef.h>

union REGS
{
    struct
    {
        unsigned int eax, ebx, ecx, edx, esi, edi;
        unsigned int cflag;
    } x;

    struct
    {
        unsigned short ax, bx, cx, dx, si, di;
        unsigned short cflag, flags;
    } w;

    struct
    {
        unsigned char al, ah;
        unsigned char bl, bh;
        unsigned char cl, ch;
        unsigned char dl, dh;
    } h;
};

static __inline int int386(int intno, union REGS *in, union REGS *out)
{
    (void)intno;
    if (out != in && out != 0 && in != 0)
        *out = *in;
    return 0;
}

#ifndef _A_SUBDIR
#define _A_SUBDIR 0x10u
#endif
#ifndef _A_ARCH
#define _A_ARCH   0x20u
#endif

/*
 * Only attrib/name are observed by FastDoom. `handle` is opaque state owned
 * by the Win32 compatibility implementation.
 */
struct find_t
{
    void *handle;
    unsigned int attrib;
    char name[260];
};

#ifdef __cplusplus
extern "C" {
#endif

int _dos_findfirst(const char *pattern, unsigned int attrib, struct find_t *out);
int _dos_findnext(struct find_t *out);

#ifdef __cplusplus
}
#endif

#endif
