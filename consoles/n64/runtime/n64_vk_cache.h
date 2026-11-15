/* n64_vk_cache.h -- where the N64 Vulkan RDP (n64_rdp_vk.cpp) keeps its compiled shaders between starts.
 *
 * Without it, every start compiled the same pipelines again (Ocarina of Time at 4x on the AYN Thor's Turnip: ~1.35 s
 * of the 1.85 s between the driver's open and the start line, and the same mid-game compiles of 8-62 ms every session).
 *
 * RECOMP_VK_CACHE_DIR (the app sets <cache>/vk_cache/<driver id or "system">/, ShellActivity; unset or empty: no file
 * is read or written, as before) holds:
 *   pipelines-<pipelineCacheUUID, 32 hex digits>.bin  the VkPipelineCache's data (Granite's blob: the UUID, a hash,
 *                                                     then the driver's data) behind our own header (below). A driver
 *                                                     update changes the UUID: a new file, the old one stays unused.
 *   mesa/                                             MESA_SHADER_CACHE_DIR, Mesa's own disk cache (Turnip), set
 *                                                     before the driver opens unless the environment already has it.
 *   ssbo-emulation/                                   the same two when Turnip's tu_enable_ssbo_emulation is on (8x,
 *                                                     n64_vk_cache_turnip_env): its shaders differ, its cache apart.
 * The directory is created when missing. The file is written to a temporary name next to it, then renamed: a start
 * killed while writing leaves the previous file whole. A file that is truncated, of another format, of another device
 * or driver (UUID), or whose checksum fails is ignored (logged): the cache starts empty and the file is replaced at the
 * next save.
 *
 * File layout, little-endian: "XRVKPC1\n" (8 bytes), the pipelineCacheUUID (16), the payload's size (u64), the
 * payload's FNV-1a 64 (u64), the payload. Plain C, no Vulkan: the tests run it anywhere (test_vk_driver.c).
 *
 * Next to it, startup-<factor>x.count: "<pipelineCacheUUID hex> <count>\n", how many pipelines the last start at that
 * internal resolution built before the game's first command (the check, then the game's command processor; every
 * pipeline the start waited for, from the cache or not). The next start shows its progress against it ("Compiling
 * shaders... 7/13", n64_vk_progress_*); the app's shader warm-up (n64_rdp.h n64_rdp_vk_warmup) writes it too, and the
 * app reads that it exists. */
#ifndef N64_VK_CACHE_H
#define N64_VK_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N64_VK_CACHE_UUID_SIZE 16
#define N64_VK_CACHE_HEADER 40                 /* magic, UUID, size, checksum */
#define N64_VK_CACHE_MAX ((size_t)256 << 20)   /* a larger payload is not read */

/* $RECOMP_VK_CACHE_DIR with a trailing '/' (and "ssbo-emulation/" after n64_vk_cache_turnip_env returned 1), created
 * when missing (parents too). 0: unset, empty, too long, or it
 * cannot be created (logged). */
int n64_vk_cache_dir(char *out, size_t n);

/* MESA_SHADER_CACHE_DIR=<dir>mesa (created) unless MESA_SHADER_CACHE_DIR is already set to something else than what
 * an earlier call set; nothing without a cache directory. Before the driver is opened (Mesa reads it when the device is created). */
void n64_vk_cache_mesa_env(void);

/* Turnip's tu_enable_ssbo_emulation (Mesa driconf, read from the environment when the driver opens; Mesa 26.2+:
 * maxStorageBufferRange 2 GiB - 1 instead of 128 MiB on a7xx, buffers of 128 MiB or more through bounds-checked global
 * memory). Before the driver opens: for a wanted factor of 8 (a 512 MB upscaled RDRAM binding) it is set to "true"
 * unless already set or RECOMP_N64_TURNIP_SSBO_EMULATION=0; for 1x-4x a value this function set earlier in the
 * process is removed (a player's own value stays). Returns 1 when the variable is now on ("true", "1", "yes", "on"),
 * and then n64_vk_cache_dir is <RECOMP_VK_CACHE_DIR>/ssbo-emulation/ (pipelines and Mesa's cache apart). Logged. */
int n64_vk_cache_turnip_env(int wanted_factor);

/* <dir>pipelines-<uuid hex>.bin. 0: no cache directory. */
int n64_vk_cache_file(char *out, size_t n, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE]);

/* The payload of the file at path when it is whole and of this UUID (malloc'd, *size its bytes), else NULL with *why
 * one of "none" (no file), "truncated", "not a cache file", "another device or driver", "checksum", "too large",
 * "out of memory", "unreadable". */
void *n64_vk_cache_load(const char *path, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE], size_t *size, const char **why);

/* Writes the payload to path through a temporary file and a rename. 0 on success, -1 (logged; the old file stays). */
int n64_vk_cache_save(const char *path, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE], const void *data, size_t size);

/* The start-up count of factor for this UUID (startup-<factor>x.count), 0 when there is none, it is of another UUID or
 * unreadable. */
int n64_vk_cache_count_load(int factor, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE]);

/* Writes it (temporary file and rename). 0, or -1 (no cache directory, count <= 0, cannot write). */
int n64_vk_cache_count_save(int factor, const uint8_t uuid[N64_VK_CACHE_UUID_SIZE], int count);

/* ---- Start-up progress ---------------------------------------------------------------------------------------------
 * While the Vulkan RDP starts (the driver's open, the check's pipelines, the game's command processor) the app shows
 * "Compiling shaders... done/total" (Android: MenuNative.nativeShaderProgress through n64_rdp_shader_progress; a
 * determinate bar when total > 0, a spinner when 0). Thread-safe (atomics): begin / expect / end on the thread that
 * creates the RDP, step on whichever thread built a pipeline, get from any thread (the app's UI thread polls it). */

/* A start begins: active, nothing built, total unknown. */
void n64_vk_progress_begin(void);

/* The count the start is expected to reach (n64_vk_cache_count_load), 0 = unknown. */
void n64_vk_progress_expect(int expected);

/* One more pipeline built (ignored when no start is active). */
void n64_vk_progress_step(void);

/* The start is over: inactive; returns how many pipelines it built. */
int n64_vk_progress_end(void);

/* 1 while a start is active, with *done the pipelines built and *total what is expected; *total is 0 (unknown: a
 * spinner) when nothing is expected or more were built than expected. 0 otherwise (*done the last start's count,
 * *total 0). Either pointer may be NULL. */
int n64_vk_progress_get(int *done, int *total);

/* The pure rule of n64_vk_progress_get: the total shown for built pipelines of expected. */
int n64_vk_progress_total(int built, int expected);

#ifdef __cplusplus
}
#endif

#endif /* N64_VK_CACHE_H */
