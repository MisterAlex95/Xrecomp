"""hle_coverage (XBA-270): the imports of a XEX against what the runtime implements."""

import os
import sys

from . import CONSOLE  # noqa: F401
from tools.xex.synth import build_xex
from tools.xex.xex import parse_xex

from .exports import all_exports, ordinal
from .hle_coverage import coverage, implemented, main, summary

TESTS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests"))


def test_runtime_table_is_read():
    have = implemented()
    for n in ("DbgPrint", "NtCreateFile", "RtlEnterCriticalSection", "XboxHardwareInfo", "KeAcquireSpinLockAtRaisedIrql"):
        assert ("xboxkrnl", n) in have, n
    assert ("xam", "XamLoaderTerminateTitle") in have
    assert all(ordinal(lib, n) is not None for lib, n in have)  # every name the HLE claims is a real export


def test_title_coverage(capsys, tmp_path):
    sys.path.insert(0, TESTS)
    import make_hello
    img = parse_xex(make_hello.build())
    rows = coverage(img)
    assert rows and all(r[4] for r in rows), [r for r in rows if not r[4]]
    have = implemented()
    missing = sorted(n for lib, n, k in all_exports() if lib == "xam" and k == "FN" and (lib, n) not in have)[:2]
    assert len(missing) == 2, "the test needs two exports without an HLE"
    data, _ = build_xex([0x4E800020], imports={"xam.xex": [(ordinal("xam", n), "thunk") for n in missing]},
                        functions=[(0, 4)])
    rows = coverage(parse_xex(data))
    assert sorted((r[2], r[4]) for r in rows) == [(n, False) for n in missing]
    path = tmp_path / "t.xex"
    path.write_bytes(data)
    assert main([str(path)]) == 0
    assert "imports: 2, covered by the HLE: 0, missing: 2" in capsys.readouterr().out


def test_tables_outside_the_kernel_are_read():
    have = implemented()
    assert ("xboxkrnl", "XMACreateContext") in have  # x360_xma.c's table
    assert ("xam", "XamInputGetState") in have       # x360_input.c, through the header's macro table
    totals = summary()
    assert totals["xboxkrnl"]["function"][0] == len([1 for lib, n, k in all_exports()
                                                     if lib == "xboxkrnl" and k == "FN" and (lib, n) in have])
