/* Shader compile progress for the window's badge (host_shader_progress.h, XBA-314). Every field is an atomic word:
 * the reporters are the renderers' threads (a GL executor, a shader worker), the reader the window's thread. A read
 * may see a total and a done from two moments; the text clamps it (never "13/12"). */
#include "host_shader_progress.h"

#include <stdio.h>
#include <time.h>

static uint32_t s_total, s_done;  /* the batch with a known total; done >= total: no batch */
static uint32_t s_live;           /* live compiles since the badge last went away */
static uint64_t s_live_ms;        /* the last one, monotonic ms */

uint64_t host_shader_progress_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void host_shader_progress_expect(uint32_t n)
{
    if (!n)
        return;
    /* A finished batch: a new one starts from 0. */
    if (__atomic_load_n(&s_done, __ATOMIC_ACQUIRE) >= __atomic_load_n(&s_total, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_done, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_total, 0, __ATOMIC_RELEASE);
    }
    __atomic_add_fetch(&s_total, n, __ATOMIC_ACQ_REL);
}

void host_shader_progress_done(uint32_t n)
{
    uint32_t total = __atomic_load_n(&s_total, __ATOMIC_ACQUIRE);
    uint32_t done = __atomic_load_n(&s_done, __ATOMIC_ACQUIRE);

    if (!n || done >= total)  /* no batch: nothing to count against */
        return;
    __atomic_add_fetch(&s_done, n, __ATOMIC_ACQ_REL);
}

void host_shader_progress_set(uint32_t done, uint32_t total)
{
    if (done > total)
        done = total;
    __atomic_store_n(&s_total, total, __ATOMIC_RELEASE);
    __atomic_store_n(&s_done, done, __ATOMIC_RELEASE);
}

void host_shader_progress_live_at(uint64_t now_ms)
{
    uint64_t last = __atomic_load_n(&s_live_ms, __ATOMIC_ACQUIRE);

    if (!last || now_ms - last >= HOST_SHADER_PROGRESS_LINGER_MS)
        __atomic_store_n(&s_live, 1, __ATOMIC_RELEASE);  /* the badge had gone: count again */
    else
        __atomic_add_fetch(&s_live, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s_live_ms, now_ms ? now_ms : 1, __ATOMIC_RELEASE);
}

void host_shader_progress_live(void)
{
    host_shader_progress_live_at(host_shader_progress_now_ms());
}

void host_shader_progress_reset(void)
{
    __atomic_store_n(&s_total, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_live, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_live_ms, 0, __ATOMIC_RELEASE);
}

int host_shader_progress_text_at(uint64_t now_ms, char *out, size_t n)
{
    uint32_t total = __atomic_load_n(&s_total, __ATOMIC_ACQUIRE);
    uint32_t done = __atomic_load_n(&s_done, __ATOMIC_ACQUIRE);
    uint32_t live = __atomic_load_n(&s_live, __ATOMIC_ACQUIRE);
    uint64_t last = __atomic_load_n(&s_live_ms, __ATOMIC_ACQUIRE);

    if (!n)
        return 0;
    out[0] = 0;
    if (total && done < total) {
        /* The one compiling now, counted from 1: "1/304" at the start, "304/304" for the last. */
        snprintf(out, n, "Shaders %u/%u", (unsigned)(done + 1), (unsigned)total);
        return 1;
    }
    if (live && last && now_ms >= last && now_ms - last < HOST_SHADER_PROGRESS_LINGER_MS) {
        snprintf(out, n, "Shaders: compiling... (%u)", (unsigned)live);
        return 1;
    }
    return 0;
}

int host_shader_progress_text(char *out, size_t n)
{
    return host_shader_progress_text_at(host_shader_progress_now_ms(), out, n);
}
