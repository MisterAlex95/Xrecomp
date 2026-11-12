/*
 * Guest exception dispatch, unwind and resume (XBA-112), end to end on lifted
 * code: fixture.s is our own x86 with MSVC-shaped EH3 and C++ frames and our
 * own minimal frame handlers; fixture_gen.c is what tools/recomp makes of it
 * (tools/recomp/seh_fixture.py). This file is the kernel's side -- RAISE and
 * UNWIND bridge into src/kernel/xbox_seh.c as RtlRaiseException and
 * RtlUnwind do -- and the scenarios:
 *
 *  - __except resumes across frames, and the __finally of the frame in
 *    between runs during the unwind;
 *  - a filter that continues execution: the raise returns, and the normal
 *    path calls the __finally directly (a funclet entry);
 *  - a filter that searches on: unhandled, the raise returns;
 *  - a resume whose anchor is on another host stack is refused, reported,
 *    and the raise returns instead of jumping;
 *  - a frame on another guest stack is not followed;
 *  - C++: a throw across a frame with a destructor (unwind funclet) into a
 *    catch-all whose continuation is resumed.
 *
 * Each checks the result, the guest stack pointer, the chain head and the
 * dispatcher's counters. Runs the same on x86-64 and arm64 (qemu in CI).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_seh.h"

/* The guest state, as the generated code and the runtime both see it. */
#include "xbox_memory_layout.h"

extern ptrdiff_t g_xbox_mem_offset;

typedef void (*recomp_func_t)(void);
extern const uint32_t seh_fixture_text_va, seh_fixture_text_size;
extern const unsigned char seh_fixture_text[];
extern const uint32_t seh_fixture_rdata_va, seh_fixture_rdata_size;
extern const unsigned char seh_fixture_rdata[];
void f_outer(void);
void f_cxx(void);

#define GUEST_SIZE   0x00200000u
#define TIB_VA       0x00004000u
#define STACK_LO     0x00100000u
#define STACK_HI     0x00180000u
#define COUNTER      0x00030000u
#define FILTER       0x00030004u
#define SEEN_CODE    0x00030008u
#define END_OF_CHAIN 0xFFFFFFFFu

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))

static int failures;
static int flip_host_stack;     /* RAISE runs under another host stack token */
static int other_guest_stack;   /* RAISE runs with another guest stack's bounds */

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

/* The game project provides these (templates/new-game/src/recomp_manual.c). */
void recomp_icall_fail_log(uint32_t va);
void recomp_icall_not_code_log(uint32_t va);
void recomp_icall_fail_log(uint32_t va)
{
    printf("  [icall] unresolved 0x%08X\n", va);
}
void recomp_icall_not_code_log(uint32_t va)
{
    printf("  [icall] not code 0x%08X\n", va);
}

/* RtlRaiseException(record), stdcall: what bridge_raise_record does. */
void sub_0001F000(void)
{
    uint32_t ret = G32(g_esp), rec = G32(g_esp + 4);

    if (flip_host_stack)
        xbox_SehSetHostStack(7);
    if (other_guest_stack)
        xbox_SehSetGuestStack(0x00001000u, 0x00002000u);
    xbox_SehRaiseRecord(rec, ret, g_esp + 8);
    xbox_SehSetHostStack(0);
    xbox_SehSetGuestStack(STACK_LO, STACK_HI);
    g_esp += 8;
    g_eax = 0;
}

/* RtlUnwind(frame, ip, record, value), stdcall: what bridge_RtlUnwind does. */
void sub_0001F010(void)
{
    uint32_t ret = G32(g_esp), frame = G32(g_esp + 4);
    uint32_t rec = G32(g_esp + 12), value = G32(g_esp + 16);

    xbox_SehUnwind(frame, rec, ret);
    if (frame && frame != END_OF_CHAIN)
        G32(TIB_VA) = frame;
    g_esp += 20;
    g_eax = value;
}

/* A guest call from the host: push a return address, call, check balance. */
static uint32_t guest_call(recomp_func_t fn, uint32_t *esp_after)
{
    g_esp = STACK_HI - 0x100;
    g_esp -= 4;
    G32(g_esp) = 0xDEAD0000u;
    fn();
    *esp_after = g_esp;
    return g_eax;
}

static void reset(int32_t filter)
{
    G32(COUNTER) = 0;
    G32(FILTER) = (uint32_t)filter;
    G32(SEEN_CODE) = 0;
    G32(TIB_VA) = END_OF_CHAIN;
    flip_host_stack = other_guest_stack = 0;
    {
        XboxSehStats s;
        xbox_SehStatsGet(&s, 1);
    }
}

int main(void)
{
    uint8_t *mem = (uint8_t *)calloc(1, GUEST_SIZE);
    uint32_t eax, esp;
    XboxSehStats st;

    if (!mem) {
        printf("no memory\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)mem;
    memcpy(mem + seh_fixture_text_va, seh_fixture_text, seh_fixture_text_size);
    memcpy(mem + seh_fixture_rdata_va, seh_fixture_rdata, seh_fixture_rdata_size);
    g_fs_base = TIB_VA;
    xbox_SehSetGuestStack(STACK_LO, STACK_HI);

    printf("seh_dispatch: lifted __try/__except, __finally, C++ catch\n");

    /* 1. Filter says execute: resume at f_outer's __except, across f_inner,
     *    whose __finally runs during the unwind. */
    reset(1);
    eax = guest_call(f_outer, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x222, "__except block ran (eax 0x222)");
    check(G32(SEEN_CODE) == 0xE0000001u, "filter saw the raised code");
    check(G32(COUNTER) == 1, "inner __finally ran once, during the unwind");
    check(esp == STACK_HI - 0x100, "guest stack balanced after the resume");
    check(G32(TIB_VA) == END_OF_CHAIN, "chain head restored");
    check(st.resumes == 1 && st.refused == 0, "one resume, nothing refused");
    check(st.anchors >= 1, "f_outer took an anchor");

    /* 2. Filter continues execution: the raise returns, the normal path
     *    calls the __finally directly. */
    reset(-1);
    eax = guest_call(f_outer, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x111, "continue-execution: the try body finished (0x111)");
    check(G32(COUNTER) == 1, "inner __finally ran once, on the normal path");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
          "stack and chain balanced");
    check(st.handled == 1 && st.resumes == 0, "handled by continuing");

    /* 3. Filter searches on: nothing handles it, the raise returns. */
    reset(0);
    eax = guest_call(f_outer, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x111 && G32(COUNTER) == 1, "unhandled: the raise returned");
    check(st.unhandled == 1 && st.handled == 0, "counted as unhandled");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
          "stack and chain balanced");

    /* 4. The resume target's anchor was taken on another host stack (a
     *    fiber, XBA-101): refused and reported, never jumped to. The
     *    handler's unwind already ran the __finally; the raise returns and
     *    f_inner's normal path runs it again, as it would on the console
     *    after an unhandled raise returns. */
    reset(1);
    flip_host_stack = 1;
    eax = guest_call(f_outer, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x111, "other host stack: refused, the raise returned");
    check(st.refused == 1 && st.resumes == 0, "counted as refused");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
          "stack and chain balanced");

    /* 5. The chain's frames lie outside the current guest stack: not
     *    followed, no filter runs. */
    reset(1);
    other_guest_stack = 1;
    eax = guest_call(f_outer, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x111 && G32(SEEN_CODE) == 0,
          "other guest stack: no handler called, the raise returned");
    check(st.refused == 1 && st.unhandled == 1, "counted as refused, unhandled");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
          "stack and chain balanced");

    /* 6. C++: throw through f_cxx_inner (destructor funclet) into
     *    f_cxx's catch-all, resumed at the continuation it returns. */
    reset(0);
    eax = guest_call(f_cxx, &esp);
    xbox_SehStatsGet(&st, 0);
    check(eax == 0x444, "catch continuation ran (eax 0x444)");
    check(G32(COUNTER) == 0x10, "inner destructor ran once, during the unwind");
    check(esp == STACK_HI - 0x100 && G32(TIB_VA) == END_OF_CHAIN,
          "stack and chain balanced");
    check(st.resumes == 1, "one resume");

    printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    free(mem);
    return failures ? 1 : 0;
}
