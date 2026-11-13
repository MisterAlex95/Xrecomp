"""A body cut before its terminator ends in a recomp_cut_body trap (WP2).

Fable's sub_001C1190 was measured to 0x001C120C, the end of a `mov eax,
[esp+0x10]`, because a wild seed started a function two bytes into that mov.
Its translation fell off the bottom of the C function: no loop back edge, no
pops, no `ret 8`, the guest stack 20 bytes off for every caller. The lifter
now ends such a body (no terminator, no function start at its end) with a
call that stops the title with a report, and the batch counts them.
"""
import os
import tempfile

import pytest

from . import config
from .lifter import Lifter
from .translator import BatchTranslator, FunctionTranslator

BASE = 0x10000
# push ebp / push edi / push ebx / mov eax, [esp+0x10]   -- cut here
CUT = bytes.fromhex("555753 8b442410")
TAIL = bytes.fromhex("43 5b 5f 5d c20800")   # inc ebx / pop ebx / pop edi / pop ebp / ret 8
LAST = BASE + 3


@pytest.fixture(autouse=True)
def restore_layout(monkeypatch):
    monkeypatch.setattr(config, "_SECTIONS", list(config._SECTIONS))


def _translator(image, placements):
    config._install(
        [config.Section(".text", BASE, 0x100, 0, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="cut-body-test")
    db = {}
    for va, size in placements:
        db[va] = {"start": f"0x{va:08X}", "end": va + size, "_addr": va,
                  "size": size, "name": f"sub_{va:08X}"}
    return FunctionTranslator(image.ljust(0x100, b"\xcc"), db), db


def test_cut_body_ends_in_the_trap():
    image = CUT + TAIL
    t, db = _translator(image, [(BASE, len(CUT))])
    code = t.translate_function(BASE, db[BASE])
    assert f"recomp_cut_body(0x{LAST:08X}u); return;" in code
    # The trap is the last statement: nothing falls off after it.
    body = code[:code.rindex("}")].rstrip()
    assert body.splitlines()[-1].strip().startswith(f"recomp_cut_body(0x{LAST:08X}u)")
    assert t.cut_bodies == {BASE: LAST}


def test_whole_body_has_no_trap():
    image = CUT + TAIL
    t, db = _translator(image, [(BASE, len(image))])
    code = t.translate_function(BASE, db[BASE])
    assert "recomp_cut_body" not in code
    assert t.cut_bodies == {}


def test_body_split_at_a_function_start_falls_through_instead():
    image = CUT + TAIL
    t, db = _translator(image, [(BASE, len(CUT)), (BASE + len(CUT), len(TAIL))])
    code = t.translate_function(BASE, db[BASE])
    assert "recomp_cut_body" not in code
    assert f"/* fallthrough 0x{BASE + len(CUT):08X} */" in code
    assert t.cut_bodies == {}


class _FakeTranslator:
    def __init__(self, cut):
        self.owned_function_starts = set()
        self.lifter = Lifter()
        self.lifter.referenced_calls = {}
        self.cut_bodies = cut

    def translate_function(self, addr, func_info):
        return f"void {func_info['name']}(void) {{}}"


def test_batch_counts_cut_bodies(capsys):
    from .__main__ import CUT_BODIES_PREFIX, _report_cut_bodies
    funcs = [(a, {"name": f"sub_{a:08X}"}) for a in (0x1000, 0x2000, 0x3000)]
    batch = BatchTranslator.__new__(BatchTranslator)
    batch.translator = _FakeTranslator({0x2000: 0x2010, 0x3000: 0x3004, 0x9000: 0x9001})
    with tempfile.TemporaryDirectory() as out:
        stats = batch.translate_batch_split(funcs, out, manual={0x3000})
        assert os.path.exists(os.path.join(out, "recomp_funcs.h"))
    # Only lifted bodies count: 0x3000 is hand-written, 0x9000 not in the list.
    assert stats["cut_bodies"] == {0x2000: 0x2010}
    capsys.readouterr()
    _report_cut_bodies(stats)
    err = capsys.readouterr().err
    assert err.startswith(f"{CUT_BODIES_PREFIX} 1: sub_00002000 (last 0x00002010)")


def test_call_into_padding_is_trapped_but_not_counted():
    # push 4 / call [abort] / int3: a call that does not return.
    body = bytes.fromhex("6a04 ff1500100100")
    t, db = _translator(body + b"\xcc", [(BASE, len(body))])
    code = t.translate_function(BASE, db[BASE])
    assert f"recomp_cut_body(0x{BASE + 2:08X}u)" in code
    assert t.cut_bodies == {}
