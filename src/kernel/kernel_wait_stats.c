/*
 * kernel_wait_stats.c - per-site wait accounting (see kernel_wait_stats.h)
 *
 * Writers are the guest threads inside wait and set bridges; they run with
 * the guest CPU gate released, so several can be here at once. Every counter
 * is a relaxed atomic add, a slot is claimed with one compare-and-swap, and
 * nothing is ever freed, so no lock is needed. The reporter reads the
 * counters without stopping anyone: a line can be one wait behind.
 */
#include "kernel_wait_stats.h"
#include "platform/host_time.h"

#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define ADD32(p, v)      ((void)_InterlockedExchangeAdd((volatile long *)(p), (long)(v)))
#define ADD64(p, v)      ((void)_InterlockedExchangeAdd64((volatile __int64 *)(p), (__int64)(v)))
#define LOAD32(p)        ((uint32_t)_InterlockedOr((volatile long *)(p), 0))
#define LOAD64(p)        ((uint64_t)_InterlockedOr64((volatile __int64 *)(p), 0))
#define CAS32(p, o, n)   ((uint32_t)_InterlockedCompareExchange((volatile long *)(p), (long)(n), (long)(o)) == (uint32_t)(o))
#define CAS64(p, o, n)   ((uint64_t)_InterlockedCompareExchange64((volatile __int64 *)(p), (__int64)(n), (__int64)(o)) == (uint64_t)(o))
#else
#define ADD32(p, v)      ((void)__atomic_fetch_add((p), (uint32_t)(v), __ATOMIC_RELAXED))
#define ADD64(p, v)      ((void)__atomic_fetch_add((p), (uint64_t)(v), __ATOMIC_RELAXED))
#define LOAD32(p)        __atomic_load_n((p), __ATOMIC_RELAXED)
#define LOAD64(p)        __atomic_load_n((p), __ATOMIC_RELAXED)
#define CAS32(p, o, n)   __extension__ ({ uint32_t _o = (o); \
        __atomic_compare_exchange_n((p), &_o, (uint32_t)(n), 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED); })
#define CAS64(p, o, n)   __extension__ ({ uint64_t _o = (o); \
        __atomic_compare_exchange_n((p), &_o, (uint64_t)(n), 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED); })
#endif

#define MAX_PROBES 32
#define MAX_TOP    16

typedef struct {
    volatile uint64_t key;          /* object << 32 | site; 0 = free */
    volatile uint32_t waits;
    volatile uint32_t wakes;
    volatile uint64_t blocked_ns;
    uint32_t reported_waits;        /* reporter only */
} WaitSite;

typedef struct {
    volatile uint32_t object;       /* 0 = free */
    volatile uint32_t sets;
} SetCount;

static WaitSite g_sites[XBOX_WAIT_STATS_SITES];
static SetCount g_sets[XBOX_WAIT_STATS_SETS];
static volatile uint32_t g_dropped_waits, g_dropped_sets;
static uint32_t g_reported_dropped;     /* reporter only */
static volatile uint32_t g_reporting;
static volatile int g_top = -1;

int xbox_wait_stats_top(void)
{
    int top = g_top;
    if (top < 0) {
        const char *e = getenv("RECOMP_WAIT_REPORT");
        top = 4;
        if (e && *e) {
            long v = strtol(e, NULL, 0);
            top = v <= 0 ? 0 : v > MAX_TOP ? MAX_TOP : (int)v;
        }
        g_top = top;
    }
    return top;
}

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static SetCount *set_slot(uint32_t object, int create)
{
    uint32_t i = hash32(object), n;
    for (n = 0; n < MAX_PROBES; n++, i++) {
        SetCount *s = &g_sets[i % XBOX_WAIT_STATS_SETS];
        uint32_t k = LOAD32(&s->object);
        if (k == object)
            return s;
        if (k == 0) {
            if (!create)
                return NULL;
            if (CAS32(&s->object, 0u, object) || LOAD32(&s->object) == object)
                return s;
        }
    }
    return NULL;
}

void xbox_wait_stats_set(uint32_t object)
{
    SetCount *s;
    if (!object || !xbox_wait_stats_top())
        return;
    s = set_slot(object, 1);
    if (s)
        ADD32(&s->sets, 1);
    else
        ADD32(&g_dropped_sets, 1);
}

uint64_t xbox_wait_stats_begin(int poll)
{
    if (poll || !xbox_wait_stats_top())
        return 0;
    return xbox_HostNowNs();
}

void xbox_wait_stats_wait(uint32_t object, uint32_t site, uint32_t status, uint64_t t0)
{
    uint64_t key = (uint64_t)object << 32 | site;
    uint32_t i, n;

    if (!key || !xbox_wait_stats_top())
        return;
    i = hash32(object ^ hash32(site));
    for (n = 0; n < MAX_PROBES; n++, i++) {
        WaitSite *w = &g_sites[i % XBOX_WAIT_STATS_SITES];
        uint64_t k = LOAD64(&w->key);
        if (k == 0 && (CAS64(&w->key, 0u, key) || LOAD64(&w->key) == key))
            k = key;
        if (k != key)
            continue;
        if (t0) {
            uint64_t now = xbox_HostNowNs();
            if (now > t0)
                ADD64(&w->blocked_ns, now - t0);
        }
        if (status < 0x40u)         /* STATUS_WAIT_0 + n, STATUS_ABANDONED is 0x80 */
            ADD32(&w->wakes, 1);
        ADD32(&w->waits, 1);
        return;
    }
    ADD32(&g_dropped_waits, 1);
}

int xbox_wait_stats_format(char *buf, size_t size)
{
    int top = xbox_wait_stats_top(), picked[MAX_TOP], npicked = 0, lines = 0, i, j;
    size_t used = 0;
    uint32_t dropped;

    if (size)
        buf[0] = 0;
    if (!top)
        return 0;
    /* Top sites among those that waited since the last report, by blocked time. */
    for (i = 0; i < XBOX_WAIT_STATS_SITES; i++) {
        uint64_t blocked;
        if (!LOAD64(&g_sites[i].key) || LOAD32(&g_sites[i].waits) == g_sites[i].reported_waits)
            continue;
        blocked = LOAD64(&g_sites[i].blocked_ns);
        for (j = npicked; j > 0 && LOAD64(&g_sites[picked[j - 1]].blocked_ns) < blocked; j--)
            if (j < top)
                picked[j] = picked[j - 1];
        if (j < top) {
            picked[j] = i;
            if (npicked < top)
                npicked++;
        }
    }
    for (i = 0; i < npicked; i++) {
        WaitSite *w = &g_sites[picked[i]];
        uint64_t key = LOAD64(&w->key);
        uint32_t object = (uint32_t)(key >> 32), site = (uint32_t)key;
        SetCount *s = set_slot(object, 0);
        char sets[32] = "";
        int n;
        if (s)
            snprintf(sets, sizeof sets, " set x%u;", (unsigned)LOAD32(&s->sets));
        n = snprintf(buf + used, used < size ? size - used : 0,
                     "  [KERNEL] waits: 0x%08X%s from 0x%08X woke x%u of %u, blocked %llu ms\n",
                     (unsigned)object, sets, (unsigned)site, (unsigned)LOAD32(&w->wakes),
                     (unsigned)LOAD32(&w->waits),
                     (unsigned long long)(LOAD64(&w->blocked_ns) / 1000000u));
        if (n > 0 && used + (size_t)n < size) {
            used += (size_t)n;
            lines++;
        }
    }
    dropped = LOAD32(&g_dropped_waits) + LOAD32(&g_dropped_sets);
    if (dropped != g_reported_dropped && lines) {
        int n = snprintf(buf + used, used < size ? size - used : 0,
                         "  [KERNEL] waits: tables full, %u waits and %u sets not counted\n",
                         (unsigned)LOAD32(&g_dropped_waits), (unsigned)LOAD32(&g_dropped_sets));
        if (n > 0 && used + (size_t)n < size) {
            used += (size_t)n;
            lines++;
            g_reported_dropped = dropped;
        }
    }
    for (i = 0; i < XBOX_WAIT_STATS_SITES; i++)
        g_sites[i].reported_waits = LOAD32(&g_sites[i].waits);
    return lines;
}

int xbox_wait_stats_report(FILE *out)
{
    char buf[(MAX_TOP + 1) * 112];
    int lines;

    if (!xbox_wait_stats_top() || !CAS32(&g_reporting, 0u, 1u))
        return 0;
    lines = xbox_wait_stats_format(buf, sizeof buf);
    if (lines)
        fputs(buf, out);
    g_reporting = 0;
    return lines;
}

void xbox_wait_stats_reset(void)
{
    memset(g_sites, 0, sizeof g_sites);
    memset(g_sets, 0, sizeof g_sets);
    g_dropped_waits = g_dropped_sets = 0;
    g_reported_dropped = 0;
    g_reporting = 0;
    g_top = -1;
}
