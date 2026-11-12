/* recomp_selftest.h (XBA-51): the runtime's risky pieces, in the calling
 * process. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1          /* REG_* in ucontext, as the decoders want */
#endif
#include "recomp_selftest.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/kernel.h"
#include "kernel/xbox_memory_layout.h"

/* Guest RAM the self-test borrows, between the synthetic image (it ends near
 * 0x00012000) and the toolkit region (0x00700000). Both 2 MB aligned, so a
 * host page (4, 16 or 64 KB) starts there. Nothing allocates from here: the
 * heap starts in the toolkit region, and the self-test never touches the heap
 * (its block table outlives a shutdown). */
#define SCRATCH_MMIO   0x00200000u     /* the trapped page */
#define SCRATCH_EXEC   0x00400000u     /* the page whose protection changes */
#define SCRATCH_MIRROR 0x00300000u     /* where the mirrors are probed */
#define PROBE_CONTIG   0x00001000u     /* offset into the contiguous window / tiled aperture */

#define WINDOW_4GB     0x100000000ull

static int s_pass, s_fail, s_skip;
static int s_probe[2] = { -1, -1 };
static long s_hpage;
static char s_reason[192];

/* ---- reporting ---------------------------------------------------------- */

static const char *why(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_reason, sizeof s_reason, fmt, ap);
    va_end(ap);
    return s_reason;
}

static int skipped(const char *name)
{
    const char *list = getenv("RECOMP_SELFTEST_SKIP");
    size_t n = strlen(name);

    while (list && *list) {
        size_t len = strcspn(list, ", \t");
        if (len == n && strncmp(list, name, n) == 0)
            return 1;
        list += len;
        list += strspn(list, ", \t");
    }
    return 0;
}

/* fail: NULL for a pass. note: shown after PASS, may be NULL. Flushed at
 * once: a later check may crash, and these lines say how far it got. */
static void report(const char *name, const char *fail, const char *note)
{
    if (fail) {
        printf("selftest %s: FAIL (%s)\n", name, fail);
        s_fail++;
    } else if (note && *note) {
        printf("selftest %s: PASS (%s)\n", name, note);
        s_pass++;
    } else {
        printf("selftest %s: PASS\n", name);
        s_pass++;
    }
    fflush(stdout);
}

/* ---- probes that never fault -------------------------------------------- */

/* The kernel copies through the address for us and says EFAULT when it
 * cannot, instead of a SIGSEGV that, with the toolkit's handler in front,
 * would end in debuggerd. */
static int probe_read(const volatile void *addr)
{
    unsigned char c;
    if (write(s_probe[1], (const void *)addr, 1) != 1)
        return 0;
    return read(s_probe[0], &c, 1) == 1;
}

static int probe_write(volatile void *addr, unsigned char value)
{
    unsigned char c;
    if (write(s_probe[1], &value, 1) != 1)
        return 0;
    if (read(s_probe[0], (void *)addr, 1) == 1)
        return 1;
    (void)!read(s_probe[0], &c, 1);          /* the byte stays in the pipe on EFAULT */
    return 0;
}

/* The permission letters of the mapping that holds addr ("rw-s"), "" if
 * none. */
static void maps_perms(uintptr_t addr, char out[5])
{
    char line[512];
    FILE *f = fopen("/proc/self/maps", "r");

    out[0] = '\0';
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        char perms[5];
        if (sscanf(line, "%llx-%llx %4s", &lo, &hi, perms) == 3 && addr >= lo && addr < hi) {
            memcpy(out, perms, 5);
            break;
        }
    }
    fclose(f);
}

/* The first address in [lo, hi) no mapping covers, or hi when they all do. */
static uint64_t maps_first_gap(uint64_t lo, uint64_t hi)
{
    char line[512];
    uint64_t cur = lo;
    FILE *f = fopen("/proc/self/maps", "r");

    if (!f)
        return lo;
    while (cur < hi && fgets(line, sizeof line, f)) {
        unsigned long long a, b;
        if (sscanf(line, "%llx-%llx", &a, &b) != 2 || b <= cur)
            continue;
        if (a > cur)
            break;                           /* sorted: nothing below a is left */
        cur = b;
    }
    fclose(f);
    return cur < hi ? cur : hi;
}

/* ---- the trapped device ------------------------------------------------- */

static struct {
    uint8_t reg[0x400];
    unsigned accesses;
} s_dev;

static uint64_t dev_rd(void *dev, uint32_t off, int size)
{
    uint64_t v = 0;
    (void)dev;
    if (size > 8) size = 8;
    if (off + (uint32_t)size <= sizeof s_dev.reg)
        memcpy(&v, s_dev.reg + off, (size_t)size);
    s_dev.accesses++;
    return v;
}

static void dev_wr(void *dev, uint32_t off, uint64_t v, int size)
{
    (void)dev;
    if (size > 8) size = 8;
    if (off + (uint32_t)size <= sizeof s_dev.reg)
        memcpy(s_dev.reg + off, &v, (size_t)size);
    s_dev.accesses++;
}

static uint32_t reg32(uint32_t off) { uint32_t v; memcpy(&v, s_dev.reg + off, 4); return v; }
static uint64_t reg64(uint32_t off) { uint64_t v; memcpy(&v, s_dev.reg + off, 8); return v; }
static void set32(uint32_t off, uint32_t v) { memcpy(s_dev.reg + off, &v, 4); }
static void set64(uint32_t off, uint64_t v) { memcpy(s_dev.reg + off, &v, 8); }

#define WANT(name, got, want) \
    do { uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want); \
         if (g_ != w_) return why("%s: got 0x%llX, want 0x%llX", name, \
                                  (unsigned long long)g_, (unsigned long long)w_); } while (0)

typedef const char *(*mmio_check_fn)(uint8_t *w);

#if defined(__aarch64__)
/* The A64 forms clang emits for MEM8/16/32() in lifted code and for the
 * runtime's own device accesses (src/platform/mmio_decode_a64.h). */
static const char *mmio_imm(uint8_t *w)           /* LDR/STR, unsigned immediate */
{
    uint64_t x = ~0ull;
    __asm__ volatile("str %w0, [%1, #0x40]" : : "r"(0xDEADBEEFu), "r"(w) : "memory");
    WANT("str w", reg32(0x40), 0xDEADBEEFu);
    __asm__ volatile("ldr %w0, [%1, #0x40]" : "+r"(x) : "r"(w) : "memory");
    WANT("ldr w clears 63:32", x, 0xDEADBEEFu);
    __asm__ volatile("str %0, [%1, #0x48]" : : "r"(0x0102030405060708ull), "r"(w) : "memory");
    x = 0;
    __asm__ volatile("ldr %0, [%1, #0x48]" : "=r"(x) : "r"(w) : "memory");
    WANT("str/ldr x", x, 0x0102030405060708ull);
    return NULL;
}

static const char *mmio_extend(uint8_t *w)        /* narrow, zero- and sign-extending */
{
    uint64_t x = ~0ull;
    __asm__ volatile("strb %w0, [%1, #0x60]" : : "r"(0x80u), "r"(w) : "memory");
    __asm__ volatile("strh %w0, [%1, #0x62]" : : "r"(0xBEEFu), "r"(w) : "memory");
    WANT("strb/strh", reg32(0x60), 0xBEEF0080u);
    __asm__ volatile("ldrb %w0, [%1, #0x60]" : "+r"(x) : "r"(w) : "memory");
    WANT("ldrb", x, 0x80);
    __asm__ volatile("ldrsb %0, [%1, #0x60]" : "=r"(x) : "r"(w) : "memory");
    WANT("ldrsb x", x, 0xFFFFFFFFFFFFFF80ull);
    __asm__ volatile("ldrsh %0, [%1, #0x62]" : "=r"(x) : "r"(w) : "memory");
    WANT("ldrsh x", x, 0xFFFFFFFFFFFFBEEFull);
    set32(0x64, 0x80000000u);
    __asm__ volatile("ldrsw %0, [%1, #0x64]" : "=r"(x) : "r"(w) : "memory");
    WANT("ldrsw", x, 0xFFFFFFFF80000000ull);
    return NULL;
}

static const char *mmio_regoff(uint8_t *w)        /* register offset, scaled index */
{
    uint64_t idx = 0x80 / 4, off = 0x84;
    uint32_t v = 0;
    set32(0x80, 0x13579BDFu);
    __asm__ volatile("ldr %w0, [%1, %2, lsl #2]" : "=r"(v) : "r"(w), "r"(idx) : "memory");
    WANT("ldr w [x, x, lsl 2]", v, 0x13579BDFu);
    __asm__ volatile("str %w0, [%1, %2]" : : "r"(0x2468ACE0u), "r"(w), "r"(off) : "memory");
    WANT("str w [x, x]", reg32(0x84), 0x2468ACE0u);
    return NULL;
}

static const char *mmio_writeback(uint8_t *w)     /* post- and pre-index: the base moves */
{
    uint8_t *q = w + 0x100;
    uint32_t v = 0;
    __asm__ volatile("str %w1, [%0], #4\n\tstr %w2, [%0], #4"
                     : "+r"(q) : "r"(0x11111111u), "r"(0x22222222u) : "memory");
    WANT("post-index base", (uintptr_t)q, (uintptr_t)(w + 0x108));
    WANT("post-index stores", reg64(0x100), 0x2222222211111111ull);
    q = w + 0x100;
    __asm__ volatile("ldr %w0, [%1, #4]!" : "=r"(v), "+r"(q) : : "memory");
    WANT("pre-index value", v, 0x22222222u);
    WANT("pre-index base", (uintptr_t)q, (uintptr_t)(w + 0x104));
    return NULL;
}

static const char *mmio_unscaled(uint8_t *w)      /* LDUR/STUR, signed offset */
{
    uint32_t v = 0;
    __asm__ volatile("stur %w0, [%1, #-4]" : : "r"(0x33333333u), "r"(w + 0x124) : "memory");
    WANT("stur", reg32(0x120), 0x33333333u);
    __asm__ volatile("ldur %w0, [%1, #-4]" : "=r"(v) : "r"(w + 0x124) : "memory");
    WANT("ldur", v, 0x33333333u);
    return NULL;
}

static const char *mmio_pair(uint8_t *w)          /* LDP/STP, LDPSW */
{
    uint64_t a = 0, b = 0;
    __asm__ volatile("stp %w0, %w1, [%2]" : : "r"(5u), "r"(0x80000006u), "r"(w + 0x140) : "memory");
    WANT("stp w", reg64(0x140), 0x8000000600000005ull);
    __asm__ volatile("ldp %w0, %w1, [%2]" : "=r"(a), "=r"(b) : "r"(w + 0x140) : "memory");
    WANT("ldp w first", a, 5);
    WANT("ldp w second", b, 0x80000006u);
    __asm__ volatile("ldpsw %0, %1, [%2]" : "=r"(a), "=r"(b) : "r"(w + 0x140) : "memory");
    WANT("ldpsw second", b, 0xFFFFFFFF80000006ull);
    return NULL;
}

static const char *mmio_simd(uint8_t *w)          /* LDR/STR q and d: the V register in the frame */
{
    static const uint64_t src[2] __attribute__((aligned(16))) =
        { 0x0011223344556677ull, 0x8899AABBCCDDEEFFull };
    uint64_t lo = 0, hi = 0;
    double d = 0;
    set64(0x180, src[0]);
    set64(0x188, src[1]);
    __asm__ volatile("ldr q0, [%2]\n\tfmov %0, d0\n\tmov %1, v0.d[1]"
                     : "=r"(lo), "=r"(hi) : "r"(w + 0x180) : "v0", "memory");
    WANT("ldr q low half", lo, src[0]);
    WANT("ldr q high half", hi, src[1]);
    __asm__ volatile("ldr q0, [%0]\n\tstr q0, [%1]" : : "r"(src), "r"(w + 0x1A0) : "v0", "memory");
    WANT("str q low half", reg64(0x1A0), src[0]);
    WANT("str q high half", reg64(0x1A8), src[1]);
    __asm__ volatile("ldr %d0, [%1]" : "=w"(d) : "r"(w + 0x188) : "memory");
    memcpy(&lo, &d, 8);
    WANT("ldr d", lo, src[1]);
    return NULL;
}

static const struct { const char *name; mmio_check_fn fn; } MMIO_CHECKS[] = {
    { "mmio_imm", mmio_imm }, { "mmio_extend", mmio_extend }, { "mmio_regoff", mmio_regoff },
    { "mmio_writeback", mmio_writeback }, { "mmio_unscaled", mmio_unscaled },
    { "mmio_pair", mmio_pair }, { "mmio_simd", mmio_simd },
};
#define MMIO_ARCH "A64"

#elif defined(__x86_64__) && defined(__linux__)
/* The x86-64 forms src/platform/mmio_decode_x86_64.h covers. */
static const char *mmio_mov(uint8_t *w)           /* MOV r/m32 and r/m64 both ways */
{
    uint64_t x = ~0ull;
    __asm__ volatile("movl %0, 0x40(%1)" : : "r"(0xDEADBEEFu), "r"(w) : "memory");
    WANT("mov m32, r32", reg32(0x40), 0xDEADBEEFu);
    __asm__ volatile("movl 0x40(%1), %k0" : "+r"(x) : "r"(w) : "memory");
    WANT("mov r32, m32 clears 63:32", x, 0xDEADBEEFu);
    __asm__ volatile("movq %0, 0x48(%1)" : : "r"(0x0102030405060708ull), "r"(w) : "memory");
    x = 0;
    __asm__ volatile("movq 0x48(%1), %0" : "=r"(x) : "r"(w) : "memory");
    WANT("mov r64 both ways", x, 0x0102030405060708ull);
    return NULL;
}

static const char *mmio_extend(uint8_t *w)        /* MOVZX / MOVSX, byte stores */
{
    uint32_t v = 0;
    __asm__ volatile("movb %b0, 0x60(%1)" : : "r"(0x80u), "r"(w) : "memory");
    WANT("mov m8, r8", s_dev.reg[0x60], 0x80);
    s_dev.reg[0x62] = 0xEF;
    s_dev.reg[0x63] = 0xBE;
    __asm__ volatile("movzbl 0x60(%1), %0" : "=r"(v) : "r"(w) : "memory");
    WANT("movzx r32, m8", v, 0x80);
    __asm__ volatile("movsbl 0x60(%1), %0" : "=r"(v) : "r"(w) : "memory");
    WANT("movsx r32, m8", v, 0xFFFFFF80u);
    __asm__ volatile("movzwl 0x62(%1), %0" : "=r"(v) : "r"(w) : "memory");
    WANT("movzx r32, m16", v, 0xBEEF);
    __asm__ volatile("movswl 0x62(%1), %0" : "=r"(v) : "r"(w) : "memory");
    WANT("movsx r32, m16", v, 0xFFFFBEEFu);
    return NULL;
}

static const char *mmio_immstore(uint8_t *w)      /* MOV r/m, imm */
{
    __asm__ volatile("movl $0x12345678, 0x80(%0)" : : "r"(w) : "memory");
    WANT("mov m32, imm32", reg32(0x80), 0x12345678u);
    __asm__ volatile("movb $0x5A, 0x84(%0)" : : "r"(w) : "memory");
    WANT("mov m8, imm8", s_dev.reg[0x84], 0x5A);
    return NULL;
}

static const char *mmio_xmm_scalar(uint8_t *w)    /* MOVD / MOVQ between XMM and memory */
{
    uint32_t v = 0;
    uint64_t q = 0;
    set32(0xA0, 0x3FC00000u);
    set64(0xA8, 0x0123456789ABCDEFull);
    __asm__ volatile("movd 0xA0(%1), %%xmm0\n\tmovd %%xmm0, %0" : "=r"(v) : "r"(w) : "xmm0", "memory");
    WANT("movd xmm, m32", v, 0x3FC00000u);
    __asm__ volatile("movd %0, %%xmm0\n\tmovd %%xmm0, 0xA4(%1)" : : "r"(0x40490FDBu), "r"(w) : "xmm0", "memory");
    WANT("movd m32, xmm", reg32(0xA4), 0x40490FDBu);
    __asm__ volatile("movq 0xA8(%1), %%xmm0\n\tmovq %%xmm0, %0" : "=r"(q) : "r"(w) : "xmm0", "memory");
    WANT("movq xmm, m64", q, 0x0123456789ABCDEFull);
    return NULL;
}

static const char *mmio_simd(uint8_t *w)          /* MOVUPS both ways: the XMM register in the frame */
{
    static const uint64_t src[2] __attribute__((aligned(16))) =
        { 0x0011223344556677ull, 0x8899AABBCCDDEEFFull };
    uint64_t lo = 0, hi = 0;
    set64(0x180, src[0]);
    set64(0x188, src[1]);
    __asm__ volatile("movups 0x180(%2), %%xmm0\n\tmovq %%xmm0, %0\n\t"
                     "movhlps %%xmm0, %%xmm1\n\tmovq %%xmm1, %1"
                     : "=r"(lo), "=r"(hi) : "r"(w) : "xmm0", "xmm1", "memory");
    WANT("movups load low half", lo, src[0]);
    WANT("movups load high half", hi, src[1]);
    __asm__ volatile("movups (%0), %%xmm0\n\tmovups %%xmm0, 0x1A0(%1)"
                     : : "r"(src), "r"(w) : "xmm0", "memory");
    WANT("movups store low half", reg64(0x1A0), src[0]);
    WANT("movups store high half", reg64(0x1A8), src[1]);
    return NULL;
}

static const struct { const char *name; mmio_check_fn fn; } MMIO_CHECKS[] = {
    { "mmio_mov", mmio_mov }, { "mmio_extend", mmio_extend }, { "mmio_immstore", mmio_immstore },
    { "mmio_xmm_scalar", mmio_xmm_scalar }, { "mmio_simd", mmio_simd },
};
#define MMIO_ARCH "x86-64"

#else
static const struct { const char *name; mmio_check_fn fn; } MMIO_CHECKS[] = { { "mmio_decoder", NULL } };
#define MMIO_ARCH "none"
#endif

#define NUM_MMIO_CHECKS (sizeof MMIO_CHECKS / sizeof MMIO_CHECKS[0])

/* ---- the checks --------------------------------------------------------- */

static uint8_t *guest(uint64_t va)
{
    return (uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + (uintptr_t)va);
}

static const char *check_window(char *note, size_t note_len)
{
    uint64_t lo = (uintptr_t)xbox_GetMemoryOffset(), gap;
    const char *e = getenv("RECOMP_GUEST_WINDOW");

    if (!xbox_GuestWindowHeld()) {
        /* The default on every 64-bit host (XBA-103; Android since XBA-21). */
        if (sizeof(void *) >= 8 && (!e || !*e || *e != '0'))
            return "not held: the 4 GB reservation was refused (layout fell back to fixed addresses)";
        snprintf(note, note_len, "not held: %s", e && *e ? "RECOMP_GUEST_WINDOW=0" : "32-bit host");
        return NULL;
    }
    if (access("/proc/self/maps", R_OK) != 0) {
        snprintf(note, note_len, "4 GB held at %p; no /proc/self/maps to check it", (void *)(uintptr_t)lo);
        return NULL;
    }
    gap = maps_first_gap(lo, lo + WINDOW_4GB);
    if (gap < lo + WINDOW_4GB)
        return why("held, but /proc/self/maps has nothing at guest 0x%08llX",
                   (unsigned long long)(gap - lo));
    snprintf(note, note_len, "4 GB held at %p", (void *)(uintptr_t)lo);
    return NULL;
}

static const char *check_mirrors(char *note, size_t note_len)
{
    uint8_t *base = (uint8_t *)xbox_GetMemoryBase();
    size_t size = xbox_GetMappedSize();
    int m;

    for (m = 1; m <= XBOX_NUM_MIRRORS; m++) {
        volatile uint8_t *at = base + SCRATCH_MIRROR + m * 16;
        volatile uint8_t *mir = base + (size_t)m * size + SCRATCH_MIRROR + m * 16;
        unsigned char a = (unsigned char)(0xA0 + m), b = (unsigned char)(0x30 + m);

        if (!probe_read(mir))
            return why("mirror %d at %p is not readable", m, (void *)mir);
        if (!probe_write(mir, a))
            return why("mirror %d at %p is not writable", m, (void *)mir);
        *mir = a;                                    /* the CPU's own store, now it is known safe */
        if (*at != a)
            return why("a write through mirror %d is not at the base (0x%02X)", m, *at);
        *at = b;
        if (*mir != b)
            return why("a write at the base is not in mirror %d (0x%02X)", m, *mir);
    }
    snprintf(note, note_len, "%d mirrors of %zu MB", XBOX_NUM_MIRRORS, size >> 20);
    return NULL;
}

static const char *check_tiled_alias(void)
{
    volatile uint8_t *contig = guest(XBOX_CONTIG_BASE + PROBE_CONTIG);
    volatile uint8_t *tiled = guest(XBOX_TILED_BASE + PROBE_CONTIG);

    if (!probe_write(contig, 0x5C))
        return why("contiguous window at %p is not writable", (void *)contig);
    if (!probe_read(tiled))
        return why("tiled aperture at %p is not readable", (void *)tiled);
    *contig = 0x5C;
    if (*tiled != 0x5C)
        return why("a write to the contiguous window is not in the tiled aperture (0x%02X)", *tiled);
    if (!probe_write(tiled, 0xC5))
        return "the tiled aperture is not writable";
    *tiled = 0xC5;
    if (*contig != 0xC5)
        return why("a write to the tiled aperture is not in the contiguous window (0x%02X)", *contig);
    return NULL;
}

/* Arms the trapped page: PAGE_NOACCESS over one host page of guest RAM, and
 * the window that routes its faults to s_dev. */
static const char *mmio_arm(void)
{
    DWORD old;

    if (xbox_PosixInstallMmioFaultHandler() != 0)
        return "xbox_PosixInstallMmioFaultHandler failed";
    if (SCRATCH_MMIO < g_xbox_image_hi || SCRATCH_EXEC + (uint32_t)s_hpage > g_xbox_region_base)
        return why("scratch pages 0x%08X-0x%08X overlap the image (ends 0x%08X) or the region (0x%08X)",
                   SCRATCH_MMIO, SCRATCH_EXEC + (uint32_t)s_hpage, g_xbox_image_hi, g_xbox_region_base);
    memset(&s_dev, 0, sizeof s_dev);
    if (xbox_PosixMmioSetTestWindow(SCRATCH_MMIO, (uint32_t)s_hpage, &s_dev, dev_rd, dev_wr) != 0)
        return "no trapped-MMIO decoder on this host";
    if (!VirtualProtect(guest(SCRATCH_MMIO), (SIZE_T)s_hpage, PAGE_NOACCESS, &old)) {
        xbox_PosixMmioSetTestWindow(0, 0, NULL, NULL, NULL);
        return why("could not make the page at guest 0x%08X fault (errno %d)", SCRATCH_MMIO, errno);
    }
    if (probe_read(guest(SCRATCH_MMIO))) {
        xbox_PosixMmioSetTestWindow(0, 0, NULL, NULL, NULL);
        return "the trapped page is still readable";
    }
    return NULL;
}

static void mmio_disarm(void)
{
    DWORD old;
    VirtualProtect(guest(SCRATCH_MMIO), (SIZE_T)s_hpage, PAGE_READWRITE, &old);
    xbox_PosixMmioSetTestWindow(0, 0, NULL, NULL, NULL);
}

static const char *check_exec_alloc(void)
{
    PVOID p = NULL;
    SIZE_T size = (SIZE_T)s_hpage;
    NTSTATUS st = xbox_NtAllocateVirtualMemory(&p, 0, &size, MEM_COMMIT | MEM_RESERVE,
                                               PAGE_EXECUTE_READWRITE);
    char perms[5];
    const char *fail = NULL;

    if (st != 0 || !p)
        return why("NtAllocateVirtualMemory(PAGE_EXECUTE_READWRITE) returned 0x%08X", (unsigned)st);
    if (!probe_write(p, 0x90) || !probe_read(p))
        fail = "the allocation is not readable and writable";
    maps_perms((uintptr_t)p, perms);
    if (!fail && perms[0] && perms[2] == 'x')
        fail = why("the allocation is mapped executable (%s)", perms);
    size = 0;
    xbox_NtFreeVirtualMemory(&p, &size, MEM_RELEASE);
    return fail;
}

static const char *check_exec_guest(void)
{
    uint8_t *page = guest(SCRATCH_EXEC);
    char perms[5];

    xbox_MmSetAddressProtect(page, (ULONG)s_hpage, PAGE_EXECUTE_READ);
    if (!probe_read(page))
        return "PAGE_EXECUTE_READ: guest RAM is not readable (the protection was refused?)";
    if (probe_write(page, 0x11))
        return "PAGE_EXECUTE_READ: guest RAM is still writable (the protection was refused)";
    maps_perms((uintptr_t)page, perms);
    if (perms[0] && perms[2] == 'x')
        return why("PAGE_EXECUTE_READ: guest RAM is mapped executable (%s)", perms);
    xbox_MmSetAddressProtect(page, (ULONG)s_hpage, PAGE_EXECUTE_READWRITE);
    if (!probe_write(page, 0x22) || !probe_read(page))
        return "PAGE_EXECUTE_READWRITE: guest RAM is not readable and writable";
    maps_perms((uintptr_t)page, perms);
    if (perms[0] && perms[2] == 'x')
        return why("PAGE_EXECUTE_READWRITE: guest RAM is mapped executable (%s)", perms);
    xbox_MmSetAddressProtect(page, (ULONG)s_hpage, PAGE_READWRITE);
    return NULL;
}

/* A title's large reservation, served lazily from the held window (XBA-103):
 * Fable reserves 608 MB for its allocator. The first and last byte are
 * writable, a write there does not reach low RAM through a mirror, and a
 * release gives back pages that read as zero. Run after the mirror and MMIO
 * checks: the reservation replaces the top mirrors until the next init. */
static const char *check_reserve(char *note, size_t note_len)
{
    const uint32_t size = 608u << 20;
    uint32_t va = xbox_ReserveAlloc(size, 0x10000), fb = 0, fs = 0;
    size_t ram = xbox_GetMappedSize();
    volatile uint8_t *lo, *hi, *alias;
    uint8_t before;

    if (!va) {
        if (!xbox_GuestWindowHeld()) {
            snprintf(note, note_len, "no lazy reservations: the guest window is not held");
            return NULL;
        }
        return "xbox_ReserveAlloc(608 MB) failed with the guest window held";
    }
    lo = guest(va);
    hi = guest((uint64_t)va + size - 1);
    alias = guest(va % ram);
    before = *alias;
    if (!probe_write(lo, (uint8_t)(before ^ 0xFF)) || !probe_write(hi, 0xA5))
        return why("the reservation at guest 0x%08X is not writable", va);
    if (*alias != before)
        return why("a write at guest 0x%08X reached low RAM at 0x%08X", va,
                   (unsigned)(va % ram));
    if (!xbox_ReserveHolds(va, size))
        return "xbox_ReserveHolds does not cover the reservation";
    if (xbox_ReserveFree(va, 0, 1, &fb, &fs) != 1 || fb != va || fs != size)
        return why("release: base 0x%08X size %u", fb, fs);
    if (!probe_read(lo) || *lo != 0 || *hi != 0)
        return "released pages do not read as zero";
    snprintf(note, note_len, "608 MB at guest 0x%08X", va);
    return NULL;
}

static const char *check_save_dir(const char *dir)
{
    char path[1024];
    unsigned char out[4096], in[sizeof out];
    int fd;
    size_t i;

    if (!dir || !*dir)
        return "no save directory";
    snprintf(path, sizeof path, "%s/selftest-%ld.tmp", dir, (long)getpid());
    for (i = 0; i < sizeof out; i++)
        out[i] = (unsigned char)(i * 7 + 3);
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0)
        return why("cannot create %s: %s", path, strerror(errno));
    if (write(fd, out, sizeof out) != (ssize_t)sizeof out || fsync(fd) != 0) {
        int e = errno;
        close(fd);
        unlink(path);
        return why("cannot write %s: %s", path, strerror(e));
    }
    close(fd);
    fd = open(path, O_RDONLY);
    if (fd < 0 || read(fd, in, sizeof in) != (ssize_t)sizeof in || memcmp(in, out, sizeof in) != 0) {
        if (fd >= 0) close(fd);
        unlink(path);
        return why("%s does not read back what was written", path);
    }
    close(fd);
    if (unlink(path) != 0)
        return why("cannot remove %s: %s", path, strerror(errno));
    return NULL;
}

/* ---- the run ------------------------------------------------------------ */

#define RUN(name, expr) \
    do { if (skipped(name)) { printf("selftest %s: SKIP (RECOMP_SELFTEST_SKIP)\n", name); \
                              fflush(stdout); s_skip++; } \
         else { note[0] = '\0'; report(name, (expr), note); } } while (0)

int recomp_selftest_run(const void *xbe, size_t xbe_len, const char *save_dir)
{
    static const char *const NEEDS_LAYOUT = "needs the layout";
    char note[160];
    int up = 0, armed = 0;
    size_t i;

    s_pass = s_fail = s_skip = 0;
    s_hpage = sysconf(_SC_PAGESIZE);
    printf("selftest: start (pid %ld, host page %ld, %s decoder, XBE %zu bytes, save dir %s)\n",
           (long)getpid(), s_hpage, MMIO_ARCH, xbe ? xbe_len : 0, save_dir ? save_dir : "none");
    fflush(stdout);
    if (pipe(s_probe) != 0) {
        report("probe", why("pipe: %s", strerror(errno)), NULL);
        printf("selftest: %d passed, %d failed\n", s_pass, s_fail);
        return s_fail;
    }
    fcntl(s_probe[0], F_SETFL, O_NONBLOCK);
    fcntl(s_probe[1], F_SETFL, O_NONBLOCK);

    /* layout first: everything but save_dir needs it. */
    {
        const char *fail = NULL;
        int already = xbox_GetMemoryBase() != NULL;  /* someone else's: not ours to test or shut down */
        if (!xbe || !xbe_len)
            fail = "no synthetic XBE";
        else if (already)
            fail = "the memory layout is already up";
        else if (!xbox_MemoryLayoutInit(xbe, xbe_len))
            fail = "xbox_MemoryLayoutInit failed (its lines above say why)";
        else if ((uintptr_t)xbox_GetMemoryBase() % (uintptr_t)s_hpage)
            fail = why("base %p is not aligned to the host page", xbox_GetMemoryBase());
        up = !already && xbox_GetMemoryBase() != NULL;
        if (!fail)
            snprintf(note, sizeof note, "base %p", xbox_GetMemoryBase());
        else
            note[0] = '\0';
        report("layout", fail, note);
    }

    RUN("window", up ? check_window(note, sizeof note) : NEEDS_LAYOUT);
    RUN("mirrors", up ? check_mirrors(note, sizeof note) : NEEDS_LAYOUT);
    RUN("tiled_alias", up ? check_tiled_alias() : NEEDS_LAYOUT);

    {
        const char *fail = up ? mmio_arm() : NEEDS_LAYOUT;
        armed = fail == NULL;
        note[0] = '\0';
        if (armed)
            snprintf(note, sizeof note, "%s, trapped page at guest 0x%08X", MMIO_ARCH, SCRATCH_MMIO);
        report("mmio_handler", fail, note);
    }
    for (i = 0; i < NUM_MMIO_CHECKS; i++) {
        const char *name = MMIO_CHECKS[i].name;
        if (skipped(name)) {
            printf("selftest %s: SKIP (RECOMP_SELFTEST_SKIP)\n", name);
            fflush(stdout);
            s_skip++;
        } else if (!MMIO_CHECKS[i].fn) {
            report(name, "no trapped-MMIO decoder on this host", NULL);
        } else if (!armed) {
            report(name, "needs mmio_handler", NULL);
        } else {
            unsigned before = s_dev.accesses;
            const char *fail = MMIO_CHECKS[i].fn(guest(SCRATCH_MMIO));
            if (!fail && s_dev.accesses == before)
                fail = "no access reached the device (the page did not trap)";
            report(name, fail, NULL);
        }
    }
    if (armed)
        mmio_disarm();

    RUN("exec_alloc", check_exec_alloc());
    RUN("exec_guest", up ? check_exec_guest() : NEEDS_LAYOUT);
    RUN("reserve", up ? check_reserve(note, sizeof note) : NEEDS_LAYOUT);
    RUN("save_dir", check_save_dir(save_dir));

    if (up) {
        BOOL again;
        int held = xbox_GuestWindowHeld();
        const char *fail = NULL;
        xbox_MemoryLayoutShutdown();
        again = xbox_MemoryLayoutInit(xbe, xbe_len);
        if (!again)
            fail = "a second xbox_MemoryLayoutInit after shutdown failed";
        else if (xbox_GuestWindowHeld() != held)
            fail = "the second init did not hold the guest window as the first did";
        if (xbox_GetMemoryBase())
            xbox_MemoryLayoutShutdown();
        RUN("reinit", fail);
    } else {
        RUN("reinit", NEEDS_LAYOUT);
    }

    close(s_probe[0]);
    close(s_probe[1]);
    s_probe[0] = s_probe[1] = -1;
    if (s_skip)
        printf("selftest: %d passed, %d failed, %d skipped\n", s_pass, s_fail, s_skip);
    else
        printf("selftest: %d passed, %d failed\n", s_pass, s_fail);
    fflush(stdout);
    return s_fail;
}
