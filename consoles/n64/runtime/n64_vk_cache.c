/* n64_vk_cache.c -- the N64 Vulkan RDP's pipeline cache file (n64_vk_cache.h). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* setenv, fsync, fileno */
#endif
#include "n64_vk_cache.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static const char MAGIC[8] = { 'X', 'R', 'V', 'K', 'P', 'C', '1', '\n' };

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* mkdir -p of path (no trailing '/' needed). 0, or -1 with errno. */
static int make_dirs(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        const int rc = mkdir(path, 0700);
        *p = '/';
        if (rc != 0 && errno != EEXIST)
            return -1;
    }
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return -1;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static int s_ssbo_emulation;  /* n64_vk_cache_turnip_env: the cache variant */
static int s_ssbo_ours;       /* tu_enable_ssbo_emulation was set by n64_vk_cache_turnip_env */
static char s_mesa_ours[1024]; /* the MESA_SHADER_CACHE_DIR n64_vk_cache_mesa_env set, "" none */

#define SSBO_SUBDIR "ssbo-emulation"

int n64_vk_cache_dir(char *out, size_t n)
{
    const char *e = getenv("RECOMP_VK_CACHE_DIR");
    if (!e || !*e || n < 2)
        return 0;
    const size_t len = strlen(e);
    const int slash = e[len - 1] == '/';
    const size_t sub = s_ssbo_emulation ? sizeof SSBO_SUBDIR : 0; /* "/ssbo-emulation" */
    if (len + (slash ? 0 : 1) + sub + 1 > n) {
        fprintf(stderr, "n64: Vulkan RDP: RECOMP_VK_CACHE_DIR is too long, no pipeline cache\n");
        return 0;
    }
    memcpy(out, e, len);
    out[len] = 0;
    size_t l = len;
    while (l > 1 && out[l - 1] == '/')
        out[--l] = 0;
    if (s_ssbo_emulation) {
        memcpy(out + l, "/" SSBO_SUBDIR, sizeof SSBO_SUBDIR + 1);
        l += sizeof SSBO_SUBDIR;
    }
    if (make_dirs(out) != 0) {
        fprintf(stderr, "n64: Vulkan RDP: cannot create %s (%s), no pipeline cache\n", out, strerror(errno));
        return 0;
    }
    out[l] = '/';
    out[l + 1] = 0;
    return 1;
}

static int truthy(const char *v)
{
    return v && (!strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on"));
}

int n64_vk_cache_turnip_env(int wanted_factor)
{
    const char *off = getenv("RECOMP_N64_TURNIP_SSBO_EMULATION");
    const int allowed = !(off && !strcmp(off, "0"));
    const char *have = getenv("tu_enable_ssbo_emulation");
    if (wanted_factor >= 8 && allowed) {
        if (!have || !*have) {
            setenv("tu_enable_ssbo_emulation", "true", 0);
            s_ssbo_ours = 1;
            fprintf(stderr, "n64: Vulkan RDP: tu_enable_ssbo_emulation=true for %dx (Turnip: storage buffers over 128 "
                            "MB; RECOMP_N64_TURNIP_SSBO_EMULATION=0 turns it off)\n", wanted_factor);
        }
    } else if (s_ssbo_ours) {
        unsetenv("tu_enable_ssbo_emulation"); /* ours, from an earlier start at 8x: never for 1x-4x */
        s_ssbo_ours = 0;
    }
    have = getenv("tu_enable_ssbo_emulation");
    s_ssbo_emulation = truthy(have);
    if (s_ssbo_emulation && !s_ssbo_ours)
        fprintf(stderr, "n64: Vulkan RDP: tu_enable_ssbo_emulation=%s from the environment\n", have);
    return s_ssbo_emulation;
}

void n64_vk_cache_mesa_env(void)
{
    const char *have = getenv("MESA_SHADER_CACHE_DIR");
    if (have && *have && strcmp(have, s_mesa_ours) != 0)
        return; /* the player's (or the environment's) own */
    char dir[1024];
    if (!n64_vk_cache_dir(dir, sizeof dir - 8))
        return;
    strcat(dir, "mesa");
    if (make_dirs(dir) != 0) {
        fprintf(stderr, "n64: Vulkan RDP: cannot create %s (%s)\n", dir, strerror(errno));
        return;
    }
    setenv("MESA_SHADER_CACHE_DIR", dir, 1);
    snprintf(s_mesa_ours, sizeof s_mesa_ours, "%s", dir);
}

int n64_vk_cache_file(char *out, size_t n, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE])
{
    char dir[1024];
    if (!n64_vk_cache_dir(dir, sizeof dir))
        return 0;
    char hex[2 * N64_VK_CACHE_UUID_SIZE + 1];
    for (int i = 0; i < N64_VK_CACHE_UUID_SIZE; i++)
        snprintf(hex + 2 * i, 3, "%02x", uuid[i]);
    const int w = snprintf(out, n, "%spipelines-%s.bin", dir, hex);
    return w > 0 && (size_t)w < n;
}

void *n64_vk_cache_load(const char *path, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE], size_t *size, const char **why)
{
    *size = 0;
    *why = "none";
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t h[N64_VK_CACHE_HEADER];
    const size_t got = fread(h, 1, sizeof h, f);
    if (got >= sizeof MAGIC && memcmp(h, MAGIC, sizeof MAGIC) != 0) {
        *why = "not a cache file";
        fclose(f);
        return NULL;
    }
    if (got < sizeof h) {
        *why = got == 0 ? "truncated" : got < sizeof MAGIC ? "not a cache file" : "truncated";
        fclose(f);
        return NULL;
    }
    if (memcmp(h + 8, uuid, N64_VK_CACHE_UUID_SIZE) != 0) {
        *why = "another device or driver";
        fclose(f);
        return NULL;
    }
    const uint64_t n = get64(h + 24), sum = get64(h + 32);
    if (n == 0 || n > N64_VK_CACHE_MAX) {
        *why = n ? "too large" : "truncated";
        fclose(f);
        return NULL;
    }
    uint8_t *data = malloc((size_t)n + 1); /* + 1: a byte past the end tells a longer file */
    if (!data) {
        *why = "out of memory";
        fclose(f);
        return NULL;
    }
    const size_t rd = fread(data, 1, (size_t)n + 1, f);
    const int err = ferror(f);
    fclose(f);
    if (err) {
        *why = "unreadable";
        free(data);
        return NULL;
    }
    if (rd != (size_t)n) {
        *why = rd < (size_t)n ? "truncated" : "not a cache file";
        free(data);
        return NULL;
    }
    if (fnv1a(data, (size_t)n) != sum) {
        *why = "checksum";
        free(data);
        return NULL;
    }
    *size = (size_t)n;
    *why = "ok";
    return data;
}

int n64_vk_cache_save(const char *path, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE], const void *data, size_t size)
{
    char tmp[1100];
    if (snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid()) >= (int)sizeof tmp)
        return -1;
    uint8_t h[N64_VK_CACHE_HEADER];
    memcpy(h, MAGIC, sizeof MAGIC);
    memcpy(h + 8, uuid, N64_VK_CACHE_UUID_SIZE);
    put64(h + 24, size);
    put64(h + 32, fnv1a(data, size));
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "n64: Vulkan RDP: cannot write %s (%s)\n", tmp, strerror(errno));
        return -1;
    }
    int ok = fwrite(h, 1, sizeof h, f) == sizeof h && fwrite(data, 1, size, f) == size && fflush(f) == 0;
    if (ok)
        fsync(fileno(f)); /* the rename must not reach the disk before the bytes */
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        fprintf(stderr, "n64: Vulkan RDP: cannot write %s (%s)\n", path, strerror(errno));
        remove(tmp);
        return -1;
    }
    return 0;
}

/* ---- Start-up count (startup-<factor>x.count) ------------------------------------------------------------------- */
static int count_file(char *out, size_t n, int factor)
{
    char dir[1024];
    if (factor < 1 || factor > 64 || !n64_vk_cache_dir(dir, sizeof dir))
        return 0;
    const int w = snprintf(out, n, "%sstartup-%dx.count", dir, factor);
    return w > 0 && (size_t)w < n;
}

static void uuid_hex(char out[2 * N64_VK_CACHE_UUID_SIZE + 1], const uint8_t uuid[N64_VK_CACHE_UUID_SIZE])
{
    for (int i = 0; i < N64_VK_CACHE_UUID_SIZE; i++)
        snprintf(out + 2 * i, 3, "%02x", uuid[i]);
}

int n64_vk_cache_count_load(int factor, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE])
{
    char path[1100], line[128], hex[2 * N64_VK_CACHE_UUID_SIZE + 1], have[2 * N64_VK_CACHE_UUID_SIZE + 1];
    if (!count_file(path, sizeof path, factor))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    const int got = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    int count = 0;
    if (!got || sscanf(line, "%32s %d", have, &count) != 2)
        return 0;
    uuid_hex(hex, uuid);
    return !strcmp(have, hex) && count > 0 && count < 100000 ? count : 0;
}

int n64_vk_cache_count_save(int factor, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE], int count)
{
    char path[1100], tmp[1200], hex[2 * N64_VK_CACHE_UUID_SIZE + 1];
    if (count <= 0 || !count_file(path, sizeof path, factor))
        return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    uuid_hex(hex, uuid);
    int ok = fprintf(f, "%s %d\n", hex, count) > 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

/* ---- Start-up progress ------------------------------------------------------------------------------------------ */
static int s_pg_active, s_pg_done, s_pg_expected; /* __atomic builtins */

#define PG_LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define PG_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

int n64_vk_progress_total(int built, int expected)
{
    return expected > 0 && built <= expected ? expected : 0;
}

void n64_vk_progress_begin(void)
{
    PG_STORE(s_pg_done, 0);
    PG_STORE(s_pg_expected, 0);
    PG_STORE(s_pg_active, 1);
}

void n64_vk_progress_expect(int expected)
{
    PG_STORE(s_pg_expected, expected > 0 ? expected : 0);
}

void n64_vk_progress_step(void)
{
    if (PG_LOAD(s_pg_active))
        __atomic_add_fetch(&s_pg_done, 1, __ATOMIC_ACQ_REL);
}

int n64_vk_progress_end(void)
{
    PG_STORE(s_pg_active, 0);
    PG_STORE(s_pg_expected, 0);
    return PG_LOAD(s_pg_done);
}

int n64_vk_progress_get(int *done, int *total)
{
    const int active = PG_LOAD(s_pg_active);
    const int d = PG_LOAD(s_pg_done), e = PG_LOAD(s_pg_expected);
    if (done)
        *done = d;
    if (total)
        *total = active ? n64_vk_progress_total(d, e) : 0;
    return active;
}
