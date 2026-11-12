/*
 * kernel_stub_report.c - the first call of a stub kernel route is reported
 * (XBA-115). See kernel_stubs.h.
 */
#include "kernel.h"
#include "kernel_stubs.h"
#include "recomp_discovery.h"

#include <stdio.h>

#define MAX_ORD 512

static volatile LONG s_calls[MAX_ORD];
static uint8_t s_is_stub[MAX_ORD];
static volatile LONG s_built;            /* 0, 1 building, 2 built */

int xbox_kernel_is_stub(uint32_t ordinal)
{
    if (s_built != 2) {
        if (InterlockedCompareExchange(&s_built, 1, 0) == 0) {
            unsigned i;
            for (i = 0; i < g_kernel_stub_count; i++)
                if (g_kernel_stub_table[i].ordinal < MAX_ORD)
                    s_is_stub[g_kernel_stub_table[i].ordinal] = 1;
            InterlockedExchange(&s_built, 2);   /* a full barrier */
        } else {
            while (InterlockedCompareExchange(&s_built, 2, 2) != 2)
                SwitchToThread();
        }
    }
    return ordinal < MAX_ORD && s_is_stub[ordinal];
}

const char *xbox_kernel_stub_name(uint32_t ordinal)
{
    unsigned i;
    for (i = 0; i < g_kernel_stub_count; i++)
        if (g_kernel_stub_table[i].ordinal == ordinal)
            return g_kernel_stub_table[i].name;
    return NULL;
}

uint32_t xbox_kernel_stub_calls(uint32_t ordinal)
{
    return ordinal < MAX_ORD ? (uint32_t)s_calls[ordinal] : 0;
}

/* 1, 10, 100, 1000 ...: the ICALL and [UNIMPL] rate limit (K-10). */
static int is_power_of_ten(uint32_t n)
{
    while (n >= 10 && n % 10 == 0)
        n /= 10;
    return n == 1;
}

void xbox_kernel_stub_note(uint32_t ordinal, int has_bridge, uint32_t ret_va)
{
    uint32_t n;
    const char *name;

    if (ordinal >= MAX_ORD)
        return;
    n = (uint32_t)InterlockedIncrement(&s_calls[ordinal]);
    if (!is_power_of_ten(n))
        return;
    name = xbox_kernel_stub_name(ordinal);
    fprintf(stderr, "  [KERNEL] STUB ordinal %u (%s) called from 0x%08X (call #%u)%s\n",
            ordinal, name ? name : "?", ret_va, n,
            has_bridge ? "" : " -- no bridge at all");
    fflush(stderr);
    if (n == 1)
        recomp_discovery_stub(ordinal, name, ret_va);
}
