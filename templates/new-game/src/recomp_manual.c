/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* g_eax, g_esp, ...: fields of the per-thread g_cpu, named by macros in
 * xbox_memory_layout.h. Never declare them yourself. */
#include "xbox_memory_layout.h"
extern ptrdiff_t g_xbox_mem_offset;

/* What the run meets and the static analysis missed goes to the discovery
 * file for the next prepare (XBA-64, src/kernel/recomp_discovery.h). */
#include "recomp_discovery.h"

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
/* Code made at run time (XBA-118, src/kernel/recomp_smc.h): an indirect
 * call or jump into mapped guest memory outside the image's code is named
 * as such ({"t":"smc","kind":"exec_outside_image"}) rather than as a
 * missing dispatch entry or a wild pointer. Still skipped. */
extern int xbox_SmcExecOutside(uint32_t target, uint32_t ret);

void recomp_icall_fail_log(uint32_t va)
{
    /* The call pushed its real return address: [g_esp] is the caller's next
     * instruction (a tail jump pushed none: then it is the current
     * function's own return address). */
    uint32_t ret = *(volatile uint32_t *)((uintptr_t)g_esp + g_xbox_mem_offset);
    if (xbox_SmcExecOutside(va, ret))
        return;
    recomp_discovery_icall(va, ret);
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static unsigned char smc[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        /* The first time: mapped guest memory (heap, stack, data) is code
         * made at run time, named once by the runtime (the push of the
         * return address is still on the stack); then quiet here. */
        int made = xbox_SmcExecOutside(va, *(volatile uint32_t *)((uintptr_t)g_esp + g_xbox_mem_offset));
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        smc[count] = (unsigned char)made;
        count++;
    }
    if (smc[i])
        return;
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces.
 *
 * Rate-limited per address, at hits 1, 10, 100, 1000 ...: KOTOR's USB
 * interrupt handler runs a `cli`/`sti` pair on every interrupt, which used to
 * fill the first 50 lines -- the whole budget -- within a minute, so any other
 * site reached later was never printed, and on the Thor it was one logcat
 * line per interrupt. The progression still tells a site hit in a loop from
 * one hit once. Past UNIMPL_SLOTS distinct addresses further sites are not
 * printed; the discovery record (when on) still gets every site once. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    enum { UNIMPL_SLOTS = 64 };
    static uint32_t seen[UNIMPL_SLOTS];
    static uint64_t hits[UNIMPL_SLOTS];
    static int count, stop = -1, full_said;
    uint64_t n;
    int i;

    if (stop < 0) {
        const char *trap = getenv("RECOMP_UNIMPL_TRAP");
        stop = trap && *trap && *trap != '0';
    }

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == UNIMPL_SLOTS) {
            recomp_discovery_unimpl(va, text);
            if (!full_said) {
                full_said = 1;
                fprintf(stderr, "[UNIMPL] %d sites reached; further ones are"
                                " not printed\n", count);
                fflush(stderr);
            }
            return;
        }
        seen[count] = va;
        hits[count] = 0;
        count++;
        recomp_discovery_unimpl(va, text);
    }
    n = ++hits[i];
    /* 1, 10, 100, 1000 ... */
    {
        uint64_t k = n;
        while (k >= 10 && k % 10 == 0)
            k /= 10;
        if (k != 1)
            return;
    }
    if (n == 1)
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
    else
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " %llu times\n", text, va, (unsigned long long)n);
    fflush(stderr);
    if (stop) abort();
}
