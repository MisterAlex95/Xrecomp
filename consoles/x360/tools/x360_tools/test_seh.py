"""seh_seeds (XBA-282): the scope tables' code becomes discovery seeds; a title without exception records gets none."""

import os
import sys

from . import CONSOLE  # noqa: F401
from tools.xex.xex import parse_xex

from .seh import seh_seeds

TESTS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests"))


def test_scope_tables_give_seeds():
    sys.path.insert(0, TESTS)
    import make_seh
    img = parse_xex(make_seh.build())
    imports, rdata, addr = make_seh.layout(0)
    words, funcs, labels = make_seh.program(make_seh.layout(len(make_seh.program(addr)[0]))[2])
    want = {labels[n] for n in ("filter1", "except1", "filter2", "except2", "finally_mid")}
    assert seh_seeds(img) == want


def test_no_records_no_seeds():
    sys.path.insert(0, TESTS)
    import make_hello
    assert seh_seeds(parse_xex(make_hello.build("none"))) == set()   # bit 31 set, but no handler words
