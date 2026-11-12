/* Guest stack switching: one host stack per guest stack (XBA-101).
 *
 * Lifted code keeps its return path on the host stack: a guest `call` is a C
 * call, a guest `ret` a C return. A title that switches stacks by hand (a
 * fiber or coroutine scheduler: `xchg esp, [mem]` or `mov esp, [mem]`, then a
 * `ret` that continues on the other stack) breaks that: the C return goes
 * back to the caller on the old stack, with the other side's registers.
 *
 * The lifter marks such a function (tools/recomp/translator.py,
 * stack_switch_sites) and, at its ret, hands the switch to
 * recomp_stack_switch_ret(). The runtime gives every guest stack a host
 * context of its own and switches host stacks there: the side being left is
 * suspended in that call, and the side being resumed comes back out of the
 * same call it was suspended in, or, the first time, starts at the address the
 * guest ret pops. Format, rules and limits: docs/technical/stack-switching.md.
 *
 * Declared again for the generated code in templates/runtime/recomp_types.h. */
#ifndef XBOXRECOMP_RECOMP_STACK_SWITCH_H
#define XBOXRECOMP_RECOMP_STACK_SWITCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called by a lifted function at its ret when it loaded esp from memory
 * before it. g_esp points at the return address the guest ret pops.
 *   saved_esp   esp when the function left its stack (what it stored)
 *   loaded_esp  esp right after it loaded the other one
 *   entry_ret   the return address it was called with (MEM32(esp) at entry)
 *   ret_imm     N of its `ret N`
 *   site        the guest address of the esp load
 * Returns when this side is resumed, with g_esp pointing at the return
 * address again; the caller then pops it as usual. Returns at once, the old
 * behaviour, when switching is off or the host has no context switch. */
void recomp_stack_switch_ret(uint32_t saved_esp, uint32_t loaded_esp, uint32_t entry_ret,
                             uint32_t ret_imm, uint32_t site);

/* A stack switch the lifter recognised but cannot carry (no ret after it in
 * the function). Records it once in the discovery file and the log. */
void recomp_stack_switch_unhandled(uint32_t site);

/* The host context the calling thread runs on: 0 on the thread's own stack,
 * otherwise a number unique for the process's life. A native longjmp is only
 * valid to a buffer armed in the same context (docs/technical/stack-switching.md
 * « Limits »). */
uint32_t recomp_stack_context(void);

/* 1 when this host can switch stacks (POSIX arm64 / x86-64) and
 * RECOMP_STACK_SWITCH is not 0. */
int recomp_stack_switch_enabled(void);

/* Tests: contexts alive on the calling thread (its own stack included once
 * switching has started), and contexts started since the process began. */
unsigned recomp_stack_switch_live(void);
unsigned recomp_stack_switch_started(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_STACK_SWITCH_H */
