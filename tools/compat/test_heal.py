"""The self-healing loop's rules (tools/compat/heal.py) and the recall measure
(tools/compat/recall.py), on fakes: no build, no game (XBA-129). The real loop
on a synthetic title: test_e2e.py::test_the_self_healing_loop."""

import json
import os
import struct
import sys
import types

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.compat import heal, recall            # noqa: E402
from tools.compat.__main__ import matrix, report  # noqa: E402


def F(seeds=(), **kinds):
    f = heal.finds([])
    f["seeds"] = list(seeds)
    for k, v in kinds.items():
        f[k] = v
    return f


def test_a_run_with_new_code_goes_round_again_crash_or_not():
    assert heal.decide(F(["0x1"]), "crash (signal 11)", None, None, 1, 0)[0] == "again"
    # A skipped indirect call does not crash: the title ran its time, and still a round.
    assert heal.decide(F(["0x1"]), None, None, None, 1, 0)[0] == "again"


def test_nothing_new_stops_named():
    assert heal.decide(F(), None, None, None, 1, 0)[:2] == ("stop", "nothing new found")
    act, why, _ = heal.decide(F(stub=[{"t": "stub", "ord": 136, "name": "KeRemoveQueue"}]),
                              "hang: no flip for the last 40 s", None, None, 1, 0)
    assert act == "stop"
    assert why == "needs a fix: hang: no flip for the last 40 s; calls stub ordinal 136 (KeRemoveQueue)"


def test_never_round_again_on_what_a_prepare_cannot_fix():
    for kind, entry, words in (
            ("exception", {"t": "exception", "va": "0x00011020", "code": "0xC0000094", "why": "divide by zero"},
             "guest exception 0xC0000094 at 0x00011020: divide by zero"),
            ("stack_switch", {"t": "stack_switch", "va": "0x1", "to": "0x2", "why": "w"},
             "stack switch at 0x1 to 0x2: w"),
            ("unimpl", {"t": "unimpl", "va": "0x3", "insn": "bsf eax, ecx"},
             "untranslated instruction: bsf eax, ecx at 0x3")):
        act, why, _ = heal.decide(F(["0x9"], **{kind: [entry]}), "crash (signal 11)", None, None, 1, 0)
        assert (act, why) == ("stop", "needs a fix: " + words), kind
    # The same entries in a run that ended well: the new code is still taken.
    assert heal.decide(F(["0x9"], unimpl=[{"t": "unimpl"}]), None, None, None, 1, 0)[0] == "again"


def test_the_cap_is_on_rounds_without_progress():
    # Progress every round: no cap but the safety net.
    idle = 0
    for n in range(2, 12):
        act, why, idle = heal.decide(F(["0x%x" % n]), "crash (signal 11): fault at 0x%x" % n, "crash (signal 11): "
                                     "fault at 0x%x" % (n - 1), 1, n, idle)
        assert act == "again" and idle == 0, (n, why)
    assert heal.decide(F(["0x99"]), "x", "y", 1, 12, 0)[:2] == ("stop", "safety cap: 12 rounds")
    # The prepare took none of the new addresses (the gates said: not code).
    assert heal.decide(F(["0x9"]), "crash", "crash 2", 0, 2, 0)[:2] == \
        ("stop", "no progress: the prepare accepted no new seed")
    # The same blocker twice (the run's seconds aside).
    assert heal.decide(F(["0x9"]), "exited with code 1 after 7 s", "exited with code 1 after 9 s", 1, 3, 0)[:2] == \
        ("stop", "no progress: the same blocker twice")
    # --max-idle 2: one such round is let through, a second in a row stops.
    act, _, idle = heal.decide(F(["0x9"]), "b", "b", 1, 3, 0, max_idle=2)
    assert act == "again" and idle == 1
    assert heal.decide(F(["0xa"]), "b", "b", 1, 4, idle, max_idle=2)[0] == "stop"


def test_the_loop_on_a_fake_runner(tmp_path):
    """heal() over a fake run_one: round 1 finds A, 2 finds B, 3 finds C which the
    gates refuse, so round 4's prepare takes nothing: stop, no progress."""
    work = tmp_path / "cw" / "t"
    work.mkdir(parents=True)
    plan = {1: ["0x00011040"], 2: ["0x00011080"], 3: ["0x00011108"], 4: []}
    refused = {"0x00011108"}
    state = {"round": 0}

    def run_one(source, a, marks):
        state["round"] += 1
        r = state["round"]
        disc = work / "discovered.jsonl"
        # The prepare: takes what the earlier runs found through the "gates".
        found = [json.loads(l)["va"] for l in disc.read_text().splitlines()] if disc.exists() else []
        seeds = [{"start": v} for v in found if v not in refused]
        (work / "discovered-seeds.json").write_text(json.dumps(seeds))
        marks["disc_offset"] = disc.stat().st_size if disc.exists() else 0
        with open(disc, "a") as f:
            for va in plan[r]:
                f.write(json.dumps({"t": "thread", "va": va, "ctx1": "0x00000000"}) + "\n")
        return {"source": "t", "stage": "boots", "blocker": "no flip; last discovery: thread %s" % plan[r][0]
                if plan[r] else "no flip"}

    a = types.SimpleNamespace(work_root=str(tmp_path / "cw"), max_rounds=12, max_idle=1)
    res = heal.heal(str(tmp_path / "t"), a, run_one)
    h = res["heal"]
    assert [x["new_seeds"] for x in h["history"]] == [1, 1, 1, 1] or h["rounds"] == 4, h
    assert h["rounds"] == 4 and h["stop"] == "no progress: the prepare accepted no new seed", h
    assert [x["accepted"] for x in h["history"]] == [None, 1, 1, 0]


def _image():
    """.text at 0x11000 (code), .rdata at 0x12000: a vtable of two code words, a lone table word."""
    text = b"\x90" * 0x200
    rdata = bytearray(0x40)
    struct.pack_into("<II", rdata, 0x00, 0x00011010, 0x00011020)     # a vtable
    struct.pack_into("<III", rdata, 0x10, 0x12345678, 0x00011030, 0x0000FFFF)   # one code word among data
    return recall.Image([(0x11000, text, True), (0x12000, bytes(rdata), False)])


def test_recall_categories_and_outcomes(tmp_path):
    work = tmp_path / "w"
    (work / "analysis" / "disasm").mkdir(parents=True)
    lines = [{"t": "run", "time": "2026-11-13T00:00:00Z"},
             {"t": "icall", "va": "0x00011010", "ret": "0x1"},     # vtable
             {"t": "icall", "va": "0x00011030", "ret": "0x1"},     # table word
             {"t": "icall", "va": "0x00011044", "ret": "0x1"},     # computed
             {"t": "run", "time": "2026-11-13T00:01:00Z"},
             {"t": "thread", "va": "0x00011080", "ctx1": "0x0"},   # thread start
             {"t": "thread_ctx", "va": "0x00011080", "routine": "0x0"},
             {"t": "unimpl", "va": "0x00011090", "insn": "x"}]
    (work / "discovered.jsonl").write_text("".join(json.dumps(l) + "\n" for l in lines) + '{"t":"icall","va"')
    (work / "discovered-seeds.json").write_text(json.dumps([{"start": "0x00011010"}, {"start": "0x00011080"}]))
    (work / "discovered-rejected.json").write_text(json.dumps(["0x00011044"]))
    (work / "analysis" / "disasm" / "functions.json").write_text(json.dumps(
        [{"start": "0x00011000", "end": "0x00011040", "name": "f"}]))
    r = recall.report(str(work), image=_image())
    assert r["misses"] == 4 and r["runs"] == 2 and r["seeded"] == 2 and r["rejected"] == 1
    assert r["categories"] == {"thread start": 1, "vtable": 1, "table word": 1, "computed jump": 1}
    assert r["inside_known"] == 1              # 0x11030, inside f and not seeded
    assert r["recall"] is None and recall.summary(r) == \
        "recall unknown (4 misses: 1 thread start, 1 vtable, 1 table word, 1 computed jump)"
    # With an icall-feedback dump: what the run reached and resolved, minus what runs taught the analysis.
    dump = tmp_path / "icall.txt"
    dump.write_text("# icall-feedback v1\n00011010 3\n00011100 1\n00011104 1\n00011108 1\n0001110C 1\n00011110 1\n")
    r = recall.report(str(work), icall_dump=str(dump), image=_image())
    assert r["found_statically"] == 5 and r["targets"] == 9 and r["recall"] == round(5 / 9, 3)
    assert recall.summary(r).startswith("56 % (4 misses")


def test_the_report_has_a_recall_column_when_a_line_has_one():
    rows = [{"source": "a", "stage": "boots", "blocker": "no flip", "toolkit": "abc",
             "recall": {"summary": "recall unknown (1 miss: 1 thread start)", "misses": 1}},
            {"source": "b", "stage": "sustained", "toolkit": "abc"}]
    t = report(rows).splitlines()
    assert t[0].endswith("| Recall |") and t[3].endswith("| recall unknown (1 miss: 1 thread start) |")
    assert "| Recall |" in matrix(rows)
    assert "Recall" not in report(rows[1:]) and "Recall" not in matrix(rows[1:])
