"""Differential test of the Xenon lifter's loads, stores, CR logic and CTR branches (XBA-270): each instruction in its
own function of a synthetic XEX2, lifted, built with the host cc and run with a known memory pattern and registers;
results, written memory, updated base registers, CR and loop counts are checked against references written here from
the architecture (big-endian memory, sign / zero extension, update forms writing the effective address back,
byte-reversed forms, single-precision loads and stores, CR bit logic, 32-bit mode's CTR test)."""

import random
import shutil
import struct
import subprocess
import sys

import pytest

from . import CONSOLE  # noqa: F401
from tools.xex.synth import build_xex
from tools.xex.xex import parse_xex

from .recomp import RUNTIME_DIRS, recompile

M64 = (1 << 64) - 1
TEXT = 0x82001000
MEM = 0x40000000
BASE = MEM + 0x100            # r3 points here
PATTERN = bytes((0x81 + 37 * k) & 0xFF for k in range(0x200))


def d(op, rt, ra, imm):
    return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)


def ds(op, rt, ra, imm, xo):
    return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFC) | xo


def x(rt, ra, rb, xo, rc=0):
    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc


def xl(bt, ba, bb, xo):
    return (19 << 26) | (bt << 21) | (ba << 16) | (bb << 11) | (xo << 1)


BLR = 0x4E800020
MFCR_R8 = x(8, 0, 0, 19)


def sx(v, bits):
    v &= (1 << bits) - 1
    return (v - (1 << bits)) & M64 if v >> (bits - 1) else v


def be(n, off):
    return int.from_bytes(PATTERN[0x100 + off:0x100 + off + n], "big")


def le(n, off):
    return int.from_bytes(PATTERN[0x100 + off:0x100 + off + n], "little")


# loads: (name, word, reference(r4) -> r7, updates r3 to (r3 + ea_offset) or None)
LOADS = [
    ("lbz", d(34, 7, 3, 5), lambda r4: (be(1, 5), None)),
    ("lhz", d(40, 7, 3, 6), lambda r4: (be(2, 6), None)),
    ("lha", d(42, 7, 3, 6), lambda r4: (sx(be(2, 6), 16), None)),
    ("lwz", d(32, 7, 3, 8), lambda r4: (be(4, 8), None)),
    ("lwa", ds(58, 7, 3, 8, 2), lambda r4: (sx(be(4, 8), 32), None)),
    ("ld", ds(58, 7, 3, 16, 0), lambda r4: (be(8, 16), None)),
    ("lbzu", d(35, 7, 3, -3), lambda r4: (be(1, -3), -3)),
    ("lhzu", d(41, 7, 3, 10), lambda r4: (be(2, 10), 10)),
    ("lhau", d(43, 7, 3, 2), lambda r4: (sx(be(2, 2), 16), 2)),
    ("lwzu", d(33, 7, 3, -8), lambda r4: (be(4, -8), -8)),
    ("ldu", ds(58, 7, 3, 24, 1), lambda r4: (be(8, 24), 24)),
    ("lbzx", x(7, 3, 4, 87), lambda r4: (be(1, r4), None)),
    ("lhzx", x(7, 3, 4, 279), lambda r4: (be(2, r4), None)),
    ("lhax", x(7, 3, 4, 343), lambda r4: (sx(be(2, r4), 16), None)),
    ("lwzx", x(7, 3, 4, 23), lambda r4: (be(4, r4), None)),
    ("lwax", x(7, 3, 4, 341), lambda r4: (sx(be(4, r4), 32), None)),
    ("ldx", x(7, 3, 4, 21), lambda r4: (be(8, r4), None)),
    ("lwzux", x(7, 3, 4, 55), lambda r4: (be(4, r4), r4)),
    ("ldux", x(7, 3, 4, 53), lambda r4: (be(8, r4), r4)),
    ("lhbrx", x(7, 3, 4, 790), lambda r4: (le(2, r4), None)),
    ("lwbrx", x(7, 3, 4, 534), lambda r4: (le(4, r4), None)),
    ("ldbrx", x(7, 3, 4, 532), lambda r4: (le(8, r4), None)),
]
VAL = 0x1122334455667788
# stores of r5 = VAL: (name, word, offset written, bytes written, r3 update)
STORES = [
    ("stb", d(38, 5, 3, 3), lambda r4: (3, VAL.to_bytes(8, "big")[7:]), None),
    ("sth", d(44, 5, 3, 4), lambda r4: (4, VAL.to_bytes(8, "big")[6:]), None),
    ("stw", d(36, 5, 3, 8), lambda r4: (8, VAL.to_bytes(8, "big")[4:]), None),
    ("std", ds(62, 5, 3, 16, 0), lambda r4: (16, VAL.to_bytes(8, "big")), None),
    ("stwu", d(37, 5, 3, -4), lambda r4: (-4, VAL.to_bytes(8, "big")[4:]), -4),
    ("stdu", ds(62, 5, 3, 8, 1), lambda r4: (8, VAL.to_bytes(8, "big")), 8),
    ("stbx", x(5, 3, 4, 215), lambda r4: (r4, VAL.to_bytes(8, "big")[7:]), None),
    ("sthx", x(5, 3, 4, 407), lambda r4: (r4, VAL.to_bytes(8, "big")[6:]), None),
    ("stwx", x(5, 3, 4, 151), lambda r4: (r4, VAL.to_bytes(8, "big")[4:]), None),
    ("stdx", x(5, 3, 4, 149), lambda r4: (r4, VAL.to_bytes(8, "big")), None),
    ("stwux", x(5, 3, 4, 183), lambda r4: (r4, VAL.to_bytes(8, "big")[4:]), "r4"),
    ("sthbrx", x(5, 3, 4, 918), lambda r4: (r4, VAL.to_bytes(8, "big")[6:][::-1]), None),
    ("stwbrx", x(5, 3, 4, 662), lambda r4: (r4, VAL.to_bytes(8, "big")[4:][::-1]), None),
    ("stdbrx", x(5, 3, 4, 660), lambda r4: (r4, VAL.to_bytes(8, "little")), None),
]
F1 = 3.140625            # exact in single
F1_BITS = struct.unpack(">Q", struct.pack(">d", F1))[0]
FP = [  # name, word, check kind
    ("lfs", d(48, 7, 3, 32), "lfs"),
    ("lfd", d(50, 7, 3, 32), "lfd"),
    ("stfs", d(52, 1, 3, 40), "stfs"),
    ("stfd", d(54, 1, 3, 40), "stfd"),
    ("stfiwx", x(1, 3, 4, 983), "stfiwx"),
]

# CR logic: CR preset, crbD = op(crbA, crbB): (name, xo)
CRL = [("crand", 257, lambda a, b: a & b), ("cror", 449, lambda a, b: a | b), ("crxor", 193, lambda a, b: a ^ b),
       ("crnand", 225, lambda a, b: 1 - (a & b)), ("crnor", 33, lambda a, b: 1 - (a | b)),
       ("creqv", 289, lambda a, b: 1 - (a ^ b)), ("crandc", 129, lambda a, b: a & (1 - b)),
       ("crorc", 417, lambda a, b: a | (1 - b))]


def crbit(cr, n):
    return (cr >> (31 - n)) & 1


def setbit(cr, n, v):
    return (cr & ~(1 << (31 - n))) | (v << (31 - n))


HARNESS = r"""
#include <stdio.h>
#include "xenon_rt.h"
extern const xe_rc_entry xe_rc_resume[];
extern const unsigned xe_rc_resume_count;
static uint8_t mem[0x400];
static const uint8_t pattern[0x200] = {%(pattern)s};
static xe_machine M;
static int run(uint32_t entry)
{
    M.cpu.pc = entry;
    M.cpu.lr = 0xDEAD0000u;
    for (int n = 0; n < 100000; n++) {
        if (M.cpu.pc == 0xDEAD0000u)
            return 0;
        xe_rc_fn f = NULL;
        for (unsigned k = 0; k < xe_rc_resume_count; k++)
            if (xe_rc_resume[k].entry == M.cpu.pc)
                f = xe_rc_resume[k].fn;
        if (!f)
            return 2;
        M.rc_exit = 0;
        f(&M);
        if (M.rc_exit)
            return 10 + M.rc_exit;
    }
    return 3;
}
static void c(uint32_t e, uint64_t r3, uint64_t r4, uint64_t r5, uint32_t cr, uint64_t f1)
{
    memset(&M.cpu, 0, sizeof M.cpu);
    memcpy(mem, pattern, sizeof pattern);
    M.cpu.gpr[3] = r3, M.cpu.gpr[4] = r4, M.cpu.gpr[5] = r5, M.cpu.cr = cr;
    memcpy(&M.cpu.fpr[1], &f1, 8);
    int rc = run(e);
    uint64_t f7;
    memcpy(&f7, &M.cpu.fpr[7], 8);
    printf("%%d %%016llx %%016llx %%08x %%016llx ", rc, (unsigned long long)M.cpu.gpr[7],
           (unsigned long long)M.cpu.gpr[3], (unsigned)M.cpu.cr, (unsigned long long)f7);
    for (int k = 0xF0; k < 0x130; k++)
        printf("%%02x", mem[k]);
    printf("\n");
}
int main(void)
{
    M.mem = mem;
    M.mem_base = 0x40000000u;
    M.mem_size = sizeof mem;
%(calls)s
    return 0;
}
"""


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_memory_cr_and_ctr(tmp_path):
    rng = random.Random(2701)
    words, funcs, kinds = [], [], []

    def func(body, kind):
        funcs.append((4 * len(words), 4 * (len(body) + 1)))
        words.extend(body + [BLR])
        kinds.append(kind)

    for name, w, fn in LOADS:
        func([w], ("load", name, fn))
    for name, w, fn, upd in STORES:
        func([w], ("store", name, fn, upd))
    for name, w, k in FP:
        func([w], ("fp", name, k))
    for name, xo, fn in CRL:
        func([xl(9, 3, 30, xo), MFCR_R8], ("crl", name, fn))
    func([(19 << 26) | (5 << 23) | (2 << 18), MFCR_R8], ("mcrf", "mcrf cr5, cr2", None))
    func([(31 << 26) | (5 << 21) | (0x28 << 12) | (144 << 1), MFCR_R8], ("mtcrf", "mtcrf 0x28, r5", None))
    # bdnz: r7 counts the iterations of mtctr r3; loop: addi r7, r7, 1; bdnz loop
    func([x(3, 9, 9, 467) & ~(0x3FF << 11) | (9 << 16), d(14, 7, 0, 0), d(14, 7, 7, 1), 0x4200FFFC], ("bdnz", None,
                                                                                                     None))
    data, _ = build_xex(words, functions=funcs, compression="none")
    s = recompile(parse_xex(data), str(tmp_path / "src"))
    assert s["interp_fallback_insns"] == 0, s["unlifted"]
    calls, checks = [], []
    for (off, _), kind in zip(funcs, kinds):
        e = TEXT + off
        if kind[0] == "load":
            for r4 in (0, 4, 13, 0x30):
                calls.append("    c(0x%08Xu, 0x%XULL, %d, 0, 0, 0);" % (e, BASE, r4))
                checks.append((kind, r4))
        elif kind[0] == "store":
            for r4 in (1, 12, 0x21):
                calls.append("    c(0x%08Xu, 0x%XULL, %d, 0x%XULL, 0, 0);" % (e, BASE, r4, VAL))
                checks.append((kind, r4))
        elif kind[0] == "fp":
            calls.append("    c(0x%08Xu, 0x%XULL, 0x20, 0, 0, 0x%016XULL);" % (e, BASE, F1_BITS))
            checks.append((kind, None))
        elif kind[0] in ("crl", "mcrf", "mtcrf"):
            for _ in range(8):
                cr = rng.getrandbits(32)
                r5 = rng.getrandbits(32)
                calls.append("    c(0x%08Xu, 0, 0, 0x%XULL, 0x%08Xu, 0);" % (e, r5, cr))
                checks.append((kind, (cr, r5)))
        elif kind[0] == "bdnz":
            for r3 in (1, 3, 17, 0x100000005):
                calls.append("    c(0x%08Xu, 0x%XULL, 0, 0, 0, 0);" % (e, r3))
                checks.append((kind, r3))
    (tmp_path / "h.c").write_text(HARNESS % {"pattern": ",".join(str(b) for b in PATTERN),
                                             "calls": "\n".join(calls)})
    inc = []
    for dd in RUNTIME_DIRS:
        inc += ["-I", dd]
    exe = str(tmp_path / "t")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-w", "-ffp-contract=off"] + inc + ["-o", exe, str(tmp_path / "h.c")] +
                   sorted(str(p) for p in (tmp_path / "src").glob("*.c")) + ["-lm"], check=True)
    out = subprocess.run([exe], capture_output=True, text=True, timeout=60).stdout.splitlines()
    assert len(out) == len(checks)
    bad = []
    for line, (kind, arg) in zip(out, checks):
        rc, r7, r3, cr, f7, memhex = line.split()
        r7, r3, cr, f7 = int(r7, 16), int(r3, 16), int(cr, 16), int(f7, 16)
        window = bytes.fromhex(memhex)                  # mem[0xF0 .. 0x130)
        name = kind[1]
        if rc != "0":
            bad.append((name, "rc " + rc))
            continue
        if kind[0] == "load":
            want, upd = kind[2](arg)
            if r7 != want:
                bad.append((name, arg, "r7 %016X want %016X" % (r7, want)))
            if r3 != (BASE + upd if upd is not None else BASE):
                bad.append((name, arg, "r3 %X" % r3))
        elif kind[0] == "store":
            off, data_ = kind[2](arg)
            exp = bytearray(PATTERN[0xF0:0x130])
            exp[0x10 + off:0x10 + off + len(data_)] = data_
            if window != bytes(exp):
                bad.append((name, arg, "memory %s want %s" % (window.hex(), bytes(exp).hex())))
            upd = kind[3]
            want3 = BASE + (arg if upd == "r4" else upd if upd is not None else 0)
            if r3 != want3:
                bad.append((name, arg, "r3 %X want %X" % (r3, want3)))
        elif kind[0] == "fp":
            k = kind[2]
            if k == "lfs":
                want = struct.unpack(">Q", struct.pack(">d", struct.unpack(">f", PATTERN[0x120:0x124])[0]))[0]
                if f7 != want:
                    bad.append((name, "f7 %016X want %016X" % (f7, want)))
            elif k == "lfd":
                if f7 != int.from_bytes(PATTERN[0x120:0x128], "big"):
                    bad.append((name, "f7 %016X" % f7))
            elif k in ("stfs", "stfd", "stfiwx"):
                exp = bytearray(PATTERN[0xF0:0x130])
                if k == "stfs":
                    exp[0x38:0x3C] = struct.pack(">f", F1)
                elif k == "stfd":
                    exp[0x38:0x40] = struct.pack(">d", F1)
                else:
                    exp[0x30:0x34] = (F1_BITS & 0xFFFFFFFF).to_bytes(4, "big")
                if window != bytes(exp):
                    bad.append((name, "memory %s" % window.hex()))
        elif kind[0] == "crl":
            cr0, _ = arg
            want = setbit(cr0, 9, kind[2](crbit(cr0, 3), crbit(cr0, 30)))
            if cr != want:
                bad.append((name, "CR %08X want %08X (from %08X)" % (cr, want, cr0)))
        elif kind[0] == "mcrf":
            cr0, _ = arg
            f2 = (cr0 >> (28 - 8)) & 0xF
            want = (cr0 & ~(0xF << (28 - 20))) | (f2 << (28 - 20))
            if cr != want:
                bad.append((name, "CR %08X want %08X" % (cr, want)))
        elif kind[0] == "mtcrf":
            cr0, r5 = arg
            m = 0xF << 20 | 0xF << 12  # fields 2 and 4 (mask 0x28)
            want = (cr0 & ~m) | (r5 & m)
            if cr != want:
                bad.append((name, "CR %08X want %08X" % (cr, want)))
        elif kind[0] == "bdnz":
            want = arg & 0xFFFFFFFF  # 32-bit mode: CTR's low word counts
            if r7 != want:
                bad.append(("bdnz", hex(arg), "%d iterations, want %d" % (r7, want)))
    assert not bad, "%d mismatches: %s" % (len(bad), bad[:10])
