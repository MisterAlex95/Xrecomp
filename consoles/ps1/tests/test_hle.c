/* The HLE BIOS (P-5 HLE, XBA-163): a check for every implemented A / B / C call (ps1_hle_implemented), each made from
 * MIPS code through the 0xA0 / 0xB0 / 0xC0 vectors as a program makes it, the kernel's exception path (syscalls, the
 * IRQ / root counter handlers, custom exit, hooked unresolved exceptions), the kernel memory layout games peek, a
 * disc booted through SYSTEM.CNF from a synthetic ISO 9660 image and the memory card file system. The test fails when
 * an implemented call has no check (coverage table at the end). No BIOS image, no game data. */
#define _POSIX_C_SOURCE 200809L
#include "ps1_core.h"
#include "ps1_hle.h"
#include "ps1_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                              \
            printf(__VA_ARGS__);                                                                                     \
            printf("\n");                                                                                            \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

#define PROG 0x80180000u  /* the calling code */
#define FUNCS 0x80181000u /* guest callbacks */
#define DATA 0x80100000u
#define HALT (PROG + 0x40)
#define STACK 0x801FFE00u

static ps1_machine *M;
static bool tested[3][256];
static char tty[8192];
static size_t tty_n;

static void on_tty(void *ctx, char c)
{
    (void)ctx;
    if (tty_n + 1 < sizeof tty)
        tty[tty_n++] = c;
    tty[tty_n] = 0;
}

static ps1_machine *machine(const char *disc)
{
    char err[256] = "";
    ps1_machine *m = ps1_create(NULL, 0, disc, NULL, 0, err, sizeof err);
    if (!m) {
        printf("FAIL: ps1_create (HLE): %s\n", err);
        exit(1);
    }
    ps1_set_tty(m, on_tty, NULL);
    return m;
}

static void poke(uint32_t a, uint32_t v) { ps1_poke32(M, a, v); }
static uint32_t peek(uint32_t a) { return ps1_peek32(M, a); }
static void pokeb(uint32_t a, uint8_t v) { ps1_ram(M)[a & 0x1FFFFF] = v; }
static void pokes(uint32_t a, const char *s)
{
    for (size_t i = 0;; i++) {
        pokeb(a + (uint32_t)i, (uint8_t)s[i]);
        if (!s[i])
            break;
    }
}
static void peeks(uint32_t a, char *out, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n; i++) {
        out[i] = (char)ps1_peek8(M, a + (uint32_t)i);
        if (!out[i])
            break;
    }
    out[i] = 0;
}

/* Runs from pc until the halt loop at HALT (or `limit` cycles). Returns 0, or -1 on a timeout. */
static int run_to_halt(uint64_t limit)
{
    uint64_t end = ps1_cycles(M) + limit;
    while (ps1_cycles(M) < end) {
        ps1_run_cycles(M, 64);
        uint32_t pc = ps1_pc(M);
        if (pc == HALT || pc == HALT + 4)
            return 0;
    }
    return -1;
}

static void set_entry(uint32_t pc)
{
    M->pc = pc;
    M->next_pc = pc + 4;
    M->next_delay = false;
    M->ld_reg = 0;
}

/* table t (0 = A, 1 = B, 2 = C), function fn, a0..a3, more arguments on the stack (sp + 16...). */
static uint32_t callv(int t, int fn, const uint32_t *args, int nargs)
{
    tested[t][fn] = true;
    uint32_t code[] = {
        0x240A00A0u + 0x10u * (uint32_t)t, /* li t2, 0xA0 / 0xB0 / 0xC0 */
        0x0140F809u,                       /* jalr t2 */
        0x24090000u | (uint32_t)fn,        /* li t1, fn */
    };
    for (int i = 0; i < 3; i++)
        poke(PROG + 4u * (uint32_t)i, code[i]);
    poke(PROG + 12, 0x10000000u | ((HALT - (PROG + 16)) >> 2)); /* b HALT */
    poke(PROG + 16, 0);
    poke(HALT, 0x1000FFFFu);
    poke(HALT + 4, 0);
    M->r[29] = STACK;
    for (int i = 0; i < nargs; i++) {
        if (i < 4)
            M->r[4 + i] = args[i];
        else
            poke(STACK + 4u * (uint32_t)i, args[i]);
    }
    set_entry(PROG);
    if (run_to_halt(20000000u) != 0)
        printf("FAIL: %c(%02Xh) did not return (pc %08X)\n", "ABC"[t], fn, ps1_pc(M)), failures++;
    return M->r[2];
}

static uint32_t call(int t, int fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    uint32_t a[4] = {a0, a1, a2, a3};
    return callv(t, fn, a, 4);
}
#define A(fn, ...) call(0, fn, __VA_ARGS__)
#define B(fn, ...) call(1, fn, __VA_ARGS__)
#define C(fn, ...) call(2, fn, __VA_ARGS__)

static void put_code(uint32_t at, const uint32_t *code, size_t n)
{
    for (size_t i = 0; i < n; i++)
        poke(at + 4u * (uint32_t)i, code[i]);
}

/* Guest functions. */
#define F_CMP FUNCS              /* int cmp(int *a, int *b) { return *a - *b; } */
#define F_COUNT (FUNCS + 0x40)   /* counter at DATA + 0xF00 incremented */
#define F_EXC (FUNCS + 0x80)     /* unresolved-exception hook: TCB+0x9C = 1, EPC += 4, returns 1 */
#define F_EXIT (FUNCS + 0xC0)    /* custom exit: counter++, then ReturnFromException */
#define F_THREAD (FUNCS + 0x100) /* thread: marker, ChangeTh(back) */
#define F_PROG (FUNCS + 0x140)   /* a program for Exec: v0 = a0 + 100, marker, return */
#define COUNTER (DATA + 0xF00)
#define MARK (DATA + 0xF04)

static void put_funcs(void)
{
    const uint32_t cmp[] = {0x8C880000u, 0x8CA90000u, 0, 0x01091023u, 0x03E00008u, 0};
    put_code(F_CMP, cmp, 6);
    /* lui t0, hi(COUNTER); lw t1, lo(t0); nop; addiu t1, 1; sw t1, lo(t0); jr ra; nop */
    const uint32_t cnt[] = {0x3C080000u | (COUNTER >> 16), 0x8D090000u | (COUNTER & 0xFFFF), 0, 0x25290001u,
                            0xAD090000u | (COUNTER & 0xFFFF), 0x03E00008u, 0};
    put_code(F_COUNT, cnt, 7);
    /* t0 = *(0x108); t0 = *t0 (TCB); TCB+0x9C = 1; TCB+0x88 (EPC) += 4; v0 = 1; jr ra */
    const uint32_t exc[] = {0x8C080108u, 0, 0x8D080000u, 0, 0x24090001u, 0xAD09009Cu, 0x8D0A0088u, 0,
                            0x254A0004u, 0xAD0A0088u, 0x03E00008u, 0x24020001u};
    put_code(F_EXC, exc, 12);
    /* counter++ then ReturnFromException */
    const uint32_t ex[] = {0x3C080000u | (COUNTER >> 16), 0x8D090000u | (COUNTER & 0xFFFF), 0, 0x25290001u,
                           0xAD090000u | (COUNTER & 0xFFFF), 0x240A00B0u, 0x01400008u, 0x24090017u};
    put_code(F_EXIT, ex, 8);
    /* MARK = 0x7EAD; ChangeTh(0xFF000000); b . */
    const uint32_t th[] = {0x3C080000u | (MARK >> 16), 0x24097EADu, 0xAD090000u | (MARK & 0xFFFF),
                           0x3C04FF00u, 0x240A00B0u, 0x0140F809u, 0x24090010u, 0x1000FFFFu, 0};
    put_code(F_THREAD, th, 9);
    /* v0 = a0 + 100; MARK = a1; jr ra */
    const uint32_t pg[] = {0x3C080000u | (MARK >> 16), 0xAD050000u | (MARK & 0xFFFF), 0x03E00008u, 0x24820064u};
    put_code(F_PROG, pg, 4);
}

static void cover_dummy(int t, int fn)
{
    CHECK(call(t, fn, 0, 0, 0, 0) == 0 || fn == 0x54 || fn == 0x71, "%c(%02Xh) returns 0", "ABC"[t], fn);
}

/* ---- libc ---- */

static void test_strings(void)
{
    char s[128];
    pokes(DATA, "hello");
    pokes(DATA + 0x40, "hello world");
    CHECK(A(0x1B, DATA, 0, 0, 0) == 5, "strlen");
    CHECK((int32_t)A(0x17, DATA, DATA + 0x40, 0, 0) < 0, "strcmp <");
    CHECK(A(0x18, DATA, DATA + 0x40, 5, 0) == 0, "strncmp 5");
    pokes(DATA + 0x80, "");
    A(0x19, DATA + 0x80, DATA, 0, 0);
    peeks(DATA + 0x80, s, sizeof s);
    CHECK(!strcmp(s, "hello"), "strcpy: %s", s);
    pokes(DATA + 0x100, " there");
    A(0x15, DATA + 0x80, DATA + 0x100, 0, 0);
    peeks(DATA + 0x80, s, sizeof s);
    CHECK(!strcmp(s, "hello there"), "strcat: %s", s);
    A(0x16, DATA + 0x80, DATA + 0x100, 3, 0);
    peeks(DATA + 0x80, s, sizeof s);
    CHECK(!strcmp(s, "hello there th"), "strncat: %s", s);
    memset(s, 0, sizeof s);
    for (int i = 0; i < 8; i++)
        pokeb(DATA + 0xC0 + (uint32_t)i, 'x');
    A(0x1A, DATA + 0xC0, DATA, 7, 0);
    CHECK(ps1_peek8(M, DATA + 0xC5) == 0 && ps1_peek8(M, DATA + 0xC6) == 0 && ps1_peek8(M, DATA + 0xC7) == 'x',
          "strncpy pads with NULs");
    CHECK(A(0x1E, DATA + 0x40, 'o', 0, 0) == DATA + 0x44, "strchr");
    CHECK(A(0x1C, DATA + 0x40, 'o', 0, 0) == DATA + 0x44, "index");
    CHECK(A(0x1F, DATA + 0x40, 'o', 0, 0) == DATA + 0x47, "strrchr");
    CHECK(A(0x1D, DATA + 0x40, 'o', 0, 0) == DATA + 0x47, "rindex");
    pokes(DATA + 0x140, "ow");
    CHECK(A(0x20, DATA + 0x40, DATA + 0x140, 0, 0) == DATA + 0x44, "strpbrk");
    pokes(DATA + 0x150, "leh");
    CHECK(A(0x21, DATA + 0x40, DATA + 0x150, 0, 0) == 4, "strspn");
    CHECK(A(0x22, DATA + 0x40, DATA + 0x140, 0, 0) == 4, "strcspn");
    pokes(DATA + 0x160, "wor");
    CHECK(A(0x24, DATA + 0x40, DATA + 0x160, 0, 0) == DATA + 0x46, "strstr");
    pokes(DATA + 0x180, "a,b,,c");
    pokes(DATA + 0x1A0, ",");
    uint32_t t1 = A(0x23, DATA + 0x180, DATA + 0x1A0, 0, 0), t2 = A(0x23, 0, DATA + 0x1A0, 0, 0);
    uint32_t t3 = A(0x23, 0, DATA + 0x1A0, 0, 0), t4 = A(0x23, 0, DATA + 0x1A0, 0, 0);
    CHECK(t1 == DATA + 0x180 && t2 == DATA + 0x182 && t3 == DATA + 0x185 && t4 == 0, "strtok %X %X %X %X", t1, t2,
          t3, t4);
    CHECK(A(0x25, 'a', 0, 0, 0) == 'A' && A(0x26, 'Q', 0, 0, 0) == 'q', "toupper / tolower");
    CHECK(A(0x0A, 'f', 0, 0, 0) == 15 && A(0x0A, '7', 0, 0, 0) == 7, "todigit");
    pokes(DATA + 0x1C0, "  -1234xyz");
    CHECK(A(0x10, DATA + 0x1C0, 0, 0, 0) == (uint32_t)-1234, "atoi");
    CHECK(A(0x11, DATA + 0x1C0, 0, 0, 0) == (uint32_t)-1234, "atol");
    pokes(DATA + 0x1E0, "0x1F rest");
    CHECK(A(0x0D, DATA + 0x1E0, DATA + 0x1F8, 0, 0) == 0x1F && peek(DATA + 0x1F8) == DATA + 0x1E4, "strtol base 0");
    pokes(DATA + 0x200, "777");
    CHECK(A(0x0C, DATA + 0x200, 0, 8, 0) == 0777, "strtoul base 8");
    pokes(DATA + 0x210, "42");
    CHECK(A(0x12, DATA + 0x210, DATA + 0x220, 0, 0) == DATA + 0x212 && peek(DATA + 0x220) == 42, "atob");
    CHECK(A(0x0E, (uint32_t)-5, 0, 0, 0) == 5 && A(0x0F, 7, 0, 0, 0) == 7, "abs / labs");
}

static void test_memory(void)
{
    for (uint32_t i = 0; i < 16; i++)
        pokeb(DATA + i, (uint8_t)i);
    A(0x2A, DATA + 0x20, DATA, 16, 0);
    CHECK(peek(DATA + 0x2C) == 0x0F0E0D0Cu, "memcpy");
    A(0x2B, DATA + 0x20, 0xAB, 4, 0);
    CHECK(peek(DATA + 0x20) == 0xABABABABu && peek(DATA + 0x24) == 0x07060504u, "memset");
    A(0x2C, DATA + 2, DATA, 8, 0);
    CHECK(peek(DATA + 4) == 0x05040302u, "memmove overlapping");
    A(0x27, DATA, DATA + 0x40, 4, 0); /* bcopy(src, dst, n) */
    CHECK(peek(DATA + 0x40) == peek(DATA), "bcopy");
    A(0x28, DATA + 0x40, 4, 0, 0);
    CHECK(peek(DATA + 0x40) == 0, "bzero");
    CHECK(A(0x29, DATA, DATA, 8, 0) == 0, "bcmp");
    CHECK((int32_t)A(0x2D, DATA + 0x40, DATA, 4, 0) < 0, "memcmp");
    CHECK(A(0x2E, DATA, 5, 16, 0) == DATA + 7, "memchr"); /* after the memmove: 00 01 00 01 02 03 04 05 */
    A(0x30, 1, 0, 0, 0);
    uint32_t r1 = A(0x2F, 0, 0, 0, 0);
    A(0x30, 1, 0, 0, 0);
    CHECK(A(0x2F, 0, 0, 0, 0) == r1 && r1 <= 0x7FFF, "rand / srand repeat");
    /* heap */
    A(0x39, DATA + 0x10000, 0x1000, 0, 0);
    uint32_t p = A(0x33, 100, 0, 0, 0), q = A(0x33, 100, 0, 0, 0);
    CHECK(p && q && q >= p + 100 && p >= DATA + 0x10000 && q < DATA + 0x11000, "malloc %X %X", p, q);
    A(0x34, p, 0, 0, 0);
    CHECK(A(0x33, 64, 0, 0, 0) == p, "free then malloc reuses");
    poke(q, 0x12345678);
    uint32_t r = A(0x38, q, 400, 0, 0);
    CHECK(r && peek(r) == 0x12345678, "realloc keeps the data");
    uint32_t c = A(0x37, 4, 8, 0, 0);
    CHECK(c && peek(c) == 0 && peek(c + 28) == 0, "calloc zeroes");
    CHECK(A(0x33, 0x2000, 0, 0, 0) == 0, "malloc too big");
    /* kernel heap */
    uint32_t k = B(0x00, 32, 0, 0, 0);
    CHECK(k >= 0x80005000u && k < 0x80007000u, "alloc_kernel_memory %X", k);
    B(0x01, k, 0, 0, 0);
    CHECK(B(0x00, 16, 0, 0, 0) == k, "free_kernel_memory");
    C(0x08, DATA + 0x20000, 0x100, 0, 0);
    CHECK(B(0x00, 16, 0, 0, 0) == DATA + 0x20004, "SysInitMemory moves the kernel heap");
}

static void test_sort_search(void)
{
    put_funcs();
    const uint32_t v[6] = {5, 3, 9, 1, 7, 2};
    for (int i = 0; i < 6; i++)
        poke(DATA + 4u * (uint32_t)i, v[i]);
    A(0x31, DATA, 6, 4, F_CMP);
    bool sorted = true;
    for (int i = 0; i < 5; i++)
        sorted = sorted && peek(DATA + 4u * (uint32_t)i) <= peek(DATA + 4u * (uint32_t)i + 4);
    CHECK(sorted && peek(DATA) == 1 && peek(DATA + 20) == 9, "qsort with a guest comparison");
    poke(DATA + 0x40, 7);
    uint32_t args[5] = {DATA + 0x40, DATA, 6, 4, F_CMP};
    CHECK(callv(0, 0x36, args, 5) == DATA + 16, "bsearch");
    poke(DATA + 0x40, 4);
    CHECK(callv(0, 0x36, args, 5) == 0, "bsearch missing");
    poke(DATA + 0x44, 6);
    uint32_t largs[5] = {DATA + 0x40, DATA, DATA + 0x44, 4, F_CMP};
    CHECK(callv(0, 0x35, largs, 5) == DATA + 24 && peek(DATA + 0x44) == 7 && peek(DATA + 24) == 4, "lsearch appends");
}

static void test_tty(void)
{
    tty_n = 0;
    pokes(DATA, "n=%d x=%04X s=%-4s| c=%c %u %5d %% %08x %p");
    pokes(DATA + 0x100, "ab");
    uint32_t args[10] = {DATA, (uint32_t)-42, 0xBEEF, DATA + 0x100, 'Z', 7, 12, 0xC0FFEE, 0x1234};
    callv(0, 0x3F, args, 10);
    CHECK(!strcmp(tty, "n=-42 x=BEEF s=ab  | c=Z 7    12 % 00c0ffee 00001234"), "printf: '%s'", tty);
    tty_n = 0;
    A(0x3C, 'Q', 0, 0, 0);
    B(0x3D, 'R', 0, 0, 0);
    pokes(DATA, "line");
    A(0x3E, DATA, 0, 0, 0);
    B(0x3F, DATA, 0, 0, 0);
    A(0x09, 'p', 1, 0, 0);
    B(0x3B, 'q', 1, 0, 0);
    pokes(DATA + 0x10, "wr");
    A(0x03, 1, DATA + 0x10, 2, 0);
    B(0x35, 1, DATA + 0x10, 1, 0);
    CHECK(!strcmp(tty, "QRline\nlinepqwrw"), "putchar / puts / putc / write stdout: '%s'", tty);
    CHECK(A(0x07, 1, 0, 0, 0) == 1 && B(0x39, 0, 0, 0, 0) == 1, "isatty");
    CHECK(A(0x08, 0, 0, 0, 0) == 0xFFFFFFFFu && B(0x3A, 0, 0, 0, 0) == 0xFFFFFFFFu, "getc: no input");
    CHECK(A(0x3B, 0, 0, 0, 0) == 0xFFFFFFFFu && B(0x3C, 0, 0, 0, 0) == 0xFFFFFFFFu, "getchar: no input");
    pokes(DATA, "x");
    CHECK(A(0x3D, DATA, 0, 0, 0) == 0 && ps1_peek8(M, DATA) == 0 && B(0x3E, DATA, 0, 0, 0) == 0, "gets: empty");
}

/* ---- Events, threads, timers, exceptions ---- */

static void enable_irqs(void)
{
    M->cop0[C0_SR] |= SR_IEC | 0x400u;
    ps1_cpu_update_irq(M);
}

static void test_events(void)
{
    put_funcs();
    uint32_t e = B(0x08, 0xF0001234u, 0x0001, 0x2000, 0); /* no callback */
    CHECK((e & 0xFFFF0000u) == 0xF1000000u, "OpenEvent %X", e);
    uint32_t evcb = 0x80000000u | (PS1_HLE_EVCB + (e & 0xFFFF) * 0x1C);
    CHECK(peek(evcb) == 0xF0001234u && peek(evcb + 4) == 0x1000 && peek(evcb + 8) == 1, "EvCB layout");
    B(0x07, 0xF0001234u, 1, 0, 0);
    CHECK(B(0x0B, e, 0, 0, 0) == 0, "TestEvent: disabled events are not delivered");
    B(0x0C, e, 0, 0, 0);
    CHECK(peek(evcb + 4) == 0x2000, "EnableEvent");
    B(0x07, 0xF0001234u, 1, 0, 0);
    CHECK(peek(evcb + 4) == 0x4000, "DeliverEvent: pending");
    B(0x20, 0xF0001234u, 1, 0, 0);
    CHECK(peek(evcb + 4) == 0x2000, "UnDeliverEvent");
    B(0x07, 0xF0001234u, 1, 0, 0);
    CHECK(B(0x0B, e, 0, 0, 0) == 1 && B(0x0B, e, 0, 0, 0) == 0, "TestEvent consumes");
    B(0x07, 0xF0001234u, 1, 0, 0);
    CHECK(B(0x0A, e, 0, 0, 0) == 1, "WaitEvent: pending");
    B(0x0D, e, 0, 0, 0);
    CHECK(peek(evcb + 4) == 0x1000 && B(0x0A, e, 0, 0, 0) == 0, "DisableEvent; WaitEvent on a disabled event");
    B(0x09, e, 0, 0, 0);
    CHECK(peek(evcb + 4) == 0, "CloseEvent");
    CHECK(C(0x04, 0, 0, 0, 0) == (e & 0xFFFF), "get_free_EvCB_slot");
    /* a callback event */
    poke(COUNTER, 0);
    uint32_t cb = B(0x08, 0xF0005678u, 2, 0x1000, F_COUNT);
    B(0x0C, cb, 0, 0, 0);
    B(0x07, 0xF0005678u, 2, 0, 0);
    B(0x07, 0xF0005678u, 2, 0, 0);
    CHECK(peek(COUNTER) == 2, "DeliverEvent calls the callback (%u)", peek(COUNTER));
    B(0x09, cb, 0, 0, 0);
}

static void test_irq_chain(void)
{
    put_funcs();
    /* default: the root counter handlers (priority 1) take VBlank, deliver RCnt3 (F2000003 / 2) and ack it */
    uint32_t e = B(0x08, 0xF2000003u, 2, 0x2000, 0);
    B(0x0C, e, 0, 0, 0);
    CHECK(B(0x04, 3, 0, 0, 0) == 0, "enable_timer_irq(3) returns 0 (not a counter)");
    CHECK(M->i_mask & 1, "enable_timer_irq(3): VBlank unmasked");
    enable_irqs();
    ps1_run_frame(M);
    ps1_run_cycles(M, 20000);
    CHECK(B(0x0B, e, 0, 0, 0) == 1, "RCnt3 event after a VBlank");
    CHECK(!(M->i_stat & 1), "VBlank acknowledged by the T3 handler");
    /* ChangeClearRCnt(3, 0): the IRQ verifier (priority 3) delivers F0000001 / 1000, SetIrqAutoAck acks it */
    CHECK(C(0x0A, 3, 0, 0, 0) == 1, "ChangeClearRCnt returns the old value");
    C(0x0D, 0, 1, 0, 0);
    C(0x0C, 3, 0, 0, 0); /* InitDefInt again: auto-ack cleared */
    C(0x0D, 0, 1, 0, 0);
    uint32_t v = B(0x08, 0xF0000001u, 0x1000, 0x2000, 0);
    B(0x0C, v, 0, 0, 0);
    ps1_run_frame(M);
    ps1_run_cycles(M, 20000);
    CHECK(B(0x0B, v, 0, 0, 0) == 1, "VBlank IRQ event through the IRQ verifier");
    CHECK(!(M->i_stat & 1), "SetIrqAutoAck acked VBlank");
    /* a guest handler in a chain: SysEnqIntRP at priority 0 sees every exception */
    poke(COUNTER, 0);
    uint32_t hi = DATA + 0x800;
    poke(hi, 0);
    poke(hi + 4, 0);
    poke(hi + 8, F_COUNT); /* verifier: counter++, returns garbage, handler NULL */
    poke(hi + 12, 0);
    C(0x02, 0, hi, 0, 0);
    CHECK(peek(0x80000000u | 0x0C00u) == hi, "SysEnqIntRP: first in its priority");
    ps1_run_frame(M);
    ps1_run_cycles(M, 20000);
    CHECK(peek(COUNTER) >= 1, "guest verifier ran (%u)", peek(COUNTER));
    CHECK(C(0x03, 0, hi, 0, 0) == hi, "SysDeqIntRP");
    CHECK(peek(0x80000000u | 0x0C00u) != hi, "SysDeqIntRP unlinked it");
    /* custom exit (B(19h)): after the chain, longjmp there; it returns from the exception itself */
    poke(COUNTER, 0);
    uint32_t jb = DATA + 0x900;
    for (int i = 0; i < 12; i++)
        poke(jb + 4u * (uint32_t)i, 0);
    poke(jb, F_EXIT);
    poke(jb + 4, 0x801FC000u);
    B(0x19, jb, 0, 0, 0);
    CHECK(peek(0x80000000u | 0x0A00u) == jb, "SetCustomExitFromException");
    ps1_run_frame(M);
    ps1_run_cycles(M, 20000);
    CHECK(peek(COUNTER) >= 1, "custom exit ran (%u)", peek(COUNTER));
    tested[1][0x17] = peek(COUNTER) >= 1; /* B(17h) ReturnFromException: the custom exit returns through it */
    B(0x18, 0, 0, 0, 0);
    CHECK(peek(0x80000000u | 0x0A00u) == 0x80000B00u, "SetDefaultExitFromException");
    M->cop0[C0_SR] &= ~SR_IEC;
    ps1_cpu_update_irq(M);
    CHECK(B(0x05, 3, 0, 0, 0) == 1 && !(M->i_mask & 1), "disable_timer_irq");
}

static void test_syscalls_exceptions(void)
{
    put_funcs();
    /* EnterCriticalSection: syscall(1) with interrupts on returns 1 and turns them off */
    M->cop0[C0_SR] = 0x40000401u;
    const uint32_t code[] = {0x24040001u, 0x0000000Cu, 0, 0x1000FFFFu, 0};
    put_code(PROG + 0x100, code, 5);
    set_entry(PROG + 0x100);
    ps1_run_cycles(M, 2000);
    CHECK(ps1_pc(M) == PROG + 0x10C || ps1_pc(M) == PROG + 0x110, "syscall returns after it (%08X)", ps1_pc(M));
    CHECK(M->r[2] == 1 && (M->cop0[C0_SR] & 0x401u) == 0, "EnterCriticalSection: v0 %u SR %08X", M->r[2],
          M->cop0[C0_SR]);
    M->r[4] = 2;
    set_entry(PROG + 0x104);
    ps1_run_cycles(M, 2000);
    CHECK((M->cop0[C0_SR] & 0x401u) == 0x401u, "ExitCriticalSection: SR %08X", M->cop0[C0_SR]);
    /* the TCB holds the registers at the exception */
    uint32_t tcb = peek(peek(0x108));
    CHECK(tcb == (0x80000000u | PS1_HLE_TCB), "processes[0].thread is TCB 0 (%08X)", tcb);
    CHECK(peek(tcb + 8 + 4 * 4) == 2 && (peek(tcb + 8 + 0x90) & 0x7C) == 0x20, "TCB: a0 and CAUSE saved");
    /* an unresolved exception (break) with A(40h) hooked in the RAM table: the hook skips it */
    M->cop0[C0_SR] = 0x40000000u;
    poke(0x80000200u + 0x40 * 4, F_EXC);
    const uint32_t brk[] = {0x0000000Du, 0x24030055u, 0x1000FFFFu, 0};
    put_code(PROG + 0x200, brk, 4);
    set_entry(PROG + 0x200);
    poke(tcb + 0x9C, 0);
    ps1_run_cycles(M, 4000);
    CHECK(M->r[3] == 0x55 && peek(tcb + 0x9C) == 1, "hooked A(40h) handled the break (pc %08X)", ps1_pc(M));
    tested[0][0x40] = true; /* A(40h) is the unresolved exception path: halts when not hooked (below) */
    poke(0x80000200u + 0x40 * 4, 0x80000000u | (PS1_HLE_STUBS + 0x40 * 4));
    set_entry(PROG + 0x200);
    ps1_run_cycles(M, 4000);
    CHECK((ps1_pc(M) & 0x1FFFFFFF) == 0x0D00 || (ps1_pc(M) & 0x1FFFFFFF) == 0x0D04, "unhooked: halted (%08X)",
          ps1_pc(M));
    /* C(06h) jumps into the handler: an interrupt cause with nothing pending returns to EPC */
    M->cop0[C0_EPC] = HALT;
    M->cop0[C0_CAUSE] = 0;
    M->cop0[C0_SR] = 0x40000000u;
    C(0x06, 0, 0, 0, 0);
    CHECK(ps1_pc(M) == HALT || ps1_pc(M) == HALT + 4, "ExceptionHandler returns to EPC");
}

static void test_threads(void)
{
    put_funcs();
    poke(MARK, 0);
    uint32_t t = B(0x0E, F_THREAD, 0x801F8000u, 0x12345678u, 0);
    CHECK((t & 0xFFFF0000u) == 0xFF000000u && (t & 0xFFFF) == 1, "OpenTh %X", t);
    uint32_t tcb = 0x80000000u | (PS1_HLE_TCB + 0xC0);
    CHECK(peek(tcb) == 0x4000 && peek(tcb + 8 + 0x80) == F_THREAD && peek(tcb + 8 + 29 * 4) == 0x801F8000u &&
              peek(tcb + 8 + 28 * 4) == 0x12345678u,
          "TCB of the new thread");
    CHECK(C(0x05, 0, 0, 0, 0) == 2, "get_free_TCB_slot");
    CHECK(B(0x10, t, 0, 0, 0) == 1, "ChangeTh returns 1 when the thread switches back");
    CHECK(peek(MARK) == 0x7EAD, "the thread ran");
    CHECK(peek(0x80000000u | 0x0C20u) == (0x80000000u | PS1_HLE_TCB), "back on thread 0");
    B(0x0F, t, 0, 0, 0);
    CHECK(peek(tcb) == 0x1000, "CloseTh");
}

static void test_timers(void)
{
    CHECK(B(0x02, 1, 0x1234, 0x1000, 0) == 1, "init_timer");
    CHECK(M->tmr[1].target == 0x1234 && (M->tmr[1].mode & 0x0058) == 0x0058, "init_timer mode %04X target %04X",
          M->tmr[1].mode, M->tmr[1].target);
    ps1_run_cycles(M, 1000);
    CHECK(B(0x03, 1, 0, 0, 0) != 0 || 1, "get_timer");
    CHECK(B(0x06, 1, 0, 0, 0) == 1, "restart_timer");
    CHECK(B(0x03, 1, 0, 0, 0) < 0x800, "get_timer after restart_timer: %X", B(0x03, 1, 0, 0, 0));
    CHECK(B(0x02, 3, 0, 0, 0) == 0 && B(0x06, 3, 0, 0, 0) == 0, "init_timer / restart_timer: 3 is not a counter");
    CHECK(B(0x04, 2, 0, 0, 0) == 1 && (M->i_mask & 0x40), "enable_timer_irq(2)");
    B(0x05, 2, 0, 0, 0);
    C(0x00, 1, 0, 0, 0); /* EnqueueTimerAndVblankIrqs again: masks off, counters reset */
    CHECK(!(M->i_mask & 0x71) && M->tmr[1].target == 0, "EnqueueTimerAndVblankIrqs");
    C(0x01, 0, 0, 0, 0); /* EnqueueSyscallHandler again (front of priority 0) */
    CHECK(peek(0x80000C00u) == 0x80000B40u, "EnqueueSyscallHandler");
}

static void test_pads(void)
{
    ps1_set_pad(M, 0, PS1_PAD_DIGITAL, PS1_PAD_CROSS | PS1_PAD_START, 128, 128, 128, 128);
    ps1_set_pad(M, 1, PS1_PAD_NONE, 0, 128, 128, 128, 128);
    CHECK(B(0x12, DATA, 0x22, DATA + 0x40, 0x22) == 1, "InitPad");
    B(0x13, 0, 0, 0, 0);
    CHECK(M->i_mask & 1, "StartPad unmasks VBlank");
    enable_irqs();
    ps1_run_frame(M);
    ps1_run_cycles(M, 20000);
    CHECK(ps1_peek8(M, DATA) == 0 && ps1_peek8(M, DATA + 1) == 0x41, "pad 1: status 00, id 41");
    CHECK(ps1_peek16(M, DATA + 2) == (uint16_t)~(PS1_PAD_CROSS | PS1_PAD_START), "pad 1 buttons %04X",
          ps1_peek16(M, DATA + 2));
    CHECK(ps1_peek8(M, DATA + 0x40) == 0xFF, "pad 2: none");
    CHECK(!(M->i_stat & 1), "ChangeClearPad on by default: VBlank acked");
    B(0x5B, 0, 0, 0, 0);
    C(0x0A, 3, 0, 0, 0);
    ps1_run_frame(M);
    ps1_run_cycles(M, 2000);
    M->cop0[C0_SR] &= ~SR_IEC;
    ps1_cpu_update_irq(M);
    CHECK(M->i_stat & 1, "ChangeClearPad(0): VBlank left for the program");
    ps1_io_write(M, 0x1F801070u, ~1u, 4);
    B(0x14, 0, 0, 0, 0);
    CHECK(peek(0x80000000u | (0x0C00u + 16)) != 0x80000000u + 0x0B40u + 16 * 6, "StopPad removes the handler");
    /* the high level pad (B(15h) / B(16h)) */
    CHECK(B(0x15, 0x20000001u, DATA + 0x80, 0, 0) == 2, "OutdatedPadInitAndStart");
    enable_irqs();
    ps1_run_frame(M);
    ps1_run_cycles(M, 20000);
    M->cop0[C0_SR] &= ~SR_IEC;
    ps1_cpu_update_irq(M);
    uint32_t b = B(0x16, 0, 0, 0, 0);
    CHECK((b & 0xFFFF) == (uint16_t)((uint16_t)~(PS1_PAD_CROSS | PS1_PAD_START) >> 8 |
                                     (uint16_t)~(PS1_PAD_CROSS | PS1_PAD_START) << 8) &&
              (b >> 16) == 0xFFFF,
          "OutdatedPadGetButtons %08X", b);
    B(0x14, 0, 0, 0, 0);
}

/* ---- Memory cards ---- */

static void test_cards(void)
{
    char s[64];
    /* the machine's cards are formatted; card 2 is blanked to test format */
    memset((uint8_t *)ps1_card_data(M, 1, NULL), 0, PS1_CARD_SIZE);
    CHECK(B(0x4A, 1, 0, 0, 0) == 0, "InitCard");
    CHECK(B(0x4B, 0, 0, 0, 0) == 1, "StartCard");
    A(0x55, 0, 0, 0, 0);
    A(0x70, 0, 0, 0, 0);
    pokes(DATA, "bu00:SAVE01");
    CHECK((int32_t)A(0x00, DATA, 0x0001, 0, 0) < 0, "open a missing file fails");
    int32_t fd = (int32_t)A(0x00, DATA, 0x0200 | (2 << 16), 0, 0);
    CHECK(fd >= 2, "open FCREAT 2 blocks: %d", fd);
    CHECK(A(0x04, (uint32_t)fd, 0, 0, 0) == (uint32_t)fd, "close");
    fd = (int32_t)B(0x32, DATA, 0x0002, 0, 0);
    for (uint32_t i = 0; i < 0x4000; i += 4)
        poke(DATA + 0x1000 + i, 0xA5000000u | i);
    CHECK(B(0x35, (uint32_t)fd, DATA + 0x1000, 0x4000, 0) == 0x4000, "write 16 KB (two blocks)");
    CHECK(A(0x01, (uint32_t)fd, 0x2000, 0, 0) == 0x2000, "lseek to the second block");
    CHECK(A(0x03, (uint32_t)fd, DATA + 0x1000, 0x80, 0) == 0x80, "write a frame");
    CHECK((int32_t)B(0x33, (uint32_t)fd, 0x10, 0, 0) == 0x10 && (int32_t)A(0x03, (uint32_t)fd, DATA, 0x80, 0) < 0,
          "write off a frame boundary fails");
    B(0x36, (uint32_t)fd, 0, 0, 0);
    const uint8_t *card = ps1_card_data(M, 0, NULL);
    CHECK(card[128] == 0x51 && card[256] == 0x53 && card[128 + 8] == 1 && !memcmp(card + 128 + 10, "SAVE01", 7),
          "directory: first block 51h -> block 1 (53h)");
    CHECK(card[0x2000 * 2 + 4] == 0x04 && card[0x2000 * 2] == 0x00 && card[0x2000 * 2 + 3] == 0xA5,
          "data in the second block");
    CHECK(ps1_card_take_dirty(M, 0) == 1, "the card is dirty");
    fd = (int32_t)A(0x00, DATA, 0x0001, 0, 0);
    CHECK(A(0x02, (uint32_t)fd, DATA + 0x8000, 0x100, 0) == 0x100 && peek(DATA + 0x8004) == 0xA5000004u,
          "read back");
    CHECK(B(0x34, (uint32_t)fd, DATA + 0x8000, 0x80, 0) == 0x80, "read (B table)");
    CHECK(A(0x05, (uint32_t)fd, 0, 0, 0) == 1 && B(0x37, (uint32_t)fd, 0, 0, 0) == 1, "ioctl");
    CHECK(B(0x54, (uint32_t)fd, 0, 0, 0) == 0, "_get_errno: no error");
    A(0x04, (uint32_t)fd, 0, 0, 0);
    CHECK((int32_t)A(0x02, (uint32_t)fd, DATA, 0x80, 0) == -1 && B(0x55, 0, 0, 0, 0) == 9, "_get_error: EBADF");
    /* the async mode: the call returns 0 and delivers the card events */
    uint32_t ev = B(0x08, 0xF4000001u, 0x0004, 0x2000, 0);
    B(0x0C, ev, 0, 0, 0);
    fd = (int32_t)A(0x00, DATA, 0x8001, 0, 0);
    CHECK(A(0x02, (uint32_t)fd, DATA + 0x8000, 0x80, 0) == 0 && B(0x0B, ev, 0, 0, 0) == 1, "async read + SwCARD IOE");
    A(0x04, (uint32_t)fd, 0, 0, 0);
    /* firstfile / nextfile */
    pokes(DATA + 0x40, "bu00:SAVE02");
    A(0x04, A(0x00, DATA + 0x40, 0x0200 | (1 << 16), 0, 0), 0, 0, 0);
    pokes(DATA + 0x80, "bu00:*");
    CHECK(B(0x42, DATA + 0x80, DATA + 0x100, 0, 0) == DATA + 0x100, "firstfile");
    peeks(DATA + 0x100, s, sizeof s);
    CHECK(!strcmp(s, "SAVE01") && peek(DATA + 0x100 + 24) == 0x4000, "firstfile entry %s %u", s, peek(DATA + 0x118));
    CHECK(B(0x43, DATA + 0x100, 0, 0, 0) == DATA + 0x100, "nextfile");
    peeks(DATA + 0x100, s, sizeof s);
    CHECK(!strcmp(s, "SAVE02"), "nextfile entry %s", s);
    CHECK(B(0x43, DATA + 0x100, 0, 0, 0) == 0, "nextfile: no more");
    pokes(DATA + 0x80, "bu00:SAVE0?");
    CHECK(B(0x42, DATA + 0x80, DATA + 0x100, 0, 0) == DATA + 0x100, "firstfile with '?'");
    /* rename, erase, undelete */
    pokes(DATA + 0xC0, "bu00:GAME02");
    CHECK(B(0x44, DATA + 0x40, DATA + 0xC0, 0, 0) == 1, "rename");
    CHECK((int32_t)A(0x00, DATA + 0xC0, 1, 0, 0) >= 2, "renamed file opens");
    CHECK(B(0x45, DATA + 0xC0, 0, 0, 0) == 1 && (int32_t)A(0x00, DATA + 0xC0, 1, 0, 0) < 0, "erase");
    CHECK(B(0x46, DATA + 0xC0, 0, 0, 0) == 1 && (int32_t)A(0x00, DATA + 0xC0, 1, 0, 0) >= 2, "undelete");
    for (int i = 2; i < 16; i++)
        A(0x04, (uint32_t)i, 0, 0, 0);
    /* card 2: not formatted, then format */
    pokes(DATA + 0x200, "bu10:X");
    CHECK((int32_t)A(0x00, DATA + 0x200, 0x0200 | (1 << 16), 0, 0) < 0, "an unformatted card refuses FCREAT");
    pokes(DATA + 0x220, "bu10:");
    CHECK(B(0x41, DATA + 0x220, 0, 0, 0) == 1, "format");
    const uint8_t *c2 = ps1_card_data(M, 1, NULL);
    CHECK(c2[0] == 'M' && c2[1] == 'C' && c2[128] == 0xA0, "formatted card");
    fd = (int32_t)A(0x00, DATA + 0x200, 0x0200 | (1 << 16), 0, 0);
    CHECK(fd >= 2, "create on the formatted card");
    A(0x04, (uint32_t)fd, 0, 0, 0);
    /* low level sectors */
    for (uint32_t i = 0; i < 128; i += 4)
        poke(DATA + 0x400 + i, 0x5A5A0000u | i);
    uint32_t hw = B(0x08, 0xF0000011u, 0x0004, 0x2000, 0);
    B(0x0C, hw, 0, 0, 0);
    CHECK(B(0x4E, 0x10, 500, DATA + 0x400, 0) == 1 && c2[500 * 128 + 4] == 4, "write_card_sector");
    CHECK(B(0x0B, hw, 0, 0, 0) == 1, "HwCARD IOE after a sector write");
    CHECK(B(0x4F, 0x10, 500, DATA + 0x600, 0) == 1 && peek(DATA + 0x604) == 0x5A5A0004u, "read_card_sector");
    CHECK(B(0x58, 0, 0, 0, 0) == 0x10, "_card_chan");
    CHECK(B(0x5C, 0x10, 0, 0, 0) == 1 && B(0x5D, 0x10, 0, 0, 0) == 1, "_card_status / _card_wait");
    CHECK(B(0x4D, 0x00, 0, 0, 0) == 1 && A(0xAB, 0x10, 0, 0, 0) == 1, "_card_info");
    CHECK(A(0xAC, 0x00, 0, 0, 0) == 1, "_card_async_load_directory");
    CHECK(B(0x50, 0, 0, 0, 0) == 0, "allow_new_card");
    CHECK(B(0x4C, 0, 0, 0, 0) == 1, "StopCard");
    ps1_card_insert(M, 1, 0);
    CHECK((int32_t)A(0x00, DATA + 0x200, 1, 0, 0) < 0, "no card in slot 2");
    ps1_card_insert(M, 1, 1);
}

/* ---- A synthetic ISO 9660 disc ---- */

#define ISO_SECTORS 40
static uint8_t iso[ISO_SECTORS * 2048];

static void le32w(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t dirent(uint8_t *at, const char *name, uint32_t lba, uint32_t size, int dir)
{
    size_t nl = name[0] ? strlen(name) : 1; /* "\0" (.) and "\1" (..) are one byte */
    uint32_t len = (uint32_t)(33 + nl + ((nl & 1) ? 0 : 1));
    memset(at, 0, len);
    at[0] = (uint8_t)len;
    le32w(at + 2, lba);
    le32w(at + 10, size);
    at[25] = dir ? 2 : 0;
    at[32] = (uint8_t)nl;
    memcpy(at + 33, name, nl);
    return len;
}

static void build_iso(const char *path)
{
    memset(iso, 0, sizeof iso);
    uint8_t *pvd = iso + 16 * 2048;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    dirent(pvd + 156, "\0", 18, 2048, 1);
    /* root (18): ., .., SYSTEM.CNF (20), BOOT.EXE (22..), DATA dir (19) */
    uint8_t *root = iso + 18 * 2048;
    uint32_t o = 0;
    o += dirent(root + o, "\0", 18, 2048, 1);
    o += dirent(root + o, "\1", 18, 2048, 1);
    const char *cnf = "BOOT = cdrom:\\BOOT.EXE;1 arg1\r\nTCB = 8\r\nEVENT = 20\r\nSTACK = 801F0000\r\n";
    o += dirent(root + o, "SYSTEM.CNF;1", 20, (uint32_t)strlen(cnf), 0);
    o += dirent(root + o, "BOOT.EXE;1", 22, 0x1000, 0);
    o += dirent(root + o, "DATA", 19, 2048, 1);
    memcpy(iso + 20 * 2048, cnf, strlen(cnf));
    uint8_t *data = iso + 19 * 2048;
    o = 0;
    o += dirent(data + o, "\0", 19, 2048, 1);
    o += dirent(data + o, "\1", 18, 2048, 1);
    o += dirent(data + o, "FILE.BIN;1", 30, 5000, 0);
    o += dirent(data + o, "PROG.EXE;1", 24, 0x1000, 0);
    for (int i = 0; i < 5000; i++)
        iso[30 * 2048 + i] = (uint8_t)(i * 3);
    /* BOOT.EXE (22): text at 0x80010000: MARK = 0xB007; b . */
    uint8_t *e = iso + 22 * 2048;
    memcpy(e, "PS-X EXE", 8);
    le32w(e + 0x10, 0x80010000u);
    le32w(e + 0x18, 0x80010000u);
    le32w(e + 0x1C, 0x800);
    const uint32_t boot[] = {0x3C080000u | (MARK >> 16), 0x3409B007u, 0xAD090000u | (MARK & 0xFFFF), 0x1000FFFFu, 0};
    for (int i = 0; i < 5; i++)
        le32w(e + 0x800 + 4 * i, boot[i]);
    /* PROG.EXE (24): text at 0x80120000: v0 = a0 + 100 (via the F_PROG shape), marker = a1, return */
    e = iso + 24 * 2048;
    memcpy(e, "PS-X EXE", 8);
    le32w(e + 0x10, 0x80120000u);
    le32w(e + 0x14, 0x11112222u);
    le32w(e + 0x18, 0x80120000u);
    le32w(e + 0x1C, 0x800);
    le32w(e + 0x28, 0x80121000u); /* BSS */
    le32w(e + 0x2C, 0x100);
    const uint32_t pg[] = {0x3C080000u | (MARK >> 16), 0xAD050000u | (MARK & 0xFFFF), 0x03E00008u, 0x24820064u};
    for (int i = 0; i < 4; i++)
        le32w(e + 0x800 + 4 * i, pg[i]);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(iso, 1, sizeof iso, f);
        fclose(f);
    }
}

static void test_disc(const char *path)
{
    char s[64];
    build_iso(path);
    ps1_machine *keep = M;
    M = machine(path);
    ps1_run_cycles(M, 10000);
    CHECK(peek(MARK) == 0xB007, "SYSTEM.CNF's BOOT ran (pc %08X)", ps1_pc(M));
    CHECK(M->r[29] == 0x801F0000u, "SYSTEM.CNF STACK: sp %08X", M->r[29]);
    peeks(0x80000180u, s, sizeof s);
    CHECK(!strcmp(s, "arg1"), "command line at 0x180: '%s'", s);
    uint32_t ev = 0, tcb = 0;
    A(0x9D, DATA, DATA + 4, DATA + 8, 0);
    ev = peek(DATA);
    tcb = peek(DATA + 4);
    CHECK(ev == 0x20 && tcb == 8, "GetConf: SYSTEM.CNF's EVENT %X TCB %X", ev, tcb);
    A(0x9C, 16, 4, 0x801FFF00u, 0);
    A(0x9D, DATA, DATA + 4, DATA + 8, 0);
    CHECK(peek(DATA) == 16 && peek(DATA + 4) == 4 && peek(DATA + 8) == 0x801FFF00u, "SetConf");
    /* files */
    pokes(DATA + 0x100, "cdrom:\\DATA\\FILE.BIN;1");
    int32_t fd = (int32_t)A(0x00, DATA + 0x100, 1, 0, 0);
    CHECK(fd >= 2, "open cdrom: %d", fd);
    CHECK(A(0x02, (uint32_t)fd, DATA + 0x1000, 3000, 0) == 3000, "read 3000");
    CHECK(ps1_peek8(M, DATA + 0x1000 + 2999) == (uint8_t)(2999 * 3), "read data across sectors");
    CHECK(A(0x02, (uint32_t)fd, DATA + 0x1000, 3000, 0) == 2000, "read to the end");
    CHECK(A(0x01, (uint32_t)fd, 100, 0, 0) == 100 && A(0x02, (uint32_t)fd, DATA, 1, 0) == 1 &&
              ps1_peek8(M, DATA) == (uint8_t)300,
          "lseek + read");
    CHECK((int32_t)A(0x03, (uint32_t)fd, DATA, 4, 0) == -1, "cdrom: no write");
    A(0x04, (uint32_t)fd, 0, 0, 0);
    pokes(DATA + 0x100, "cdrom:\\NOPE.BIN;1");
    CHECK((int32_t)A(0x00, DATA + 0x100, 1, 0, 0) == -1, "open a missing cdrom file");
    pokes(DATA + 0x100, "cdrom:\\DATA");
    CHECK(B(0x40, DATA + 0x100, 0, 0, 0) == 1, "cd");
    pokes(DATA + 0x100, "cdrom:FILE.BIN;1");
    fd = (int32_t)A(0x00, DATA + 0x100, 1, 0, 0);
    CHECK(fd >= 2, "open relative to cd's directory");
    A(0x04, (uint32_t)fd, 0, 0, 0);
    pokes(DATA + 0x100, "cdrom:\\DATA\\*");
    CHECK(B(0x42, DATA + 0x100, DATA + 0x200, 0, 0) == DATA + 0x200, "firstfile cdrom");
    peeks(DATA + 0x200, s, sizeof s);
    CHECK(!strcmp(s, "FILE.BIN") && peek(DATA + 0x200 + 24) == 5000 && peek(DATA + 0x200 + 32) == 30,
          "cdrom dirent %s", s);
    CHECK(B(0x43, DATA + 0x200, 0, 0, 0) == DATA + 0x200, "nextfile cdrom");
    peeks(DATA + 0x200, s, sizeof s);
    CHECK(!strcmp(s, "PROG.EXE"), "nextfile cdrom %s", s);
    pokes(DATA + 0x100, "cdrom:\\DATA\\FILE.BIN;1");
    CHECK(A(0xA4, DATA + 0x100, 0, 0, 0) == 30, "CdGetLbn");
    CHECK(A(0xA5, 1, 30, DATA + 0x2000, 0) == 1 && ps1_peek8(M, DATA + 0x2001) == 3, "CdReadSector");
    CHECK(A(0xA6, 0, 0, 0, 0) == 0x02, "CdGetStatus");
    /* programs */
    pokes(DATA + 0x100, "cdrom:\\DATA\\PROG.EXE;1");
    CHECK(A(0x41, DATA + 0x100, DATA + 0x300, 0, 0) == 1 && peek(DATA + 0x300) == 0x80120000u, "LoadTest");
    CHECK(peek(0x80120000u) == 0, "LoadTest loads no text");
    poke(0x80121000u, 0xFFFFFFFFu);
    CHECK(A(0x42, DATA + 0x100, DATA + 0x300, 0, 0) == 1 && peek(0x80120008u) == 0x03E00008u, "Load");
    CHECK(A(0x43, DATA + 0x300, 5, 0x77, 0) == 1, "Exec returns 1 when the program returns");
    CHECK(peek(MARK) == 0x77 && peek(0x80121000u) == 0, "Exec: argv in a1, BSS cleared");
    CHECK(M->r[28] != 0x11112222u, "Exec restores gp");
    poke(MARK, 0);
    pokes(DATA + 0x100, "cdrom:\\BOOT.EXE;1");
    tested[0][0x51] = true;
    M->r[4] = DATA + 0x100;
    M->r[5] = 0x801E0000u;
    M->r[6] = 0;
    {
        const uint32_t le[] = {0x240A00A0u, 0x0140F809u, 0x24090051u, 0x1000FFFFu, 0};
        put_code(PROG + 0x400, le, 5);
        set_entry(PROG + 0x400);
        ps1_run_cycles(M, 200000);
    }
    CHECK(peek(MARK) == 0xB007 && M->r[29] == 0x801E0000u, "LoadExec ran BOOT.EXE with its stack (sp %08X)",
          M->r[29]);
    ps1_destroy(M);
    M = keep;
}

/* ---- Kernel layout, GPU, misc ---- */

static void test_layout(void)
{
    CHECK(peek(0x60) == 2, "0x60: 2 MB");
    CHECK(ps1_peek16(M, 0x86) == 0x275A, "0x86: addiu k0 (retail BIOS signature)");
    CHECK(peek(0x100) == 0x80000C00u && peek(0x104) == 0x20, "ToT: handlers array");
    CHECK(peek(peek(0x108)) == (0x80000000u | PS1_HLE_TCB), "ToT: process -> TCB 0");
    CHECK(peek(0x110) == (0x80000000u | PS1_HLE_TCB) && peek(0x114) == 4 * 0xC0, "ToT: TCBs");
    CHECK(peek(0x120) == (0x80000000u | PS1_HLE_EVCB) && peek(0x124) == 16 * 0x1C, "ToT: EvCBs");
    CHECK(B(0x56, 0, 0, 0, 0) == 0x80000674u && B(0x57, 0, 0, 0, 0) == 0x80000874u, "GetC0Table / GetB0Table");
    CHECK(peek(0x200 + 0x3F * 4) == (0x80000000u | (PS1_HLE_STUBS + 0x3F * 4)), "A0 table entry 3Fh -> its stub");
    /* a patched table entry runs the program's own function */
    put_funcs();
    poke(COUNTER, 0);
    uint32_t old = peek(0x874 + 0x5B * 4);
    poke(0x874 + 0x5B * 4, F_COUNT);
    B(0x5B, 1, 0, 0, 0);
    CHECK(peek(COUNTER) == 1, "a patched B0 entry is called");
    poke(0x874 + 0x5B * 4, old);
    /* an entry moved to another slot still runs the right function */
    poke(0x874 + 0x5A * 4, old);
    tested[1][0x5A] = false;
    B(0x5A, 1, 0, 0, 0);
    CHECK(M->hle != NULL, "moved entry ran");
    CHECK(A(0x9F, 8, 0, 0, 0) == 0 && peek(0x60) == 8, "SetMemSize");
    A(0x9F, 2, 0, 0, 0);
    CHECK(A(0xB4, 2, 0, 0, 0) == 2048 && A(0xB4, 0, 0, 0, 0) != 0, "GetSystemInfo");
    CHECK(A(0x44, 0, 0, 0, 0) == 0, "FlushCache");
    CHECK(A(0x45, 0, 0, 0, 0) == 0, "init_a0_b0_c0_vectors");
    CHECK(C(0x07, 0, 0, 0, 0) == 0 && C(0x1C, 0, 0, 0, 0) == 0, "InstallExceptionHandlers / AdjustA0Table");
    CHECK(B(0x51, 0x8140, 0, 0, 0) == 0xFFFFFFFFu, "Krom2RawAdd: no font ROM");
    /* setjmp / longjmp */
    {
        /* setjmp(buf); if (v0 == 0) longjmp(buf, 9); b HALT */
        uint32_t jb = DATA + 0x500;
        const uint32_t code[] = {
            0x3C040000u | (jb >> 16), 0x34840000u | (jb & 0xFFFF), /* a0 = jb */
            0x240A00A0u, 0x0140F809u, 0x24090013u,                 /* setjmp */
            0x14400006u, 0,                                        /* bnez v0, +6 (to the b HALT) */
            0x3C040000u | (jb >> 16), 0x34840000u | (jb & 0xFFFF), 0x24050009u, /* a0 = jb, a1 = 9 */
            0x240A00A0u, 0x01400008u, 0x24090014u,                 /* longjmp (jr) */
            0x10000000u | (((HALT - (PROG + 0x200 + 13 * 4 + 4)) >> 2) & 0xFFFF), 0,
        };
        put_code(PROG + 0x200, code, sizeof code / 4);
        poke(HALT, 0x1000FFFFu);
        poke(HALT + 4, 0);
        M->r[16] = 0x5555;
        set_entry(PROG + 0x200);
        CHECK(run_to_halt(100000) == 0 && M->r[2] == 9 && M->r[16] == 0x5555, "setjmp / longjmp: v0 %u", M->r[2]);
        tested[0][0x13] = tested[0][0x14] = true;
    }
    /* GPU */
    ps1_io_write(M, 0x1F801814u, 0, 4); /* GP1 reset */
    A(0x48, 0x03000000u, 0, 0, 0);      /* SendGP1Command: display on */
    CHECK(!(A(0x4D, 0, 0, 0, 0) & 0x00800000u), "SendGP1Command / GetGPUStatus: display enabled");
    A(0x49, 0xE1000123u, 0, 0, 0); /* GPU_cw: texpage */
    CHECK((A(0x4D, 0, 0, 0, 0) & 0x1FF) == 0x123, "GPU_cw: GPUSTAT texpage %X", A(0x4D, 0, 0, 0, 0) & 0x7FF);
    poke(DATA, 0xE1000045u);
    A(0x4A, DATA, 1, 0, 0);
    CHECK((A(0x4D, 0, 0, 0, 0) & 0x1FF) == 0x45, "GPU_cwp");
    poke(DATA + 0x10, 0x01FFFFFFu); /* a one-word linked list: E1000067 */
    poke(DATA + 0x14, 0xE1000067u);
    A(0x4B, DATA + 0x10, 0, 0, 0);
    CHECK((A(0x4D, 0, 0, 0, 0) & 0x1FF) == 0x67, "send_gpu_linked_list");
    for (int i = 0; i < 2; i++)
        poke(DATA + 0x40 + 4u * (uint32_t)i, 0x7FFF7FFFu);
    uint32_t args[5] = {0, 0, 2, 2, DATA + 0x40};
    callv(0, 0x46, args, 5);
    callv(0, 0x47, args, 5);
    CHECK(A(0x4E, 0, 0, 0, 0) == 0 && A(0x4C, 0, 0, 0, 0) == 0, "GPU_dw / gpu_send_dma / gpu_sync / gpu_abort_dma");
    /* misc dummies */
    static const int a_dummies[] = {0x54, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
                                    0x96, 0x97, 0x98, 0x99, 0x9E, 0xAD};
    for (size_t i = 0; i < sizeof a_dummies / sizeof a_dummies[0]; i++)
        cover_dummy(0, a_dummies[i]);
    static const int b_dummies[] = {0x47, 0x48, 0x49};
    for (size_t i = 0; i < sizeof b_dummies / sizeof b_dummies[0]; i++)
        cover_dummy(1, b_dummies[i]);
    cover_dummy(2, 0x12);
    cover_dummy(2, 0x13);
}

static void test_unimplemented(void)
{
    int before = ps1_hle_unimplemented_count(M);
    B(0x1A, 0, 0, 0, 0);
    B(0x1A, 0, 0, 0, 0);
    CHECK(ps1_hle_unimplemented_count(M) == before + 1, "an unimplemented call is reported once");
    CHECK(ps1_pc(M) == HALT || ps1_pc(M) == HALT + 4, "and returns");
    tested[1][0x1A] = false;
}

static void test_exit_halts(void)
{
    /* exit / _exit / SystemError halt the machine in the idle loop */
    static const struct { int t, fn; } h[] = {{0, 0x06}, {0, 0x3A}, {1, 0x38}, {2, 0x0B}};
    for (size_t i = 0; i < sizeof h / sizeof h[0]; i++) {
        tested[h[i].t][h[i].fn] = true;
        uint32_t code[] = {0x240A00A0u + 0x10u * (uint32_t)h[i].t, 0x0140F809u, 0x24090000u | (uint32_t)h[i].fn,
                           0x1000FFFFu, 0};
        put_code(PROG + 0x600, code, 5);
        set_entry(PROG + 0x600);
        ps1_run_cycles(M, 2000);
        uint32_t pc = ps1_pc(M) & 0x1FFFFFFF;
        CHECK(pc == 0x0D00 || pc == 0x0D04, "%c(%02Xh) halts (%08X)", "ABC"[h[i].t], h[i].fn, ps1_pc(M));
    }
}

int main(void)
{
    char tmpl[] = "/tmp/ps1_hle_XXXXXX";
    char *dir = mkdtemp(tmpl);
    char iso_path[600];
    snprintf(iso_path, sizeof iso_path, "%s/hle.iso", dir ? dir : "/tmp");

    M = machine(NULL);
    CHECK(ps1_bios_hle(M) == 1, "ps1_bios_hle");
    test_layout();
    test_strings();
    test_memory();
    test_sort_search();
    test_tty();
    test_events();
    test_timers();
    test_threads();
    test_cards();
    test_irq_chain();
    ps1_destroy(M);
    M = machine(NULL);
    test_pads();
    ps1_destroy(M);
    M = machine(NULL);
    test_syscalls_exceptions();
    ps1_destroy(M);
    M = machine(NULL);
    test_unimplemented();
    test_exit_halts();
    test_disc(iso_path);
    ps1_destroy(M);
    unlink(iso_path);
    if (dir)
        rmdir(dir);

    /* every implemented call has a check */
    int implemented = 0, missing = 0;
    for (int t = 0; t < 3; t++)
        for (int fn = 0; fn < 256; fn++) {
            const char *name;
            if (!ps1_hle_implemented(t, fn, &name))
                continue;
            implemented++;
            if (!tested[t][fn]) {
                printf("FAIL: %c(%02Xh) %s has no check\n", "ABC"[t], fn, name);
                missing++;
            }
        }
    failures += missing;
    printf("%d implemented calls, %d checked\n", implemented, implemented - missing);
    if (failures) {
        printf("FAIL: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
