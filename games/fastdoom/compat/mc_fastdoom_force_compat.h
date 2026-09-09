/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_FORCE_COMPAT_H
#define MC_FASTDOOM_FORCE_COMPAT_H

/*
 * Declarations used by generated MSVC-compatible FastDoom sources.
 * This header intentionally does NOT remap open/close/read/write with macros;
 * FastDoom uses open/close as enum constants as well.
 */
#include "io.h"
#include "strings.h"

#ifndef R_OK
#define R_OK 4
#endif

#endif
