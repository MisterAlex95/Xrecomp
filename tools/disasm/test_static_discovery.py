"""Static discovery (XBA-137) on synthetic XBEs of our own (tools/disasm/
synthetic.py): every target of the corpus is found at the first analysis, no
decoy becomes a function, and the growth is what the targets account for.

    python -m pytest tools/disasm/test_static_discovery.py -s     (-s: the report)
"""

import contextlib
import io
import json
import os
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("capstone")

from tools.disasm import discovery                                   # noqa: E402
from tools.disasm.synthetic import (DATA, Title, code_labels,        # noqa: E402
                                    discovery_corpus, CORPUS_IMPORTS)

LAYOUTS = ((16, 0xCC), (1, 0xCC), (4, 0x90), (8, 0xCC), (16, 0x00))


def analyse(t, tmp_path, level, cap=None, imports=CORPUS_IMPORTS, name="default.xbe"):
    """tools.xbe_parser + tools.disasm on title `t`: (functions by start, summary)."""
    from tools.xbe_parser.xbe_parser import XBEParser, export_json
    from tools.disasm.disasm import Disassembler
    os.makedirs(tmp_path, exist_ok=True)
    xbe = t.write(os.path.join(tmp_path, name), "entry", imports=imports)
    xjson = xbe[:-4] + "_analysis.json"
    export_json(XBEParser(xbe).parse(), xjson)
    out = os.path.join(tmp_path, "out-%d" % level)
    with contextlib.redirect_stdout(io.StringIO()):
        assert Disassembler(xbe, analysis_json=xjson, output_dir=out, force=True,
                            static_discovery=level, discovery_cap=cap).run()
    funcs = json.load(open(os.path.join(out, "functions.json")))
    summary = json.load(open(os.path.join(out, "summary.json")))
    return {int(f["start"], 16): f for f in funcs}, summary


def targets(t):
    return {va: n for va, n in code_labels(t).items() if n.startswith("T_")}


@pytest.mark.parametrize("al,fill", LAYOUTS)
def test_every_target_is_found_and_no_decoy(tmp_path, al, fill):
    t, decoys = discovery_corpus(al, fill)
    funcs, summary = analyse(t, str(tmp_path), discovery.FULL)
    missing = sorted(n for va, n in targets(t).items() if va not in funcs)
    assert not missing, missing
    seeded = sorted(k for k, va in decoys.items() if va in funcs)
    assert not seeded, seeded
    # Whatever discovery added starts on real code.
    code = code_labels(t)
    wrong = sorted("%08X %s" % (a, f["detection_method"]) for a, f in funcs.items()
                   if f["detection_method"] in discovery.METHODS and a not in code)
    assert not wrong, wrong
    assert summary["discovery"]["level"] == 2


@pytest.mark.parametrize("al,fill", LAYOUTS)
def test_the_shapes_the_passes_before_missed(tmp_path, al, fill):
    """Off, the run would have to reveal these; on, they are found."""
    t, _ = discovery_corpus(al, fill)
    off, _ = analyse(t, str(tmp_path), discovery.OFF)
    on, summary = analyse(t, str(tmp_path), discovery.FULL)
    va = t.va
    # The thread routine packed after a noreturn kernel call, the gap
    # function nothing references and the code after `jmp $`.
    for name in ("T_thread2", "T_gap", "T_after_jmp"):
        assert va(name) in on, name
    assert va("T_thread2") not in off and va("T_after_jmp") not in off
    # fatal ends at its noreturn call.
    assert int(on[va("fatal")]["end"], 16) == va("T_thread2")
    # The exception frame's __except block is inside guarded, not a function.
    g = on[va("guarded")]
    assert int(g["end"], 16) > va("H_resume")
    # (a table-word alias of it may exist, as before: it does not cut guarded)
    assert on.get(va("H_resume"), {}).get("detection_method", "tail_jump_alias") == "tail_jump_alias"
    assert int(off[va("guarded")]["end"], 16) <= va("H_filter")
    # Switch arms are in their function, never entries of their own.
    for fn, arms in (("switcher", ("S_c0", "S_c1", "S_c2")), ("regswitch", ("R_c0", "R_c1", "R_c2"))):
        f = on[va(fn)]
        assert int(f["end"], 16) > va(arms[-1]), (fn, f["end"])
        assert not [a for a in arms if va(a) in on], fn
    added = summary["discovery"]["added"]
    # (a callback that lands in a gap and closes there is the immediate
    # pass's: callback_arg counts only what that pass could not take)
    assert added["gap_body"] >= 1 and sum(added.values()) >= 2, added


def test_levels_and_cap(tmp_path):
    t, _ = discovery_corpus(1, 0xCC)
    strong, s1 = analyse(t, str(tmp_path / "a"), discovery.STRONG)
    assert t.va("T_thread2") in strong and s1["discovery"]["added"]["gap_body"] == 0
    assert t.va("T_after_jmp") not in strong        # gap code is level 2
    capped, s2 = analyse(t, str(tmp_path / "b"), discovery.FULL, cap=0)
    assert sum(s2["discovery"]["added"].values()) == 0 and s2["discovery"]["capped"]


def test_env_switch(monkeypatch):
    monkeypatch.delenv(discovery.ENV_LEVEL, raising=False)
    assert discovery.level_from_env() == discovery.FULL
    for v, want in (("0", 0), ("1", 1), ("2", 2), ("off", 0), ("9", 2)):
        monkeypatch.setenv(discovery.ENV_LEVEL, v)
        assert discovery.level_from_env() == want
    monkeypatch.setenv(discovery.ENV_CAP, "12.5")
    assert discovery.cap_from_env() == 12.5


def _decoy_title(seed):
    """A title whose only code is a few functions, and whose data is decoys:
    words and immediates naming every byte of the code that is not an entry,
    floats, colours, strings, timestamps near the image base."""
    import random
    rnd = random.Random(seed)
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    x.label("entry").hex("558bec").call("f1").call("f2").hex("ebfe").align(16)
    x.label("f1").hex("558bec 83ec08 8b4508 03450c 8945fc 8be5 5d c20800").align(16)
    x.label("f2").hex("56 57 8b7c240c 33f6 85ff 7e06 46 3bf7 7cfa 8bc6 5f 5e c3").align(16)
    # Data in .text after the code.
    x.label("blob").raw(bytes(rnd.randrange(256) for _ in range(rnd.randrange(16, 48)))).align(16)
    x.label("words").raw(b"".join(rnd.choice([b"\x00\x00\x80\x3f", b"\xff\x80\x40\xc0",
                                               b"Text", b"\x12\x34\x01\x00"]) for _ in range(8)))
    x.align(16)
    x.label("str").raw(b"Options\0Video\0Sound\0").align(16)
    x.label("tail").hex("c3")
    t._layout()
    lo, hi = t.va("entry"), t.va("tail")
    entries = {t.va(n) for n in ("entry", "f1", "f2", "tail")}
    words = [a for a in range(lo, hi) if a not in entries]
    for a in rnd.sample(words, min(len(words), 40)):
        r.u32(a)
    for _ in range(16):
        r.f32(rnd.uniform(-100, 100))
        r.u32(0x00010000 + rnd.randrange(0x3000))         # "timestamps", offsets near the base
        r.u32(rnd.randrange(1 << 32))                       # colours
    return t, set(words)


def test_decoy_corpus_false_positive_rate(tmp_path, capsys):
    """No decoy is ever seeded: across 40 decoy titles, every function
    start is a real entry (the rate printed is the measure; it must be 0)."""
    titles = seeded = decoys = before = before_titles = 0
    for seed in range(40):
        t, words = _decoy_title(seed)
        funcs, _ = analyse(t, str(tmp_path / str(seed)), discovery.FULL, imports=())
        bad = sorted(a for a in funcs if a in words)
        titles += 1
        decoys += len(words)
        seeded += len(bad)
        assert not bad, (seed, ["%08X %s" % (a, funcs[a]["detection_method"]) for a in bad])
        # The same with static discovery off: the passes before XBA-137.
        off, _ = analyse(t, str(tmp_path / str(seed)), discovery.OFF, imports=())
        n = sum(1 for a in off if a in words)
        before += n
        before_titles += n > 0
    with capsys.disabled():
        print("\n  decoy corpus: %d titles, %d decoy addresses, %d seeded (%.2f %%); "
              "off: %d seeded in %d titles"
              % (titles, decoys, seeded, 100.0 * seeded / max(1, decoys), before, before_titles))


def test_growth_report(tmp_path, capsys):
    """Function count and lifted bytes, off vs on, on the corpus layouts:
    the growth is the targets, nothing else."""
    rows = []
    for al, fill in LAYOUTS:
        t, _ = discovery_corpus(al, fill)
        off, _ = analyse(t, str(tmp_path / ("%d%x" % (al, fill))), discovery.OFF)
        on, _ = analyse(t, str(tmp_path / ("%d%x" % (al, fill))), discovery.FULL)
        size = lambda fs: sum(f["size"] for f in fs.values())
        rows.append((al, fill, len(off), len(on), size(off), size(on)))
        # Lifted bytes may shrink (an alias becomes a function, a split ends
        # a body) and grow by the code that was missing, never by much more.
        assert size(on) <= size(off) * 1.5 + 64
    with capsys.disabled():
        print("\n  static discovery growth (functions, lifted bytes), off -> on:")
        for al, fill, f0, f1, b0, b1 in rows:
            print("    align %2d fill %02X: %3d -> %3d functions (%+.0f %%), %4d -> %4d bytes (%+.0f %%)"
                  % (al, fill, f0, f1, 100.0 * (f1 - f0) / f0, b0, b1, 100.0 * (b1 - b0) / max(1, b0)))


def test_the_e2e_title_needs_no_run(tmp_path):
    """tools/compat/test_e2e.py's discovery title, analysis only: what its
    run reaches is found at level 2, and level 0 (before XBA-137) misses the
    thread routine after the noreturn call and the gap function."""
    from tools.disasm.synthetic import E2E_DECOYS, E2E_IMPORTS, discovery_e2e_title
    t = discovery_e2e_title()
    on, _ = analyse(t, str(tmp_path), discovery.FULL, imports=E2E_IMPORTS)
    off, _ = analyse(t, str(tmp_path), discovery.OFF, imports=E2E_IMPORTS)
    for name in ("thread_fn", "m1", "gap_fn", "fatal", "make_obj"):
        assert t.va(name) in on, name
    assert t.va("thread_fn") not in off and t.va("gap_fn") not in off
    assert not [n for n, add in E2E_DECOYS if t.va(n) + add in on]


def test_rtti_vtable_methods(tmp_path):
    """MSVC RTTI (tools.rtti) names a method packed straight after a call
    that never returns, inside the body of its caller: only the RTTI
    vtable says it is an entry, and it becomes one at a clean split."""
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    x.label("entry").hex("558bec 6a00").call("check").hex("83c404 ebfe").align(16)
    x.label("check").hex("8b442404 85c0 74").rel8("C_ret").call("die")
    x.label("C_ret").hex("c3").align(16)
    x.label("fatal").hex("6a00").call_kernel(0).align(16)
    x.label("die").hex("6a02").call("fatal")
    x.label("method").hex("8b4108 40 c20400").align(16)
    x.label("m0").hex("33c0 c3").align(16)
    # TypeDescriptor, CompleteObjectLocator, ClassHierarchyDescriptor, the vtable.
    r.label("td").u32(0).u32(0).raw(b".?AVWidget@@\0").align(4, 0)
    r.label("col").u32(0).u32(0).u32(0).abs("td").abs("chd")
    r.label("chd").u32(0).u32(0).u32(1).abs("bca")
    r.label("bca").abs("bcd")
    r.label("bcd").abs("td").u32(0).u32(0).u32(0xFFFFFFFF).u32(0).u32(0)
    r.abs("col")
    r.label("vtbl").abs("m0").abs("method").u32(0)
    on, s = analyse(t, str(tmp_path), discovery.STRONG, imports=(49,))
    assert t.va("method") in on and on[t.va("method")]["detection_method"] == "rtti_vtable", \
        on.get(t.va("method"))
    assert int(on[t.va("die")]["end"], 16) == t.va("method")


def test_seh_filter_away_from_its_function(tmp_path):
    """An __except filter the scope table names, parked after another
    function's call that never returns: a function of its own (seh_funclet);
    the __except block in the gap after its function extends that one."""
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    x.label("entry").hex("558bec").call("guarded").hex("6a00").call("check").hex("83c404 ebfe").align(16)
    x.label("guarded").hex("558bec 6aff").push("scopetbl").push("eh3")
    x.hex("64a100000000 50 648925 00000000 83ec08 c745fc00000000 33c0 c745fcffffffff")
    x.label("L_ret").hex("8b4df0 64890d00000000 8be5 5d c3")
    x.label("resume").hex("8b65e8 eb").rel8("L_ret").align(16)
    x.label("eh3").hex("b801000000 c3").align(16)
    x.label("check").hex("8b442404 85c0 74").rel8("C_ret").call("die")
    x.label("C_ret").hex("c3").align(16)
    x.label("fatal").hex("6a00").call_kernel(0).align(16)
    x.label("die").hex("6a02").call("fatal")
    x.label("filter").hex("b801000000 c3").align(16)
    r.label("scopetbl").u32(0xFFFFFFFF).abs("filter").abs("resume")
    on, s = analyse(t, str(tmp_path), discovery.STRONG, imports=(49,))
    assert on.get(t.va("filter"), {}).get("detection_method") == "seh_funclet", on.get(t.va("filter"))
    assert int(on[t.va("guarded")]["end"], 16) > t.va("resume")


def test_random_blobs_between_functions(tmp_path, capsys):
    """A larger title: 1,500 small functions (a third called, a third in a
    table, a third named nowhere), a 64-byte random blob after every 50th,
    random words in .rdata. At level 2 every function start is a real one;
    the passes before XBA-137 made functions in the blobs."""
    import random
    rnd = random.Random(1)
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    n = 1500
    x.label("entry").hex("558bec")
    for i in range(0, n, 3):
        x.call("f%d" % i)
    x.hex("ebfe").align(16)
    bodies = ["558bec 8b4508 03450c 5d c3", "56 8b742408 8bc6 5e c3", "33c0 c3", "8b4104 c20400",
              "558bec 83ec10 8b4508 85c0 7405 8b00 eb02 33c0 8be5 5d c20400"]
    for i in range(n):
        x.label("f%d" % i).hex(rnd.choice(bodies)).align(16)
        if i % 50 == 0:
            x.raw(bytes(rnd.randrange(256) for _ in range(64))).align(16)
    for i in range(1, n, 3):
        r.abs("f%d" % i)
    for _ in range(n // 4):
        r.u32(rnd.randrange(1 << 32))
    t._layout()
    labels = {t.va(name) for name in t.labels()}
    on, _ = analyse(t, str(tmp_path), discovery.FULL, imports=())
    off, _ = analyse(t, str(tmp_path), discovery.OFF, imports=())
    false_on = sorted(a for a in on if a not in labels)
    false_off = sorted(a for a in off if a not in labels)
    assert not false_on, ["%08X %s" % (a, on[a]["detection_method"]) for a in false_on]
    found_on = sum(1 for a in labels if a in on)
    found_off = sum(1 for a in labels if a in off)
    assert found_on >= found_off
    with capsys.disabled():
        print("\n  random blobs: %d functions, found %d -> %d, false starts %d -> %d (off -> on)"
              % (len(labels), found_off, found_on, len(false_off), len(false_on)))
