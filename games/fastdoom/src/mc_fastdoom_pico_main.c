/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pico entry point for FastDoom.
 *
 * The selected WAD lives in MCWAD.CFG on the SD card. IWAD selections become
 * "-iwad <file>"; PWAD selections are launched as
 * "-iwad <current-base> -file <file>".
 */

#include <stdio.h>

#include "d_main.h"
#include "m_misc.h"

#include "mc_fastdoom_pico_wad.h"

extern void MC_FastDoomPicoBoot(void);

int main(void)
{
    static char arg0[] = "fastdoom";
    static char arg_iwad[] = "-iwad";
    static char arg_file[] = "-file";
    static char arg_disabledemo[] = "-disabledemo";
    static mc_fd_wad_boot_t wad;
    static char *argv_iwad[] = {
        arg0, arg_iwad, wad.selected, NULL
    };
    static char *argv_pwad[] = {
        arg0,
        arg_iwad, wad.base,
        arg_file, wad.selected,
        arg_disabledemo,
        NULL
    };

    MC_FastDoomPicoBoot();
    (void)mc_fd_pico_wad_boot_load(&wad);

    if (wad.mode == MC_FD_WAD_MODE_PWAD)
    {
        /*
         * FastDoom's built-in attract demos were recorded against the stock
         * IWAD maps. With a PWAD loaded, demo1/demo2/demo3 may immediately
         * enter replaced level data and are not a meaningful compatibility
         * test. FastDoom already provides -disabledemo specifically to suppress
         * these deferred attract-mode demos while keeping TITLEPIC/CREDIT/menu
         * operation intact.
         */
        myargc = 6;
        myargv = argv_pwad;

        printf("MCFDOOM1 starting FastDoom iwad=%s pwad=%s disabledemo=1\n",
               wad.base, wad.selected);
    }
    else
    {
        myargc = 3;
        myargv = argv_iwad;

        printf("MCFDOOM1 starting FastDoom iwad=%s\n", wad.selected);
    }

    fflush(stdout);
    D_DoomMain();

    for (;;)
    {
        /* D_DoomMain/D_DoomLoop are not expected to return. */
    }
}
