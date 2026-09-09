/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_COMPAT_STRINGS_H
#define MC_FASTDOOM_COMPAT_STRINGS_H

#include <string.h>

#if defined(_MSC_VER)
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif

#endif
