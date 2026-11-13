"""tools.prepare.discovered: seeds from earlier runs' discovery files (XBA-97),
re-gated on every prepare so a poisoned folder heals (profile-free WP1)."""

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


class FakeChecker:
    """Executable below 0x00800000; `refuse` {va: why} says no to the rest."""

    def __init__(self, refuse=None):
        self.refuse = refuse or {}
        self.asked = []

    def is_exec(self, va):
        return 0x00010000 <= va < 0x00800000

    def verdict(self, va):
        self.asked.append(va)
        if va in self.refuse:
            return False, self.refuse[va]
        return True, "new function"


NOT_A_BODY = {0x23456: "does not read as a function body"}


def refresh(tmp_path, checker, log=None):
    return discovered.refresh(str(tmp_path), "x.xbe", str(tmp_path / "functions.json"), checker=checker,
                              log=(log.append if log is not None else lambda m: None))


def read(tmp_path, name):
    p = tmp_path / name
    return json.loads(p.read_text()) if p.exists() else None


def test_first_prepare_has_nothing(tmp_path):
    assert refresh(tmp_path, FakeChecker()) is None
    assert discovered.digest(None) == ""


def test_new_addresses_are_gated_and_the_digest_is_stable(tmp_path):
    (tmp_path / "discovered.jsonl").write_text(DISC)
    (tmp_path / "functions.json").write_text("[]")
    c, log = FakeChecker(NOT_A_BODY), []
    p = refresh(tmp_path, c, log)
    assert sorted(c.asked) == [0x12345, 0x23456]          # unimpl and d3d are not seeds
    assert read(tmp_path, "discovered-seeds.json")[0]["start"] == "0x00012345"
    assert read(tmp_path, "discovered-rejected.json") == ["0x00023456"]
    assert p and discovered.digest(p)
    assert any("1 seed(s) added" in m for m in log)
    d0 = discovered.digest(p)
    # Again with the same file: the same seeds, the same digest, nothing said.
    log.clear()
    p = refresh(tmp_path, FakeChecker(NOT_A_BODY), log)
    assert discovered.digest(p) == d0 and log == []
    assert not discovered.changed(str(tmp_path))
    # A later run adds one: said, and it joins the seeds.
    with open(tmp_path / "discovered.jsonl", "a") as f:
        f.write('{"t":"icall","va":"0x00099999","ret":"0x00011111"}\n')
    assert discovered.changed(str(tmp_path))
    refresh(tmp_path, FakeChecker(NOT_A_BODY), log)
    assert [e["start"] for e in read(tmp_path, "discovered-seeds.json")] == ["0x00012345", "0x00099999"]
    assert any("1 new address(es)" in m for m in log)


def test_a_poisoned_seeds_file_is_revalidated(tmp_path):
    """The launcher's Fable folder: an older prepare seeded what a wild call
    site produced (one of them two bytes into a mov). The next prepare drops
    them, says so, keeps the legitimate one, and the digest changes so the
    analysis runs again."""
    (tmp_path / "functions.json").write_text("[]")
    (tmp_path / "discovered.jsonl").write_text("\n".join([
        '{"t":"run","time":"2026-11-13T01:29:49Z"}',
        '{"t":"icall","va":"0x00000000","ret":"0x007570B0"}',
        '{"t":"icall","va":"0xFF084E8B","ret":"0x000CFA7A"}',
        '{"t":"icall","va":"0x00525000","ret":"0x000CFA7A"}',
        '{"t":"icall","va":"0x001C120A","ret":"0x000CFA7A"}',
        '{"t":"run","time":"2026-11-13T13:36:36Z"}',
        '{"t":"icall","va":"0x005861C5","ret":"0x00583D55"}',
        '{"t":"smc","va":"0x00404000","kind":"code_write","at":"0x00405159","ret":"0x001C1A53"}',
        '{"t":"icall","va":"0x0063C5B3","ret":"0x001C1A70"}',
    ]) + "\n")
    old = [{"start": "0x%08X" % va, "observed": True, "note": "Indirect-call target observed at runtime; x"}
           for va in (0x00525000, 0x001C120A, 0x005861C5)]
    (tmp_path / "discovered-seeds.json").write_text(json.dumps(old, indent=1) + "\n")
    (tmp_path / "discovered-rejected.json").write_text('["0x00000000", "0xFF084E8B"]\n')
    d0 = discovered.digest(str(tmp_path / "discovered-seeds.json"))
    log = []
    p = refresh(tmp_path, FakeChecker({0x001C120A: "inside the instruction at 0x001C1208 of sub_001C1190"}), log)
    assert read(tmp_path, "discovered-seeds.json") == [old[2]]           # kept as it was
    assert discovered.digest(p) != d0
    assert read(tmp_path, "discovered-held.json") == ["0x001C120A", "0x00525000", "0x0063C5B3"]
    assert read(tmp_path, "discovered-rejected.json") == ["0x00000000", "0xFF084E8B"]
    said = "\n".join(log)
    assert "00525000  dropped from discovered-seeds.json: from a wild call site" in said
    assert "001C120A  dropped from discovered-seeds.json" in said
    assert "2 seed(s) dropped" in said
    assert "0063C5B3  held: written after the run went wrong (smc 0x00404000)" in said
    # Healed: the next prepare changes nothing.
    log.clear()
    refresh(tmp_path, FakeChecker(), log)
    assert log == [] and not discovered.changed(str(tmp_path))
    # A healthy v2 run reports one of the held addresses: --check sees it, the
    # prepare releases it (the static gates still run on it).
    with open(tmp_path / "discovered.jsonl", "a") as f:
        f.write('{"t":"run","time":"2026-11-14T00:00:00Z","v":2}\n'
                '{"t":"icall","va":"0x0063C5B3","ret":"0x00033333"}\n')
    assert discovered.changed(str(tmp_path))
    c = FakeChecker()
    refresh(tmp_path, c, log)
    assert 0x0063C5B3 in c.asked
    assert [e["start"] for e in read(tmp_path, "discovered-seeds.json")] == ["0x005861C5", "0x0063C5B3"]
    assert read(tmp_path, "discovered-held.json") == ["0x001C120A", "0x00525000"]


def test_without_an_analysis_seeds_are_still_regated(tmp_path):
    """functions.json gone: nothing new is seeded, a kept seed from a wild
    site still goes."""
    (tmp_path / "discovered.jsonl").write_text(
        '{"t":"run","time":"x"}\n{"t":"icall","va":"0xFF084E8B","ret":"0x000CFA7A"}\n'
        '{"t":"icall","va":"0x00525000","ret":"0x000CFA7A"}\n{"t":"run","time":"y"}\n'
        '{"t":"icall","va":"0x00030000","ret":"0x00011111"}\n')
    (tmp_path / "discovered-seeds.json").write_text('[{"start": "0x00525000", "observed": true}]')
    assert refresh(tmp_path, FakeChecker()) is None
    assert not (tmp_path / "discovered-seeds.json").exists()
    assert read(tmp_path, "discovered-held.json") == ["0x00525000"]


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
