/* libxba_vkdriver.so: a Vulkan driver the player supplied (Mesa Turnip...), opened through libadrenotools
 * (third_party/adrenotools, BSD-2). A library of its own, never linked by libmain.so: the N64 runtime dlopens it only
 * when a custom driver is selected (consoles/n64/runtime/n64_vk_driver.c), so linkernsbypass's static constructor
 * (it patches the page of dlopen to find private linker symbols) never runs for a player who did not pick one.
 *
 * xba_vkdriver_open(hook_dir, driver_dir, lib_name): a dlopen handle of a private copy of the system libvulkan.so
 * whose driver is <driver_dir><lib_name> (driver_dir ends with '/'), or NULL; dlsym(handle, "vkGetInstanceProcAddr")
 * gives what Vulkan::Context::init_loader takes. hook_dir is the app's native library directory, where the four hook
 * libraries are. A non-NULL handle does not prove the custom driver was loaded: adrenotools' hook loads the device's
 * own when the custom one fails (logcat tag hook_impl), so the caller checks the device's driverID.
 * xba_vkdriver_last_error(): why the last open returned NULL, for the log. */
#include <adrenotools/driver.h>
#include <android_linker_ns.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define XBA_EXPORT __attribute__((visibility("default")))

static char last_error[256];

XBA_EXPORT const char *xba_vkdriver_last_error(void) { return last_error; }

XBA_EXPORT void *xba_vkdriver_open(const char *hook_dir, const char *driver_dir, const char *lib_name)
{
    last_error[0] = 0;
    if (!hook_dir || !*hook_dir || !driver_dir || !*driver_dir || !lib_name || !*lib_name) {
        snprintf(last_error, sizeof last_error, "missing hook dir, driver dir or library name");
        return NULL;
    }
    if (driver_dir[strlen(driver_dir) - 1] != '/') {
        snprintf(last_error, sizeof last_error, "driver dir %s does not end with '/'", driver_dir);
        return NULL;
    }
    char path[1024];
    struct stat st;
    snprintf(path, sizeof path, "%s%s", driver_dir, lib_name);
    if (stat(path, &st) != 0) {
        snprintf(last_error, sizeof last_error, "no driver file %s", path);
        return NULL;
    }
    snprintf(path, sizeof path, "%s/libmain_hook.so", hook_dir);
    if (stat(path, &st) != 0) {
        snprintf(last_error, sizeof last_error, "no hook library %s", path);
        return NULL;
    }
    if (!linkernsbypass_load_status()) {
        snprintf(last_error, sizeof last_error, "linkernsbypass could not find the linker's private symbols");
        return NULL;
    }
    void *h = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hook_dir, driver_dir,
                                         lib_name, NULL, NULL);
    if (!h) {
        const char *e = dlerror();
        snprintf(last_error, sizeof last_error, "adrenotools could not open libvulkan%s%s", e ? ": " : "",
                 e ? e : "");
    }
    return h;
}
