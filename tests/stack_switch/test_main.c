/* Guest stack switching (XBA-101): recomp_stack_switch.c against guest code
 * in the shape the lifter emits for it.
 *
 * The switching function is the one tools/recomp lifts from
 *
 *     push ebx; push ebp; push esi; push edi
 *     xchg esp, [slot]
 *     pop edi; pop esi; pop ebp; pop ebx
 *     ret
 *
 * (Fable's scheduler without the cli/sti and fs:[0]), copied from the
 * translator's output; the other guest functions are written the same way.
 * Where the sti was, the bytes just popped are written over, as an interrupt
 * handler let in there does: the runtime must not tell contexts apart by them.
 *
 *   1. two coroutines: the first switch starts the other side at the address
 *      its ret pops, every later one resumes it, and each side comes back out
 *      of the call it left with its esp and registers
 *   2. copy-stack fibers (Fable's shape): two fibers parked at the same esp and
 *      the same return address, told apart by their stack bytes; then two
 *      that only differ past their first 64 bytes (the same job under the same
 *      frames with the same registers, Fable's loader fibers); a resume that
 *      matches neither is recorded in the discovery file
 *   3. a coroutine whose body returns into an exit routine continues there
 *   4. a switch to a place no context is parked at and no function starts is
 *      the old behaviour, recorded in the discovery file
 *   5. the same coroutines on another host thread: its own contexts
 *   every switch hands the exception dispatcher (xbox_seh.h, stubbed here) the
 *   context id as host stack token and the guest stack's bounds: the root's
 *   own back when it resumes, and for a new stack in a heap block, its top
 *   (scenario 1's B; scenario 5's B is not in one: unknown)
 *   off: RECOMP_STACK_SWITCH=0 (argv[1] = "off") is the old behaviour
 *   RECOMP_STACK_SWITCH_WHOLE=0: twins by their first 64 bytes only, which
 *   mixes up the fibers alike over them (checked as such)
 */
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include "recomp_stack_switch.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

RECOMP_TLS RecompCpu g_cpu;
ptrdiff_t g_xbox_mem_offset;
volatile int g_recomp_cas64_lock;   /* arm64: the unaligned atomics' lock (xbox_memory_layout.c) */

#define GUEST_BASE 0x00100000u
#define GUEST_SIZE 0x00100000u
static uint8_t *guest;

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

/* Guest addresses. */
#define VA_SW        0x00011000u    /* the switch, slot at MEM32(VA_SLOT_PTR) */
#define VA_CO_B      0x00011100u    /* coroutine B's entry */
#define VA_CO_B_RET  0x00011120u    /* after B's call to the switch */
#define VA_A_RET     0x00011140u    /* after A's call to the switch */
#define VA_FIBER     0x00011200u    /* a copy-stack fiber's entry */
#define VA_FIBER_RET 0x00011220u    /* after a fiber's call to the switch */
#define VA_DEEP      0x00011280u    /* a fiber that is itself only far up its stack */
#define VA_DEEP_RET  0x000112A0u
#define VA_BODY      0x00011300u    /* scenario 3: a body that returns */
#define VA_EXIT      0x00011340u    /* ...into this exit routine */
#define VA_EXIT_RET  0x00011360u
#define VA_NOWHERE   0x00011401u    /* the middle of some function */

/* The exception dispatcher's side of a switch (src/kernel/xbox_seh.c), and the
 * heap's block lookup it uses for a new stack's top: [HEAP_LO, HEAP_HI) is one
 * block here. */
#define HEAP_LO 0x00130000u
#define HEAP_HI 0x00140000u
static RECOMP_TLS uintptr_t t_seh_token;
static RECOMP_TLS uint32_t t_seh_lo, t_seh_hi;
void xbox_SehSetHostStack(uintptr_t token) { t_seh_token = token; }
void xbox_SehSetGuestStack(uint32_t lo, uint32_t hi) { t_seh_lo = lo; t_seh_hi = hi; }
void xbox_SehGetGuestStack(uint32_t *lo, uint32_t *hi) { *lo = t_seh_lo; *hi = t_seh_hi; }
uint32_t xbox_HeapBlockSize(uint32_t va) { return va >= HEAP_LO && va < HEAP_HI ? HEAP_HI - va : 0; }

/* Guest data: per-thread slot so two threads can run scenario 1 at once. */
static RECOMP_TLS uint32_t t_slot;

static void sub_00011000(void)      /* the switch, as lifted */
{
    uint32_t ebp = 0;
    ebp = g_ebp;  /* frameless: caller's frame */
    uint32_t _ss_save = 0, _ss_load = 0, _ss_site = 0, _ss_entry = MEM32(esp); /* stack switch (XBA-101) */
    (void)_ss_load; (void)_ss_site; (void)_ss_entry;
    ebp = g_seh_ebp; /* fpo_leaf: inherit caller's frame */

    PUSH32(esp, ebx);
    PUSH32(esp, ebp);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    _ss_save = esp; _ss_site = 0x00011004u; /* stack switch */
    { uint32_t _xo = RECOMP_ATOMIC_XCHG32(XBOX_PTR(t_slot), (uint32_t)(esp));
      esp = _xo; }  /* xchg */
    _ss_load = esp;
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ebp);
    POP32(esp, ebx);
    /* Fable's has an sti here: a posted interrupt runs on this stack and
     * writes over what was just popped. */
    memset(guest + (_ss_load - GUEST_BASE), 0xEE, esp - _ss_load);
    g_ebp = ebp; g_seh_ebp = ebp; if (_ss_save) recomp_stack_switch_ret(_ss_save, _ss_load, _ss_entry, 0u, _ss_site); esp += 4; return; /* ret */
}

/* ── 1. two coroutines ── */

static RECOMP_TLS unsigned t_a_count, t_b_count, t_b_bad, t_seh_bad;
static RECOMP_TLS uint32_t t_b_ctx, t_b_hi;     /* t_b_hi: B's expected stack top */

static void sub_00011100(void)      /* B: count, check A is in step, give the CPU back */
{
    uint32_t mine = 0xB0B0B000u;    /* a C local: lives on B's host stack */
    t_b_ctx = recomp_stack_context();
    for (;;) {
        t_b_count++;
        if (t_a_count != t_b_count || mine != 0xB0B0B000u || ebx != 0x0000B0B0u)
            t_b_bad++;
        if (t_seh_token != t_b_ctx || t_seh_lo != 0 || t_seh_hi != t_b_hi)
            t_seh_bad++;
        ebx = 0x0000B0B0u;          /* B's own callee-saved register */
        PUSH32(esp, VA_CO_B_RET);
        sub_00011000();
    }
}

/* Coroutine B's stack: what the switch pops (edi, esi, ebp, ebx), then the
 * address its ret goes to. */
static uint32_t make_b_stack(uint32_t top)
{
    uint32_t sp = top - 4;
    MEM32(sp) = 0;                  /* B's own return address: never used */
    sp -= 4; MEM32(sp) = VA_CO_B;   /* ret */
    sp -= 4; MEM32(sp) = 0x0000B0B0u;   /* ebx */
    sp -= 4; MEM32(sp) = 0;         /* ebp */
    sp -= 4; MEM32(sp) = 0;         /* esi */
    sp -= 4; MEM32(sp) = 0;         /* edi */
    return sp;
}

static void two_coroutines(uint32_t slot, uint32_t a_top, uint32_t b_top, int rounds)
{
    t_slot = slot;
    t_a_count = t_b_count = t_b_bad = t_seh_bad = 0;
    t_b_hi = b_top > HEAP_LO && b_top <= HEAP_HI ? b_top : 0;
    xbox_SehSetGuestStack(a_top - 0x10000u, a_top);     /* the thread's own stack */
    esp = a_top;
    MEM32(slot) = make_b_stack(b_top);
    for (int i = 0; i < rounds; i++) {
        uint32_t before = esp;
        t_a_count++;
        ebx = 0x0000A0A0u;
        PUSH32(esp, VA_A_RET);
        sub_00011000();
        if (t_seh_token != 0 || t_seh_lo != a_top - 0x10000u || t_seh_hi != a_top)
            t_seh_bad++;
        if (esp != before || ebx != 0x0000A0A0u || t_b_count != t_a_count) {
            fprintf(stderr, "  round %d: esp %08X (want %08X) ebx %08X counts %u/%u\n",
                    i, esp, before, ebx, t_a_count, t_b_count);
            failures++;
            return;
        }
    }
}

/* ── 2. copy-stack fibers ── */

#define SHARED_TOP  0x00170000u     /* one stack area for every fiber */
#define FIBER_BYTES 0x200u          /* at most this much of it per fiber */
static uint32_t fiber_save[4] = { 0x00160000u, 0x00161000u,     /* each fiber's saved bytes */
                                  0x00162000u, 0x00163000u };
static uint32_t fiber_len[4];
static unsigned fiber_count[4], fiber_bad;

static void sub_00011200(void)      /* a fiber: ebx names it */
{
    uint32_t id = ebx;              /* C local, on the fiber's host stack */
    PUSH32(esp, 0xF1BE0000u | id);  /* guest local, on the shared area */
    for (;;) {
        fiber_count[id]++;
        if (MEM32(esp) != (0xF1BE0000u | id) || ebx != id)
            fiber_bad++;
        PUSH32(esp, VA_FIBER_RET);
        sub_00011000();
    }
}

/* A fiber whose own word is 96 bytes above where it parks, under frames and
 * registers every such fiber has alike. */
static void sub_00011280(void)
{
    uint32_t id = ebx;              /* C local, on the fiber's host stack */
    PUSH32(esp, 0xDEE90000u | id);
    for (int i = 0; i < 19; i++)
        PUSH32(esp, 0x5A5A5A5Au);   /* frames in between: the same for all */
    for (;;) {
        fiber_count[id]++;
        if (MEM32(esp + 19 * 4) != (0xDEE90000u | id))
            fiber_bad++;
        ebx = 0;                    /* nothing of its own in the registers */
        PUSH32(esp, VA_DEEP_RET);
        sub_00011000();
    }
}

/* The resumer: copy the fiber's bytes in under SHARED_TOP, switch to it, and
 * when it gives the CPU back, copy them out again. */
static void resume_fiber(int id)
{
    uint32_t sp = SHARED_TOP - fiber_len[id];
    memcpy(guest + (sp - GUEST_BASE), guest + (fiber_save[id] - GUEST_BASE), fiber_len[id]);
    MEM32(t_slot) = sp;
    PUSH32(esp, VA_A_RET);
    sub_00011000();
    sp = MEM32(t_slot);
    fiber_len[id] = SHARED_TOP - sp;
    CHECK(fiber_len[id] <= FIBER_BYTES);
    memcpy(guest + (fiber_save[id] - GUEST_BASE), guest + (sp - GUEST_BASE), fiber_len[id]);
}

static void new_fiber(int id, uint32_t entry)
{
    uint32_t b = fiber_save[id];
    MEM32(b + 0x00) = 0;            /* edi */
    MEM32(b + 0x04) = 0;            /* esi */
    MEM32(b + 0x08) = 0;            /* ebp */
    MEM32(b + 0x0C) = (uint32_t)id; /* ebx */
    MEM32(b + 0x10) = entry;        /* ret */
    MEM32(b + 0x14) = 0;
    fiber_len[id] = 0x18;
}

/* ── 3. a body that returns into an exit routine ── */

static unsigned body_count, exit_count;

static void sub_00011300(void) { body_count++; esp += 4; return; /* ret */ }

static void sub_00011340(void)
{
    for (;;) {
        exit_count++;
        PUSH32(esp, VA_EXIT_RET);
        sub_00011000();
    }
}

/* ── dispatch ── */

recomp_func_t recomp_lookup(uint32_t va);
recomp_func_t recomp_lookup(uint32_t va)
{
    switch (va) {
    case VA_SW:     return sub_00011000;
    case VA_CO_B:   return sub_00011100;
    case VA_FIBER:  return sub_00011200;
    case VA_DEEP:   return sub_00011280;
    case VA_BODY:   return sub_00011300;
    case VA_EXIT:   return sub_00011340;
    default:        return NULL;
    }
}
recomp_func_t recomp_lookup_manual(uint32_t va);
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

static void *thread_main(void *arg)
{
    (void)arg;
    two_coroutines(0x00100010u, 0x001F0000u, 0x001E0000u, 1000);
    CHECK(t_b_bad == 0);
    CHECK(t_seh_bad == 0);
    CHECK(recomp_stack_switch_live() == 2);     /* its own, not the main thread's */
    return NULL;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    static char buf[8192];
    size_t n;
    if (!f)
        return NULL;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    char disc[256];
    const char *tmp = getenv("TMPDIR");
    char *text;

    guest = calloc(1, GUEST_SIZE);
    g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)guest - GUEST_BASE);

    if (argc > 1 && !strcmp(argv[1], "off")) {
        /* The old behaviour: the C return comes straight back, B never runs
         * and A is left on B's stack. */
        CHECK(!recomp_stack_switch_enabled());
        t_slot = 0x00100000u;
        esp = 0x00180000u;
        MEM32(t_slot) = make_b_stack(0x00140000u);
        PUSH32(esp, VA_A_RET);
        sub_00011000();
        CHECK(t_b_count == 0);
        CHECK(esp == 0x00140000u - 4);          /* past B's ret address */
        printf("stack_switch off: %s\n", failures ? "FAIL" : "ok");
        return failures ? 1 : 0;
    }

    if (!recomp_stack_switch_enabled()) {
        printf("stack_switch: this host has no stack switch\n");
        return 77;
    }
    snprintf(disc, sizeof disc, "%s/xba_stack_switch_%d.jsonl", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    remove(disc);
    setenv("RECOMP_DISCOVERY", disc, 1);

    /* 1 */
    CHECK(recomp_stack_context() == 0);
    two_coroutines(0x00100000u, 0x00180000u, 0x00140000u, 10000);
    CHECK(t_b_bad == 0);
    CHECK(t_seh_bad == 0);      /* the dispatcher followed every switch */
    CHECK(t_b_ctx != 0);
    CHECK(recomp_stack_context() == 0);
    CHECK(recomp_stack_switch_started() == 1);
    CHECK(recomp_stack_switch_live() == 2);
    printf("  1. two coroutines, %u rounds: %s\n", t_a_count, failures ? "FAIL" : "ok");

    /* 2: F0 F1 F0 F0 F1 F1 F0 ... both park at the same esp, returning to
     * the same address; the stack bytes tell them apart. */
    {
        static const int order[] = { 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 1 };
        unsigned want[2] = { 0, 0 };
        int before = failures;
        t_slot = 0x00100004u;
        new_fiber(0, VA_FIBER);
        new_fiber(1, VA_FIBER);
        for (unsigned k = 0; k < sizeof order / sizeof order[0]; k++) {
            resume_fiber(order[k]);
            want[order[k]]++;
        }
        CHECK(fiber_count[0] == want[0] && fiber_count[1] == want[1]);
        CHECK(fiber_bad == 0);
        CHECK(recomp_stack_switch_started() == 3);
        printf("  2. copy-stack fibers, %u + %u resumes: %s\n", want[0], want[1],
               failures > before ? "FAIL" : "ok");

        /* The same with two fibers alike over their first 64 bytes: only the
         * whole stack says which one the resumer copied back. With
         * RECOMP_STACK_SWITCH_WHOLE=0 (the A/B) the first 64 bytes cannot. */
        const char *whole = getenv("RECOMP_STACK_SWITCH_WHOLE");
        int first64 = whole && *whole == '0';
        before = failures;
        want[0] = want[1] = 0;
        new_fiber(2, VA_DEEP);
        new_fiber(3, VA_DEEP);
        for (unsigned k = 0; k < sizeof order / sizeof order[0]; k++) {
            resume_fiber(2 + order[k]);
            want[order[k]]++;
        }
        if (first64) {
            CHECK(fiber_bad != 0);  /* the A/B tells them apart by 64 bytes: it cannot */
        } else {
            CHECK(fiber_count[2] == want[0] && fiber_count[3] == want[1]);
            CHECK(fiber_bad == 0);
        }
        CHECK(recomp_stack_switch_started() == 5);
        printf("  2. copy-stack fibers alike over 64 bytes%s, %u + %u resumes: %s\n",
               first64 ? " (first 64 bytes only: mixed up, as expected)" : "", want[0],
               want[1], failures > before ? "FAIL" : "ok");

        /* A resume no snapshot matches is the first sign of a run going
         * wrong (WP8: Fable's wild calls of 02:29 followed one, when the
         * popped bytes still counted), so it is in the discovery file, keyed
         * by where the context resumes. None so far; then fiber 2's bytes
         * come back with a word of its frames changed (past what the switch
         * pops, inside the first 64 bytes): neither twin matches. */
        before = failures;
        text = slurp(disc);
        CHECK(!text || !strstr(text, "does not match"));
        MEM32(fiber_save[2] + 0x18) ^= 0x01000000u;
        resume_fiber(2);
        text = slurp(disc);
        CHECK(text && strstr(text, "{\"t\":\"stack_switch\",\"va\":\"0x000112A0\","
                                   "\"to\":\"0x0016FF98\",\"why\":\"resumed a parked "
                                   "context whose stack does not match\"}"));
        printf("  2. a resume no snapshot matches is recorded: %s\n",
               failures > before ? "FAIL" : "ok");
    }

    /* 3 */
    {
        int before = failures;
        uint32_t sp = 0x00150000u;
        t_slot = 0x00100008u;
        sp -= 4; MEM32(sp) = 0;
        sp -= 4; MEM32(sp) = VA_EXIT;   /* where the body returns */
        sp -= 4; MEM32(sp) = VA_BODY;   /* ret */
        sp -= 16;                       /* edi esi ebp ebx */
        memset(guest + (sp - GUEST_BASE), 0, 16);
        MEM32(t_slot) = sp;
        PUSH32(esp, VA_A_RET);
        sub_00011000();
        PUSH32(esp, VA_A_RET);
        sub_00011000();
        CHECK(body_count == 1 && exit_count == 2);
        printf("  3. body returning into an exit routine: %s\n", failures > before ? "FAIL" : "ok");
    }

    /* 4 */
    {
        int before = failures;
        uint32_t sp = 0x00158000u, esp0;
        t_slot = 0x0010000Cu;
        sp -= 4; MEM32(sp) = VA_NOWHERE;
        sp -= 16;
        memset(guest + (sp - GUEST_BASE), 0, 16);
        MEM32(t_slot) = sp;
        esp0 = esp;
        PUSH32(esp, VA_A_RET);
        sub_00011000();
        CHECK(esp == 0x00158000u);      /* the old behaviour: on with the other stack */
        esp = esp0;
        text = slurp(disc);
        CHECK(text && strstr(text, "{\"t\":\"stack_switch\",\"va\":\"0x00011004\",\"to\":\"0x00011401\","));
        CHECK(text && strstr(text, "{\"t\":\"icall\",\"va\":\"0x00011401\""));
        printf("  4. nowhere to go: old behaviour, recorded: %s\n", failures > before ? "FAIL" : "ok");
        remove(disc);
    }

    /* 5 */
    {
        int before = failures;
        pthread_t th;
        CHECK(pthread_create(&th, NULL, thread_main, NULL) == 0);
        pthread_join(th, NULL);
        CHECK(recomp_stack_switch_started() == 7);
        printf("  5. another host thread: %s\n", failures > before ? "FAIL" : "ok");
    }

    printf("stack_switch: %s\n", failures ? "FAIL" : "ok");
    return failures ? 1 : 0;
}
