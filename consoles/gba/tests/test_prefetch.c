/* rc_pf_fetch_i (gba_recomp_rt.h, XBA-277), the cartridge prefetch buffer's opcode fetch for every instruction of
 * lifted code, against rc_pf_fetch, the function the interpreter uses: from random buffer states (every flag,
 * count, capacity, duty, cycles left, and `now` before, at and after the state's time) and fetch addresses (at the
 * head, beside it, in the other state), both must return the same cycles and leave the same state. Our own code. */
#include "gba_internal.h"

#include <stdio.h>
#include <string.h>

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 32) % n;
}

static int same(const gba_rc_prefetch *a, const gba_rc_prefetch *b)
{
    return a->time == b->time && a->head == b->head && a->left == b->left && a->count == b->count &&
           a->cap == b->cap && a->duty == b->duty && a->valid == b->valid && a->run == b->run &&
           a->thumb == b->thumb;
}

int main(void)
{
    static gba_rc_head x, y;
    long fast = 0, n = 4000000, bad = 0;
    for (int r = 0; r < 16; r++)
        for (int s = 0; s < 2; s++) {
            x.wait16[r][s] = (uint8_t)(1 + (r + s) % 5);
            x.wait32[r][s] = (uint8_t)(2 * x.wait16[r][s]);
        }
    for (long i = 0; i < n && bad < 10; i++) {
        gba_rc_prefetch p;
        memset(&p, 0, sizeof p);
        p.thumb = rnd(2);
        p.cap = p.thumb ? 8 : 4;
        p.duty = (uint8_t)(1 + rnd(8));
        p.time = 1000 + rnd(64);
        p.head = 0x08000100u + 2 * rnd(64);
        p.left = 1 + rnd(p.duty);
        p.count = (uint8_t)rnd(p.cap + 1u);
        p.valid = rnd(8) != 0;
        p.run = rnd(4) != 0;
        bool thumb = rnd(8) ? p.thumb : !p.thumb;
        uint32_t w = thumb ? 2 : 4;
        uint32_t addr = rnd(4) ? p.head : p.head + w * (rnd(5) - 2u);
        uint64_t now = p.time - 8 + rnd(40);
        uint32_t ncost = 1 + rnd(9);
        x.pf = p;
        y = x;
        uint32_t a = rc_pf_fetch_i(&x, now, addr, thumb, ncost);
        uint32_t b = rc_pf_fetch(&y, now, addr, thumb, ncost);
        fast += p.valid && addr == p.head && p.thumb == thumb;
        if (a != b || !same(&x.pf, &y.pf)) {
            bad++;
            printf("FAIL case %ld: cycles %u vs %u; state valid %d run %d count %u left %u time %llu now %llu\n", i,
                   a, b, p.valid, p.run, p.count, p.left, (unsigned long long)p.time, (unsigned long long)now);
        }
    }
    if (bad)
        return 1;
    printf("prefetch: %ld fetches (%ld at the head), rc_pf_fetch_i == rc_pf_fetch\n", n, fast);
    return 0;
}
