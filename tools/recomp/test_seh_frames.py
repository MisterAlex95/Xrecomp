"""Exception frames and what they let a handler reach (XBA-112).

seh_frames.analyze_seh_frame reads the frame a function registers and the
compiler's tables behind it. Checked here on hand-built instruction lists and
tables, for the three registration shapes MSVC 7.x emits (inline EH3,
__SEH_prolog, C++), and end to end on the synthetic title in
tests/seh_dispatch: the generated fixture must be what the lifter produces
today, so the CMake test (x86-64 and arm64) runs current code.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Instruction, Operand  # noqa: E402
from tools.recomp.seh_frames import analyze_seh_frame  # noqa: E402

START, END = 0x00011000, 0x00011100
SCOPE, HANDLER3, FUNCINFO, STUB = 0x00020000, 0x00012000, 0x00020100, 0x00012100
PROLOG = 0x00013000


def _insn(addr, mnemonic, *ops, call_target=None):
    i = Instruction(addr, 1, mnemonic, "", "")
    i.operands = list(ops)
    i.call_target = call_target
    return i


def imm(v):
    return Operand(type="imm", imm=v)


def reg(r):
    return Operand(type="reg", reg=r)


def fs0():
    return Operand(type="mem", mem_seg="fs", mem_disp=0, mem_size=4)


def ebp_m4():
    return Operand(type="mem", mem_base="ebp", mem_disp=-4, mem_size=4)


def _reader(words):
    def read(va):
        return words.get(va)
    return read


def _table(base, *dwords):
    return {base + 4 * k: v & 0xFFFFFFFF for k, v in enumerate(dwords)}


def _code(va):
    return 0x00010000 <= va < 0x00014000


def test_inline_eh3_frame_gives_filters_finally_and_except_blocks():
    insns = [
        _insn(START, "push", reg("ebp")),
        _insn(START + 1, "push", imm(-1)),
        _insn(START + 2, "push", imm(SCOPE)),
        _insn(START + 3, "push", imm(HANDLER3)),
        _insn(START + 4, "mov", reg("eax"), fs0()),
        _insn(START + 5, "push", reg("eax")),
        _insn(START + 6, "mov", fs0(), reg("esp")),
        _insn(START + 7, "mov", ebp_m4(), imm(0)),
        _insn(START + 8, "mov", ebp_m4(), imm(1)),
    ]
    words = _table(SCOPE, -1, START + 0x40, START + 0x30,   # 0: __except
                   0, 0, START + 0x50)                       # 1: __finally
    words[SCOPE + 24] = 0x12345678                           # past the table
    f = analyze_seh_frame(insns, START, END, _reader(words), is_code=_code)
    assert f.kind == "eh3" and f.table == SCOPE
    assert f.anchor_after == START + 6
    assert f.funclets == {START + 0x40, START + 0x50}
    assert f.resume == [START + 0x30]


def test_seh_prolog_frame_reads_the_scope_table_pushed_before_the_call():
    insns = [
        _insn(START, "push", imm(0x20)),
        _insn(START + 1, "push", imm(SCOPE)),
        _insn(START + 2, "call", imm(PROLOG), call_target=PROLOG),
        _insn(START + 3, "mov", ebp_m4(), imm(0)),
    ]
    words = _table(SCOPE, -1, START + 0x40, START + 0x30)
    f = analyze_seh_frame(insns, START, END, _reader(words), {PROLOG},
                          is_code=_code)
    assert f.kind == "eh3" and f.anchor_after == START + 2
    assert f.resume == [START + 0x30] and f.funclets == {START + 0x40}


def test_scope_table_stops_at_an_entry_that_is_not_one():
    insns = [_insn(START, "push", imm(-1)), _insn(START + 1, "push", imm(SCOPE)),
             _insn(START + 2, "push", imm(HANDLER3)),
             _insn(START + 3, "mov", fs0(), reg("esp"))]
    # No try-level stores: read until an entry fails (enclosing >= index).
    words = _table(SCOPE, -1, START + 0x40, START + 0x30, 5, 0, START + 0x50)
    f = analyze_seh_frame(insns, START, END, _reader(words), is_code=_code)
    assert f.funclets == {START + 0x40} and f.resume == [START + 0x30]


def _prolog_frame(*level_stores):
    return [_insn(START, "push", imm(0x20)), _insn(START + 1, "push", imm(SCOPE)),
            _insn(START + 2, "call", imm(PROLOG), call_target=PROLOG)] + [
        _insn(START + 3 + k, *ops) for k, ops in enumerate(level_stores)]


# The next function's scope table follows this one's in .rdata; its first
# entry starts at enclosing level -1 as well, so only the try levels this
# function enters bound its own (XBA-139).
_NEIGHBOURS = _table(SCOPE, -1, START + 0x40, START + 0x30,
                     -1, 0x00013540, 0x00013530)


def test_and_ebp_minus4_zero_enters_level_zero():
    """MSVC's short form `and dword ptr [ebp-4], 0`: one entry, not the
    neighbour's too."""
    insns = _prolog_frame(("and", ebp_m4(), imm(0)),
                          ("or", ebp_m4(), imm(-1)))
    f = analyze_seh_frame(insns, START, END, _reader(_NEIGHBOURS), {PROLOG},
                          is_code=_code)
    assert f.funclets == {START + 0x40} and f.resume == [START + 0x30]
    assert f.outside == set()


def test_a_level_stored_from_a_zeroed_register():
    insns = _prolog_frame(("xor", reg("ebx"), reg("ebx")),
                          ("mov", ebp_m4(), reg("ebx")))
    f = analyze_seh_frame(insns, START, END, _reader(_NEIGHBOURS), {PROLOG},
                          is_code=_code)
    assert f.outside == set() and f.funclets == {START + 0x40}


def test_a_register_overwritten_is_not_known():
    """`mov [ebp-4], ebx` after ebx changed says nothing: with no other
    store the table is read as before (until an entry fails)."""
    insns = _prolog_frame(("xor", reg("ebx"), reg("ebx")),
                          ("mov", reg("ebx"), Operand(type="mem", mem_base="ebp",
                                                      mem_disp=8)),
                          ("mov", ebp_m4(), reg("ebx")))
    f = analyze_seh_frame(insns, START, END, _reader(_NEIGHBOURS), {PROLOG},
                          is_code=_code)
    assert f.outside == {0x00013540, 0x00013530}


def test_cxx_frame_gives_unwind_actions_catches_and_continuations():
    catch = START + 0x60
    insns = [
        _insn(START, "push", imm(-1)),
        _insn(START + 1, "push", imm(STUB)),
        _insn(START + 2, "mov", reg("eax"), fs0()),
        _insn(START + 3, "push", reg("eax")),
        _insn(START + 4, "mov", fs0(), reg("esp")),
        _insn(catch, "mov", reg("eax"), imm(START + 0x70)),
        _insn(catch + 1, "ret"),
    ]
    stub = b"\xB8" + struct.pack("<I", FUNCINFO) + b"\xE9\x00\x00\x00\x00\x90\x90"
    words = {STUB + k: struct.unpack_from("<I", stub, k)[0] for k in (0, 4)}
    unwind, tries, handlers = FUNCINFO + 0x40, FUNCINFO + 0x60, FUNCINFO + 0x80
    words.update(_table(FUNCINFO, 0x19930520, 2, unwind, 1, tries, 0, 0))
    words.update(_table(unwind, -1, START + 0x50, 0, 0))
    words.update(_table(tries, 1, 1, 2, 1, handlers))
    words.update(_table(handlers, 0, 0, 0, catch))
    f = analyze_seh_frame(insns, START, END, _reader(words), is_code=_code)
    assert f.kind == "cxx" and f.table == FUNCINFO
    assert f.funclets == {START + 0x50, catch}
    assert f.resume == [START + 0x70]


def test_no_frame_no_answer():
    insns = [_insn(START, "push", reg("ebp")), _insn(START + 1, "ret")]
    assert analyze_seh_frame(insns, START, END, _reader({})) is None


def test_unlinking_a_frame_is_not_registering_one():
    insns = [_insn(START, "mov", reg("ecx"), Operand(type="mem", mem_base="ebp",
                                                    mem_disp=-0x10)),
             _insn(START + 1, "mov", fs0(), reg("ecx"))]
    assert analyze_seh_frame(insns, START, END, _reader({})) is None


def test_targets_outside_the_function_are_set_apart():
    insns = [_insn(START, "push", imm(-1)), _insn(START + 1, "push", imm(SCOPE)),
             _insn(START + 2, "push", imm(HANDLER3)),
             _insn(START + 3, "mov", fs0(), reg("esp")),
             _insn(START + 4, "mov", ebp_m4(), imm(0))]
    words = _table(SCOPE, -1, 0x00013500, START + 0x30)
    f = analyze_seh_frame(insns, START, END, _reader(words), is_code=_code)
    assert f.outside == {0x00013500} and f.funclets == set()
    assert f.resume == [START + 0x30]


# ---- the synthetic title ---------------------------------------------------

def test_generated_fixture_is_current():
    from tools.recomp import seh_fixture
    with open(seh_fixture.GENERATED_C, encoding="utf-8") as f:
        assert f.read() == seh_fixture.lift(), (
            "tests/seh_dispatch/fixture_gen.c is stale: "
            "python -m tools.recomp.seh_fixture write")


def test_fixture_json_matches_its_source():
    if not all(shutil.which(t) for t in ("as", "ld", "objcopy", "nm")):
        pytest.skip("binutils not on PATH")
    probe = subprocess.run(["ld", "-V"], capture_output=True, text=True)
    if "elf_i386" not in probe.stdout:
        pytest.skip("ld has no elf_i386 emulation")
    from tools.recomp import seh_fixture
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "f.json")
        assert seh_fixture.build(out=out) == seh_fixture.load(), (
            "fixture.json is stale: python -m tools.recomp.seh_fixture build")


def test_fixture_frames_anchor_only_where_there_is_somewhere_to_resume():
    from tools.recomp import seh_fixture
    code = seh_fixture.lift()
    # f_outer, f_div and f_probe (__except) and f_cxx (catch) anchor;
    # f_inner (__finally only) and f_cxx_inner (destructor only) do not.
    assert code.count("recomp_seh_anchor(") == 8   # each anchored body twice
    for name in ("f_outer__seh", "f_inner__seh", "f_cxx__seh",
                 "f_cxx_inner__seh", "f_div__seh", "f_probe__seh"):
        assert f"void {name}(uint32_t _seh_target)" in code
    # The __finally a normal path calls directly is the funclet entry.
    assert "void sub_00010133(void) { f_inner__seh(0x00010133u); }" in code


def test_no_seh_anchors_keeps_the_funclets_and_drops_the_setjmp():
    from tools.recomp import seh_fixture
    code = seh_fixture.lift(seh_anchors=False)
    assert "recomp_seh_anchor(" not in code and "RECOMP_SETJMP" not in code
    assert "void sub_00010133(void) { f_inner__seh(0x00010133u); }" in code


def test_the_seh_copy_stays_when_its_first_funclet_is_already_a_function():
    from tools.recomp.translator import place_funclet_entries
    proto = "void sub_00001000__seh(uint32_t _seh_target);"
    copy = f"/* copy */\n{proto}\nvoid sub_00001000__seh(uint32_t _seh_target) {{ }}\n"
    first = (0x1100, "sub_00001100",
             copy + "\n" + proto + "\nvoid sub_00001100(void) { sub_00001000__seh(0x00001100u); }\n")
    second = (0x1200, "sub_00001200",
              proto + "\nvoid sub_00001200(void) { sub_00001000__seh(0x00001200u); }\n")
    entries = [(0x1000, "sub_00001000", first), (0x1000, "sub_00001000", second)]

    # Nothing in the way: both entries are added, the copy with the first.
    plain = [(0x1000, "sub_00001000", "void sub_00001000(void) { }\n")]
    place_funclet_entries(plain, entries)
    assert [a for a, _, _ in plain] == [0x1000, 0x1100, 0x1200]
    assert "".join(c for _, _, c in plain).count("__seh(uint32_t _seh_target) {") == 1

    # The first funclet is a detected function: its entry goes, the copy stays.
    split = [(0x1000, "sub_00001000", "void sub_00001000(void) { }\n"),
             (0x1100, "sub_00001100", "void sub_00001100(void) { /* own */ }\n")]
    place_funclet_entries(split, entries)
    assert [a for a, _, _ in split] == [0x1000, 0x1100, 0x1200]
    code = "".join(c for _, _, c in split)
    assert code.count("__seh(uint32_t _seh_target) {") == 1
    assert code.count("void sub_00001100(void) {") == 1
    assert "__seh(uint32_t _seh_target) {" in split[0][2]
