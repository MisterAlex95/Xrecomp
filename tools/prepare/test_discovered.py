"""tools.prepare.discovered: seeds from earlier runs' discovery files (XBA-97)."""

import json
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.prepare import discovered        # noqa: E402

DISC = "\n".join([
    '{"t":"run","time":"2026-11-12T00:00:00Z"}',
    '{"t":"icall","va":"0x00012345","ret":"0x00011111"}',
    '{"t":"thread","va":"0x00023456","ctx1":"0x00045678"}',
    '{"t":"unimpl","va":"0x00056780","insn":"bsf eax, ecx"}',
    '{"t":"d3d","va":"0x0085FDE0","slot":"0x00862288"}',
]) + "\n"


def fake_gate(calls):
    def gate(xbe, functions, candidates, existing, analysis_json, log=print):
        calls.append(sorted(candidates))
        # 0x12345 reads as a function, 0x23456 does not.
        return [{"start": "0x%08X" % va, "observed": True, "note": "t"} for va in candidates if va == 0x12345]
    return gate


def test_first_prepare_has_nothing(tmp_path):
    assert discovered.refresh(str(tmp_path), "x.xbe", str(tmp_path / "functions.json"), gate=fake_gate([])) is None
    assert discovered.digest(None) == ""


def test_new_addresses_are_gated_once(tmp_path):
    (tmp_path / "discovered.jsonl").write_text(DISC)
    (tmp_path / "functions.json").write_text("[]")
    calls, log = [], []
    p = discovered.refresh(str(tmp_path), "x.xbe", str(tmp_path / "functions.json"), gate=fake_gate(calls),
                           log=log.append)
    assert calls == [[0x12345, 0x23456]]                  # unimpl and d3d are not seeds
    assert json.loads((tmp_path / "discovered-seeds.json").read_text())[0]["start"] == "0x00012345"
    assert json.loads((tmp_path / "discovered-rejected.json").read_text()) == ["0x00023456"]
    assert p and discovered.digest(p)
    d0 = discovered.digest(p)
    # Again with the same file: nothing new to gate, same seeds, same digest.
    calls.clear()
    p = discovered.refresh(str(tmp_path), "x.xbe", str(tmp_path / "functions.json"), gate=fake_gate(calls))
    assert calls == [] and discovered.digest(p) == d0
    # A later run adds one: only that one is gated.
    with open(tmp_path / "discovered.jsonl", "a") as f:
        f.write('{"t":"icall","va":"0x00099999","ret":"0x00011111"}\n')
    discovered.refresh(str(tmp_path), "x.xbe", str(tmp_path / "functions.json"), gate=fake_gate(calls))
    assert calls == [[0x99999]]


def test_static_discovery_in_the_stamp_and_the_counts(tmp_path):
    """XBA-137: the switches change the stamp only off their default, and the
    disassembler's summary gives pack.json's "analysis"."""
    assert discovered.mode_suffix({}) == ""
    assert discovered.mode_suffix({"RECOMP_STATIC_DISCOVERY": "2"}) == ""
    assert discovered.mode_suffix({"RECOMP_STATIC_DISCOVERY": "0"}) == "|sd=0:"
    assert discovered.mode_suffix({"RECOMP_STATIC_DISCOVERY_CAP": "5"}) == "|sd=2:5"
    assert discovered.analysis_counts(str(tmp_path)) is None
    (tmp_path / "disasm").mkdir()
    (tmp_path / "disasm" / "summary.json").write_text(json.dumps({
        "total_functions": 120, "functions_by_method": {"call_target": 100, "gap_body": 3},
        "discovery": {"level": 2, "cap": None, "capped": False,
                      "added": {"callback_arg": 1, "ctor_vtable": 0, "gap_body": 3}, "refused": {"x": 9}}}))
    c = discovered.analysis_counts(str(tmp_path))
    assert c["functions"] == 120 and c["static_discovery"]["added"]["gap_body"] == 3
    assert "refused" not in c["static_discovery"]
    assert discovered.counts_line(c) == "120 functions, static discovery +4 (callback_arg 1, gap_body 3)"
