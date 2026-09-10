/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_COMPAT_CONIO_H
#define MC_FASTDOOM_COMPAT_CONIO_H

#include <stdio.h>

/*
 * FastDoom uses getch() for a few interactive DOS-era prompts.
 * On the portable targets this maps to the active standard input.
 */
static __inline int getch(void)
{
    return getchar();
}

#if defined(MC_FASTDOOM_PICO)

/*
 * hu_stuff.c retains optional ISA/debug-card FPS output using inp()/outp().
 * There is no x86 I/O-port space on RP2350 and these paths are not selected
 * by the Pico configuration, so compile them as deterministic no-ops.
 *
 * Keep the return value at zero for reads.  outp()/outpw() traditionally
 * return the value written, which also makes these shims usable if another
 * retained DOS helper references the return value later in the port.
 */
static __inline int inp(unsigned int port)
{
    (void)port;
    return 0;
}

static __inline int outp(unsigned int port, int value)
{
    (void)port;
    return value;
}

static __inline unsigned int inpw(unsigned int port)
{
    (void)port;
    return 0u;
}

static __inline unsigned int outpw(unsigned int port, unsigned int value)
{
    (void)port;
    return value;
}

#endif /* MC_FASTDOOM_PICO */

#endif
