/* dlfcn.h for Windows (XBA-278, xbr_posix_win32.h): the console runtimes load a title pack (libtitle, prepared by
 * tools.prepare) and the GL / EGL entry points with dlopen; on Windows that is LoadLibrary. On the include path of
 * the host layer and the console runtimes on Windows only. */
#ifndef XBR_DLFCN_WIN32_H
#define XBR_DLFCN_WIN32_H

#include <stdint.h>
#include <stdio.h>

#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_LOCAL 0x0
#define RTLD_GLOBAL 0x100
#define RTLD_DEFAULT ((void *)0)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>

static inline void *dlopen(const char *path, int flags)
{
    (void)flags;
    return path ? (void *)LoadLibraryA(path) : (void *)GetModuleHandleA(NULL);
}

static inline void *dlsym(void *h, const char *name)
{
    return (void *)(uintptr_t)GetProcAddress(h ? (HMODULE)h : GetModuleHandleA(NULL), name);
}

static inline int dlclose(void *h)
{
    return h && FreeLibrary((HMODULE)h) ? 0 : -1;
}

/* dladdr: the module an address is in (dli_fbase, dli_fname); no symbol names. */
typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;

static inline int dladdr(const void *addr, Dl_info *info)
{
    static char name[MAX_PATH];
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (const char *)addr, &m) || !m)
        return 0;
    GetModuleFileNameA(m, name, sizeof name);
    info->dli_fname = name;
    info->dli_fbase = (void *)m;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    return 1;
}

static inline const char *dlerror(void)
{
    static char msg[48];
    unsigned long e = GetLastError();
    if (!e)
        return NULL;
    snprintf(msg, sizeof msg, "Windows error %lu", e);
    return msg;
}

#endif /* XBR_DLFCN_WIN32_H */
