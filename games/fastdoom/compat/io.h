/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MC_FASTDOOM_COMPAT_IO_H
#define MC_FASTDOOM_COMPAT_IO_H

#if defined(MC_FASTDOOM_PICO)

/*
 * FastDoom has a vldoor_e enumerator literally named "open".
 *
 * newlib's <fcntl.h> declares the POSIX open() function, and C puts enum
 * constants and functions in the same ordinary identifier namespace. The
 * generated FastDoom source already rewrites actual open(...) calls to
 * _open(...), so hide only the declaration while importing the normal O_*
 * constants. The fcntl include guard then prevents later source-level
 * <fcntl.h> includes from reintroducing open().
 */
#define open mc_fd_hidden_posix_open
#include <fcntl.h>
#undef open

#include <sys/stat.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int   _open(const char *filename, int oflag, ...);
int   _close(int fd);
int   _read(int fd, char *buffer, int count);
int   _write(int fd, char *buffer, int count);
off_t _lseek(int fd, off_t offset, int origin);

long _tell(int fd);
long _filelength(int fd);
int  _access(const char *path, int mode);

#ifdef __cplusplus
}
#endif

/*
 * newlib's syscall ABI spells the data argument as char *. FastDoom passes
 * byte *, char (*)[N], structs, and other byte-addressable objects. All are
 * valid I/O buffers, so give the Pico overlay type-safe wrappers accepting
 * void * / const void * and perform the ABI cast in exactly one place.
 */
static inline int mc_fd_read(int fd, void *buffer, int count)
{
    return _read(fd, (char *)buffer, count);
}

static inline int mc_fd_write(int fd, const void *buffer, int count)
{
    return _write(fd, (char *)buffer, count);
}

#ifndef O_BINARY
#define O_BINARY 0
#endif

#elif defined(_MSC_VER)

#include <fcntl.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

int  __cdecl _open(const char *filename, int oflag, ...);
int  __cdecl _close(int fd);
int  __cdecl _read(int fd, void *buffer, unsigned int count);
int  __cdecl _write(int fd, const void *buffer, unsigned int count);
long __cdecl _lseek(int fd, long offset, int origin);
long __cdecl _tell(int fd);
long __cdecl _filelength(int fd);
int  __cdecl _access(const char *path, int mode);

#ifdef __cplusplus
}
#endif

#ifndef O_BINARY
#define O_BINARY _O_BINARY
#endif

#else

#include_next <io.h>

#endif

#endif
