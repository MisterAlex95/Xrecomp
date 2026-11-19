"""The x86 interpreter (src/kernel/xbox_interp_core.c) against lifted code
(XBA-303): random instruction sequences, each lifted by the real translator
and run both ways from the same state.

A test helper, after lift_fixture: each sequence becomes one lifted function
(at its own base, ending in ret) compiled at -O2 against the real
recomp_types.h, with the interpreter core linked in. main() runs every
function from a random state (registers, a data area, a seed per vector),
then the interpreter from the same state over the same bytes (copied into
guest memory), and prints every register or byte that differs.

The lifter's flags are lazy: a flag is only ever observed through its
consumer (setcc, cmovcc, adc, sbb, jcc). So the sequences compare flags
through consumers, and only for flags the last setter defines (the CPU
leaves AF after a shift, SF after a mul, ... undefined, and the lifter
does not model them).
"""
import os
import random
import subprocess
import tempfile

from . import config
from .lift_fixture import gnu_cc, RUNTIME
from .translator import FunctionTranslator

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CORE = os.path.join(ROOT, "src", "kernel", "xbox_interp_core.c")
BASE = 0x10000
STRIDE = 0x400
DATA = 0x8000           # data area the sequences address absolutely, 256 bytes
RAM = 0x80000

R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
POOL = [0, 1, 2, 3, 6, 7]       # registers a sequence may write (not esp / ebp)

ALL = frozenset("CPAZSO")


def modrm(mod, reg, rm):
    return bytes([(mod << 6) | (reg << 3) | rm])


def mem_abs(reg, off, data=None):
    """modrm for [data + off] (disp32, no base)."""
    return modrm(0, reg, 5) + ((DATA if data is None else data) + off).to_bytes(4, "little")


class Gen:
    """Random straight-line sequences with flag consumers only of defined flags."""

    def __init__(self, seed, data=None):
        self.r = random.Random(seed)
        self.defined = frozenset()
        self.data = DATA if data is None else data

    def _mem(self, reg, off):
        return mem_abs(reg, off, self.data)

    def reg(self):
        return self.r.choice(POOL)

    def reg8(self):
        # al cl dl bl ah ch dh bh: all but esp / ebp aliases are fine to write
        return self.r.choice([0, 1, 2, 3, 4, 5, 6, 7])

    def off(self, width):
        return self.r.randrange(0, 256 - width, width)

    def imm(self, width):
        return self.r.choice([0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000,
                              0xFFFFFFFF, self.r.getrandbits(32)]) & ((1 << (8 * width)) - 1)

    def consumer(self):
        """A setcc / cmovcc / adc / sbb whose flags are defined, or b''."""
        need = {0x0: "O", 0x2: "C", 0x4: "Z", 0x6: "CZ", 0x8: "S", 0xA: "P", 0xC: "SO", 0xE: "ZSO"}
        ok = [cc for cc, f in need.items() if set(f) <= self.defined]
        if not ok:
            return b""
        cc = self.r.choice(ok) | self.r.randrange(2)
        k = self.r.randrange(4)
        if k == 0:
            return bytes([0x0F, 0x90 | cc]) + self._mem(0, self.off(1))            # setcc [m]
        if k == 1:
            return bytes([0x0F, 0x90 | cc]) + modrm(3, 0, self.reg8())           # setcc r8
        if k == 2:
            return bytes([0x0F, 0x40 | cc]) + modrm(3, self.reg(), self.reg())   # cmovcc
        if "C" in self.defined:
            op = self.r.choice([0x11, 0x19, 0x13, 0x1B])                         # adc / sbb
            d = bytes([op]) + modrm(3, self.reg(), self.reg())
            self.defined = ALL
            return d
        return b""

    def alu(self):
        op = self.r.randrange(8)
        w = self.r.choice([1, 2, 4, 4, 4])
        pre = b"\x66" if w == 2 else b""
        k = self.r.randrange(5)
        base = op * 8 + (0 if w == 1 else 1)
        if (op in (2, 3)) and "C" not in self.defined:
            op, base = 0, (0 if w == 1 else 1)
        if k == 0:
            code = pre + bytes([base]) + modrm(3, self.reg8() if w == 1 else self.reg(), self.reg8() if w == 1 else self.reg())
        elif k == 1:
            code = pre + bytes([base + 2]) + self._mem(self.reg8() if w == 1 else self.reg(), self.off(w))
        elif k == 2:
            code = pre + bytes([base]) + self._mem(self.reg8() if w == 1 else self.reg(), self.off(w))
        elif k == 3:
            if w == 1:
                code = bytes([0x80]) + modrm(3, op, self.reg8()) + bytes([self.imm(1)])
            elif self.r.randrange(2):
                code = pre + bytes([0x83]) + modrm(3, op, self.reg()) + bytes([self.imm(1)])
            else:
                code = pre + bytes([0x81]) + modrm(3, op, self.reg()) + self.imm(w).to_bytes(w, "little")
        else:
            if w == 1:
                code = bytes([0x80]) + self._mem(op, self.off(1)) + bytes([self.imm(1)])
            else:
                code = pre + bytes([0x81]) + self._mem(op, self.off(w)) + self.imm(w).to_bytes(w, "little")
        self.defined = ALL - {"A"} if op in (1, 4, 6) else ALL
        return code

    def step(self):
        r = self.r
        k = r.randrange(16)
        if k <= 4:
            return self.alu()
        if k == 5:                                              # test
            w = r.choice([1, 4])
            self.defined = ALL - {"A"}
            return bytes([0x84 if w == 1 else 0x85]) + modrm(3, self.reg(), self.reg())
        if k == 6:                                              # inc / dec / neg / not
            w = r.choice([1, 4, 4])
            sub = r.choice([0, 1]) if w == 4 else r.choice([0, 1])
            g = r.randrange(3)
            if g == 0:
                self.defined = (self.defined & {"C"}) | (ALL - {"C"})
                return bytes([0xFE if w == 1 else 0xFF]) + modrm(3, sub, self.reg8() if w == 1 else self.reg())
            if g == 1:
                self.defined = ALL
                return bytes([0xF6 if w == 1 else 0xF7]) + modrm(3, 3, self.reg8() if w == 1 else self.reg())
            return bytes([0xF7]) + modrm(3, 2, self.reg())
        if k == 7:                                              # shifts and rotates by imm
            w = r.choice([1, 2, 4, 4])
            pre = b"\x66" if w == 2 else b""
            kind = r.choice([0, 1, 4, 5, 7])
            cnt = r.randrange(1, 8 * w if kind in (4, 5, 7) else 32)
            if kind in (4, 5, 7):
                self.defined = frozenset("CZSP") | ({"O"} if cnt == 1 else set())
                if w < 4 and cnt >= 8 * w:
                    self.defined = frozenset("ZSP")
            else:
                self.defined = (self.defined - {"C", "O"}) | ({"C"} if cnt % (8 * w) else set())
                if cnt % (8 * w) == 0 and kind in (0, 1):
                    self.defined = self.defined - {"C"}
                self.defined = frozenset(self.defined)
            return pre + bytes([0xC0 if w == 1 else 0xC1]) + modrm(3, kind, self.reg8() if w == 1 else self.reg()) + bytes([cnt])
        if k == 8:                                              # imul
            self.defined = frozenset("CO")
            if r.randrange(2):
                return bytes([0x0F, 0xAF]) + modrm(3, self.reg(), self.reg())
            return bytes([0x69]) + modrm(3, self.reg(), self.reg()) + self.imm(4).to_bytes(4, "little")
        if k == 9:                                              # movzx / movsx / lea / xchg / bswap
            g = r.randrange(5)
            if g == 0:
                return bytes([0x0F, r.choice([0xB6, 0xBE])]) + modrm(3, self.reg(), self.reg8())
            if g == 1:
                return bytes([0x0F, r.choice([0xB7, 0xBF])]) + self._mem(self.reg(), self.off(2))
            if g == 2:
                return bytes([0x8D]) + modrm(1, self.reg(), self.reg()) + bytes([r.randrange(256)])
            if g == 3:
                return bytes([0x87]) + modrm(3, self.reg(), self.reg())
            return bytes([0x0F, 0xC8 | self.reg()])
        if k == 10:                                             # shld / shrd imm
            cnt = r.randrange(1, 32)
            self.defined = frozenset("CZSP") | ({"O"} if cnt == 1 else set())
            return bytes([0x0F, r.choice([0xA4, 0xAC])]) + modrm(3, self.reg(), self.reg()) + bytes([cnt])
        if k == 11:                                             # bt family, imm
            self.defined = frozenset("C")
            return bytes([0x0F, 0xBA]) + modrm(3, r.randrange(4, 8), self.reg()) + bytes([r.randrange(32)])
        if k == 12:                                             # mov forms
            g = r.randrange(4)
            if g == 0:
                return bytes([0x89]) + self._mem(self.reg(), self.off(4))
            if g == 1:
                return bytes([0x8B]) + self._mem(self.reg(), self.off(4))
            if g == 2:
                return bytes([0xB8 | self.reg()]) + self.imm(4).to_bytes(4, "little")
            return bytes([0x88]) + modrm(3, self.reg8(), self.reg8())
        if k == 13:                                             # cdq / cwde / push-pop pair
            g = r.randrange(3)
            if g == 0:
                return b"\x99"
            if g == 1:
                return b"\x98"
            return bytes([0x50 | self.reg(), 0x58 | self.reg()])
        if k == 14:                                             # mul edx:eax
            self.defined = frozenset("CO")
            return bytes([0xF7]) + modrm(3, 4, r.choice([1, 3, 6, 7]))
        if k == 15:
            return self.extra()
        return self.consumer()

    def extra(self):
        """Rarer forms: xadd, cmpxchg, bsf / bsr, rcl / rcr, shifts by cl, div,
        string instructions, based memory operands."""
        r = self.r
        g = r.randrange(9)
        if g == 0:
            self.defined = ALL
            if r.randrange(2):
                return bytes([0x0F, 0xC1]) + modrm(3, self.reg(), self.reg())
            return bytes([0x0F, 0xC1]) + self._mem(self.reg(), self.off(4))
        if g == 1:
            self.defined = ALL
            if r.randrange(2):
                return bytes([0x0F, 0xB1]) + modrm(3, self.reg(), self.reg())
            return bytes([0x0F, 0xB1]) + self._mem(self.reg(), self.off(4))
        if g == 2:                       # bsf / bsr of a nonzero source
            src = self.reg()
            self.defined = frozenset("Z")
            return bytes([0x81]) + modrm(3, 1, src) + (1).to_bytes(4, "little") + \
                bytes([0x0F, r.choice([0xBC, 0xBD])]) + modrm(3, self.reg(), src)
        if g == 3 and "C" in self.defined:   # rcl / rcr by 1 or imm
            kind = r.choice([2, 3])
            cnt = r.randrange(1, 32)
            self.defined = frozenset("CO") if cnt == 1 else frozenset("C")
            return bytes([0xC1]) + modrm(3, kind, self.reg()) + bytes([cnt])
        if g == 4:                       # shift by cl: what is defined depends on cl
            self.defined = frozenset()
            return bytes([0xD3]) + modrm(3, r.choice([0, 1, 4, 5, 7]), r.choice([0, 2, 3, 6, 7]))
        if g == 5:                       # div ecx, safely: edx = 0, ecx odd
            self.defined = frozenset()
            return b"\x31\xD2" + bytes([0x81]) + modrm(3, 1, 1) + (1).to_bytes(4, "little") + b"\xF7\xF1"
        if g == 6:                       # string instructions over the data area
            cnt = r.randrange(0, 12)
            back = r.randrange(2)
            esi = self.data + (100 if back else 4)
            edi = self.data + (200 if back else 120)
            op = r.choice([0xA4, 0xA5, 0xAA, 0xAB, 0xAC, 0xA6, 0xAE])
            rep = r.choice([b"", b"\xF3", b"\xF2"]) if op in (0xA6, 0xAE) else r.choice([b"", b"\xF3"])
            code = (b"\xFD" if back else b"\xFC") + bytes([0xBE]) + esi.to_bytes(4, "little") + \
                bytes([0xBF]) + edi.to_bytes(4, "little") + bytes([0xB9]) + cnt.to_bytes(4, "little") + rep + bytes([op]) + b"\xFC"
            if op in (0xA6, 0xAE):
                self.defined = ALL if (cnt or not rep) else frozenset()
            return code
        if g == 7:                       # [esi + disp8] after esi = data
            off = r.randrange(0, 120)
            return bytes([0xBE]) + self.data.to_bytes(4, "little") + bytes([0x8B]) + modrm(1, r.choice([0, 1, 2, 3, 7]), 6) + bytes([off]) + \
                bytes([0x01]) + modrm(1, r.choice([0, 1, 2, 3, 7]), 6) + bytes([off + 4])
        # push imm / pop
        return bytes([0x68]) + self.imm(4).to_bytes(4, "little") + bytes([0x58 | self.reg()])

    def jcc_over(self, body):
        """A conditional jump over `body` on a defined condition, or body as is."""
        need = {0x0: "O", 0x2: "C", 0x4: "Z", 0x6: "CZ", 0x8: "S", 0xA: "P", 0xC: "SO", 0xE: "ZSO"}
        ok = [cc for cc, f in need.items() if set(f) <= self.defined]
        if not ok or not body or len(body) > 127:
            return body
        cc = self.r.choice(ok) | self.r.randrange(2)
        return bytes([0x70 | cc, len(body)]) + body

    def sequence(self, n):
        out = [self.alu()]
        for _ in range(n):
            if self.r.randrange(6) == 0:
                before = self.defined
                body = self.step()
                after = self.defined
                self.defined = before
                code = self.jcc_over(body)
                # the join: flags are defined only where both paths define them
                self.defined = before & after if code is not body else after
                out.append(code)
            else:
                out.append(self.step())
            if self.r.randrange(3) == 0:
                out.append(self.consumer())
        out.append(self.consumer())
        out.append(b"\xC3")
        return b"".join(out)


class Layout:
    """Where things are in guest memory. host: guest memory is a host buffer
    (g_xbox_mem_offset); native: an i386 process maps the guest addresses
    themselves (offset 0), so the bytes can also run on the CPU."""

    def __init__(self, native=False):
        self.native = native
        # the same addresses on both, so golden vectors made on the CPU run
        # anywhere: an i386 process maps them, a host buffer holds them
        self.base, self.data, self.stack, self.ebp = 0x00400000, 0x00200000, 0x00300000, 0x002F0000
        self.stride = STRIDE


PRELUDE = r"""
#define RECOMP_GENERATED_CODE 1
#include "recomp_types.h"
#include "xbox_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS RecompCpu g_cpu = { .r_eflags = 0x202u };
uint32_t g_xbox_code_lo, g_xbox_code_hi;
void recomp_unimpl(const char *text, uint32_t va) { (void)text; (void)va; }
/* What the runtime would answer; the sequences reach none of them. */
volatile int g_recomp_cas64_lock;
#define STUB __attribute__((weak))
STUB void recomp_debug_service(uint32_t s, uint32_t a) { (void)s; (void)a; }
STUB void recomp_divide_error(uint32_t va, uint32_t d) { printf("divide error at %08X\n", va); (void)d; }
STUB void xbox_GuestInterruptFlag(int e) { (void)e; }
STUB void xbox_Halt(uint32_t va) { (void)va; }
STUB uint32_t xbox_PortIn(uint32_t p, int w) { (void)p; (void)w; return 0; }
STUB void xbox_PortOut(uint32_t p, uint32_t v, int w) { (void)p; (void)v; (void)w; }
STUB uint32_t xbox_ReadCr(int n) { (void)n; return 0; }
STUB void xbox_WriteCr(int n, uint32_t v) { (void)n; (void)v; }
STUB uint32_t xbox_ReadDr(int n) { (void)n; return 0; }
STUB void xbox_WriteDr(int n, uint32_t v) { (void)n; (void)v; }
STUB uint64_t xbox_ReadDtr(int w) { (void)w; return 0; }
STUB void xbox_WriteDtr(int w, uint64_t v) { (void)w; (void)v; }
STUB uint64_t xbox_ReadTimeStampCounter(void) { return 0; }
STUB void xbox_Cpuid(uint32_t l, uint32_t s, uint32_t o[4]) { (void)l; (void)s; o[0] = o[1] = o[2] = o[3] = 0; }
#define DATA_VA  @DATA@u
#define STACK_VA @STACK@u
#define EBP_VA   @EBP@u
#define CODE_VA  @BASE@u
#define STRIDE   @STRIDE@u
#define NATIVE   @NATIVE@
#if NATIVE
static void layout(void)
{
    if (mmap((void *)(uintptr_t)(DATA_VA & ~0xFFFFFu), 0x100000u, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED
        || mmap((void *)(uintptr_t)(STACK_VA - 0x10000u), 0x10000u, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED
        || mmap((void *)(uintptr_t)CODE_VA, 0x100000u, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
        printf("mmap failed\n");
        exit(2);
    }
    g_xbox_mem_offset = 0;
}
/* The bytes on the CPU: guest registers in, the code called (its ret comes back
 * here), guest registers out. */
typedef struct { uint32_t v[8]; } nregs;   /* eax ecx edx ebx esp ebp esi edi */
nregs g_nin, g_nout;
uint32_t g_ncode, g_nsave;
void run_native(void);
__asm__(".text\n.globl run_native\nrun_native:\n"
        "  pushal\n  pushfl\n  movl %esp, g_nsave\n"
        "  movl g_nin+16, %esp\n  pushl $1f\n"
        "  movl g_nin+4, %ecx\n  movl g_nin+8, %edx\n  movl g_nin+12, %ebx\n  movl g_nin+20, %ebp\n"
        "  movl g_nin+24, %esi\n  movl g_nin+28, %edi\n  movl g_nin+0, %eax\n  cld\n"
        "  jmp *g_ncode\n"
        "1:\n  movl %eax, g_nout+0\n  movl %ecx, g_nout+4\n  movl %edx, g_nout+8\n  movl %ebx, g_nout+12\n"
        "  movl %esp, g_nout+16\n  movl %esi, g_nout+24\n  movl %edi, g_nout+28\n"
        "  movl g_nsave, %esp\n  popfl\n  popal\n  cld\n  ret\n");
#else
static uint8_t g_ram[0x500000] __attribute__((aligned(16)));
static void layout(void) { g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)g_ram; }
#endif
#define GUEST(a) ((uint8_t *)(uintptr_t)(a) + g_xbox_mem_offset)
static uint64_t s_rng;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)s_rng; }
static uint32_t pick(void)
{
    static const uint32_t edge[] = { 0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu };
    uint32_t k = rnd() % 16;
    return k < 11 ? edge[k] : rnd();
}
static void state(uint64_t seed)
{
    int i;
    s_rng = seed * 0x9E3779B97F4A7C15ull + 1;
    {
        RecompFrame *fb = g_cpu.r_fbase, *fl = g_cpu.r_flimit;
        memset(&g_cpu, 0, sizeof g_cpu);
        g_cpu.r_fbase = fb; g_cpu.r_flimit = fl; g_cpu.r_ftop = fb;
    }
    g_cpu.r_eflags = 0x202u;
    g_eax = pick(); g_ecx = pick(); g_edx = pick(); g_ebx = pick(); g_esi = pick(); g_edi = pick();
    g_ebp = EBP_VA;
    for (i = 0; i < 256; i++) GUEST(DATA_VA)[i] = (uint8_t)rnd();
    g_esp = STACK_VA;
    g_esp -= 4; MEM32(g_esp) = 0xDEAD0000u;      /* the return address of the call */
}
typedef struct { uint32_t r[8]; uint8_t data[256]; } snap_t;
static void take(snap_t *s)
{
    s->r[0] = g_eax; s->r[1] = g_ecx; s->r[2] = g_edx; s->r[3] = g_ebx;
    s->r[4] = g_esp; s->r[5] = 0; s->r[6] = g_esi; s->r[7] = g_edi;
    memcpy(s->data, GUEST(DATA_VA), 256);
}
static uint32_t fnv(const uint8_t *p)
{
    uint32_t h = 2166136261u;
    int i;
    for (i = 0; i < 256; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static const char *k_names[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
static int compare(const char *what, int seq, uint64_t seed, const snap_t *a, const snap_t *b)
{
    int i, bad = 0;
    for (i = 0; i < 8; i++)
        if (a->r[i] != b->r[i]) { printf("%s seq %d seed %llu: %s %08X vs %08X\n", what, seq, (unsigned long long)seed, k_names[i], a->r[i], b->r[i]); bad++; }
    for (i = 0; i < 256; i++)
        if (a->data[i] != b->data[i]) { printf("%s seq %d seed %llu: data+%d %02X vs %02X\n", what, seq, (unsigned long long)seed, i, a->data[i], b->data[i]); bad++; break; }
    return bad;
}
"""

MAIN = r"""
int main(void)
{
    int i, bad = 0, ran = 0, lifter_bad = 0;
    uint64_t v;
    layout();
    for (i = 0; i < (int)(sizeof k_fn / sizeof k_fn[0]); i++) {
        uint32_t code = CODE_VA + (uint32_t)i * STRIDE;
        int lifter_ok = 1;
        for (v = 0; v < @VECTORS@ull; v++) {
            snap_t a, b, n;
            XboxInterpStop stop;
            int r;
            uint64_t seed = v * 1000003ull + (uint64_t)i;
            (void)n;
            state(seed);
            memcpy(GUEST(code), k_code[i], k_len[i]);
            k_fn[i]();
            take(&a);
            state(seed);
            memset(&stop, 0, sizeof stop);
            r = xbox_InterpRun(code, 100000, &stop);
            if (r != XBOX_INTERP_RETURNED) {
                printf("INTERP seq %d: stopped (%d) at 0x%08X, bytes %02X %02X %02X %02X\n", i, r, stop.va,
                       stop.bytes[0], stop.bytes[1], stop.bytes[2], stop.bytes[3]);
                bad++;
                break;
            }
            take(&b);
            ran++;
#if NATIVE
            state(seed);
            g_nin.v[0] = g_eax; g_nin.v[1] = g_ecx; g_nin.v[2] = g_edx; g_nin.v[3] = g_ebx;
            g_nin.v[4] = g_esp + 4; g_nin.v[5] = g_ebp; g_nin.v[6] = g_esi; g_nin.v[7] = g_edi;
            g_ncode = code;
            run_native();
            g_eax = g_nout.v[0]; g_ecx = g_nout.v[1]; g_edx = g_nout.v[2]; g_ebx = g_nout.v[3];
            g_esp = g_nout.v[4]; g_esi = g_nout.v[6]; g_edi = g_nout.v[7];
            take(&n);
            printf("GOLD %d %llu %08X %08X %08X %08X %08X %08X %08X %08X\n", i, (unsigned long long)seed,
                   n.r[0], n.r[1], n.r[2], n.r[3], n.r[4], n.r[6], n.r[7], fnv(n.data));
            if (compare("INTERP", i, seed, &n, &b)) { bad++; break; }
            if (lifter_ok && compare("LIFTER", i, seed, &n, &a)) { lifter_bad++; lifter_ok = 0; }
#else
            if (compare("INTERP", i, seed, &a, &b)) { bad++; break; }
#endif
        }
#if NATIVE
        printf("LIFT %d %d\n", i, lifter_ok);
#endif
    }
    @EXTRA@
    printf("ran %d, bad %d, lifter differs from the CPU in %d sequences\n", ran, bad, lifter_bad);
    return bad ? 1 : 0;
}
"""


def translate_at(code, name, base):
    config._install(
        [config.Section(".text", base, len(code), 0, len(code), True)],
        entry_point=base, kernel_thunk_addr=base, origin="interp-fixture")
    db = {base: {"start": f"0x{base:08X}", "end": base + len(code), "_addr": base, "size": len(code)}}
    c = FunctionTranslator(code, db).translate_function(base, db[base])
    return c.replace(f"sub_{base:08X}", name)


def source(seqs, vectors, lay, extra_main=""):
    """The C of the whole harness."""
    funcs, table, codes = [], [], []
    for i, code in enumerate(seqs):
        base = lay.base + i * lay.stride
        assert len(code) < lay.stride
        funcs.append(translate_at(code, f"lifted_{i}", base))
        table.append(f"lifted_{i}")
        codes.append("{" + ", ".join(f"0x{b:02X}" for b in code) + "}")
    pre = PRELUDE
    for k, v in (("@DATA@", f"0x{lay.data:X}"), ("@STACK@", f"0x{lay.stack:X}"), ("@EBP@", f"0x{lay.ebp:X}"),
                 ("@BASE@", f"0x{lay.base:X}"), ("@STRIDE@", f"0x{lay.stride:X}"),
                 ("@NATIVE@", "1" if lay.native else "0")):
        pre = pre.replace(k, v)
    out = pre + "\n".join(funcs) + "\n"
    out += f"static void (*const k_fn[{len(seqs)}])(void) = {{ {', '.join(table)} }};\n"
    out += "".join(f"static const uint8_t k_code_{i}[] = {c};\n" for i, c in enumerate(codes))
    out += f"static const uint8_t *const k_code[{len(seqs)}] = {{ {', '.join(f'k_code_{i}' for i in range(len(seqs)))} }};\n"
    out += f"static const unsigned k_len[{len(seqs)}] = {{ {', '.join(str(len(c)) for c in seqs)} }};\n"
    out += MAIN.replace("@VECTORS@", str(vectors)).replace("@EXTRA@", extra_main)
    return out


def build_and_run(seqs, vectors, extra_main="", opt="-O2", cflags=()):
    """Lift every sequence, build them with the interpreter on this host, run
    `vectors` states through each, interpreter against lifted code. Returns
    (returncode, output)."""
    cc = gnu_cc()
    main = source(seqs, vectors, Layout(False), extra_main)
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "t.c")
        exe = os.path.join(d, "t")
        with open(src, "w") as f:
            f.write(main)
        built = subprocess.run(
            [cc, "-std=gnu11", opt, "-w", *cflags, "-I", RUNTIME, "-I", os.path.join(ROOT, "src", "kernel"),
             src, CORE, "-o", exe, "-lm", "-no-pie", "-Wl,--unresolved-symbols=ignore-all"],
            capture_output=True, text=True)
        if built.returncode != 0:
            raise AssertionError(built.stderr[-4000:])
        run = subprocess.run([exe], capture_output=True, text=True, timeout=600, cwd=d)
        return run.returncode, run.stdout + run.stderr


I386_IMAGE = "xboxrecomp-gcc-i386"     # tools/conformance's linux/386 container


def build_native(seqs, vectors, workdir):
    """Write the three-way harness for a linux/386 process into workdir:
    the bytes on the CPU, the interpreter, the lifted code. Returns the shell
    command that builds and runs it there (sources mounted at /w, /rt, /k)."""
    main = source(seqs, vectors, Layout(True))
    os.makedirs(workdir, exist_ok=True)
    with open(os.path.join(workdir, "t.c"), "w") as f:
        f.write(main)
    return ("gcc -std=gnu11 -O2 -w -I /rt -I /k /w/t.c /k/xbox_interp_core.c -o /w/t -lm -no-pie "
            "-Wl,--unresolved-symbols=ignore-all && /w/t")


def run_native(seqs, vectors, workdir):
    """build_native, run in the linux/386 container. The interpreter must
    match the CPU (INTERP lines fail); the lifter's differences are counted
    and printed (LIFTER lines)."""
    script = build_native(seqs, vectors, workdir)
    r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/386",
                        "-v", f"{workdir}:/w", "-v", f"{os.path.abspath(RUNTIME)}:/rt:ro",
                        "-v", f"{os.path.join(ROOT, 'src', 'kernel')}:/k:ro",
                        "-v", f"{os.path.join(ROOT, 'templates')}:/templates:ro",
                        I386_IMAGE, "sh", "-c", script], capture_output=True, text=True, timeout=1200)
    return r.returncode, r.stdout + r.stderr


def lifter_falls_back(code, base):
    """1 when the lifted C reads the never-assigned fallback flag variable,
    or leaves an instruction unlifted: the lifter does not model that consumer
    after that setter (a gap of the lifter's lazy flags, not something to
    compare against)."""
    c = translate_at(code, "probe", base)
    if "RECOMP_UNIMPL" in c:
        return True
    if "_cf" in c and "int _cf" not in c:
        return True                 # the lifter reads a carry it never declared (does not build)
    return c.count("_flags") > c.count("int _flags = 0;") + c.count("(void)_flags")


def sequences(count, length=10, seed=1000, native=False, filtered=True):
    """count sequences (the lifter models them fully when filtered); returns
    (seqs, rejected)."""
    lay = Layout(native)
    out, rejected, k = [], 0, 0
    while len(out) < count:
        code = Gen(seed + k, lay.data).sequence(length)
        k += 1
        if filtered and lifter_falls_back(code, lay.base + len(out) * lay.stride):
            rejected += 1
            continue
        out.append(code)
    return out, rejected


GOLDEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "interp_golden.json")


def make_golden(n, length, seed, vectors, workdir, path=GOLDEN):
    """Run fresh sequences on the CPU (linux/386 container) and keep what it
    computed: the conformance vectors the interpreter is held to on any host
    (test_interp_diff.py), and per sequence whether the lifter agreed."""
    import json
    seqs, _ = sequences(n, length, seed, native=True, filtered=False)
    rc, out = run_native(seqs, vectors, workdir)
    gold = {i: [] for i in range(len(seqs))}
    lift = {}
    for line in out.splitlines():
        f = line.split()
        if f and f[0] == "GOLD":
            gold[int(f[1])].append([int(f[2])] + [int(x, 16) for x in f[3:]])
        elif f and f[0] == "LIFT":
            lift[int(f[1])] = f[2] == "1"
    if rc != 0 or any(len(v) != vectors for v in gold.values()):
        raise AssertionError(out[-3000:])
    doc = {"_why": "XBA-303: x86 sequences run on a CPU (python3 -m tools.recomp.interp_fixture --golden); "
                   "per vector: seed, eax ecx edx ebx esp esi edi, FNV-1a of the 256-byte data area",
           "sequences": [{"code": c.hex(), "lifter_matches_cpu": lift.get(i, False), "vectors": gold[i]}
                         for i, c in enumerate(seqs)]}
    write_golden(doc, path)
    return doc


def write_golden(doc, path=GOLDEN):
    """One sequence per line."""
    import json
    lines = ['{"_why": %s,' % json.dumps(doc["_why"]), '"sequences": [',
             ",\n".join(json.dumps(s, separators=(",", ":")) for s in doc["sequences"]), "]}"]
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


GOLD_MAIN = r"""
typedef struct { uint64_t seed; uint32_t r[8]; } gold_t;
@TABLE@
int main(void)
{
    int i, k, bad = 0, ran = 0;
    layout();
    for (i = 0; i < (int)(sizeof k_code / sizeof k_code[0]); i++) {
        uint32_t code = CODE_VA + (uint32_t)i * STRIDE;
        for (k = 0; k < @VECTORS@; k++) {
            const gold_t *g = &k_gold[i][k];
            XboxInterpStop stop;
            snap_t b;
            uint32_t got[8];
            int r, j;
            state(g->seed);
            memcpy(GUEST(code), k_code[i], k_len[i]);
            r = xbox_InterpRun(code, 100000, &stop);
            if (r != XBOX_INTERP_RETURNED) {
                printf("seq %d: stopped (%d) at 0x%08X\n", i, r, stop.va);
                bad++;
                break;
            }
            take(&b);
            got[0] = b.r[0]; got[1] = b.r[1]; got[2] = b.r[2]; got[3] = b.r[3];
            got[4] = b.r[4]; got[5] = b.r[6]; got[6] = b.r[7]; got[7] = fnv(b.data);
            ran++;
            for (j = 0; j < 8; j++)
                if (got[j] != g->r[j]) {
                    static const char *nm[8] = { "eax", "ecx", "edx", "ebx", "esp", "esi", "edi", "data" };
                    printf("seq %d seed %llu: %s cpu %08X interp %08X\n", i, (unsigned long long)g->seed, nm[j], g->r[j], got[j]);
                    bad++;
                }
            if (bad) break;
        }
    }
    printf("ran %d, bad %d\n", ran, bad);
    return bad ? 1 : 0;
}
"""


def golden_source(doc):
    """A host harness: the interpreter alone against the CPU's vectors."""
    lay = Layout(False)
    seqs = [bytes.fromhex(s["code"]) for s in doc["sequences"]]
    nv = len(doc["sequences"][0]["vectors"])
    pre = PRELUDE
    for k, v in (("@DATA@", f"0x{lay.data:X}"), ("@STACK@", f"0x{lay.stack:X}"), ("@EBP@", f"0x{lay.ebp:X}"),
                 ("@BASE@", f"0x{lay.base:X}"), ("@STRIDE@", f"0x{lay.stride:X}"), ("@NATIVE@", "0")):
        pre = pre.replace(k, v)
    tab = "".join(f"static const uint8_t k_code_{i}[] = {{{', '.join(f'0x{b:02X}' for b in c)}}};\n"
                  for i, c in enumerate(seqs))
    tab += f"static const uint8_t *const k_code[{len(seqs)}] = {{ {', '.join(f'k_code_{i}' for i in range(len(seqs)))} }};\n"
    tab += f"static const unsigned k_len[{len(seqs)}] = {{ {', '.join(str(len(c)) for c in seqs)} }};\n"
    rows = []
    for s in doc["sequences"]:
        rows.append("{" + ", ".join("{%dull, {%s}}" % (v[0], ", ".join(f"0x{x:08X}u" for x in v[1:]))
                                    for v in s["vectors"]) + "}")
    tab += f"static const gold_t k_gold[{len(seqs)}][{nv}] = {{\n" + ",\n".join(rows) + "\n};\n"
    return pre + GOLD_MAIN.replace("@TABLE@", tab).replace("@VECTORS@", str(nv))


def run_golden(doc=None, cc=None, extra=(), runner=()):
    """Build golden_source on this host and run it. Returns (rc, output)."""
    import json
    if doc is None:
        with open(GOLDEN) as f:
            doc = json.load(f)
    cc = cc or gnu_cc()
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "g.c")
        exe = os.path.join(d, "g")
        with open(src, "w") as f:
            f.write(golden_source(doc))
        built = subprocess.run(
            [cc, "-std=gnu11", "-O2", "-w", *extra, "-I", RUNTIME, "-I", os.path.join(ROOT, "src", "kernel"),
             src, CORE, "-o", exe, "-lm", "-no-pie", "-Wl,--unresolved-symbols=ignore-all"],
            capture_output=True, text=True)
        if built.returncode != 0:
            raise AssertionError(built.stderr[-4000:])
        run = subprocess.run([*runner, exe], capture_output=True, text=True, timeout=600, cwd=d)
        return run.returncode, run.stdout + run.stderr


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="XBA-303 interpreter vectors")
    ap.add_argument("--golden", action="store_true",
                    help="regenerate data/interp_golden.json on the CPU (docker, linux/386)")
    ap.add_argument("--count", type=int, default=300)
    ap.add_argument("--length", type=int, default=16)
    ap.add_argument("--seed", type=int, default=303000)
    ap.add_argument("--vectors", type=int, default=8)
    ap.add_argument("--workdir", default=os.path.join(tempfile.gettempdir(), "xba303-golden"))
    a = ap.parse_args()
    if a.golden:
        doc = make_golden(a.count, a.length, a.seed, a.vectors, a.workdir)
        ok = sum(1 for s in doc["sequences"] if s["lifter_matches_cpu"])
        print(f"wrote {GOLDEN}: {len(doc['sequences'])} sequences, the lifter matches the CPU on {ok}")
