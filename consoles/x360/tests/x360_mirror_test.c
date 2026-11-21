/* x360_mirror: the lifted code's GPR loads and stores (RC_LWZ .. RC_ST64 as packs build them: XE_RUNTIME, the base
 * and skew in RC_PROLOGUE's locals, xenon_rt.h) keep their order through the physical views. The runtime maps the
 * same physical page at 0xA0000000 + p and 0xC0000000 + p (x360_mem.c); a compiler that could tell two constant
 * guest addresses apart would see two different host addresses and could forward a load over a store through the
 * other view, or drop a store it thinks nothing reads. Here one file page is mapped at both places of a 4 GB
 * reservation and functions in lifted vocabulary write through one view and read through the other. Built at -O2
 * (and run by run_hello_arm64.sh's NDK / cross builds too, with the packs' -O1): the same results at any level.
 * Linux and macOS (mmap of a file); 77 where the reservation cannot be made. */
#define XE_RUNTIME 1
#include "xenon_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_OFF 0x00010000u /* 64 KB: a page boundary on every host */
#define VIEW_A (0xA0000000u + PAGE_OFF)
#define VIEW_C (0xC0000000u + PAGE_OFF)

static int fails;

static void check(const char *what, uint64_t got, uint64_t want)
{
    if (got != want) {
        printf("FAIL %s: %016llx, expected %016llx\n", what, (unsigned long long)got, (unsigned long long)want);
        fails++;
    }
}

/* a vector's first word as the guest stores it (big-endian) */
static uint32_t xv_to_bytes_word(xe_v128 v)
{
    uint8_t b[16];
    xv_to_bytes(v, b);
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/* read, write the other view, read again: the second read sees the write */
__attribute__((noinline)) static void t_raw(xe_machine *m_)
{
    RC_PROLOGUE;
    uint32_t a = RC_LWZ(VIEW_A);
    RC_STW(VIEW_C, 0x22222222u);
    uint32_t b = RC_LWZ(VIEW_A);
    R(3) = a;
    R(4) = b;
}

/* write, read the other view, write again: the read sees the first write (it is not dead) */
__attribute__((noinline)) static void t_waw(xe_machine *m_)
{
    RC_PROLOGUE;
    RC_STW(VIEW_A + 4, 0x11111111u);
    uint32_t a = RC_LWZ(VIEW_C + 4);
    RC_STW(VIEW_A + 4, 0x33333333u);
    R(3) = a;
}

/* read, then write the other view: the read keeps the old value */
__attribute__((noinline)) static void t_war(xe_machine *m_)
{
    RC_PROLOGUE;
    uint64_t a = RC_LD64(VIEW_A + 8);
    RC_ST64(VIEW_C + 8, 0x4444444455555555ull);
    R(3) = a;
}

/* every width, each read through the other view of the write, the address from a register */
__attribute__((noinline)) static void t_widths(xe_machine *m_)
{
    RC_PROLOGUE;
    uint32_t a = (uint32_t)R(5), x = (uint32_t)R(6);
    RC_STB(a + 16, 0x9A);
    R(7) = RC_LBZ(x + 16);
    RC_STH(a + 18, 0xBCDE);
    R(8) = RC_LHZ(x + 18);
    RC_STW(a + 20, 0x01234567u);
    R(9) = RC_LWZ(x + 20);
    RC_ST64(a + 24, 0x89ABCDEF01234567ull);
    R(10) = RC_LD64(x + 24);
    R(11) = RC_LWZ(x + 24); /* the high word of the doubleword just written, big-endian */
}

/* the vector accesses (xv_load / xv_store through xe_ptr) against each other and against a GPR store */
__attribute__((noinline)) static void t_vmx(xe_machine *m_)
{
    RC_PROLOGUE;
    V(1) = xv_load(m_, VIEW_A + 32);
    xv_store(m_, VIEW_C + 32, V(2));
    V(3) = xv_load(m_, VIEW_A + 32);
    RC_STW(VIEW_C + 48, 0x5A5A5A5Au);
    V(4) = xv_load(m_, VIEW_A + 48);
}

int main(void)
{
    static xe_machine m;
    const size_t span = (size_t)1 << 32;
    uint8_t *mem = mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (mem == MAP_FAILED) {
        printf("skip: no 4 GB reservation\n");
        return 77;
    }
    char path[] = "/tmp/x360_mirror_XXXXXX";
    int fd = mkstemp(path);
    long page = sysconf(_SC_PAGESIZE);
    if (fd >= 0)
        unlink(path);
    if (fd < 0 || page <= 0 || page > 0x10000 || ftruncate(fd, page) != 0) {
        printf("skip: no file page\n");
        return 77;
    }
    if (mmap(mem + VIEW_A, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED ||
        mmap(mem + VIEW_C, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        printf("skip: cannot map the views\n");
        return 77;
    }
    m.mem = mem;
    m.cur = &m.cpu;
    m.view4k_skew = 0;
    xe_cpu *c = &m.cpu;

    xe_gstw(mem, 0, VIEW_A, 0x12345678u);
    t_raw(&m);
    check("raw first read", R(3), 0x12345678u);
    check("raw read after the other view's write", R(4), 0x22222222u);

    t_waw(&m);
    check("waw read between the writes", R(3), 0x11111111u);
    check("waw final", xe_glwz(mem, 0, VIEW_C + 4), 0x33333333u);

    xe_gstd(mem, 0, VIEW_A + 8, 0x0102030405060708ull);
    t_war(&m);
    check("war read before the other view's write", R(3), 0x0102030405060708ull);
    check("war final", xe_gld(mem, 0, VIEW_A + 8), 0x4444444455555555ull);

    R(5) = VIEW_A;
    R(6) = VIEW_C;
    t_widths(&m);
    check("stb / lbz", R(7), 0x9A);
    check("sth / lhz", R(8), 0xBCDE);
    check("stw / lwz", R(9), 0x01234567u);
    check("std / ld", R(10), 0x89ABCDEF01234567ull);
    check("std / lwz high word", R(11), 0x89ABCDEFu);
    check("big-endian bytes", mem[VIEW_A + 24], 0x89);

    for (unsigned i = 0; i < 16; i++) {
        mem[VIEW_A + 32 + i] = (uint8_t)i;
        mem[VIEW_A + 48 + i] = 0;
    }
    c->vr[2] = xv_splat32(0xC3C3C3C3u);
    t_vmx(&m);
    check("vmx first load", xv_to_bytes_word(c->vr[1]), 0x00010203u);
    check("vmx load after the other view's store", xv_to_bytes_word(c->vr[3]), 0xC3C3C3C3u);
    check("vmx load after a GPR store", xv_to_bytes_word(c->vr[4]), 0x5A5A5A5Au);

    if (fails)
        return 1;
    printf("x360_mirror: ok\n");
    return 0;
}
