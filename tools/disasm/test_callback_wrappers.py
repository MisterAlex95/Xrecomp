"""Thread routines handed to a library's CreateThread (GitHub issue 1).

An XDK title never calls PsCreateSystemThreadEx itself: its CreateThread
does, with the library's startup as the routine and its own lpStartAddress
argument as StartContext1. Static discovery follows a callback argument
through such a wrapper (discovery.py callback_args), so the start address a
caller pushes for it is strong evidence: on a synthetic title of our own
(synthetic.py thread_wrapper_title) every routine is a function at the first
analysis, where the passes before left them inside the body packed in front.
"""

import os
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("capstone")

from tools.disasm import discovery                                       # noqa: E402
from tools.disasm.synthetic import WRAPPER_IMPORTS, thread_wrapper_title  # noqa: E402
from tools.disasm.test_static_discovery import LAYOUTS, analyse          # noqa: E402

ROUTINES = ("T_start", "T_start2", "T_start3", "T_start4")


@pytest.mark.parametrize("al,fill", LAYOUTS)
def test_start_addresses_pushed_for_a_wrapper_are_functions(tmp_path, al, fill):
    t = thread_wrapper_title(al, fill)
    on, summary = analyse(t, str(tmp_path), discovery.FULL, imports=WRAPPER_IMPORTS)
    for name in ROUTINES:
        assert t.va(name) in on, name
        assert on[t.va(name)]["detection_method"] == "callback_arg", name
    # Each one cut the body packed in front of it at the call that never returns.
    for n in ("", "2", "3", "4"):
        assert int(on[t.va("end_thread" + n)]["end"], 16) == t.va("T_start" + n)
    assert summary["discovery"]["added"]["callback_arg"] >= len(ROUTINES)
    # create_thread, create_thread_esp, begin_thread; not note_ctx (a context).
    assert summary["discovery"]["callback_wrappers"] == 3
    # The routine that ends in the call that never returns, the next function
    # packed after it: with padding in front a boundary pass has it, without
    # (al 1) only this evidence does -- no body closes there for the others.
    assert t.va("T_open") in on
    if al == 1:
        assert on[t.va("T_open")]["detection_method"] == "callback_arg"
    assert int(on[t.va("T_open")]["end"], 16) == t.va("after_open")
    # What is not a start address stays out: a data address pushed as one,
    # and a word pushed for a wrapper that forwards it as StartContext2.
    assert t.va("ptd") + 8 not in on
    assert t.va("D_note") + 1 not in on


def test_the_passes_before_missed_them(tmp_path):
    """Level 0 (and the kernel-call-only rule): an immediate naming code
    inside a measured body is no evidence, so the routines do not exist and
    their threads end at once (« [ICALL] Failed to resolve VA »)."""
    t = thread_wrapper_title()
    off, _ = analyse(t, str(tmp_path), discovery.OFF, imports=WRAPPER_IMPORTS)
    assert not [n for n in ROUTINES if t.va(n) in off]
    packed = thread_wrapper_title(1)
    off, _ = analyse(packed, str(tmp_path / "packed"), discovery.OFF, imports=WRAPPER_IMPORTS)
    assert not [n for n in ROUTINES + ("T_open",) if packed.va(n) in off]


def test_strict_keeps_them(tmp_path):
    t = thread_wrapper_title()
    strict, _ = analyse(t, str(tmp_path), discovery.FULL, imports=WRAPPER_IMPORTS, strict=True)
    assert all(t.va(n) in strict for n in ROUTINES)
    assert t.va("D_note") + 1 not in strict
