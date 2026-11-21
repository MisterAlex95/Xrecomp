"""The gap sweep steps over a known entry by its decode, not by a byte scan.

SpikeOut: Battle Street (GitHub #3): a function reached only by a conditional tail branch (an alias entry)
sat alone at the start of its section, `call ; push 0x3CCC40 ; push 0 ;
call stdcall(8) ; xor eax, eax ; ret`. The sweep skipped the known entry by
looking for the next padding byte and took the 0xCC inside the push's
immediate for an int3, so it went on three bytes into that push, found a
closed body there (`cmp al, 0 ; push 0 ; call ; xor ; ret`) and made it a
function. The build then cut the real function at that start: its lifted
body lost the `push 0x3CCC40`, the stdcall callee popped a word that was
never pushed, and the caller's loop came back with ebx and esi restored from
the wrong slots: a null call spun forever after the Story mode's first
movie, black screen. No game bytes here: the shape is rebuilt by hand.
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

from tools.disasm import discovery                       # noqa: E402
from tools.disasm.synthetic import Title, CODE           # noqa: E402


def _title(body):
    t = Title()
    text = t.section(".text")
    text.label("entry").hex("558bec").call("caller").hex("ebfe").align(16)
    # Reaches the target by a conditional tail branch only: an alias entry.
    text.label("caller").hex("85c0 0f85").rel("target").hex("c3").align(16)
    text.label("helper").hex("b801000000 c3").align(16)
    text.label("stdcall8").hex("8b442404 c20800").align(16)
    ext = t.section("sty_exte", flags=CODE)
    body(ext.label("target")).align(16)
    ext.label("other").hex("558bec b802000000 5d c3").align(16)
    return t


def _analyse(t, tmp_path):
    from tools.xbe_parser.xbe_parser import XBEParser, export_json
    from tools.disasm.disasm import Disassembler
    xbe = t.write(os.path.join(tmp_path, "default.xbe"), "entry")
    xjson = xbe[:-4] + "_analysis.json"
    export_json(XBEParser(xbe).parse(), xjson)
    out = os.path.join(tmp_path, "out")
    with contextlib.redirect_stdout(io.StringIO()):
        assert Disassembler(xbe, analysis_json=xjson, output_dir=out, force=True,
                            static_discovery=discovery.FULL).run()
    return {int(f["start"], 16): f for f in json.load(open(os.path.join(out, "functions.json")))}


def _cc_in_push(s):
    # call ; push 0x3CCC40 (a 0xCC in the immediate) ; push 0 ; call ; xor ; ret
    return s.call("helper").hex("6840cc3c00 6a00").call("stdcall8").hex("33c0 c3")


def _zeros_in_mov(s):
    # The other shape, also in SpikeOut: `mov dword [eax+0x10], 0` ends in
    # four zero bytes, which the scan took for zero fill, and the bytes after
    # it (a 16-byte boundary) became a function inside this one.
    return (s.hex("a1 00700100 83784 00b c7401000000000 7515")
            .hex("6882000000").call("helper").hex("83c404 b901000000 33c0 c3")
            .hex("b802000000 c3"))


@pytest.mark.parametrize("body", (_cc_in_push, _zeros_in_mov))
def test_no_function_starts_inside_the_entry(tmp_path, body):
    t = _title(body)
    funcs = _analyse(t, str(tmp_path))
    target, end = t.va("target"), t.va("other")
    inside = [a for a in funcs if target < a < end]
    assert not inside, ["%08X %s" % (a, funcs[a]["detection_method"]) for a in inside]
