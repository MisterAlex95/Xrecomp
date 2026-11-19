"""Stack slots without volatile and without the g_cpu barrier (XBA-307).

A function that keeps its stack to itself gets STK forms for [esp+N],
[frame ebp+N] and push/pop; one whose stack address leaves as data, or that
loads esp from elsewhere, keeps the MEM forms. Every function gets
RECOMP_STK_FENCE() at its entry and before its returns (not after a tail
call). The last test lifts the bench corpus and checks that the STK build
computes what the MEM build computes (RECOMP_STK_FAST=0), at -O2.
"""
import json
import os
import shutil
import subprocess
import sys

import pytest

from . import config, stack_fast
from .stack_fast import classify_address, rewrite, rewrite_text
from .translator import FunctionTranslator

BASE = 0x10000


@pytest.fixture(autouse=True)
def restore_layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", list(config._SECTIONS))


def _translate(hexbytes):
    image = bytes.fromhex(hexbytes)
    config._install([config.Section(".text", BASE, 0x100, 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin="stack-fast-test")
    db = {BASE: {"start": hex(BASE), "end": BASE + len(image), "_addr": BASE,
                 "size": len(image), "name": f"sub_{BASE:08X}"}}
    t = FunctionTranslator(image.ljust(0x100, b"\xcc"), db)
    return t, t.translate_function(BASE, db[BASE])


def test_frame_function_gets_stk_forms_and_fences():
    # push ebp; mov ebp, esp; mov eax, [ebp+8]; mov [ebp-4], eax; pop ebp; ret
    t, code = _translate("55 8bec 8b4508 8945fc 5d c3")
    assert BASE in t.stack_fast_functions, t.stack_fast_skipped
    assert "STK_PUSH32(esp, ebp);" in code
    assert "eax = STK32(ebp + 8);" in code
    assert "STK32(ebp + -4) = eax;" in code
    assert "STK_POP32(esp, ebp);" in code
    assert "MEM32(" not in code and "PUSH32(esp" not in code.replace("STK_PUSH32(esp", "")
    body = code[code.index("{"):]
    assert body.splitlines()[1].strip().startswith("RECOMP_STK_FENCE();")
    assert "RECOMP_STK_FENCE(); return; /* ret */" in code


def test_frameless_ebp_stays_a_pointer():
    # mov eax, [ebp+4]; mov ecx, [esp+4]; ret -- ebp is not a frame here
    t, code = _translate("8b4504 8b4c2404 c3")
    assert BASE in t.stack_fast_functions
    assert "MEM32(ebp + 4)" in code
    assert "STK32(esp + 4)" in code


@pytest.mark.parametrize("name,hexbytes", [
    ("lea of a slot", "8d442404 c3"),                 # lea eax, [esp+4]
    ("esp as a value", "8bc4 c3"),                     # mov eax, esp
    ("push esp", "54 58 c3"),                          # push esp; pop eax
    ("SEH registration", "64892500000000 c3"),         # mov fs:[0], esp
    ("stack switch", "8be0 c3"),                       # mov esp, eax
    ("frame pointer escapes", "55 8bec 8d45fc 5d c3"),  # lea eax, [ebp-4]
    ("pushad", "60 61 c3"),
])
def test_escapes_keep_the_old_forms(name, hexbytes):
    t, code = _translate(hexbytes)
    assert BASE not in t.stack_fast_functions, name
    assert BASE in t.stack_fast_skipped
    assert "STK" not in code.replace("RECOMP_STK_FENCE", "")
    assert "RECOMP_STK_FENCE();" in code              # every function is fenced


def test_floats_and_doubles_go_through_words():
    # fld dword [esp+4]; fstp qword [esp+8]; fild dword [esp+4]; fistp qword [esp+8]; ret
    t, code = _translate("d9442404 dd5c2408 db442404 df7c2408 c3")
    assert BASE in t.stack_fast_functions, t.stack_fast_skipped
    assert "STKF(esp + 4)" in code
    assert "STKD_W(esp + 8, " in code
    assert "STKS32(esp + 4)" in code
    assert "STKS64_W(esp + 8, " in code
    assert "MEMF(" not in code and "MEMD(" not in code and "SMEM" not in code


def test_x87_environment_helpers_give_up():
    t, code = _translate("d93424 c3")                   # fnstenv [esp]
    assert BASE not in t.stack_fast_functions
    assert "passed to recomp_fnstenv" in t.stack_fast_skipped[BASE]


def test_frame_used_after_its_teardown_is_kept():
    # push ebp; mov ebp, esp; pop ebp; mov eax, [ebp+4]; ret
    t, code = _translate("55 8bec 5d 8b4504 c3")
    assert BASE not in t.stack_fast_functions
    assert "teardown" in t.stack_fast_skipped[BASE]


def test_frame_in_a_coalesced_body_is_kept():
    t, _ = _translate("55 8bec 8b4508 5d c3")
    instructions, _ = t.decode_function(BASE, BASE + 8)
    assert stack_fast.eligible(instructions)[0]
    ok, _, why = stack_fast.eligible(instructions, single_body=False)
    assert not ok and "coalesced" in why


def test_tail_call_stays_a_sibling_call():
    lines = stack_fast.fence(["    g_seh_ebp = ebp; sub_00011000(); return; /* tail jmp */",
                              "    RECOMP_FRAME_LEAVE(); return;",
                              "    g_seh_ebp = ebp; RECOMP_ITAIL_RET(MEM32(eax + 0x38), 24u); return;",
                              "    if (_cf) sub_00011000(); return;"])
    assert lines == [
        "    g_seh_ebp = ebp; RECOMP_STK_FENCE(); sub_00011000(); return; /* tail jmp */",
        "    RECOMP_FRAME_LEAVE(); RECOMP_STK_FENCE(); return;",
        "    g_seh_ebp = ebp; RECOMP_STK_FENCE(); RECOMP_ITAIL_RET(MEM32(eax + 0x38), 24u); return;",
        "    if (_cf) sub_00011000(); RECOMP_STK_FENCE(); return;"]


def test_stack_values_handed_to_other_helpers_give_up():
    check = stack_fast.check_stack_uses
    check("    _fa = (uint32_t)(esp) & 0xFFFFFFFFu;", False)
    check("    eax = STK32(esp + 4) + recomp_f(STK32(esp)); /* (esp) */", False)
    check("    _o = RECOMP_ATOMIC_XCHG32(XBOX_PTR(esp + 4), eax);", False)
    check("    eax = MEM32(ebp + 8);", False)                  # ebp is no frame here
    with pytest.raises(stack_fast.GiveUp):
        check("    recomp_fnsave(esp + 4, 0);", False)
    with pytest.raises(stack_fast.GiveUp):
        check("    eax = some_helper((ebp - 8));", True)


def test_a_lone_return_after_if_is_braced():
    assert stack_fast.fence(["    if (_cf) return;", "    else return;"]) == \
        ["    if (_cf) { RECOMP_STK_FENCE(); return; }", "    else { RECOMP_STK_FENCE(); return; }"]


def test_switch_off(monkeypatch):
    monkeypatch.setenv("XBA_RECOMP_STK", "0")
    _, code = _translate("55 8bec 8b4508 5d c3")
    assert "STK" not in code
    assert "PUSH32(esp, ebp);" in code


def test_address_classes():
    assert classify_address("esp", False) == "stack"
    assert classify_address("esp + ecx * 4 + 0x10", False) == "stack"
    assert classify_address("(esp + 8) + 4", False) == "stack"
    assert classify_address("ebp - 0x10", True) == "stack"
    assert classify_address("ebp - 0x10", False) == "other"
    assert classify_address("eax + 4", True) == "other"
    assert classify_address("(esp) + (((int32_t)(eax) >> 5) * 4)", False) == "unknown"


def test_rewrite_forms():
    r = lambda s: rewrite_text(s, True)
    assert r("MEMF(esp + 4) = (float)fp_top(); fp_pop();") == "STKF_W(esp + 4, (float)fp_top()); fp_pop();"
    assert r("fp_push(MEMD(ebp - 8));") == "fp_push(STKD(ebp - 8));"
    assert r("PUSH32(esp, MEM32(ebp + 8));") == "STK_PUSH32(esp, STK32(ebp + 8));"
    assert r("eax = MEM32(MEM32(esp + 4) + 8);") == "eax = MEM32(STK32(esp + 4) + 8);"
    assert r("eax = ecx & MEM32(esp);") == "eax = ecx & STK32(esp);"
    assert r("MEM16(esp + 2) |= 3;") == "STK16(esp + 2) |= 3;"
    assert r("xmm0 = XMM_MEM(esp); XMM_STORE(esp + 16, xmm0);") == \
        "xmm0 = STK_XMM(esp); STK_XMM_STORE(esp + 16, xmm0);"
    # Typed stack accesses the rewrite cannot turn into words give the function up.
    assert rewrite(["    MEMF(esp) += 1.0f;"], True)[1].startswith("compound")
    assert rewrite(["    p = &MEMD(esp);"], True)[1].startswith("address")
    assert rewrite(["    eax = SMEM16((esp) + ((int32_t)eax >> 3));"], True)[1] is not None
    # A word left as MEM32 is fine: it is the same type as STK32.
    assert rewrite(["    eax = MEM32((esp) + ((int32_t)eax >> 3));"], True)[1] is None


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def test_bench_corpus_computes_the_same(tmp_path):
    """The bench corpus (w_stack: stack locals, an x87 spill read back as
    dwords, a double pushed as two dwords and read as a qword, halfword and
    byte stores read as dwords, a frame function) at -O2: STK build and MEM
    build (RECOMP_STK_FAST=0) agree on every checksum."""
    cc = shutil.which("clang") or shutil.which("gcc")
    if not (cc and shutil.which("as") and shutil.which("ld") and shutil.which("objcopy")):
        pytest.skip("needs a C compiler and binutils for i386 (xba-deps)")
    out = str(tmp_path / "bench")
    env = dict(os.environ, CC=cc)

    def bench(*args):
        r = subprocess.run([sys.executable, "-m", "tools.bench"] + list(args), cwd=ROOT, env=env,
                           capture_output=True, text=True, timeout=900)
        assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-3000:]
        return r.stdout
    try:
        bench("lift", out)
    except AssertionError as e:
        if "elf_i386" in str(e) or "--32" in str(e):
            pytest.skip("binutils without i386")
        raise
    bench("build", out, "base", "stk0", "--cc", cc)
    sums = {}
    for v in ("base", "stk0"):
        rows = [json.loads(l) for l in bench("run", out, v, "--layout", "exe", "--reps", "1",
                                             "--scale", "0.05").splitlines() if l.startswith("{")]
        sums[v] = {w: r["checksum"] for w, r in rows[0]["exe"].items()}
    assert sums["base"] == sums["stk0"]
    gen = open(os.path.join(out, "gen", "recomp_0000.c")).read()
    assert "STK_PUSH32(" in gen and "STKD_W(" in gen and "STK16(" in gen and "STK8(" in gen
