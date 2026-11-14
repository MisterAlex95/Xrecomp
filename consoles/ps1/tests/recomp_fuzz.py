#!/usr/bin/env python3
"""Differential fuzzing of the PS1 lifter against the interpreter (XBA-161 / XBA-162).

    recomp_fuzz.py --ps1-run PATH --bios PATH --work DIR [--seeds 0-5] [--frames 12] [--insns 400]

For each seed: a synthetic PS-X EXE of random MIPS I code (every ALU form, shifts, MULT / DIV and HI / LO, loads and
stores of every width with LWL / LWR / SWL / SWR, load-use hazards, loads in delay slots, forward conditional
branches with random delay slots, linking branches, JAL / JALR to leaf subroutines, GTE transfers, LWC2 / SWC2 and
commands) in an endless loop, under a fast root-counter IRQ (timer 2, a small target) whose handler, installed at
the exception vector, acknowledges it and returns, so interrupts land at any instruction of the lifted code, delay
slots and loads in flight included. It is lifted and built (tools/consoles/ps1_recomp_build.py, discovery loop)
and run by ps1_run --pack --lockstep: the interpreter and the recompiled machine must agree on the whole state
(registers, cycles, scheduler, RAM, GTE, VRAM) after every frame. Exit 0 when every seed agrees, 1 at the first
difference, 77 when Python, a C compiler or the BIOS is missing (skipped).
"""

import argparse
import os
import random
import shutil
import struct
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
BUILD = os.path.join(REPO, "tools", "consoles", "ps1_recomp_build.py")

BASE = 0x80010000
DATA = 0x80100000  # r28 points here (4 KB of scratch data)
REGS = list(range(1, 26))  # random destinations / sources (r26 / r27 = k0 / k1: the IRQ handler's; r28 = data base)
K0, K1, GP, RA = 26, 27, 28, 31

# GTE commands (imm25 forms the GTE implements): RTPS, NCLIP, OP, DPCS, INTPL, MVMVA, NCDS, CDP, NCDT, NCCS, CC,
# NCS, NCT, SQR, DCPL, DPCT, AVSZ3, AVSZ4, RTPT, GPF, GPL, NCCT, with sf / lm variations.
GTE_CMDS = [0x0180001, 0x0280030, 0x1400006, 0x170000C, 0x0780010, 0x0980011, 0x04A6012, 0x0E80413, 0x1280414,
            0x0F80416, 0x108041B, 0x138041C, 0x0C8041E, 0x0D80420, 0x0A00428, 0x0680029, 0x078002A, 0x158002D,
            0x168002E, 0x190003D, 0x1A0003E, 0x0F8003F, 0x0400012, 0x0080012]


def r_type(funct, rs=0, rt=0, rd=0, sa=0):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def j_type(op, target):
    return (op << 26) | ((target >> 2) & 0x3FFFFFF)


NOP = 0


class Gen:
    def __init__(self, seed, insns):
        self.r = random.Random(seed)
        self.n = insns
        self.code = []  # words, from BASE
        self.fixups = []  # (index, kind, label)
        self.labels = {}

    def pc(self):
        return BASE + 4 * len(self.code)

    def emit(self, w):
        self.code.append(w & 0xFFFFFFFF)

    def li(self, rt, v):
        self.emit(i_type(0x0F, 0, rt, v >> 16))
        self.emit(i_type(0x0D, rt, rt, v & 0xFFFF))

    def reg(self):
        return self.r.choice(REGS)

    def simple(self):
        """One random instruction that does not transfer control (fit for a delay slot)."""
        r = self.r
        k = r.random()
        if k < 0.30:
            f = r.choice([0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x04, 0x06, 0x07])
            return r_type(f, self.reg(), self.reg(), self.reg())
        if k < 0.38:
            return r_type(r.choice([0x00, 0x02, 0x03]), 0, self.reg(), self.reg(), r.randrange(32))
        if k < 0.52:
            op = r.choice([0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F])
            return i_type(op, self.reg(), self.reg(), r.randrange(0x10000))
        if k < 0.58:
            if r.random() < 0.5:
                return r_type(r.choice([0x18, 0x19, 0x1A, 0x1B]), self.reg(), self.reg())
            return r_type(r.choice([0x10, 0x12]), 0, 0, self.reg()) if r.random() < 0.7 else \
                r_type(r.choice([0x11, 0x13]), self.reg())
        if k < 0.76:  # loads off r28
            op = r.choice([0x20, 0x24, 0x21, 0x25, 0x23, 0x22, 0x26])
            off = r.randrange(0, 1024)
            if op in (0x21, 0x25):
                off &= ~1
            elif op == 0x23:
                off &= ~3
            return i_type(op, GP, self.reg(), off)
        if k < 0.88:  # stores off r28
            op = r.choice([0x28, 0x29, 0x2B, 0x2A, 0x2E])
            off = r.randrange(0, 1024)
            if op == 0x29:
                off &= ~1
            elif op == 0x2B:
                off &= ~3
            return i_type(op, GP, self.reg(), off)
        # GTE
        g = r.random()
        if g < 0.3:
            return (0x12 << 26) | (4 << 21) | (self.reg() << 16) | (r.randrange(32) << 11)  # mtc2
        if g < 0.45:
            return (0x12 << 26) | (6 << 21) | (self.reg() << 16) | (r.randrange(32) << 11)  # ctc2
        if g < 0.6:
            return (0x12 << 26) | (0 << 21) | (self.reg() << 16) | (r.randrange(32) << 11)  # mfc2
        if g < 0.7:
            return (0x12 << 26) | (2 << 21) | (self.reg() << 16) | (r.randrange(32) << 11)  # cfc2
        if g < 0.8:
            return i_type(r.choice([0x32, 0x3A]), GP, r.randrange(32), r.randrange(0, 256) * 4)  # lwc2 / swc2
        return (0x12 << 26) | (1 << 25) | r.choice(GTE_CMDS)

    def block(self, subs):
        """Straight-line code with forward branches inside it."""
        r = self.r
        n = r.randrange(12, 40)
        start = len(self.code)
        pending = []  # (index of the branch, target index)
        i = 0
        while i < n:
            k = r.random()
            if k < 0.12 and i + 3 < n:
                # forward conditional branch over 1..6 instructions, random delay slot
                skip = r.randrange(1, 7)
                op = r.choice(["beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"])
                if op in ("beq", "bne"):
                    w = i_type(4 if op == "beq" else 5, self.reg(), self.reg(), skip + 1)
                elif op in ("blez", "bgtz"):
                    w = i_type(6 if op == "blez" else 7, self.reg(), 0, skip + 1)
                else:
                    rt = {"bltz": 0, "bgez": 1, "bltzal": 0x10, "bgezal": 0x11}[op]
                    w = i_type(1, self.reg(), rt, skip + 1)
                self.emit(w)
                self.emit(self.simple())
                i += 2
                for _ in range(skip):
                    self.emit(self.simple())
                    i += 1
                continue
            if k < 0.16 and subs:
                # call a leaf subroutine: JAL, or JALR through a register, random delay slot
                target = r.choice(subs)
                if r.random() < 0.6:
                    self.fixups.append((len(self.code), "jal", target))
                    self.emit(0)
                else:
                    rr = self.reg()
                    self.fixups.append((len(self.code), "li_hi", (target, rr)))
                    self.emit(0)
                    self.fixups.append((len(self.code), "li_lo", (target, rr)))
                    self.emit(0)
                    self.emit(r_type(0x09, rr, 0, RA))
                self.emit(self.simple())
                i += 3
                continue
            self.emit(self.simple())
            i += 1
        return start

    def program(self, timer_target):
        r = self.r
        # handler words for 0x80000080 (interpreted: below 64 KB): ack every IRQ, count it, return to EPC
        handler = [
            i_type(0x0F, 0, K0, 0x1F80),          # lui k0, 0x1F80
            i_type(0x2B, K0, 0, 0x1070),          # sw zero, 0x1070(k0)   I_STAT: acknowledge all
            i_type(0x23, K0, K1, 0x0000),         # lw k1, 0(k0)          scratchpad counter
            NOP,
            i_type(0x09, K1, K1, 1),              # addiu k1, k1, 1
            i_type(0x2B, K0, K1, 0x0000),         # sw k1, 0(k0)
            (0x10 << 26) | (0 << 21) | (K0 << 16) | (14 << 11),  # mfc0 k0, EPC
            NOP,
            r_type(0x08, K0),                     # jr k0
            (0x10 << 26) | (1 << 25) | 0x10,      # rfe
        ]
        # install it
        self.li(1, 0x80000080)
        for k, w in enumerate(handler):
            self.li(2, w)
            self.emit(i_type(0x2B, 1, 2, 4 * k))
        # timer 2: sysclk, IRQ at target, repeat, reset at target; I_MASK = timer 2; SR = IEc | IM2 | CU2
        self.li(1, 0x1F801100)
        self.li(2, timer_target)
        self.emit(i_type(0x2B, 1, 2, 0x28))   # T2 target
        self.li(2, 0x0058)
        self.emit(i_type(0x2B, 1, 2, 0x24))   # T2 mode
        self.emit(i_type(0x2B, 1, 0, 0x20))   # T2 counter = 0
        self.li(2, 1 << 6)
        self.emit(i_type(0x2B, 1, 2, 0x74 - 0x100))  # I_MASK (0x1F801074)
        self.li(2, 0x40000401)
        self.emit((0x10 << 26) | (4 << 21) | (2 << 16) | (12 << 11))  # mtc0 v0, SR
        self.li(GP, DATA)
        for reg in REGS:
            self.li(reg, r.randrange(1 << 32))
        loop = len(self.code)
        nblocks = max(1, self.n // 25)
        # leaf subroutines are laid out after the loop; their addresses are fixed up
        sub_labels = ["sub%d" % k for k in range(3)]
        for _ in range(nblocks):
            self.block(sub_labels)
        self.emit(j_type(2, BASE + 4 * loop))
        self.emit(NOP)
        for name in sub_labels:
            self.labels[name] = self.pc()
            for _ in range(r.randrange(2, 8)):
                self.emit(self.simple())
            self.emit(r_type(0x08, RA))  # jr ra
            self.emit(self.simple())
        for idx, kind, arg in self.fixups:
            if kind == "jal":
                self.code[idx] = j_type(3, self.labels[arg])
            elif kind == "li_hi":
                self.code[idx] = i_type(0x0F, 0, arg[1], self.labels[arg[0]] >> 16)
            else:
                self.code[idx] = i_type(0x0D, arg[1], arg[1], self.labels[arg[0]] & 0xFFFF)
        return b"".join(struct.pack("<I", w) for w in self.code)


def make_exe(text):
    text += bytes((-len(text)) % 2048)
    head = bytearray(2048)
    head[0:8] = b"PS-X EXE"
    struct.pack_into("<10I", head, 0x10, BASE, 0, BASE, len(text), 0, 0, 0, 0, 0x801FFF00, 0)
    return bytes(head) + text


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ps1-run", required=True)
    ap.add_argument("--bios", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--seeds", default="0-5")
    ap.add_argument("--frames", type=int, default=40)  # the BIOS takes ~16 frames before the EXE starts
    ap.add_argument("--insns", type=int, default=400)
    args = ap.parse_args(argv)
    if not os.path.exists(args.bios):
        print("skipped: BIOS %s not built" % args.bios)
        return 77
    if not shutil.which(os.environ.get("CC", "cc")):
        print("skipped: no C compiler")
        return 77
    lo, _, hi = args.seeds.partition("-")
    seeds = range(int(lo), int(hi or lo) + 1)
    os.makedirs(args.work, exist_ok=True)
    for seed in seeds:
        g = Gen(seed, args.insns)
        target = random.Random(seed * 7 + 1).choice([37, 100, 211, 450, 997, 2000])
        exe = os.path.join(args.work, "fuzz%d.exe" % seed)
        with open(exe, "wb") as f:
            f.write(make_exe(g.program(target)))
        work = os.path.join(args.work, "fuzz%d" % seed)
        shutil.rmtree(work, ignore_errors=True)
        run_args = "--bios %s" % args.bios
        r = subprocess.run([sys.executable, BUILD, exe, work, "--count", "--loop", "3", "--ps1-run", args.ps1_run,
                            "--frames", str(args.frames), "--run-args", run_args],
                           capture_output=True, text=True, check=False, timeout=900)
        if r.returncode != 0:
            print(r.stdout + r.stderr)
            print("seed %d: the pack did not build / converge" % seed)
            return 1
        r = subprocess.run([args.ps1_run, exe, "--bios", args.bios, "--frames", str(args.frames), "--quiet", "--pack",
                            os.path.join(work, "libtitle.so"), "--lockstep", "--stats"],
                           capture_output=True, text=True, check=False, timeout=900)
        out = r.stdout.strip().replace("\n", " | ")
        print("seed %d (timer target %d): %s" % (seed, target, out))
        if "LOCKSTEP OK" not in r.stdout:
            return 1
    print("all seeds identical to the interpreter")
    return 0


if __name__ == "__main__":
    sys.exit(main())
