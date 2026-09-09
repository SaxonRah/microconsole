/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_COMPAT_CONIO_H
#define MC_FASTDOOM_COMPAT_CONIO_H

#include <stdio.h>

/*
 * Only getch() is needed by the portable FastDoom sources that we compile.
 * Do not define inp/outp/outpw here: MSVC reserves those names as intrinsics,
 * and the Raylib target never performs port I/O.
 */
static __inline int getch(void)
{
    return getchar();
}

#endif
