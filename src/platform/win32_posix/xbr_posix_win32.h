/* xbr_posix_win32 -- the few POSIX calls the host layer and the console runtimes use, on Windows (XBA-278).
 *
 * Force-included (target xbr_posix_win32, src/platform/host_core.cmake: -include with MinGW-w64 / clang, /FI with
 * MSVC) into the host layer and every console runtime on Windows only, so their sources stay POSIX: setenv /
 * unsetenv, localtime_r / gmtime_r, mkdir with a mode, fsync, pread / pwrite, sysconf(_SC_NPROCESSORS_ONLN),
 * timespec_get on an old C runtime, O_CLOEXEC. dlopen and friends are in this folder's dlfcn.h (LoadLibrary).
 *
 * It includes no <windows.h> (its macros collide with the runtimes' own names): the two Win32 calls it needs are
 * declared here. Linux, macOS and Android never see this file. */
#ifndef XBR_POSIX_WIN32_H
#define XBR_POSIX_WIN32_H
#ifdef _WIN32

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <time.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static inline int xbr_setenv(const char *k, const char *v, int overwrite)
{
    if (!overwrite && getenv(k))
        return 0;
    return _putenv_s(k, v) == 0 ? 0 : -1;
}
static inline int xbr_unsetenv(const char *k)
{
    return _putenv_s(k, "") == 0 ? 0 : -1; /* an empty value removes the variable */
}
#define setenv xbr_setenv
#define unsetenv xbr_unsetenv

static inline struct tm *xbr_localtime_r(const time_t *t, struct tm *r)
{
    return localtime_s(r, t) == 0 ? r : NULL;
}
static inline struct tm *xbr_gmtime_r(const time_t *t, struct tm *r)
{
    return gmtime_s(r, t) == 0 ? r : NULL;
}
#define localtime_r xbr_localtime_r
#define gmtime_r xbr_gmtime_r

static inline int xbr_mkdir(const char *p, int mode)
{
    (void)mode;
    return _mkdir(p);
}
#define mkdir(p, m) xbr_mkdir((p), (int)(m))
#define fsync(fd) _commit(fd)

static inline long long xbr_pio(int fd, void *rbuf, const void *wbuf, size_t n, long long off)
{
    long long at = _lseeki64(fd, 0, SEEK_CUR);
    if (at < 0 || _lseeki64(fd, off, SEEK_SET) < 0)
        return -1;
    int r = rbuf ? _read(fd, rbuf, (unsigned)n) : _write(fd, wbuf, (unsigned)n);
    _lseeki64(fd, at, SEEK_SET);
    return r;
}
#define pread(fd, b, n, off) xbr_pio((fd), (b), NULL, (n), (long long)(off))
#define pwrite(fd, b, n, off) xbr_pio((fd), NULL, (b), (n), (long long)(off))

/* sysconf(_SC_NPROCESSORS_ONLN) only. */
__declspec(dllimport) unsigned long __stdcall GetActiveProcessorCount(unsigned short group);
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif
static inline long xbr_sysconf(int name)
{
    return name == _SC_NPROCESSORS_ONLN ? (long)GetActiveProcessorCount(0xFFFF) : -1; /* ALL_PROCESSOR_GROUPS */
}
#define sysconf(n) xbr_sysconf(n)

/* readlink: no symbolic links to read here (the PS1 runtime's /proc/self/exe: it looks beside the program another
 * way); -1. */
static inline long xbr_readlink(const char *p, char *buf, size_t n)
{
    (void)p, (void)buf, (void)n;
    errno = EINVAL;
    return -1;
}
#define readlink xbr_readlink

/* C11's timespec_get, missing from MinGW's msvcrt headers. */
#if !defined(TIME_UTC)
#define TIME_UTC 1
struct _FILETIME;
__declspec(dllimport) void __stdcall GetSystemTimePreciseAsFileTime(struct _FILETIME *ft);
static inline int xbr_timespec_get(struct timespec *ts, int base)
{
    uint64_t t;
    if (base != TIME_UTC)
        return 0;
    GetSystemTimePreciseAsFileTime((struct _FILETIME *)&t); /* 100 ns since 1601 */
    t -= 116444736000000000ull;
    ts->tv_sec = (time_t)(t / 10000000ull);
    ts->tv_nsec = (long)(t % 10000000ull) * 100;
    return base;
}
#define timespec_get xbr_timespec_get
#endif

#endif /* _WIN32 */
#endif /* XBR_POSIX_WIN32_H */
