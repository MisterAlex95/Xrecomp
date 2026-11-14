#!/usr/bin/env python3
"""Differential fuzzing of the N64 lifter against the interpreter (XBA-170 / XBA-171).

    recomp_fuzz.py --n64-run PATH --work DIR [--seeds 0-5] [--frames 10] [--insns 600]

For each seed: a synthetic ROM (our own boot code in place of IPL3: it copies the program to 0x80000400 and jumps
there) of random MIPS III code -- every ALU form with the 32-bit sign-extension rules and the 64-bit forms, shifts
of every kind, MULT / DIV / DMULT / DDIV and HI / LO, ADD / SUB / DADD overflows and traps that raise, loads and
stores of every width with LWL / LWR / LDL / LDR / SWL / SWR / SDL / SDR, through cached and uncached RDRAM,
forward conditional branches and branch-likely forms with random delay slots, linking branches, JAL / JALR to leaf
subroutines, counted backward loops, COP1 moves, loads / stores, arithmetic, conversions and compares with BC1T /
BC1F (likely forms too) under random rounding modes and FR modes, CTC1 / CFC1, MFC0 COUNT / RANDOM, CACHE and SYNC
-- in an endless loop, under a fast COUNT = COMPARE timer interrupt whose handler (installed at 0x80000180 at run
time: recorded by the discovery loop, then lifted) moves COMPARE on and returns, and skips the instruction of any
other exception (overflow, trap, FPE). So interrupts land at any instruction of the lifted code, delay slots
included. It is lifted and built (tools/consoles/n64_recomp_build.py, discovery loop) and run by n64_run --pack
--lockstep: the interpreter and the recompiled machine must agree on the whole state (GPRs, FPRs, COP0, time,
instruction count, scheduler, RDRAM) after every frame. Exit 0 when every seed agrees, 1 at the first difference,
77 when Python or a C compiler is missing (skipped).
"""

import argparse
import os
import random
import shutil
import struct
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
BUILD = os.path.join(REPO, "tools", "consoles", "n64_recomp_build.py")

BASE = 0x80000400
DATA = 0x80100000  # r28 points here (4 KB of scratch data), r29 at its uncached alias
REGS = list(range(1, 26))  # random destinations / sources (k0 / k1: the handler's; r28 / r29: data bases; r30: loops)
K0, K1, GP, SP, FP, RA = 26, 27, 28, 29, 30, 31


def r_type(funct, rs=0, rt=0, rd=0, sa=0):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def j_type(op, target):
    return (op << 26) | ((target >> 2) & 0x3FFFFFF)


def cop0(sub, rt, rd):
    return (0x10 << 26) | (sub << 21) | (rt << 16) | (rd << 11)


def cop1(fmt, ft, fs, fd, funct):
    return (0x11 << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct


ERET = 0x42000018
NOP = 0

ALU3 = [0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x2D, 0x2F, 0x04, 0x06, 0x07, 0x14, 0x16, 0x17]
SHIFT = [0x00, 0x02, 0x03, 0x38, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F]
IMM = [0x09, 0x19, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F]
MULDIV = [0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F]
OVF3 = [0x20, 0x22, 0x2C, 0x2E]
TRAPS = [0x30, 0x31, 0x32, 0x33, 0x34, 0x36]
LOADS = [(0x20, 1), (0x24, 1), (0x21, 2), (0x25, 2), (0x23, 4), (0x27, 4), (0x37, 8)]
STORES = [(0x28, 1), (0x29, 2), (0x2B, 4), (0x3F, 8)]
PARTIAL = [0x22, 0x26, 0x1A, 0x1B, 0x2A, 0x2E, 0x2C, 0x2D]  # LWL LWR LDL LDR SWL SWR SDL SDR
BRANCH2 = [0x04, 0x05, 0x14, 0x15]  # BEQ BNE BEQL BNEL
BRANCH1 = [0x06, 0x07, 0x16, 0x17]  # BLEZ BGTZ BLEZL BGTZL
REGIMM_B = [0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13]  # BLTZ BGEZ (L) (AL) (ALL)
FOPS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]  # arithmetic and roundings


class Gen:
    def __init__(self, seed, insns):
        self.r = random.Random(seed)
        self.n = insns
        self.code = []
        self.fr = self.r.random() < 0.5
        self.period = self.r.choice([40, 90, 150, 300, 600])

    def pc(self):
        return BASE + 4 * len(self.code)

    def emit(self, w):
        self.code.append(w & 0xFFFFFFFF)

    def li(self, rt, v):
        self.emit(i_type(0x0F, 0, rt, v >> 16))
        self.emit(i_type(0x0D, rt, rt, v & 0xFFFF))

    def reg(self):
        return self.r.choice(REGS)

    def src(self):
        return self.r.choice(REGS + [0, RA])

    def freg(self, double=False):
        f = self.r.randrange(32)
        if double and not self.fr and self.r.random() < 0.85:
            f &= ~1
        return f

    def simple(self):
        """One random instruction that neither transfers control nor raises (fit for a delay slot)."""
        r = self.r
        k = r.random()
        if k < 0.30:
            return r_type(r.choice(ALU3), self.src(), self.src(), self.reg())
        if k < 0.42:
            return r_type(r.choice(SHIFT), 0, self.src(), self.reg(), r.randrange(32))
        if k < 0.55:
            return i_type(r.choice(IMM), self.src(), self.reg(), r.randrange(0x10000))
        if k < 0.62:
            return r_type(r.choice(MULDIV), self.src(), self.src(), 0)
        if k < 0.66:
            return r_type(r.choice([0x10, 0x12]), 0, 0, self.reg())  # MFHI MFLO
        if k < 0.68:
            return r_type(r.choice([0x11, 0x13]), self.src(), 0, 0)  # MTHI MTLO
        if k < 0.78:
            op, size = r.choice(LOADS)
            return i_type(op, r.choice([GP, SP]), self.reg(), r.randrange(0, 0xF80, size))
        if k < 0.86:
            op, size = r.choice(STORES)
            return i_type(op, r.choice([GP, SP]), self.src(), r.randrange(0, 0xF80, size))
        if k < 0.92:
            return i_type(r.choice(PARTIAL), r.choice([GP, SP]), self.reg(), r.randrange(0, 0xF80))
        if k < 0.95:  # COP1 moves and loads / stores
            c = r.randrange(8)
            if c < 4:
                return (0x11 << 26) | ([0, 1, 4, 5][c] << 21) | (self.reg() << 16) | (self.freg(c in (1, 3)) << 11)
            op = [0x31, 0x35, 0x39, 0x3D][c - 4]
            return i_type(op, r.choice([GP, SP]), self.freg(op in (0x35, 0x3D)), r.randrange(0, 0xF80, 8))
        if k < 0.97:
            return cop0(0, self.reg(), r.choice([9, 1]))  # MFC0 COUNT / RANDOM (interpreted)
        if k < 0.98:
            return i_type(0x2F, GP, r.choice([0x19, 0x15, 0x01]), r.randrange(0, 0xF80, 16))  # CACHE
        if k < 0.985:
            return 0x0000000F  # SYNC
        return (0x11 << 26) | (2 << 21) | (self.reg() << 16) | (31 << 11)  # CFC1 FCSR

    def raising(self):
        """An instruction that may raise (the handler skips it): never placed in a delay slot."""
        r = self.r
        k = r.random()
        if k < 0.35:
            return r_type(r.choice(OVF3), self.src(), self.src(), self.reg())
        if k < 0.45:
            return i_type(r.choice([0x08, 0x18]), self.src(), self.reg(), r.randrange(0x10000))  # ADDI DADDI
        if k < 0.55:
            return r_type(r.choice(TRAPS), self.src(), self.src(), 0)
        fmt = r.choice([16, 17, 17, 20, 21])
        if k < 0.85:
            d = fmt in (17, 21)
            if fmt in (20, 21):
                return cop1(fmt, 0, self.freg(d), self.freg(), r.choice([32, 33]))  # CVT.S / CVT.D from W / L
            funct = r.choice(FOPS + [32, 33, 36, 37])
            if (fmt, funct) in ((16, 32), (17, 33)):
                funct = 36
            return cop1(fmt, self.freg(d), self.freg(d), self.freg(d), funct)
        return cop1(r.choice([16, 17]), self.freg(True), self.freg(True), 0, 48 + r.randrange(16))  # C.cond

    def branch(self, remaining):
        """A forward branch over 1..k instructions with a random delay slot; returns the words."""
        r = self.r
        skip = r.randint(1, min(6, remaining))
        k = r.random()
        if k < 0.45:
            w = i_type(r.choice(BRANCH2), self.src(), self.src(), skip)
        elif k < 0.65:
            w = i_type(r.choice(BRANCH1), self.src(), 0, skip)
        elif k < 0.85:
            w = (1 << 26) | (self.src() << 21) | (r.choice(REGIMM_B) << 16) | (skip & 0xFFFF)
        else:
            w = (0x11 << 26) | (8 << 21) | (r.randrange(4) << 16) | (skip & 0xFFFF)  # BC1F / BC1T / BC1FL / BC1TL
        out = [w, self.simple()]
        for _ in range(skip - 1):
            out.append(self.simple())
        return out

    def program(self):
        r = self.r
        handler = self.handler()
        # install the exception handler at 0x80000180
        fix_handler = len(self.code)
        self.li(8, 0)
        self.li(9, 0x80000180)
        self.emit(i_type(0x09, 0, 10, len(handler)))
        loop = self.pc()
        self.emit(i_type(0x23, 8, 11, 0))
        self.emit(i_type(0x09, 8, 8, 4))
        self.emit(i_type(0x2B, 9, 11, 0))
        self.emit(i_type(0x09, 10, 10, -1))
        self.emit(i_type(0x05, 10, 0, (loop - self.pc() - 4) >> 2))
        self.emit(i_type(0x09, 9, 9, 4))
        self.li(GP, DATA)
        self.li(SP, DATA | 0x20000000)
        for reg in REGS + [FP, RA]:
            self.li(reg, r.getrandbits(32))
            if r.random() < 0.4:
                self.emit(r_type(0x3C, 0, reg, reg, r.randrange(32)))  # DSLL32: a 64-bit value
                self.emit(i_type(0x0D, reg, reg, r.getrandbits(16)))
        self.li(8, (0x20008001 | (0x04000000 if self.fr else 0)))  # CU1 (FR) IM7 IE
        self.emit(cop0(4, 8, 12))
        for f in range(32):
            self.emit((0x11 << 26) | ((5 if r.random() < 0.5 else 4) << 21) | (self.reg() << 16) | (f << 11))
        self.emit(i_type(0x0D, 0, 8, r.randrange(4)))  # rounding mode
        self.emit((0x11 << 26) | (6 << 21) | (8 << 16) | (31 << 11))
        self.emit(cop0(0, 8, 9))
        self.emit(i_type(0x09, 8, 8, self.period))
        self.emit(cop0(4, 8, 11))
        # the body, in an endless loop; subroutines after it
        nsubs = 4
        top = self.pc()
        body, i = [], 0
        jal_sites = []
        while i < self.n:
            k = r.random()
            left = self.n - i
            if k < 0.10 and left > 8:
                words = self.branch(left - 2)
            elif k < 0.13:
                jal_sites.append(len(body))
                words = [j_type(0x03, 0), self.simple()]
            elif k < 0.15:
                jal_sites.append(-len(body) - 1)  # JALR through r25
                words = [0, 0, r_type(0x09, 25, 0, RA), self.simple()]
            elif k < 0.17 and left > 12:  # counted backward loop on r30
                inner = [self.simple() for _ in range(r.randint(1, 5))]
                words = [i_type(0x09, 0, FP, r.randint(1, 4))] + inner
                words += [i_type(0x09, FP, FP, -1), i_type(0x05, FP, 0, -(len(inner) + 2)), self.simple()]
            elif k < 0.27:
                words = [self.raising()]
            elif k < 0.29:  # set the rounding mode (and sometimes an enable: FPE exceptions, also after the result)
                words = [i_type(0x0D, 0, 1, r.randrange(4) | (r.choice([0, 0, 0, 0x800, 0x400, 0x200, 0x100, 0x80]))),
                         (0x11 << 26) | (6 << 21) | (1 << 16) | (31 << 11)]
            else:
                words = [self.simple()]
            body.extend(words)
            i += len(words)
        end = top + 4 * len(body)
        subs_at = end + 8
        sub_addrs = []
        sub_words = []
        for _ in range(nsubs):
            sub_addrs.append(subs_at + 4 * len(sub_words))
            for _ in range(r.randint(1, 8)):
                sub_words.append(self.simple())
            sub_words += [r_type(0x08, RA), self.simple()]
        for site in jal_sites:
            t = r.choice(sub_addrs)
            if site >= 0:
                body[site] = j_type(0x03, t)
            else:
                s = -site - 1
                body[s] = i_type(0x0F, 0, 25, t >> 16)
                body[s + 1] = i_type(0x0D, 25, 25, t & 0xFFFF)
        self.code.extend(body)
        self.emit(j_type(0x02, top))
        self.emit(NOP)
        self.code.extend(sub_words)
        hsrc = self.pc()
        self.code.extend(handler)
        # patch the handler source address
        self.code[fix_handler] = i_type(0x0F, 0, 8, hsrc >> 16)
        self.code[fix_handler + 1] = i_type(0x0D, 8, 8, hsrc & 0xFFFF)
        return self.code

    def handler(self):
        """At 0x80000180: the timer interrupt moves COMPARE on and counts; any other exception skips the instruction
        (the generator never puts a raising instruction in a delay slot)."""
        h = [
            cop0(0, K0, 13),                      # mfc0 k0, Cause
            i_type(0x0C, K0, K0, 0x7C),           # andi k0, k0, 0x7C
            i_type(0x04, K0, 0, 5),               # beq k0, zero, irq
            NOP,
            cop0(0, K0, 14),                      # mfc0 k0, EPC
            i_type(0x09, K0, K0, 4),              # addiu k0, k0, 4
            cop0(4, K0, 14),                      # mtc0 k0, EPC
            ERET,
            i_type(0x0F, 0, K0, DATA >> 16),      # irq: lui k0, DATA
            i_type(0x23, K0, K1, 0xFF0),          # lw k1, 0xFF0(k0)
            i_type(0x09, K1, K1, 1),              # addiu k1, k1, 1
            i_type(0x2B, K0, K1, 0xFF0),          # sw k1, 0xFF0(k0)
            cop0(0, K1, 9),                       # mfc0 k1, Count
            i_type(0x09, K1, K1, self.period),    # addiu k1, k1, period
            cop0(4, K1, 11),                      # mtc0 k1, Compare
            ERET,
        ]
        return h


def boot_code(nbytes):
    """Our IPL3 stand-in (runs from DMEM at 0xA4000040): copy ROM 0x1000.. to 0x80000400 and jump there."""
    w = []
    w += [i_type(0x0F, 0, 8, 0xB000), i_type(0x0D, 8, 8, 0x1000)]   # t0 = 0xB0001000
    w += [i_type(0x0F, 0, 9, 0xA000), i_type(0x0D, 9, 9, 0x0400)]   # t1 = 0xA0000400
    w += [i_type(0x0F, 0, 10, nbytes >> 16), i_type(0x0D, 10, 10, nbytes & 0xFFFF)]
    w += [i_type(0x23, 8, 11, 0), i_type(0x09, 8, 8, 4), i_type(0x2B, 9, 11, 0), i_type(0x09, 10, 10, -4),
          i_type(0x05, 10, 0, -5), i_type(0x09, 9, 9, 4)]
    w += [i_type(0x0F, 0, 8, 0x8000), i_type(0x0D, 8, 8, 0x0400), r_type(0x08, 8), NOP]
    return w


def make_rom(seed, insns):
    g = Gen(seed, insns)
    code = g.program()
    prog = struct.pack(">%dI" % len(code), *code)
    prog += b"\0" * (-len(prog) % 16)
    hdr = bytearray(0x1000)
    struct.pack_into(">IIIIII", hdr, 0, 0x80371240, 0x0000000F, BASE, 0x0000144C, 0x12345678 ^ seed, 0x9ABCDEF0)
    hdr[0x20:0x34] = b"XBA RCFUZZ          "
    hdr[0x3B:0x3F] = b"NXFE"
    boot = boot_code(len(prog))
    struct.pack_into(">%dI" % len(boot), hdr, 0x40, *boot)
    rom = bytes(hdr) + prog
    rom += b"\0" * (-len(rom) % 0x10000)
    return rom, g


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--n64-run", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--seeds", default="0-5")
    ap.add_argument("--frames", type=int, default=10)
    ap.add_argument("--insns", type=int, default=600)
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("JOBS", "4")))
    args = ap.parse_args(argv)
    if not shutil.which(os.environ.get("CC", "cc")):
        print("skipped: no C compiler")
        return 77
    lo, _, hi = args.seeds.partition("-")
    seeds = range(int(lo), int(hi or lo) + 1)
    os.makedirs(args.work, exist_ok=True)
    for seed in seeds:
        rom, g = make_rom(seed, args.insns)
        d = os.path.join(args.work, "seed%d" % seed)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        path = os.path.join(d, "fuzz.z64")
        with open(path, "wb") as f:
            f.write(rom)
        r = subprocess.run([sys.executable, BUILD, path, os.path.join(d, "pack"), "--count", "--loop", "4",
                            "--n64-run", args.n64_run, "--frames", str(args.frames), "--jobs", str(args.jobs)],
                           capture_output=True, text=True, timeout=1200, check=False)
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            print("FAIL: seed %d: the pack did not build / converge" % seed)
            return 1
        r = subprocess.run([args.n64_run, path, "--quiet", "--pack", os.path.join(d, "pack", "libtitle.so"),
                            "--lockstep", "--stats", "--frames", str(args.frames)],
                           capture_output=True, text=True, timeout=600, check=False)
        out = r.stdout
        native = [ln for ln in out.splitlines() if "native" in ln or ln.startswith("recomp:")]
        print("seed %d (FR=%d, timer every %d COUNT ticks): %s" % (seed, g.fr, g.period,
                                                                   " / ".join(native) or out.strip()))
        if "LOCKSTEP OK" not in out:
            print(out + r.stderr)
            print("FAIL: seed %d differs from the interpreter" % seed)
            return 1
    print("recomp_fuzz: %d seeds, all in lockstep with the interpreter" % len(seeds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
