"""A call to a routine that never returns, padded with int3, ends a body.

exit(), _CxxThrowException and an XDK library's ExitThread read as returning
functions; the compiler knows better and emits nothing after a call to them,
so the alignment int3 lands right behind the call that ends a function.
Static discovery takes such a call as the end of the body (discovery.py
noreturn_targets), so the function packed after it starts cleanly and an
immediate naming it is a function. One title had seven of these, two reached
at run time (one after `call exit`, one after `call _CxxThrowException`): on
a synthetic title of our own (synthetic.py noreturn_int3_title) they are
found, and the bodies where the same calls go on stay whole.
"""

import contextlib
import io
import os
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("capstone")

from tools.disasm import discovery                                                # noqa: E402
from tools.disasm.synthetic import NORETURN_INT3_IMPORTS, noreturn_int3_title    # noqa: E402
from tools.disasm.test_static_discovery import LAYOUTS, analyse                   # noqa: E402

ROUTINES = ("T_after_exit", "T_getter", "T_crt")
# Function -> where its body ends: at the noreturn call for the three in front
# of a routine, past the code after it for the three that go on.
ENDS = {"quit": "quit_end", "fail": "fail_end", "end_thread": "end_thread_end",
        "keep_going": "keep_going_end", "trapped": "trapped_end",
        "after_code": "after_code_end"}
INSIDE = ("K_ok", "TR_trap", "TR_rest", "AC_rest")


def end_of(funcs, t, name):
    return int(funcs[t.va(name)]["end"], 16)


@pytest.mark.parametrize("al,fill", LAYOUTS)
def test_the_function_after_a_padded_noreturn_call_starts_cleanly(tmp_path, al, fill):
    t = noreturn_int3_title(al, fill)
    on, _ = analyse(t, str(tmp_path), discovery.FULL, imports=NORETURN_INT3_IMPORTS)
    for name in ROUTINES:
        assert t.va(name) in on, name
    for func, end in ENDS.items():
        assert end_of(on, t, func) == t.va(end), func
    # Nothing in the bodies that go on became a start.
    for name in INSIDE:
        assert t.va(name) not in on, name


def test_strict_finds_them_too(tmp_path):
    t = noreturn_int3_title()
    on, _ = analyse(t, str(tmp_path), discovery.FULL, imports=NORETURN_INT3_IMPORTS,
                    strict=True)
    assert all(t.va(n) in on for n in ROUTINES)
    assert not [n for n in INSIDE if t.va(n) in on]


def test_the_passes_before_missed_them(tmp_path):
    """Level 0: the body runs on over the int3 and swallows the routine."""
    t = noreturn_int3_title()
    off, _ = analyse(t, str(tmp_path), discovery.OFF, imports=NORETURN_INT3_IMPORTS)
    assert t.va("T_after_exit") not in off
    assert end_of(off, t, "quit") > t.va("T_after_exit")


def test_the_targets_are_the_padded_ones(tmp_path):
    """do_exit, throw_fn and exit_thread are noreturn targets; helper and
    doexit_impl, called and followed by code only, are not."""
    from tools.disasm.disasm import Disassembler
    from tools.xbe_parser.xbe_parser import XBEParser, export_json
    t = noreturn_int3_title()
    xbe = t.write(os.path.join(str(tmp_path), "default.xbe"), "entry",
                  imports=NORETURN_INT3_IMPORTS)
    xjson = xbe[:-4] + "_analysis.json"
    export_json(XBEParser(xbe).parse(), xjson)
    dis = Disassembler(xbe, analysis_json=xjson, output_dir=str(tmp_path / "out"),
                       force=True, static_discovery=discovery.FULL)
    with contextlib.redirect_stdout(io.StringIO()):
        assert dis.run()
    targets = dis.func_detector.discovery.noreturn_targets
    assert {t.va(n) for n in ("do_exit", "throw_fn", "exit_thread")} <= targets
    assert not {t.va(n) for n in ("helper", "doexit_impl")} & targets
