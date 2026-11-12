/*
 * Per-site wait accounting (kernel_wait_stats.h), the [KERNEL] waits lines of
 * the periodic summary:
 *
 *   1. sets per object and waits per (object, return address): wakes, waits,
 *      blocked time; the line carries the object's set count
 *   2. sites ranked by blocked time, at most the top N; a site that did not
 *      wait since the last report is left out; nothing waited, no line
 *   3. a zero-timeout poll is counted but not timed
 *   4. a full table drops new keys, says so, and keeps counting the old ones
 *   5. four threads counting on one site lose nothing (lock-free adds)
 *
 * With RECOMP_WAIT_REPORT=0 (the _off case): nothing is counted, no line.
 * Cross-platform: calls the accounting directly, no guest code.
 */
#include "kernel.h"
#include "kernel_wait_stats.h"
#include "platform/host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;
static char buf[8192];

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) {
        failures++;
        printf("--- report ---\n%s--------------\n", buf);
    }
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (; (hay = strstr(hay, needle)) != NULL; hay += strlen(needle))
        n++;
    return n;
}

/* A wait that "blocked" for ms milliseconds: begin's timestamp moved back. */
static void wait_for(uint32_t object, uint32_t site, uint32_t status, unsigned ms)
{
    uint64_t t0 = xbox_wait_stats_begin(0);
    if (t0)
        t0 -= (uint64_t)ms * 1000000u;
    xbox_wait_stats_wait(object, site, status, t0);
}

#define HAMMER 100000
static DWORD WINAPI hammer(LPVOID arg)
{
    int i;
    (void)arg;
    for (i = 0; i < HAMMER; i++) {
        xbox_wait_stats_set(0x00300000u);
        xbox_wait_stats_wait(0x00300000u, 0x00012345u, 0, xbox_wait_stats_begin(1));
    }
    return 0;
}

int main(void)
{
    const char *e = getenv("RECOMP_WAIT_REPORT");
    int lines, i;

    setvbuf(stdout, NULL, _IONBF, 0);
    xbox_wait_stats_reset();

    if (e && *e == '0') {
        for (i = 0; i < 10; i++) {
            xbox_wait_stats_set(0x0027B90Cu);
            wait_for(0x0027B90Cu, 0x0020B9A6u, 0, 5);
        }
        check(xbox_wait_stats_top() == 0, "RECOMP_WAIT_REPORT=0: off");
        check(xbox_wait_stats_begin(0) == 0, "no timestamp taken");
        check(xbox_wait_stats_format(buf, sizeof buf) == 0 && !buf[0], "no line");
        check(xbox_wait_stats_report(stdout) == 0, "report prints nothing");
        printf(failures ? "%d FAILED\n" : "all passed\n", failures);
        return failures ? 1 : 0;
    }

    check(xbox_wait_stats_top() == 4, "default: on, top 4 sites");

    /* 1. Black's shape: one vblank event set every frame, two waiters. */
    for (i = 0; i < 5040; i++)
        xbox_wait_stats_set(0x0027B90Cu);
    for (i = 0; i < 170; i++)
        wait_for(0x0027B90Cu, 0x0020B9A6u, i < 164 ? 0 : 0x102u /* STATUS_TIMEOUT */, 500);
    for (i = 0; i < 30; i++)
        wait_for(0x0027B90Cu, 0x001FBDD9u, 0, 100);
    lines = xbox_wait_stats_format(buf, sizeof buf);
    check(lines == 2, "two sites, two lines");
    check(strstr(buf, "  [KERNEL] waits: 0x0027B90C set x5040; from 0x0020B9A6 woke x164 of 170, blocked 85")
          != NULL, "main loop: set x5040, woke x164 of 170, ~85000 ms");
    check(strstr(buf, "from 0x001FBDD9 woke x30 of 30, blocked 30") != NULL, "stream thread: x30, ~3000 ms");
    check(strstr(buf, "0x0020B9A6") < strstr(buf, "0x001FBDD9"), "ranked by blocked time");

    /* 2. Only sites that waited since the last report; none, no line. */
    check(xbox_wait_stats_format(buf, sizeof buf) == 0 && !buf[0], "nothing waited since: no line");
    wait_for(0x0027B90Cu, 0x001FBDD9u, 0, 1);
    lines = xbox_wait_stats_format(buf, sizeof buf);
    check(lines == 1 && strstr(buf, "from 0x001FBDD9 woke x31 of 31") && !strstr(buf, "0x0020B9A6"),
          "one site waited: it alone, totals cumulative");
    for (i = 0; i < 8; i++)
        wait_for(0x00400000u + 16u * i, 0x00050000u, 0, 10u * (i + 1));
    lines = xbox_wait_stats_format(buf, sizeof buf);
    check(lines == 4, "eight sites waited: the top four listed");
    check(strstr(buf, "0x00400070") && strstr(buf, "0x00400040") && !strstr(buf, "0x00400030"),
          "the four with the most blocked time");
    check(strstr(buf, "0x00400070") < strstr(buf, "0x00400060"), "in order");
    check(!strstr(buf, " set x") , "an object never set has no set count");

    /* 2b. A multiple-object wait is marked. */
    xbox_wait_stats_wait_multi(0x00600000u, 0x00061000u, 1, xbox_wait_stats_begin(0), 3, 0);
    xbox_wait_stats_wait_multi(0x00600000u, 0x00062000u, 0x102u, xbox_wait_stats_begin(0), 2, 1);
    xbox_wait_stats_format(buf, sizeof buf);
    check(strstr(buf, "from 0x00061000 (any of 3) woke x1 of 1") && strstr(buf, "from 0x00062000 (all of 2) woke x0 of 1"),
          "multiple-object waits: (any of 3), (all of 2)");

    /* 3. A poll is counted, not timed. */
    for (i = 0; i < 1000; i++)
        xbox_wait_stats_wait(0x00500000u, 0x00060000u, 0x102u, xbox_wait_stats_begin(1));
    lines = xbox_wait_stats_format(buf, sizeof buf);
    check(lines == 1 && strstr(buf, "0x00500000 from 0x00060000 woke x0 of 1000, blocked 0 ms"),
          "1000 polls: woke x0 of 1000, blocked 0 ms");

    /* 4. Full tables. */
    for (i = 0; i < 2 * XBOX_WAIT_STATS_SITES; i++)
        wait_for(0x01000000u + 4u * i, 0x00070000u, 0, 1);
    for (i = 0; i < 2 * XBOX_WAIT_STATS_SETS; i++)
        xbox_wait_stats_set(0x02000000u + 4u * i);
    wait_for(0x0027B90Cu, 0x0020B9A6u, 0, 1);
    xbox_wait_stats_set(0x0027B90Cu);
    lines = xbox_wait_stats_format(buf, sizeof buf);
    check(strstr(buf, "set x5041; from 0x0020B9A6 woke x165 of 171") != NULL, "old keys still counted");
    check(count_of(buf, "tables full,") == 1, "drops reported");
    wait_for(0x0027B90Cu, 0x0020B9A6u, 0, 1);
    xbox_wait_stats_format(buf, sizeof buf);
    check(!strstr(buf, "tables full"), "drops reported once while unchanged");

    /* 5. Concurrent writers. */
    xbox_wait_stats_reset();
    {
        HANDLE t[4];
        char want[128];
        for (i = 0; i < 4; i++)
            t[i] = CreateThread(NULL, 0, hammer, NULL, 0, NULL);
        for (i = 0; i < 4; i++) {
            WaitForSingleObject(t[i], INFINITE);
            CloseHandle(t[i]);
        }
        xbox_wait_stats_format(buf, sizeof buf);
        snprintf(want, sizeof want, "0x00300000 set x%d; from 0x00012345 woke x%d of %d,",
                 4 * HAMMER, 4 * HAMMER, 4 * HAMMER);
        check(strstr(buf, want) != NULL, "4 threads x 100000: no count lost");
    }

    /* The cost the hot path adds: one timed wait, one poll, one set. */
    {
        uint64_t a = xbox_HostNowNs(), b;
        for (i = 0; i < 1000000; i++)
            xbox_wait_stats_wait(0x0027B90Cu, 0x0020B9A6u, 0, xbox_wait_stats_begin(0));
        b = xbox_HostNowNs();
        printf("  cost: timed wait %.1f ns", (double)(b - a) / 1e6);
        a = xbox_HostNowNs();
        for (i = 0; i < 1000000; i++)
            xbox_wait_stats_wait(0x0027B90Cu, 0x0020B9A6u, 0, xbox_wait_stats_begin(1));
        b = xbox_HostNowNs();
        printf(", poll %.1f ns", (double)(b - a) / 1e6);
        a = xbox_HostNowNs();
        for (i = 0; i < 1000000; i++)
            xbox_wait_stats_set(0x0027B90Cu);
        b = xbox_HostNowNs();
        printf(", set %.1f ns\n", (double)(b - a) / 1e6);
    }

    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
