"""--split-bytes (XBA-209): chunks cut by size at boundaries a re-lift keeps,
each one self-contained (its own declarations, no recomp_funcs.h)."""

import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.translator import chunk_declarations, stable_chunks  # noqa: E402


def _title(n, seed=1):
    rnd = random.Random(seed)
    out, addr = [], 0x11000
    for _ in range(n):
        size = rnd.choice((300, 800, 2000, 5000, 12000))
        out.append((addr, "sub_%08X" % addr, "x" * size))
        addr += rnd.randrange(16, 400)
    return out


def _texts(chunks):
    return {tuple(a for a, _, _ in c) for c in chunks}


def test_sizes_stay_in_bounds():
    t = _title(4000)
    chunks = stable_chunks(t, 200000)
    assert [x for c in chunks for x in c] == t          # every function once, in order
    sizes = [sum(len(code) + 1 for _, _, code in c) for c in chunks]
    assert all(s <= 300000 + 12001 for s in sizes)      # 1.5 x the target, plus the function that crossed it
    assert all(s >= 150000 for s in sizes[:-1])         # 3/4 of it, the last chunk apart
    mean = sum(sizes) / len(sizes)
    assert 150000 < mean < 300000


def test_an_added_function_moves_few_boundaries():
    """--split N shifts every chunk after the new function; here only its own chunk (and rarely the next) change."""
    t = _title(4000)
    before = stable_chunks(t, 200000)
    for at in (37, 1500, 3900):
        a = t[at][0]
        t2 = t[:at + 1] + [(a + 1, "sub_%08X" % (a + 1), "y" * 2000)] + t[at + 1:]
        after = stable_chunks(t2, 200000)
        changed = len(_texts(after) - _texts(before))
        assert changed <= 3, (at, changed, len(before))
        dropped = t[:at] + t[at + 1:]                   # and one gone
        assert len(_texts(stable_chunks(dropped, 200000)) - _texts(before)) <= 3


def test_declarations_are_the_names_used():
    declared = {"sub_00011000": "void sub_00011000(void);", "sub_00011040": "void sub_00011040(void);",
                "hook": "void hook(void);", "unused": "void unused(void);"}
    chunk = [(0x11000, "sub_00011000", "void sub_00011000(void)\n{\n    RECOMP_ABI_CALL(0x00011040u, sub_00011040);"
                                       "\n    hook(); /* sub_000110400 is not a name */\n}\n")]
    assert chunk_declarations(chunk, declared) == [
        "void hook(void);", "void sub_00011000(void);", "void sub_00011040(void);"]
