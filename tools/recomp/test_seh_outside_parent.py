"""Exception funclets outside their parent's range, through disasm and the
lift (XBA-139).

A title's prepare log had 18 lines `SEH: 0x... has handler targets outside
it ...` (translator._seh_frame_of). These synthetic titles (tools/disasm/
synthetic.py: our own bytes) lay out a frame's funclets the ways a linker
does -- after the parent's ret, after another function, in a gap past
padding, a C++ frame's unwind action and catch block -- and run the
prepare's own commands (tools.xbe_parser, tools.disasm, tools.recomp) on
them: every funclet must be dispatchable (a recomp_dispatch.c entry) and the
lift must not report a lost target, with the default analysis and with
RECOMP_STATIC_DISCOVERY_STRICT=1 (where the older table-word pass does not
alias the scope table's words, so static discovery's SEH pass does the work).

The one shape that did print the line: a function entering try level 0 with
`and dword ptr [ebp-4], 0` (MSVC's short form) read the scope table of the
function linked after it as its own, and reported that function's __except
block as its own lost target (seh_frames._try_level_stores).
"""
import json
import os
import re
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("capstone")

from tools.disasm.synthetic import DATA, Title  # noqa: E402


def _parent_eh3(x, name, scope, levels=(0,), and0=False):
    """push ebp ; mov ebp, esp ; push -1 ; push scope ; push eh3 ;
    link fs:[0] ; enter each try level ; leave the try ; unlink ; ret."""
    x.label(name).hex("558bec 6aff").push(scope).push("eh3")
    x.hex("64a100000000 50 648925 00000000 83ec08")
    for lv in levels:
        if and0 and lv == 0:
            x.hex("8365fc00")                   # and dword ptr [ebp-4], 0
        else:
            x.hex("c745fc").u32(lv)             # mov dword ptr [ebp-4], lv
    x.hex("33c0 c745fcffffffff")
    x.label(name + "_ret").hex("8b4df0 64890d00000000 8be5 5d c3")


def _title(*callees):
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    x.label("entry").hex("558bec")
    for c in callees:
        x.call(c)
    x.hex("ebfe").align(16)
    return t, x, r


def _handler(x):
    x.label("eh3").hex("b801000000 c3").align(16)


def filter_after_another_function():
    """The filter packed after another function's ret, the __except block
    right after the parent's."""
    t, x, r = _title("guarded", "other")
    _parent_eh3(x, "guarded", "scopetbl")
    x.label("resume").hex("8b65e8 eb").rel8("guarded_ret").align(16)
    _handler(x)
    x.label("other").hex("8b442404 40 c3")
    x.label("filter").hex("b801000000 c3").align(16)
    x.label("next").hex("33c0 c3").align(16)
    r.label("scopetbl").u32(0xFFFFFFFF).abs("filter").abs("resume")
    return t, ("filter",), ("resume",)


def filter_and_except_after_ret():
    """MSVC's own layout: filter and __except block after the parent's ret."""
    t, x, r = _title("guarded", "other")
    _parent_eh3(x, "guarded", "scopetbl")
    x.label("filter").hex("b801000000 c3")
    x.label("resume").hex("8b65e8 eb").rel8("guarded_ret").align(16)
    _handler(x)
    x.label("other").hex("8b442404 40 c3").align(16)
    r.label("scopetbl").u32(0xFFFFFFFF).abs("filter").abs("resume")
    return t, ("filter",), ("resume",)


def finally_in_a_far_gap():
    """A __finally parked in a gap past padding, after another function."""
    t, x, r = _title("guarded", "other", "third")
    _parent_eh3(x, "guarded", "scopetbl")
    x.align(16)
    _handler(x)
    x.label("other").hex("8b442404 40 c3").align(16)
    x.label("fin").hex("33c0 40 c3").align(16)
    x.label("third").hex("8b442404 48 c3").align(16)
    r.label("scopetbl").u32(0xFFFFFFFF).u32(0).abs("fin")
    return t, ("fin",), ()


def two_levels_split():
    """Two scope entries: a filter after the parent, a __finally after
    another function."""
    t, x, r = _title("guarded", "other")
    _parent_eh3(x, "guarded", "scopetbl", levels=(0, 1))
    x.label("filter").hex("b801000000 c3")
    x.label("resume").hex("8b65e8 eb").rel8("guarded_ret").align(16)
    _handler(x)
    x.label("other").hex("8b442404 40 c3").align(16)
    x.label("fin").hex("33c0 40 c3").align(16)
    r.label("scopetbl").u32(0xFFFFFFFF).abs("filter").abs("resume")
    r.u32(0).u32(0).abs("fin")
    return t, ("filter", "fin"), ("resume",)


def cxx_unwind_and_catch_after_ret():
    """A C++ frame: the destructor action and the catch block after the
    parent's ret; the catch hands back a continuation inside the parent."""
    t, x, r = _title("fcxx", "other")
    x.label("fcxx").hex("558bec 6aff").push("ehstub")
    x.hex("64a100000000 50 648925 00000000 83ec08 c745fc00000000 33c0")
    x.label("cont").hex("c745fcffffffff 8b4df4 64890d00000000 8be5 5d c3")
    x.label("unwind").hex("33c0 c3")
    x.label("catch").hex("b8").abs("cont").hex("c3").align(16)
    x.label("ehstub").hex("b8").abs("funcinfo").hex("e9").rel("cxxh").align(16)
    x.label("cxxh").hex("b801000000 c3").align(16)
    x.label("other").hex("8b442404 40 c3").align(16)
    r.label("funcinfo").u32(0x19930520).u32(1).abs("unwmap").u32(1).abs("trymap")
    r.u32(0).u32(0)
    r.label("unwmap").u32(0xFFFFFFFF).abs("unwind")
    r.label("trymap").u32(0).u32(0).u32(1).u32(1).abs("handlers")
    r.label("handlers").u32(0).u32(0).u32(0).abs("catch")
    return t, ("unwind", "catch"), ()


def and0_neighbours():
    """Two guarded functions entering level 0 with `and [ebp-4], 0`, their
    scope tables next to each other in .rdata."""
    t, x, r = _title("guarded", "g2")
    _parent_eh3(x, "guarded", "scopetbl", and0=True)
    x.label("filter").hex("b801000000 c3")
    x.label("resume").hex("8b65e8 eb").rel8("guarded_ret").align(16)
    _parent_eh3(x, "g2", "scopetbl2", and0=True)
    x.label("filter2").hex("b801000000 c3")
    x.label("resume2").hex("8b65e8 eb").rel8("g2_ret").align(16)
    _handler(x)
    r.label("scopetbl").u32(0xFFFFFFFF).abs("filter").abs("resume")
    r.label("scopetbl2").u32(0xFFFFFFFF).abs("filter2").abs("resume2")
    return t, ("filter", "filter2"), ("resume", "resume2")


LAYOUTS = (filter_after_another_function, filter_and_except_after_ret,
           finally_in_a_far_gap, two_levels_split,
           cxx_unwind_and_catch_after_ret, and0_neighbours)


def _run(cmd, env):
    r = subprocess.run([sys.executable, "-m"] + cmd, cwd=ROOT, env=env,
                       capture_output=True, text=True, timeout=600)
    assert r.returncode == 0, (cmd, r.stdout[-2000:], r.stderr[-3000:])
    return r.stdout + r.stderr


def lift(t, tmp, strict):
    """The prepare's analysis and lift on `t`: (lift output, functions by
    start, disasm summary, recomp_dispatch.c)."""
    os.makedirs(tmp, exist_ok=True)
    env = dict(os.environ, RECOMP_STATIC_DISCOVERY_STRICT="1" if strict else "0")
    env.pop("RECOMP_STATIC_DISCOVERY", None)
    xbe = t.write(os.path.join(tmp, "default.xbe"), "entry", imports=(49,))
    disasm = os.path.join(tmp, "disasm")
    gen = os.path.join(tmp, "gen")
    _run(["tools.xbe_parser", xbe, "--json", os.path.join(tmp, "default_analysis.json")], env)
    _run(["tools.disasm", xbe, "-o", disasm, "--force"], env)
    out = _run(["tools.recomp", xbe, "--all", "--split", "250", "--game-name", "seh",
                "--disasm-dir", disasm, "--func-id-dir", os.path.join(tmp, "none"),
                "--abi-dir", os.path.join(tmp, "none"), "--gen-dir", gen,
                "-o", os.path.join(tmp, "out")], env)
    with open(os.path.join(disasm, "functions.json")) as f:
        funcs = {int(e["start"], 16): e for e in json.load(f)}
    with open(os.path.join(disasm, "summary.json")) as f:
        summary = json.load(f)
    with open(os.path.join(gen, "recomp_dispatch.c")) as f:
        dispatch = f.read()
    return out, funcs, summary, dispatch


@pytest.mark.parametrize("strict", (False, True), ids=("default", "strict"))
@pytest.mark.parametrize("layout", LAYOUTS, ids=lambda f: f.__name__)
def test_every_funclet_dispatches_and_none_is_lost(tmp_path, layout, strict):
    t, funclets, resumes = layout()
    out, funcs, summary, dispatch = lift(t, str(tmp_path), strict)
    assert "SEH: " not in out, [l for l in out.splitlines() if "SEH: " in l]
    for name in funclets:
        va = t.va(name)
        assert re.search(r"\{ 0x%08Xu, \(recomp_func_t\)\w+ \}" % va, dispatch), \
            (name, hex(va), funcs.get(va))
    # A resume target is inside its parent (the extension of 917e91d).
    for name in resumes:
        va = t.va(name)
        assert any(s <= va < int(f["end"], 16) for s, f in funcs.items()
                   if f["detection_method"] != "tail_jump_alias"), (name, hex(va))
    # Nothing was read from a neighbour's table.
    refused = summary["discovery"]["refused"]
    assert not refused.get("seh: resume target in another function"), refused
    assert '"seh_lost": 0' in out


def test_the_health_line_counts_a_lost_target(tmp_path):
    """Before _try_level_stores, and0_neighbours under strict printed
    `SEH: <guarded> ... 0x<resume2>` -- g2's __except block -- and counted
    it; the reading of the frame is what fixes it, so a frame that does lose
    a target still counts: a __finally that no pass takes (the scope table
    names the middle of another function's path, which no pass may split)."""
    t, x, r = _title("guarded", "other")
    _parent_eh3(x, "guarded", "scopetbl")
    x.align(16)
    _handler(x)
    x.label("other").hex("8b442404")
    x.label("lost").hex("40 c3").align(16)  # inc eax ; ret, after a mov
    r.label("scopetbl").u32(0xFFFFFFFF).u32(0).abs("lost")
    out, funcs, _, dispatch = lift(t, str(tmp_path), True)
    assert t.va("lost") not in funcs
    line = [l for l in out.splitlines() if "SEH: " in l]
    assert line and "0x%08X (in sub_%08X)" % (t.va("lost"), t.va("other")) in line[0], \
        out[-3000:]
    assert '"seh_lost": 1' in out
