/*
 * kernel_wait_stats.h - per-site wait accounting for the periodic kernel report
 *
 * A title stuck on a loading screen is usually waiting: on what, from where,
 * and how often was that object set? The periodic [KERNEL] summary answers
 * "which kernel calls"; this answers "which waits", one line per wait site:
 *
 *   [KERNEL] waits: 0x0027B90C set x5040; from 0x0020B9A6 woke x164 of 170, blocked 83900 ms
 *
 * "set" counts KeSetEvent / KePulseEvent / NtSetEvent / NtPulseEvent and the
 * device models' sets (xbox_KeSetGuestEvent) on the object; "from" is the
 * guest return address of the wait call; "woke" counts waits that returned
 * satisfied (status below 0x40), "of" all waits from that site; "blocked" is
 * the host time spent inside those waits (zero-timeout polls are not timed).
 * Wakes far below sets on an event two threads share means wake-ups are lost.
 * Totals are cumulative; a site is listed only when it waited since the last
 * report, ranked by blocked time.
 *
 * Cheap on purpose: two fixed tables (no allocation), relaxed atomics, no
 * lock; a full table drops new keys and says how many. Multi-object waits are
 * filed under their first object and marked "(any of N)" / "(all of N)".
 *
 * RECOMP_WAIT_REPORT  unset: on, top 4 sites per report
 *                     0:     off, nothing counted, no line
 *                     N:     top N sites (1-16)
 */
#ifndef KERNEL_WAIT_STATS_H
#define KERNEL_WAIT_STATS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XBOX_WAIT_STATS_SITES  256   /* (object, site) pairs */
#define XBOX_WAIT_STATS_SETS   128   /* objects that were set */

/* Sites listed per report; 0 when RECOMP_WAIT_REPORT=0. */
int xbox_wait_stats_top(void);

/* Count one set (or pulse) of an object. */
void xbox_wait_stats_set(uint32_t object);

/* Timestamp for xbox_wait_stats_wait, read before the wait: 0 when the
 * accounting is off or the wait is a poll (poll != 0), which is not timed. */
uint64_t xbox_wait_stats_begin(int poll);

/* Count one wait on `object` from guest return address `site` that returned
 * `status`; t0 from xbox_wait_stats_begin. */
void xbox_wait_stats_wait(uint32_t object, uint32_t site, uint32_t status, uint64_t t0);

/* The same for a multiple-object wait on `count` objects filed under the
 * first; the line then says "(any of N)" or "(all of N)". */
void xbox_wait_stats_wait_multi(uint32_t object, uint32_t site, uint32_t status, uint64_t t0,
                                uint32_t count, int wait_all);

/* Write the report lines (each "  [KERNEL] waits: ...\n") into buf and
 * return how many; 0 lines when nothing waited since the last call. Marks
 * the listed state as reported. */
int xbox_wait_stats_format(char *buf, size_t size);

/* xbox_wait_stats_format to a stream. One reporter at a time; a concurrent
 * call returns 0 without printing. */
int xbox_wait_stats_report(FILE *out);

/* Tests: forget everything and read RECOMP_WAIT_REPORT again. Not thread safe. */
void xbox_wait_stats_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_WAIT_STATS_H */
