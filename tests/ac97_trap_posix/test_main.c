/*
 * The AC'97 bus-master write trap on a POSIX host, reached the way a title
 * reaches it: a store to the bus-master page (0xFEC00000) faults, the
 * process's SIGBUS/SIGSEGV handler hands the fault to xbox_PosixMmioFault,
 * and the store completes without the RR ("reset registers") bit, which is
 * what the controller does once the reset is over.
 *
 * The two-thread case is the regression. The first POSIX trap opened the page
 * process-wide while it emulated one store, so a second thread's plain store
 * in that window kept its RR bit; DirectSound's channel reset reads the
 * control byte once and spins on it for ever. Fable resets channels from two
 * threads during movies and hung there. The trap now writes through a second
 * writable view and never opens the guest page.
 *
 *   ac97_trap_posix_test test.xbe [fallback|race]
 *
 * fallback: RECOMP_AC97_ALIAS=0, the mprotect path, one thread.
 * race:     the mprotect path with two threads -- expected to FAIL; kept to
 *           show that the two-thread check catches the bug.
 */
#include "xbox_memory_layout.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supplied by recompiled game code in a real build; nothing here calls one. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define AC97_NABM   0xFEC00000u
#define RR          0x02u
#define RPBM        0x01u
#define ITERATIONS  100000

static int failures;
static uintptr_t s_base;
static volatile sig_atomic_t s_unhandled;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-56s %s%s%s\n", what, ok ? "PASS" : "FAIL",
           detail && !ok ? " -- " : "", detail && !ok ? detail : "");
    if (!ok) failures++;
}

static void on_fault(int sig, siginfo_t *si, void *ucv)
{
    if (xbox_PosixMmioFault(ucv, (uintptr_t)si->si_addr))
        return;
    s_unhandled = 1;
    signal(sig, SIG_DFL);           /* re-faults and dies: a FAIL by crash */
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    long n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0) {
        rewind(f);
        buf = malloc((size_t)n);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
        *len = (size_t)n;
    }
    fclose(f);
    return buf;
}

struct worker {
    uint32_t off;                   /* channel control byte, NABM + off */
    int bad;                        /* read-backs with RR set */
    int lost_rpbm;                  /* read-backs that lost RPBM */
    uint8_t first_bad;
};

/* What DirectSound does per channel: set RR (with RPBM, which must stay),
 * then read the byte back. */
static void *hammer(void *arg)
{
    struct worker *w = arg;
    volatile uint8_t *ctl = (volatile uint8_t *)(s_base + AC97_NABM + w->off);
    int i;

    for (i = 0; i < ITERATIONS; i++) {
        uint8_t v;
        *ctl = RR | RPBM;
        v = *ctl;
        if (v & RR) {
            if (!w->bad) w->first_bad = v;
            w->bad++;
        }
        if (!(v & RPBM))
            w->lost_rpbm++;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    size_t xbe_len = 0;
    unsigned char *xbe;
    struct sigaction sa;
    const char *mode = argc > 2 ? argv[2] : "view";
    int threads = strcmp(mode, "fallback") == 0 ? 1 : 2;
    char detail[128];

    setvbuf(stdout, NULL, _IONBF, 0);
    xbe = argc > 1 ? load_file(argv[1], &xbe_len) : NULL;
    if (!xbe) {
        fprintf(stderr, "usage: %s test.xbe [fallback|race]\n", argv[0]);
        return 2;
    }
    if (strcmp(mode, "view") != 0)
        setenv("RECOMP_AC97_ALIAS", "0", 1);
    /* The trap is armed with the rest of the codec-ready model. */
    setenv("RECOMP_AC97_READY", "1", 1);
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 2;
    }
    s_base = (uintptr_t)xbox_GetMemoryOffset();

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

#if !defined(__aarch64__) && !(defined(__x86_64__) && defined(__linux__))
    printf("no store decoder on this host: the trap stays off (skipped)\n");
    return 77;
#else
    {
        volatile uint32_t *status =
            (volatile uint32_t *)(s_base + AC97_NABM + 0x130);
        volatile uint8_t *civ = (volatile uint8_t *)(s_base + AC97_NABM + 0x104);
        struct worker w[2] = { { 0x10B, 0, 0, 0 }, { 0x17B, 0, 0, 0 } };
        pthread_t t[2];
        int i;

        snprintf(detail, sizeof detail, "status 0x%08X", *status);
        check((*status & 0x100u) != 0,
              "codec-ready bit survives arming the trap", detail);

        *civ = 0x5A;                                /* not a control byte */
        snprintf(detail, sizeof detail, "read 0x%02X", *civ);
        check(*civ == 0x5A, "a store elsewhere on the page is kept as is",
              detail);
        *(volatile uint32_t *)(s_base + AC97_NABM + 0x118) = 0x03000000u;
        snprintf(detail, sizeof detail, "byte 0x11B = 0x%02X",
                 *(volatile uint8_t *)(s_base + AC97_NABM + 0x11B));
        check(*(volatile uint8_t *)(s_base + AC97_NABM + 0x11B) == RPBM,
              "a wide store covering a control byte drops only RR", detail);

        for (i = 0; i < threads; i++)
            pthread_create(&t[i], NULL, hammer, &w[i]);
        for (i = 0; i < threads; i++)
            pthread_join(t[i], NULL);
        for (i = 0; i < threads; i++) {
            char what[80];
            snprintf(what, sizeof what,
                     "%s: %d RR stores to 0x%03X, every read-back clear",
                     threads > 1 ? "two threads" : "one thread",
                     ITERATIONS, w[i].off);
            snprintf(detail, sizeof detail,
                     "%d read-backs had RR (first 0x%02X)", w[i].bad,
                     w[i].first_bad);
            check(w[i].bad == 0, what, detail);
            snprintf(detail, sizeof detail, "%d lost RPBM", w[i].lost_rpbm);
            check(w[i].lost_rpbm == 0, "  ... and RPBM kept", detail);
        }
    }
    check(!s_unhandled, "no access fell through to the crash path", NULL);
    xbox_MemoryLayoutShutdown();
    printf("\n%d failure(s) (%s)\n", failures, mode);
    return failures ? 1 : 0;
#endif
}
