"""The blocker words (tools/compat/blockers.py) are one set for classify(),
heal.py and the app's SelfHeal: the strings here are SelfHealTest's
theBlockerWordsAreTheDesktops, word for word."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.compat import blockers, heal          # noqa: E402
from tools.compat.__main__ import classify       # noqa: E402
from tools.compat.test_compat import line        # noqa: E402

FIXTURE = [
    {"t": "smc", "va": "0x00400000", "kind": "exec_outside_image", "ret": "0x00011234", "where": "heap",
     "bytes": "5589E583EC10C390"},
    {"t": "smc", "va": "0x00012000", "kind": "code_write", "at": "0x00012345", "section": ".text", "by": "a store",
     "ret": "0x00011000"},
    {"t": "stub", "ord": 136, "name": "KeRemoveQueue", "ret": "0x00012345"},
    {"t": "stub", "ord": 206, "name": "NtQueueApcThread", "ret": "0x00012399"},
    {"t": "exception", "va": "0x00040000", "code": "0xC0000005",
     "why": "host fault on a guest address no guest handler took"},
    {"t": "exception", "va": "0x00011020", "code": "0xC0000094", "why": "divide by zero"},
]
EXC = "guest exception 0xC0000005 at 0x00040000: host fault on a guest address no guest handler took (+1)"
SMC = ("code written or run at run time x2: exec_outside_image 0x00400000 (heap, from 0x00011234), "
       "code_write 0x00012345 (.text)")
STUB = "calls stub ordinal 136 (KeRemoveQueue), ordinal 206 (NtQueueApcThread)"


def test_the_words():
    assert blockers.exception_words(blockers.of_kind(FIXTURE, "exception")) == EXC
    assert blockers.smc_words(blockers.of_kind(FIXTURE, "smc")) == SMC
    assert blockers.stub_words(blockers.of_kind(FIXTURE, "stub")) == STUB


def test_heal_and_classify_say_them():
    f = heal.finds(FIXTURE)
    assert heal.named_blocker(f) == EXC and heal.stub_words(f) == STUB
    assert heal.named_blocker(heal.finds(FIXTURE[1:2])) == "code written or run at run time x1: code_write 0x00012345 (.text)"
    st = [line(t, threads=2) for t in range(1000, 60001, 1000)]
    blocker = classify(st, [], ("signal", 11), 60, 60, "SIGSEGV\n", FIXTURE)[1]
    assert EXC in blocker and SMC in blocker, blocker
    hang = classify(st, [], None, 60, 60, "", FIXTURE)[1]
    assert EXC in hang and SMC in hang and STUB in hang, hang


def test_a_guessed_d3d_layout_is_named():
    """XBA-105's d3d_layout entries, after the blocker (not after a crash)."""
    disc = [{"t": "d3d_layout", "va": "0x1", "lib": "D3D8LTCG", "build": 5933, "rule": "fence_event", "used": "0x1DCC"},
            {"t": "d3d_layout", "va": "0x2", "lib": "D3D8LTCG", "build": 5933, "rule": "rpb_nodes", "used": "0x264"}]
    words = "D3D layout guessed for D3D8LTCG 5933: fence_event, rpb_nodes"
    assert blockers.d3d_layout_words(disc) == words
    st = [line(t, threads=2) for t in range(1000, 60001, 1000)]
    assert classify(st, [], None, 60, 60, "", disc)[1].endswith("; " + words)
    assert words not in classify(st, [], ("signal", 11), 60, 60, "SIGSEGV\n", disc)[1]
