"""tools.seed_from_log reads a run's log and the runtime's discovery file (XBA-64),
and keeps what a run that went wrong reported out of the seeds (profile-free WP1)."""

import json
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.disasm.synthetic import Title                 # noqa: E402
from tools.seed_from_log.__main__ import (                # noqa: E402
    SeedChecker, classify_discovery, gate, gate_discovery, main, scan_log)

DISCOVERY = """{"t":"run","time":"2026-11-11T18:00:00Z"}
{"t":"icall","va":"0x0012ABCD","ret":"0x00123456"}
{"t":"thread","va":"0x00034560","ctx1":"0x00045670"}
{"t":"thread_ctx","va":"0x00045670","routine":"0x00034560"}
{"t":"unimpl","va":"0x00056780","insn":"bsf eax, ecx"}
{"t":"icall","va":"0x0012AB
"""


def test_discovery_file():
    got = scan_log(DISCOVERY)
    assert sorted(got) == [0x00034560, 0x00045670, 0x0012ABCD]     # no unimpl, no torn last line
    assert got[0x00034560].startswith("Thread start routine")


def test_run_log_still_read():
    log = ("[ICALL] Failed to resolve VA 0x00012345 (total calls: 9)\n"
           "  [KERNEL] PsCreateSystemThreadEx: start routine 0x00023456 not found in dispatch!\n")
    assert sorted(scan_log(log)) == [0x00012345, 0x00023456]


# ── The call-site and taint rules (profile-free WP1) ─────────────────────────────

def exe(va):
    return 0x00010000 <= va < 0x00800000


def lines(*entries):
    return "\n".join(json.dumps(e, separators=(",", ":")) for e in entries) + "\n"


def run(v=None):
    e = {"t": "run", "time": "2026-11-13T01:29:49Z"}
    if v:
        e["v"] = v
    return e


def icall(va, ret, **kw):
    return dict({"t": "icall", "va": "0x%08X" % va, "ret": "0x%08X" % ret}, **kw)


def test_a_wild_call_site_yields_no_seed():
    """Fable's ret 0x000CFA7A: targets in and out of the executable range
    from one site. None of them is a seed, even the ones before the first
    non-code target; a site that only ever reached code still seeds."""
    c = classify_discovery(lines(
        run(),
        icall(0x00584074, 0x00583D55),        # a legitimate site
        icall(0x00525000, 0x000CFA7A),        # executable, before the site went wild
        icall(0xFF084E8B, 0x000CFA7A),        # not code: the site is wild
        icall(0x001C120A, 0x000CFA7A),
    ), exe)
    assert c[0x00584074]["clean"]
    assert not c[0x00525000]["clean"] and "wild call site" in c[0x00525000]["why"]
    assert not c[0x001C120A]["clean"]


def test_the_call_site_rule_is_per_run():
    c = classify_discovery(lines(
        run(), icall(0xFF084E8B, 0x000CFA7A),
        run(), icall(0x00525000, 0x000CFA7A),  # the same site, a run where it reached only code
    ), exe)
    assert c[0x00525000]["clean"]


def test_entries_after_the_first_sign_are_held_until_a_healthy_run():
    """An older file: the order decides (an smc, an exception, a call to a
    non-code address taint the rest of the segment). A v2 file says so
    itself; an untainted entry in a later run releases the address."""
    text = lines(
        run(),
        {"t": "thread", "va": "0x00034560", "ctx1": "0x00000000"},          # before: clean
        {"t": "smc", "va": "0x00404000", "kind": "code_write", "at": "0x00405159", "ret": "0x001C1A53"},
        icall(0x0063C5B3, 0x00011111),
        run(),
        {"t": "exception", "va": "0x00048B64", "code": "0xC0000094", "why": "x"},
        icall(0x00070000, 0x00011111),
        run(2),
        icall(0x00071000, 0x00022222, tainted=True),                         # v2: the runtime says so
        {"t": "wild", "va": "0x00000000", "ret": "0x007570B0"},
        icall(0x00072000, 0x00022222),                                       # after a wild call
    )
    c = classify_discovery(text, exe)
    assert c[0x00034560]["clean"]
    assert not c[0x0063C5B3]["clean"] and "smc 0x00404000" in c[0x0063C5B3]["why"]
    assert not c[0x00070000]["clean"] and "exception" in c[0x00070000]["why"]
    assert not c[0x00071000]["clean"] and "tainted" in c[0x00071000]["why"]
    assert not c[0x00072000]["clean"] and "wild 0x00000000" in c[0x00072000]["why"]
    # A healthy run reports two of them again (a v2 runtime writes them untainted).
    c = classify_discovery(text + lines(run(2), icall(0x0063C5B3, 0x00033333),
                                        icall(0x00071000, 0x00033333)), exe)
    assert c[0x0063C5B3]["clean"] and c[0x00071000]["clean"] and not c[0x00070000]["clean"]


def test_without_the_xbe_only_the_runtime_signs_count():
    """tools.prepare --check has no XBE: every icall target is taken as code,
    wild/smc/exception entries still taint."""
    c = classify_discovery(lines(run(2), icall(0xFF084E8B, 0x000CFA7A), icall(0x00525000, 0x000CFA7A),
                                 {"t": "wild", "va": "0x00000000", "ret": "0x1"}, icall(0x00070000, 0x2)))
    assert c[0x00525000]["clean"] and not c[0x00070000]["clean"]


# ── The static gates on a synthetic XBE of our own ──────────────────────

def fable_shape(tmp_path):
    """.text with:
      f1    a function whose `mov eax, [esp+0x10]` has a seed two bytes into
            it (`mid`), which decodes as a body (and al, 0x10 ... ret 8):
            Fable's 0x001C120A;
      f2    a function no analysis found (a run's legitimate miss);
      f3    a function the analysis gave too long a body: after its ret, a
            data blob the linear sweep reads out of phase, then `xid`, a
            real entry in unclaimed bytes (Halo's XPP init shape);
      f4    a `mov eax, imm32` whose immediate is a prologue (`pro`, at
            f4 + 1): a seed there is kept, the body is the one wrong."""
    t = Title()
    text = t.section(".text")
    text.label("f1").hex("55 8bec 57 8b7d08").label("load").hex("8b442410 894704 5f 5d c20800").align(16)
    text.label("f2").hex("8bc1 c3").align(16)
    text.label("f3").hex("33c0 c3").hex("b8 0000").label("xid").hex("8bc1 40 c3").align(16)
    text.label("f4").hex("b8 558bec83 ec10 8be5 5d c3").align(16)
    text.label("end")
    xbe = t.write(tmp_path / "default.xbe", entry="f1")
    from tools.xbe_parser.xbe_parser import XBEParser, export_json
    export_json(XBEParser(xbe).parse(), str(tmp_path / "default_analysis.json"))
    funcs = [{"start": "0x%08X" % t.va(a), "end": "0x%08X" % t.va(b), "name": "sub_%08X" % t.va(a)}
             for a, b in (("f1", "f2"), ("f3", "f4"), ("f4", "end"))]
    fj = tmp_path / "functions.json"
    fj.write_text(json.dumps(funcs))
    return t, xbe, fj


def test_a_seed_inside_an_instruction_of_a_body_is_refused(tmp_path):
    t, xbe, fj = fable_shape(tmp_path)
    mid = t.va("load") + 2
    c = SeedChecker(xbe, fj)
    assert c.engine.probes_as_function_body(mid)       # what the old gate asked: it passed
    ok, why = c.verdict(mid)
    assert not ok and "inside the instruction at 0x%08X" % t.va("load") in why
    assert c.verdict(t.va("f2")) == (True, "new function")
    ok, note = c.verdict(t.va("xid"))                   # unclaimed bytes after a blob: kept
    assert ok and note.startswith("alias inside")
    assert c.verdict(t.va("f4") + 1)[0]                 # a prologue: kept
    # The run-log gate has the same static gates.
    assert [e["start"] for e in gate(xbe, fj, {mid: "x", t.va("f2"): "y"}, log=lambda m: None)] \
        == ["0x%08X" % t.va("f2")]


def test_a_body_the_seed_itself_cut_still_counts(tmp_path):
    """The poisoned analysis: f1 ends at the seed (the junk seed's own body
    starts there, and is not trusted). The instruction that starts in f1 and
    runs past its end still covers the seed."""
    t, xbe, fj = fable_shape(tmp_path)
    mid = t.va("load") + 2
    funcs = json.loads(fj.read_text())
    funcs[0]["end"] = "0x%08X" % mid
    funcs.append({"start": "0x%08X" % mid, "end": "0x%08X" % t.va("f2"), "name": "sub_%08X" % mid})
    fj.write_text(json.dumps(funcs))
    assert not SeedChecker(xbe, fj, untrusted={mid}).verdict(mid)[0]


def test_the_fable_case_end_to_end(tmp_path, capsys):
    """A corrupted run's wild site gives a body-shaped target, a healthy run
    a real miss: only the miss becomes a seed, through gate_discovery and the
    command line."""
    t, xbe, fj = fable_shape(tmp_path)
    mid, f2 = t.va("load") + 2, t.va("f2")
    text = lines(run(), icall(mid, 0x000CFA7A), icall(0xFF084E8B, 0x000CFA7A),
                 run(), icall(f2, 0x00583D55), icall(0x00FFFFF0, 0x00599999))
    accepted, refused, held = gate_discovery(text, SeedChecker(xbe, fj))
    assert sorted(accepted) == [f2] and sorted(held) == [mid]
    assert sorted(refused) == [0x00FFFFF0, 0xFF084E8B]
    (tmp_path / "discovered.jsonl").write_text(text)
    seeds = tmp_path / "seeds.json"
    assert main([str(tmp_path / "discovered.jsonl"), xbe, "--functions", str(fj), "--seeds", str(seeds)]) == 0
    assert [e["start"] for e in json.loads(seeds.read_text())] == ["0x%08X" % f2]
    assert "held: from a wild call site" in capsys.readouterr().out


def test_no_analysis_json_yet(tmp_path):
    """A prepare in a fresh work folder re-gates a copied seeds file before its
    analysis exists: the checker parses the XBE itself, and writes nothing."""
    t, xbe, fj = fable_shape(tmp_path)
    os.remove(tmp_path / "default_analysis.json")
    c = SeedChecker(xbe, None, tmp_path / "default_analysis.json")
    assert c.is_exec(t.va("f2")) and c.verdict(t.va("f2"))[0]
    assert not (tmp_path / "default_analysis.json").exists()
