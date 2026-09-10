/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_FORCE_COMPAT_H
#define MC_FASTDOOM_FORCE_COMPAT_H

/*
 * FastDoom's source assumes boolean storage is int-compatible. Several
 * command-line helpers accept int * and are passed boolean *. GCC treats an
 * enum type as distinct even when it has the same 32-bit representation, so
 * give the Pico build the int-backed ABI the source actually expects before
 * doomtype.h sees __BYTEBOOL__.
 */
#if defined(MC_FASTDOOM_PICO) && !defined(__BYTEBOOL__)
#define __BYTEBOOL__

#ifdef false
#undef false
#endif
#ifdef true
#undef true
#endif

enum
{
    false = 0,
    true = 1
};

typedef int boolean;
typedef unsigned char byte;
#endif

/*
 * Do not macro-remap open/close globally: FastDoom also uses those words as
 * door-state enum constants. compat/io.h hides only newlib's open() prototype.
 */
#include "io.h"
#include "strings.h"

#ifndef R_OK
#define R_OK 4
#endif

#endif
