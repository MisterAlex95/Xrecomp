"""A table-named function the linear sweep stepped over by one byte.

Burnout 3's XPP section starts with its class-driver tables. The sweep came
out of the zeros one byte early and decoded `00 55 8B` (add [ebp-0x75], dl)
at 0x00363063, so the XID class driver's Init at 0x00363064 -- `push ebp;
mov ebp, esp`, named only by a table word -- was not an instruction boundary.
_pass_data_ptr_targets dropped it, the Init was never lifted and the pad
never attached. A prologue whose stream falls back into the sweep's is now
realigned; anything else off the boundaries is still dropped.
"""
import struct

from tools.disasm.engine import DisasmEngine
from tools.disasm.functions import FunctionDetector
from tools.disasm.labels import LabelManager
from tools.disasm.loader import BinaryImage, SectionInfo

BASE = 0x10000
TARGET = BASE + 0x23
# push ebp; mov ebp, esp; sub esp, 0x60; push esi; xor esi, esi;
# pop esi; mov esp, ebp; pop ebp; ret
INIT = bytes.fromhex("558bec83ec605633f65e8be55dc3")
# xor eax, eax; inc eax; ret -- no prologue
NO_PROLOGUE = bytes.fromhex("31c040c3")


def _section(body_at_target, before=b"\x00"):
    # +0    ret, padded: the entry point (a known function)
    # +16   the table: one word naming TARGET
    # +20   15 zero bytes: the sweep pairs them up and takes the last one
    #       with the first two bytes at TARGET
    # +0x23 the function nothing calls
    body = b"\xc3" + b"\xcc" * 15
    body += struct.pack("<I", TARGET) + b"\x00" * 14 + before
    assert len(body) == TARGET - BASE
    return body + body_at_target + b"\xcc" * 16


def _detect(text_bytes):
    text = SectionInfo('XPP', BASE, len(text_bytes), 0, len(text_bytes),
                       False, True, '')
    image = BinaryImage('synthetic', text_bytes, 0, 0x20000, BASE, 0, [text])
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    detector = FunctionDetector(engine, image, None, LabelManager())
    detector._pass_known_addresses()
    detector._build_functions([text])
    return engine, detector, text


def test_sweep_steps_over_the_target():
    engine, _, _ = _detect(_section(INIT))
    assert TARGET not in engine.instructions
    assert TARGET - 1 in engine.instructions        # add [ebp-0x75], dl
    assert TARGET + 3 in engine.instructions        # sub esp, 0x60: back in step


def test_prologue_off_the_sweep_is_realigned_and_named():
    engine, detector, text = _detect(_section(INIT))
    assert detector._pass_data_ptr_targets([text])
    assert TARGET in detector._alias_entries
    assert engine.instructions[TARGET].mnemonic == "push"
    assert TARGET + 1 in engine.instructions        # mov ebp, esp


def test_no_prologue_off_the_sweep_is_still_dropped():
    # The same layout, but the bytes at TARGET open with no prologue: the
    # sweep's stream stands, nothing is decoded, no alias.
    engine, detector, text = _detect(_section(b"\x00" + NO_PROLOGUE))
    assert TARGET not in engine.instructions
    before = set(engine.instructions)
    detector._pass_data_ptr_targets([text])
    assert TARGET not in detector._alias_entries
    assert set(engine.instructions) == before


def test_prologue_bytes_inside_an_instruction_are_not_realigned():
    # The byte before TARGET is code, not fill: the sweep's `8b 55 8b`
    # (mov edx, [ebp-0x75]) is a real instruction and `55` inside it only
    # reads as push ebp. Burnout 3 has such a word (0x00230012, the `53` of
    # `mov edx, [ebx+0xc]`).
    engine, detector, text = _detect(_section(INIT, before=b"\x8b"))
    assert TARGET not in engine.instructions
    before = set(engine.instructions)
    detector._pass_data_ptr_targets([text])
    assert TARGET not in detector._alias_entries
    assert set(engine.instructions) == before
