/*
 * RECOMP_POLL_WAIT's runtime side (xbox_GuestPollWait, xbox_memory_layout.c).
 *
 * The lifter puts RECOMP_POLL_WAIT on the back edge of a loop that does
 * nothing but `test dword [mem], imm ; jne/je itself`. The runtime answers
 * one such word: the PFB flush kick (NV2A 0x100410, bit 16, waiting while
 * set), through xbox_Nv2aKickWait, which returns once the ack thread has
 * cleared the bit. Every other word -- a plain guest word, another NV2A
 * register, another mask, a loop that waits while the bit is clear -- returns
 * at once and leaves the word alone, so the loop polls as it always did.
 *
 *   guest_poll_wait_test on  <xbe>   default: the kick word waits
 *   guest_poll_wait_test off <xbe>   RECOMP_POLL_WAIT=0: nothing waits
 */
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define PFB_FLUSH   0x00100410u
#define FLUSH_BIT   0x00010000u

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "on";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int on = strcmp(mode, "off") != 0;
    volatile uint32_t plain = FLUSH_BIT | 0x5u;
    volatile uint32_t *kick, *other;
    size_t xbe_len = 0;
    unsigned char *xbe;
    int i, waited;

    setvbuf(stdout, NULL, _IONBF, 0);
    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }
    kick = xbox_Nv2aRegPtr(PFB_FLUSH);
    other = xbox_Nv2aRegPtr(PFB_FLUSH + 4);
    if (!kick || !other) {
        puts("NV2A register file not mapped");
        return 2;
    }

    printf("guest poll wait, %s\n", on ? "default" : "RECOMP_POLL_WAIT=0");

    check(xbox_GuestPollWait(&plain, FLUSH_BIT, 1, 0x00010000u) == 0,
          "a plain guest word returns at once");
    check(plain == (FLUSH_BIT | 0x5u), "and is left alone");
    check(xbox_GuestPollWait(&plain, FLUSH_BIT, 0, 0x00010000u) == 0,
          "a plain word waited on while clear returns at once");

    *other = FLUSH_BIT;
    check(xbox_GuestPollWait(other, FLUSH_BIT, 1, 0x00010000u) == 0,
          "another NV2A register returns at once");
    check(*other == FLUSH_BIT, "and is left alone");
    *other = 0;

    check(xbox_GuestPollWait(kick, 0x00000001u, 1, 0x00010000u) == 0,
          "the kick register with another mask returns at once");
    check(xbox_GuestPollWait(kick, FLUSH_BIT, 0, 0x00010000u) == 0,
          "the kick bit waited on while clear returns at once");

    /* D3D's KickOff: set the bit, then poll until the ack thread clears it.
     * The loop as lifted: test, hook on the back edge, test again. */
    for (i = 0, waited = 0; i < 100; i++) {
        int spins = 0;
        *kick |= FLUSH_BIT;
        while (*kick & FLUSH_BIT) {
            waited += xbox_GuestPollWait(kick, FLUSH_BIT, 1, 0x00010000u);
            if (++spins > 100000000) break;
        }
    }
    check(!(*kick & FLUSH_BIT), "100 kicks are all acked");
    if (on)
        check(waited > 0, "the kick waits for the ack thread");
    else
        check(waited == 0, "the kick never waits with RECOMP_POLL_WAIT=0");
    printf("  (%d of the loop's hooks waited)\n", waited);

    xbox_MemoryLayoutShutdown();
    free(xbe);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
