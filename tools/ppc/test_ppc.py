"""Tests of the shared PowerPC frontend (XBA-225): decoder, discovery, lifter text. Synthetic code, no toolchain."""

import struct

from tools.ppc import asm
from tools.ppc.discover import Discovery
from tools.ppc.isa import BCLR, BCOND, BRANCH, INVALID, Decoder
from tools.ppc.lift import FnLifter, mask


class Img:
    def __init__(self, base, words):
        self.base = base
        self.data = struct.pack(">%dI" % len(words), *words)

    def in_code(self, a):
        return self.base <= a and a + 4 <= self.base + len(self.data)

    def u32(self, a):
        return struct.unpack_from(">I", self.data, a - self.base)[0]


def test_decode_known_words():
    d = Decoder()
    cases = {
        0x9421FFF0: ("stwu", "stwu r1, -16(r1)"),
        0x7C0802A6: ("mfspr", "mfspr r0, lr"),
        0x4E800020: ("bclr", "blr"),
        0x38600000: ("addi", "addi r3, r0, 0"),
        0x5463103A: ("rlwinm", "rlwinm r3, r3, 2, 0, 29"),
        0x7C632214: ("add", "add r3, r3, r4"),
        0xFC20082A: ("fadd", None),
        0xEC21102A: ("fadds", None),
        0x7C0004AC: ("sync", None),
        0x4C000064: ("rfi", None),
        0x44000002: ("sc", None),
    }
    for w, (name, text) in cases.items():
        i = d.decode(w, 0x80003100)
        assert i.name == name, hex(w)
        if text:
            assert i.text == text, (hex(w), i.text)


def test_opcode_4_is_left_to_the_layer():
    i = Decoder().decode(asm.ps_add(1, 2, 3), 0)
    assert i.kind == INVALID and i.name is None


def test_branch_targets_and_kinds():
    d = Decoder()
    i = d.decode(asm.b(0x80003100, 0x80003000), 0x80003100)
    assert i.kind == BRANCH and i.target == 0x80003000 and not i.lk
    i = d.decode(asm.bl(0x80003100, 0x80004000), 0x80003100)
    assert i.kind == BRANCH and i.lk and i.target == 0x80004000
    i = d.decode(asm.bdnz(0x80003110, 0x80003100), 0x80003110)
    assert i.kind == BCOND and i.target == 0x80003100 and i.rd == 16
    assert d.decode(asm.blr(), 0).kind == BCLR


def test_mask():
    assert mask(16, 23) == 0x0000FF00
    assert mask(0, 31) == 0xFFFFFFFF
    assert mask(30, 1) == 0xC0000003


def test_discovery_follows_calls_and_loops():
    base = 0x80003100
    f2 = base + 0x20
    words = [
        asm.li(3, 10),                  # 00
        asm.mtctr(3),                   # 04
        asm.addi(4, 4, 1),              # 08  loop
        asm.bdnz(base + 0x0C, base + 8),  # 0C
        asm.bl(base + 0x10, f2),        # 10
        asm.b(base + 0x14, base + 0x14),  # 14 idle
        0, 0,                           # 18 1C: never reached
        asm.mulli(3, 3, 3),             # 20  f2
        asm.blr(),                      # 24
    ]
    img = Img(base, words)
    disc = Discovery(img, Decoder(), [base])
    funcs = disc.run()
    assert [f.entry for f in funcs] == [base, f2]
    assert funcs[0].addrs == {base + 4 * k for k in range(6)}
    assert funcs[1].addrs == {f2, f2 + 4}
    lifter = FnLifter(img, disc.insn, funcs[0], "f")
    text = lifter.function()
    assert lifter.leaders == {base, base + 8, base + 0x10, base + 0x14}
    assert "c->ctr--; if (c->ctr != 0) { goto L_80003108; }" in text
    assert "c->lr = 0x80003114u; RC_EXIT(0x80003120u);" in text
    assert "RC_IDLE(0x80003114u);" in text
    assert "RC_BLOCK(0x80003108u, 2);" in text


def test_interpreter_hand_offs_are_blocks_of_their_own():
    base = 0x80003100
    words = [asm.li(3, 1), 0x4C000064, asm.li(3, 2), asm.blr()]  # rfi in the middle: path ends there
    img = Img(base, words)
    disc = Discovery(img, Decoder(), [base])
    f = disc.run()[0]
    assert f.addrs == {base, base + 4}
    lifter = FnLifter(img, disc.insn, f, "f")
    text = lifter.function()
    assert "RC_INTERP(0x80003104u);" in text and lifter.fallbacks == 1
    assert base + 4 in lifter.leaders
