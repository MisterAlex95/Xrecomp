/* n64_vk_driver.c -- a Vulkan driver the player supplied, for the N64 Vulkan RDP (n64_vk_driver.h). */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* dladdr */
#endif
#include "n64_vk_driver.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VKD_PATH_MAX 1024

static struct {
    char dir[VKD_PATH_MAX];   /* RECOMP_VK_DRIVER_DIR, with its trailing '/' */
    char state[VKD_PATH_MAX]; /* its parent: where trying and last_start live */
    char id[128];
    int has_state; /* the parent is a vk_drivers directory: the marker and last_start are written there */
    int open; /* n64_vk_driver_open wrote the marker and waits for n64_vk_driver_done */
} vkd;

static const char *env(const char *k)
{
    const char *v = getenv(k);
    return v && *v ? v : NULL;
}

int n64_vk_driver_selected(void)
{
    const char *force = env("RECOMP_VK_DRIVER");
    if (force && !strcmp(force, "system"))
        return 0;
    return env("RECOMP_VK_DRIVER_DIR") != NULL;
}

const char *n64_vk_driver_id(void) { return vkd.id; }

static void write_file(const char *name, const char *text)
{
    if (!vkd.has_state)
        return;
    char path[VKD_PATH_MAX + 192];
    snprintf(path, sizeof path, "%s/%s", vkd.state, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "n64: Vulkan RDP: cannot write %s\n", path);
        return;
    }
    fputs(text, f);
    fclose(f);
}

/* 1 when the marker of a previous start names this driver. */
static int marker_is_ours(void)
{
    char path[VKD_PATH_MAX + 32], line[160] = "";
    if (!vkd.has_state)
        return 0;
    snprintf(path, sizeof path, "%s/trying", vkd.state);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    if (!fgets(line, sizeof line, f))
        line[0] = 0;
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    return !strcmp(line, vkd.id);
}

/* The directory libmain.so (or whatever holds this code) was loaded from: on Android, the native library directory. */
static int own_dir(char *out, size_t n)
{
    Dl_info info;
    if (!dladdr((void *)&n64_vk_driver_open, &info) || !info.dli_fname || info.dli_fname[0] != '/')
        return 0;
    snprintf(out, n, "%s", info.dli_fname);
    char *slash = strrchr(out, '/');
    if (!slash)
        return 0;
    *slash = 0;
    return 1;
}

/* 1 when the id can name a file next to the drivers: [A-Za-z0-9_-]+ (the app's ids are 16 hex digits). */
static int id_is_file_name(const char *id)
{
    if (!*id)
        return 0;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' ||
              *p == '-'))
            return 0;
    return 1;
}

/* last_start ("<id> <status>", the last start with any custom driver) and <id>.status ("<status>", this driver's last
 * start: the app reads it first, so a game with its own driver does not hide another driver's outcome). */
static void last_start(const char *status)
{
    char line[200];
    snprintf(line, sizeof line, "%s %s\n", vkd.id, status);
    write_file("last_start", line);
    if (id_is_file_name(vkd.id)) {
        char name[160];
        snprintf(name, sizeof name, "%s.status", vkd.id);
        snprintf(line, sizeof line, "%s\n", status);
        write_file(name, line);
    }
}

void *n64_vk_driver_open(void)
{
    vkd.open = 0;
    vkd.id[0] = 0;
    if (!n64_vk_driver_selected())
        return NULL;
    const char *dir = env("RECOMP_VK_DRIVER_DIR"), *lib = env("RECOMP_VK_DRIVER_LIB");
    const size_t dl = strlen(dir);
    snprintf(vkd.dir, sizeof vkd.dir, "%s%s", dir, dir[dl - 1] == '/' ? "" : "/");
    /* the parent directory and the default id: the last component */
    snprintf(vkd.state, sizeof vkd.state, "%s", vkd.dir);
    size_t sl = strlen(vkd.state);
    while (sl > 1 && vkd.state[sl - 1] == '/')
        vkd.state[--sl] = 0;
    char *slash = strrchr(vkd.state, '/');
    const char *base = slash ? slash + 1 : vkd.state;
    snprintf(vkd.id, sizeof vkd.id, "%s", env("RECOMP_VK_DRIVER_ID") ? env("RECOMP_VK_DRIVER_ID") : base);
    vkd.id[strcspn(vkd.id, " \t\r\n")] = 0;
    if (slash && slash != vkd.state)
        *slash = 0;
    else
        snprintf(vkd.state, sizeof vkd.state, ".");
    /* never a file outside the layout <files>/vk_drivers/<id>/ */
    const char *parent = strrchr(vkd.state, '/');
    vkd.has_state = !strcmp(parent ? parent + 1 : vkd.state, "vk_drivers");
    if (!vkd.has_state)
        fprintf(stderr, "n64: Vulkan RDP: custom driver %s is not in a vk_drivers directory: no trial marker\n",
                vkd.id);

    if (vkd.dir[0] != '/' || !lib || strchr(lib, '/')) {
        fprintf(stderr, "n64: Vulkan RDP: custom driver %s: RECOMP_VK_DRIVER_DIR must be absolute and "
                        "RECOMP_VK_DRIVER_LIB a file name (%s, %s); system driver\n",
                vkd.id, vkd.dir, lib ? lib : "unset");
        last_start("open_failed");
        return NULL;
    }
    if (marker_is_ours()) {
        fprintf(stderr, "n64: Vulkan RDP: custom driver %s did not survive the last start (marker %s/trying); "
                        "skipped until it is picked again, system driver\n", vkd.id, vkd.state);
        last_start("crashed");
        return NULL;
    }
    char path[VKD_PATH_MAX + 256];
    snprintf(path, sizeof path, "%s%s", vkd.dir, lib);
    if (access(path, R_OK) != 0) {
        fprintf(stderr, "n64: Vulkan RDP: custom driver %s: no file %s; system driver\n", vkd.id, path);
        last_start("open_failed");
        return NULL;
    }
    char hooks[VKD_PATH_MAX];
    if (env("RECOMP_VK_HOOK_DIR"))
        snprintf(hooks, sizeof hooks, "%s", env("RECOMP_VK_HOOK_DIR"));
    else if (!own_dir(hooks, sizeof hooks))
        hooks[0] = 0;

    char marker[200];
    snprintf(marker, sizeof marker, "%s\n", vkd.id);
    write_file("trying", marker);
    vkd.open = 1;

    void *wrapper = dlopen("libxba_vkdriver.so", RTLD_NOW | RTLD_LOCAL);
    if (!wrapper) {
        const char *e = dlerror();
        fprintf(stderr, "n64: Vulkan RDP: custom driver %s: cannot load libxba_vkdriver.so (%s); system driver\n",
                vkd.id, e ? e : "?");
        n64_vk_driver_done("open_failed");
        return NULL;
    }
    typedef void *(*open_fn)(const char *, const char *, const char *);
    typedef const char *(*error_fn)(void);
    open_fn xopen = (open_fn)dlsym(wrapper, "xba_vkdriver_open");
    error_fn xerror = (error_fn)dlsym(wrapper, "xba_vkdriver_last_error");
    void *handle = xopen ? xopen(hooks, vkd.dir, lib) : NULL;
    if (!handle) {
        fprintf(stderr, "n64: Vulkan RDP: custom driver %s (%s%s, hooks in %s): %s; system driver\n", vkd.id, vkd.dir,
                lib, hooks[0] ? hooks : "?", xerror && xerror()[0] ? xerror() : "cannot be opened");
        n64_vk_driver_done("open_failed");
        return NULL;
    }
    void *gipa = dlsym(handle, "vkGetInstanceProcAddr");
    if (!gipa) {
        fprintf(stderr, "n64: Vulkan RDP: custom driver %s: no vkGetInstanceProcAddr; system driver\n", vkd.id);
        n64_vk_driver_done("open_failed");
        return NULL;
    }
    fprintf(stderr, "n64: Vulkan RDP: custom driver %s opened (%s%s)\n", vkd.id, vkd.dir, lib);
    return gipa;
}

void n64_vk_driver_done(const char *status)
{
    if (!vkd.open)
        return;
    vkd.open = 0;
    char path[VKD_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/trying", vkd.state);
    if (vkd.has_state)
        remove(path);
    last_start(status);
}
