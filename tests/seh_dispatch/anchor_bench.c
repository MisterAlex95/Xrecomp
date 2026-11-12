/* Per-call cost of an SEH resume anchor (XBA-112).
 *
 * A function with an __except or a catch takes recomp_seh_anchor + _setjmp
 * every time it is called, right after registering its frame. This times a
 * lifted-shaped function that registers an EH3 frame, calls a leaf and
 * unregisters, with and without the anchor, and prints the difference.
 * Built beside the test (seh_anchor_bench), not run by ctest:
 *
 *   ./seh_anchor_bench [iterations]
 *
 * Standalone on purpose: xbox_seh.c and a few stubs, no runtime, so the
 * number is the anchor and nothing else. Reference (2026-11-12): 4.1 ns
 * plain, ~6 ns more anchored on a Ryzen AI MAX+ 395 (x86-64, glibc). Under
 * qemu-user the ratio means nothing; measure arm64 on a device. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "xbox_seh.h"
#include "xbox_memory_layout.h"
#if defined(_WIN32)
#define RECOMP_SETJMP(jb) setjmp(jb)
#else
#define RECOMP_SETJMP(jb) _setjmp(jb)
#endif
RECOMP_TLS RecompCpu g_cpu;
ptrdiff_t g_xbox_mem_offset;
typedef void (*fn_t)(void);
fn_t recomp_lookup(uint32_t v){(void)v;return 0;}
fn_t recomp_lookup_manual(uint32_t v){(void)v;return 0;}
fn_t recomp_lookup_kernel(uint32_t v){(void)v;return 0;}
int xbox_GuestCpuEnterCallback(void){return 0;}
void xbox_GuestCpuLeaveCallback(int t){(void)t;}
void xbox_GuestCpuAcquire(void){}
#define M32(a) (*(volatile uint32_t*)((uintptr_t)(a)+g_xbox_mem_offset))
static const uint32_t res[] = {0x10100};
static const recomp_seh_desc desc = {0x10000, 1, res};
#if defined(_MSC_VER)
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif
BENCH_NOINLINE static void leaf(void){ g_cpu.r_esp += 4; }
#define BODY(ANCHOR) \
    uint32_t ebp; g_cpu.r_esp -= 4; M32(g_cpu.r_esp) = 0; ebp = g_cpu.r_esp; \
    g_cpu.r_esp -= 16; M32(g_cpu.r_esp+12) = 0xFFFFFFFF; M32(g_cpu.r_esp+8)=0x20000; \
    M32(g_cpu.r_esp+4)=0x12000; M32(g_cpu.r_esp)=M32(g_cpu.r_fs_base); M32(g_cpu.r_fs_base)=g_cpu.r_esp; \
    ANCHOR \
    M32(ebp-4)=0; g_cpu.r_esp-=4; M32(g_cpu.r_esp)=0x10050; leaf(); M32(ebp-4)=0xFFFFFFFF; \
out: M32(g_cpu.r_fs_base)=M32(ebp-16); g_cpu.r_esp = ebp + 4; g_cpu.r_esp += 4;
BENCH_NOINLINE static void plain(void){ BODY() }
BENCH_NOINLINE static void anchored(void){ BODY(
    switch (RECOMP_SETJMP(*recomp_seh_anchor(M32(g_cpu.r_fs_base), ebp, &desc))) { case 0: break; case 1: ebp = g_cpu.r_seh_ebp; goto out; default: break; }) }
static double now(void){struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc, char**argv){
    long n = argc>1 ? atol(argv[1]) : 20000000; int r;
    uint8_t *mem = calloc(1, 0x200000); g_xbox_mem_offset=(ptrdiff_t)mem;
    g_cpu.r_fs_base=0x4000; M32(0x4000)=0xFFFFFFFF;
    xbox_SehSetGuestStack(0x100000,0x180000);
    for (r=0;r<3;r++){
        double t0,t1,t2; long i;
        g_cpu.r_esp=0x17F000; t0=now(); for(i=0;i<n;i++){ g_cpu.r_esp-=4; plain(); }
        t1=now(); for(i=0;i<n;i++){ g_cpu.r_esp-=4; anchored(); } t2=now();
        printf("plain %.2f ns/call, anchored %.2f ns/call, anchor costs %.2f ns\n",
            (t1-t0)*1e9/n,(t2-t1)*1e9/n,((t2-t1)-(t1-t0))*1e9/n);
    }
    return 0;
}
