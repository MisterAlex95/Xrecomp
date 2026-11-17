"""block_flag_out_state is lift_basic_block's out-state, without the lift.

The translator's back-edge fixed point only wants each block's out flag
state, and used to get it from full lifts it then threw away (a fifth of
tools.recomp on a title-size image). The walker applies the same transition
(_flag_transition) and skips the same instructions lift_basic_block consumes
without it. This pins the two together over blocks that reach every arm of
the transition, from several incoming states.
"""
import pytest

pytest.importorskip("capstone")

from .disasm import BasicBlock, Disassembler, Operand   # noqa: E402
from .lifter import Lifter, block_flag_out_state, lift_basic_block  # noqa: E402

VA = 0x11000

BLOCKS = [
    "39c8 7405",                 # cmp eax, ecx ; je   (fused)
    "85c0 89d8 0f94c0",          # test ; mov ; sete
    "01c8 0f42c1",               # add ; cmovb
    "d3e0 7405",                 # shl eax, cl ; je
    "c1e000 7405",               # shl eax, 0 ; je
    "c1e004 d3e8 d3f8",          # shl 4 ; shr cl ; sar cl
    "85c0 d1c1 d3c8 7405",       # test ; rol 1 ; ror cl ; je
    "c1c120 7405",               # rol ecx, 0x20 ; je
    "f9 f5 f8 7205",             # stc ; cmc ; clc ; jb
    "9d 7405",                   # popfd ; je
    "f7f1 7405",                 # div ecx ; je
    "f00fc106 7405",             # lock xadd [esi], eax
    "f0ff0e 7505",               # lock dec [esi] ; jne
    "0fbce1 7405",               # bsf esp, ecx
    "0fa3c8 7205",               # bt eax, ecx ; jb
    "e800000000 7405",           # call ; je
    "9f 9e 7405",                # lahf ; sahf ; je
    "dfe9 7705",                 # fucomip ; ja
    "f3a6 7405",                 # repe cmpsb ; je
    "f3a4 7405",                 # rep movsb ; je
    "a6 7205",                   # cmpsb ; jb
    "e2fe",                      # loop
    "e3fe",                      # jecxz
    "0f31 7405",                 # rdtsc ; je
    "0fa2 7405",                 # cpuid (unknown to the tracker)
    "0fc2c100 7405",             # cmpps (SSE mask) ; je
]

INCOMING = [
    None,
    ("cmp", [Operand(type="reg", reg="eax"), Operand(type="reg", reg="ecx")]),
    ("eflags", []),
    ("__cf_only", []),
]


def _block(hexcode):
    code = bytes.fromhex(hexcode.replace(" ", ""))
    insns = list(Disassembler()._cfg_stream(code, 0, len(code), VA))
    assert sum(i.size for i in insns) == len(code), hexcode
    return BasicBlock(start=VA, instructions=insns)


@pytest.mark.parametrize("hexcode", BLOCKS)
@pytest.mark.parametrize("needs_cf", (False, True))
def test_the_walk_agrees_with_the_lift(hexcode, needs_cf):
    bb = _block(hexcode)
    for incoming in INCOMING:
        lifter = Lifter()
        lifter.needs_cf = needs_cf
        _, lifted = lift_basic_block(lifter, bb, flag_state=incoming)
        assert block_flag_out_state(bb, flag_state=incoming) == lifted, \
            (hexcode, incoming)
