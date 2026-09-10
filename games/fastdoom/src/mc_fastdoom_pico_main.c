/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pico entry point for the first real FastDoom Cortex-M33 target.
 *
 * The command line is intentionally deterministic. Once FatFS is connected,
 * /doom.wad becomes the default SD-card IWAD; until then startup reaches the
 * real FastDoom file probe, reports the failure over USB, and remains alive.
 */

#include <stdio.h>

#include "d_main.h"
#include "m_misc.h"

extern void MC_FastDoomPicoBoot(void);

int main(void)
{
    static char arg0[] = "fastdoom";
    static char arg1[] = "-iwad";
    static char arg2[] = "doom.wad";
    static char *argv[] = {arg0, arg1, arg2, NULL};

    MC_FastDoomPicoBoot();

    myargc = 3;
    myargv = argv;

    printf("MCFDOOM1 starting FastDoom iwad=%s\n", arg2);
    fflush(stdout);

    D_DoomMain();

    for (;;)
    {
        /* D_DoomMain/D_DoomLoop are not expected to return. */
    }
}
