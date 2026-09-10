/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_COMPAT_UNISTD_H
#define MC_FASTDOOM_COMPAT_UNISTD_H

#if defined(_MSC_VER) || defined(MC_FASTDOOM_PICO)
#include "io.h"
#ifndef R_OK
#define R_OK 4
#endif
#else
#include_next <unistd.h>
#endif

#endif
