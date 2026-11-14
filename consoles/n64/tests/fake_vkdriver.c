/* fake_vkdriver.c -- a stand-in for libxba_vkdriver.so (android/app/src/main/cpp/vkdriver.c) in test_vk_driver.c
 * --wrapper: xba_vkdriver_open "opens" any driver by returning a handle to this library, whose vkGetInstanceProcAddr
 * is never called. Lets the desktop test reach the outcomes reported after a successful open. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* dladdr */
#endif
#include <dlfcn.h>
#include <stddef.h>

void *vkGetInstanceProcAddr(void *instance, const char *name);
void *xba_vkdriver_open(const char *hooks, const char *dir, const char *lib);
const char *xba_vkdriver_last_error(void);

void *vkGetInstanceProcAddr(void *instance, const char *name)
{
    (void)instance;
    (void)name;
    return NULL;
}

void *xba_vkdriver_open(const char *hooks, const char *dir, const char *lib)
{
    (void)hooks;
    (void)dir;
    (void)lib;
    Dl_info info;
    if (!dladdr((void *)&xba_vkdriver_open, &info) || !info.dli_fname)
        return NULL;
    return dlopen(info.dli_fname, RTLD_NOW | RTLD_LOCAL);
}

const char *xba_vkdriver_last_error(void) { return ""; }
