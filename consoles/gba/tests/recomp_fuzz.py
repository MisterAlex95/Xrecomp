#!/usr/bin/env python3
"""Differential fuzzing of the GBA lifter against the interpreter (XBA-151).

    recomp_fuzz.py --gba-run PATH --work DIR [--seeds 0-7] [--frames 20] [--insns 160]

For each seed: a synthetic ROM of random ARM and Thumb instructions (every data-processing form, shifts by
immediate and by register, multiplies, loads and stores of every width, LDM / STM, PUSH / POP, conditional forward
branches, MRS, MSR flags, ARM <-> Thumb switches) in an endless loop, with a fast timer IRQ so interrupts land at
any instruction of the lifted code. It is lifted (gba_tools.recomp), built (tools/consoles/gba_recomp_build.py)
and run by gba_run --pack --lockstep: the interpreter and the recompiled machine must agree on the whole state
(registers, cycles, memory, frame) after every frame. Exit 0 when every seed agrees, 1 at the first difference,
77 when Python's or a C compiler is missing (skipped).
"""

import argparse
import os
import random
import shutil
import struct
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
BUILD = os.path.join(REPO, "tools", "consoles", "gba_recomp_build.py")

LOW = [0, 1, 2, 3, 4, 5]                 # Thumb destinations (r6 / r7 hold the bases)
ARM_D = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14]  # ARM destinations (r12 = base, r13 = sp)


class Asm:
    def __init__(self):
        self.words = {}  # offset -> (bytes)
        self.pc = 0

    def emit32(self, w):
        self.words[self.pc] = struct.pack("<I", w & 0xFFFFFFFF)
        self.pc += 4

    def emit16(self, h):
        self.words[self.pc] = struct.pack("<H", h & 0xFFFF)
        self.pc += 2

    def align4(self):
        if self.pc & 2:
            self.emit16(0x46C0)  # nop (mov r8, r8)

    def image(self):
        size = max(k + len(v) for k, v in self.words.items())
        buf = bytearray(size)
        for k, v in self.words.items():
            buf[k:k + len(v)] = v
        return bytes(buf)


def arm_const(a, rd, value):
    """ldr rd, [pc] ; b over ; .word value"""
    a.emit32(0xE59F0000 | rd << 12)
    a.emit32(0xEA000000)
    a.emit32(value)


def rand_arm(r, a):
    cond = 0xE if r.random() < 0.6 else r.randrange(15)
    k = r.random()
    if k < 0.45:  # data processing
        opc = r.randrange(16)
        s = 1 if 8 <= opc <= 11 else r.randrange(2)
        rn = r.choice(ARM_D + [12, 13, 15])
        rd = 0 if 8 <= opc <= 11 else r.choice(ARM_D)
        f = r.randrange(3)
        if f == 0:
            op2 = 1 << 25 | r.randrange(16) << 8 | r.randrange(256)
        elif f == 1:
            op2 = r.randrange(32) << 7 | r.randrange(4) << 5 | r.choice(ARM_D + [15])
        else:
            op2 = r.choice(ARM_D) << 8 | r.randrange(4) << 5 | 1 << 4 | r.choice(ARM_D + [15])
        a.emit32(cond << 28 | opc << 21 | s << 20 | rn << 16 | rd << 12 | op2)
    elif k < 0.52:  # multiply
        rd, rn, rs, rm = (r.choice(ARM_D) for _ in range(4))
        a.emit32(cond << 28 | r.randrange(2) << 21 | r.randrange(2) << 20 | rd << 16 | rn << 12 | rs << 8 | 0x90 | rm)
    elif k < 0.57:  # long multiply
        hi, lo = r.sample(ARM_D, 2)
        rs, rm = r.choice(ARM_D), r.choice(ARM_D)
        a.emit32(cond << 28 | 1 << 23 | r.randrange(2) << 22 | r.randrange(2) << 21 | r.randrange(2) << 20 | hi << 16
                 | lo << 12 | rs << 8 | 0x90 | rm)
    elif k < 0.72:  # LDR / STR / LDRB / STRB [r12, #+-imm]
        a.emit32(cond << 28 | 1 << 26 | 1 << 24 | r.randrange(2) << 23 | r.randrange(2) << 22 | r.randrange(2) << 20
                 | 12 << 16 | r.choice(ARM_D) << 12 | r.randrange(256))
    elif k < 0.80:  # halfword / signed transfers
        imm = r.randrange(256)
        sh = r.choice([1, 1, 2, 3])
        a.emit32(cond << 28 | 1 << 24 | r.randrange(2) << 23 | 1 << 22 | r.randrange(2) << 20 | 12 << 16
                 | r.choice(ARM_D) << 12 | (imm >> 4) << 8 | 1 << 7 | sh << 5 | 1 << 4 | (imm & 15))
    elif k < 0.86:  # LDM / STM r12 (no write-back)
        lst = 0
        while not lst:
            lst = sum(1 << x for x in ARM_D if r.random() < 0.3)
        a.emit32(cond << 28 | 4 << 25 | r.randrange(2) << 24 | r.randrange(2) << 23 | r.randrange(2) << 20 | 12 << 16
                 | lst)
    elif k < 0.92:  # conditional forward branch over 1..3 instructions
        skip = r.randrange(1, 4)
        a.emit32(cond << 28 | 0xA << 24 | (skip - 1))
        for _ in range(skip):
            rand_arm_simple(r, a)
    elif k < 0.96:  # MRS
        a.emit32(cond << 28 | 0x010F0000 | r.choice(ARM_D) << 12)
    else:  # MSR cpsr_f, #imm (the interpreter runs it)
        a.emit32(cond << 28 | 0x0328F000 | 4 << 8 | r.randrange(16) << 4)


def rand_arm_simple(r, a):
    cond = r.randrange(15)
    opc = r.choice([0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15])
    a.emit32(cond << 28 | 1 << 25 | opc << 21 | r.randrange(2) << 20 | r.choice(ARM_D) << 16
             | r.choice(ARM_D) << 12 | r.randrange(16) << 8 | r.randrange(256))


def rand_thumb(r, a, pushed):
    k = r.random()
    d = r.choice(LOW)
    if k < 0.12:
        a.emit16(r.randrange(3) << 11 | r.randrange(32) << 6 | r.randrange(8) << 3 | d)
    elif k < 0.22:
        a.emit16(0x1800 | r.randrange(4) << 9 | r.randrange(8) << 6 | r.randrange(8) << 3 | d)
    elif k < 0.32:
        a.emit16((4 + r.randrange(4)) << 11 | d << 8 | r.randrange(256))
    elif k < 0.50:
        a.emit16(0x4000 | r.randrange(16) << 6 | r.randrange(8) << 3 | d)
    elif k < 0.57:  # hi register ADD / CMP / MOV (no write to sp, r12 or pc)
        op = r.randrange(3)
        dst = r.choice(LOW + [8, 9, 10, 11, 14]) if op != 1 else r.randrange(16)
        src = r.randrange(16)
        a.emit16(0x4400 | op << 8 | (dst >> 3) << 7 | src << 3 | (dst & 7))
    elif k < 0.67:  # LDR / STR / LDRB / STRB [r7, #imm5]
        a.emit16(0x6000 | r.randrange(4) << 11 | r.randrange(32) << 6 | 7 << 3 | d)
    elif k < 0.72:  # LDRH / STRH [r7, #imm5 * 2]
        a.emit16(0x8000 | r.randrange(2) << 11 | r.randrange(32) << 6 | 7 << 3 | d)
    elif k < 0.79:  # register offset [r7, r6], every kind
        a.emit16(0x5000 | r.randrange(8) << 9 | 6 << 6 | 7 << 3 | d)
    elif k < 0.84:  # [sp, #imm8 * 4]
        a.emit16(0x9000 | r.randrange(2) << 11 | d << 8 | r.randrange(64))
    elif k < 0.87:  # ldr rd, [pc, #0] ; b over ; .word random (a literal pool, as compilers lay them out)
        a.align4()
        a.emit16(0x4800 | d << 8)
        a.emit16(0xE001)
        a.emit16(r.getrandbits(16))
        a.emit16(r.getrandbits(16))
    elif k < 0.90:  # add rd, pc / sp, #imm
        a.emit16((0xA000 if r.random() < 0.5 else 0xA800) | d << 8 | r.randrange(256))
    elif k < 0.95:  # push, popped later
        lst = r.randrange(1, 64)
        a.emit16(0xB400 | lst)  # (no LR: POP {..., PC} would leave the loop)
        pushed.append(lst)
    else:  # conditional forward branch over 1..2 instructions
        skip = r.randrange(1, 3)
        a.emit16(0xD000 | r.randrange(14) << 8 | (skip - 1))
        for _ in range(skip):
            a.emit16(0x4000 | r.randrange(16) << 6 | r.randrange(8) << 3 | r.choice(LOW))


def make_rom(seed, n, irq=True):
    r = random.Random(seed)
    a = Asm()
    a.emit32(0xEA00002E)  # b 0x080000C0
    a.pc = 0xA0
    for b in b"XBAFUZZ\0\0\0\0\0XFUZ00":
        a.words[a.pc] = bytes([b])
        a.pc += 1
    a.pc = 0xC0
    handler_fix = a.pc
    arm_const(a, 12, 0x03000100)
    arm_const(a, 13, 0x03000600)
    arm_const(a, 0, 0x03007FFC)
    arm_const(a, 1, 0)  # patched below: the IRQ handler
    a.emit32(0xE5801000)  # str r1, [r0]
    arm_const(a, 0, 0x04000100)
    arm_const(a, 1, 0x00C0FF00 + r.randrange(0xC0))  # TM0: reload 0xFFxx, enable + IRQ, prescaler 1
    a.emit32(0xE5801000)  # str r1, [r0]
    arm_const(a, 0, 0x04000200)
    arm_const(a, 1, 0x00000008)  # IE = timer 0
    a.emit32(0xE1C010B0)  # strh r1, [r0]
    a.emit32(0xE3A01001)  # mov r1, #1
    a.emit32(0xE1C010B8 if irq else 0xE1A00000)  # strh r1, [r0, #8] (IME), or a nop: no interrupts
    for reg in ARM_D:
        arm_const(a, reg, r.getrandbits(32))
    loop = a.pc
    for _ in range(n):
        rand_arm(r, a)
    # to Thumb: add r11, pc, #1 ; bx r11  (lands right after)
    a.emit32(0xE28FB001)
    a.emit32(0xE12FFF1B)
    a.emit16(0x2603)  # movs r6, #3
    a.emit16(0x0637)  # lsls r7, r6, #24
    a.emit16(0x37FF)  # adds r7, #255
    a.emit16(0x3701)  # adds r7, #1   -> 0x03000100
    a.emit16(0x2640)  # movs r6, #0x40
    pushed = []
    for _ in range(n):
        rand_thumb(r, a, pushed)
    for lst in reversed(pushed):
        a.emit16(0xBC00 | lst)  # pop {list}
    a.align4()
    a.emit16(0x4778)  # bx pc
    a.emit16(0x46C0)
    off = (loop - (a.pc + 8)) // 4
    a.emit32(0xEA000000 | (off & 0xFFFFFF))  # b loop
    handler = a.pc
    a.emit32(0xE3A00301)  # mov r0, #0x04000000
    a.emit32(0xE2800C02)  # add r0, r0, #0x200
    a.emit32(0xE3A01008)  # mov r1, #8
    a.emit32(0xE1C010B2)  # strh r1, [r0, #2] (IF: acknowledge timer 0)
    a.emit32(0xE12FFF1E)  # bx lr
    img = bytearray(a.image())
    struct.pack_into("<I", img, handler_fix + 3 * 12 + 8, 0x08000000 + handler)
    return bytes(img)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--gba-run", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--seeds", default="0-7")
    ap.add_argument("--frames", type=int, default=20)
    ap.add_argument("--insns", type=int, default=160)
    args = ap.parse_args(argv)
    cc = os.environ.get("CC", "cc")
    if not shutil.which(cc.split()[0]):
        print("skipped: no C compiler (%s)" % cc)
        return 77
    lo, _, hi = args.seeds.partition("-")
    seeds = range(int(lo), int(hi or lo) + 1)
    os.makedirs(args.work, exist_ok=True)
    for seed in seeds:
        d = os.path.join(args.work, "seed%d" % seed)
        os.makedirs(d, exist_ok=True)
        rom = os.path.join(d, "fuzz.gba")
        with open(rom, "wb") as f:
            f.write(make_rom(seed, args.insns))
        # the discovery loop too: the IRQ handler is only known once a run met it
        b = subprocess.run([sys.executable, BUILD, rom, d, "--count", "--loop", "3", "--gba-run", args.gba_run,
                            "--frames", "3"], capture_output=True, text=True, check=False)
        if b.returncode != 0:
            print("seed %d: pack build failed\n%s%s" % (seed, b.stdout, b.stderr))
            return 1
        r = subprocess.run([args.gba_run, rom, "--quiet", "--frames", str(args.frames), "--pack",
                            os.path.join(d, "libtitle.so"), "--lockstep", "--stats"],
                           capture_output=True, text=True, check=False, timeout=600)
        out = (r.stdout + r.stderr).strip()
        print("seed %d: %s" % (seed, out.replace("\n", " | ")))
        if r.returncode != 0 or "LOCKSTEP OK" not in out:
            print("seed %d: DIFFERS (ROM kept in %s)" % (seed, rom))
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
