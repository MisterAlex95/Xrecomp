"""Static discovery of packed pointer tables (XBA-140): records whose function
pointer is off a dword boundary, naming frameless routines (the MSVC CRT's
x87 math dispatch is the case that prompted it). On synthetic titles of our
own (tools/disasm/synthetic.py): every routine is a function at the first
analysis, no decoy is, and the decoy corpus stays clean.

    python -m pytest tools/disasm/test_packed_tables.py -s     (-s: the report)
"""

import os
import random
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("capstone")

from tools.disasm import discovery                                   # noqa: E402
from tools.disasm.synthetic import (DATA, X87_ROUTINES, Title,       # noqa: E402
                                    packed_table_title)
from tools.disasm.test_static_discovery import LAYOUTS, _decoy_title, analyse   # noqa: E402

ROUTINES = [n for n, _ in X87_ROUTINES] + ["X_in1", "X_in2"]


def _named_only_unaligned(t):
    """The routines no aligned .rdata word names: the older table-word pass
    cannot see them."""
    r = t.sections[1]
    t.va("entry")                       # laid out
    aligned = {name for off, _, name, add in r.fixups if (r.va + off) % 4 == 0 and not add}
    return sorted(n for n in ROUTINES if n not in aligned)


@pytest.mark.parametrize("al,fill", LAYOUTS)
@pytest.mark.parametrize("strict", (False, True))
def test_packed_table_routines_are_functions(tmp_path, al, fill, strict):
    t, decoys = packed_table_title(al, fill)
    on, s = analyse(t, str(tmp_path / "on"), discovery.FULL, imports=(), strict=strict)
    missing = [n for n in ROUTINES if t.va(n) not in on]
    assert not missing, missing
    seeded = sorted(k for k, va in decoys.items() if va in on)
    assert not seeded, seeded
    assert s["discovery"]["added"]["packed_table"] >= 5, s["discovery"]
    # owner keeps its body: the routines inside it are aliases, not cuts.
    assert int(on[t.va("owner")]["end"], 16) > t.va("O_tail")
    # Without the pass (level 1: strong evidence only) the packed words are
    # never read; the routines that a word happens to name at an aligned
    # address are the older table-word pass's, the rest are lost.
    off, s1 = analyse(t, str(tmp_path / "off"), discovery.STRONG, imports=(), strict=strict)
    assert s1["discovery"]["added"]["packed_table"] == 0
    assert not [n for n in _named_only_unaligned(t) if t.va(n) in off]


def test_the_fixture_is_packed():
    """The fixture means what it says: the tables' pointer fields fall at
    every phase of a dword, and every decoy word is off a dword boundary."""
    t, decoys = packed_table_title()
    r = t.sections[1]
    phases = {(r.va + off) % 4 for off, _, name, _ in r.fixups if name.startswith("X_")}
    assert phases == {0, 1, 2, 3}
    decoy_words = [r.va + off for off, _, name, add in r.fixups if not name.startswith("X_")
                   or add]
    assert decoy_words and all(va % 4 for va in decoy_words)


def test_levels(tmp_path):
    """Level 2 (the default) only: the evidence is a run of data words, not
    the strong evidence of level 1."""
    t, _ = packed_table_title()
    lost = _named_only_unaligned(t)
    assert len(lost) >= 5
    for lvl in (discovery.OFF, discovery.STRONG):
        funcs, s = analyse(t, str(tmp_path / str(lvl)), lvl, imports=())
        assert not [n for n in lost if t.va(n) in funcs]
    capped, s = analyse(t, str(tmp_path / "cap"), discovery.FULL, imports=(), cap=0)
    assert s["discovery"]["added"]["packed_table"] == 0 and s["discovery"]["capped"]


def test_short_runs_and_single_words_stay_out(tmp_path):
    """Two records are not a table, nor are three naming one routine."""
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    x.label("entry").hex("558bec ebfe").align(16)
    x.label("prev").hex("33c0 c3")
    x.label("A").hex("d9fe c3")
    x.label("B").hex("d9ff c3")
    x.label("C").hex("d9fa c3").align(16)
    x.label("last").hex("c3")
    # Every word at phase 1, records of 8 bytes: no aligned word for the
    # older table-word pass.
    r.raw(b"\x01").abs("A").raw(b"\x00" * 4).abs("B")        # two records
    r.raw(b"\x00" * 8)
    for _ in range(3):                                          # one routine
        r.abs("C").raw(b"\x00" * 4)
    assert all((r.va or 0) % 4 == 0 and off % 4 == 1 for off, *_ in r.fixups)
    funcs, s = analyse(t, str(tmp_path), discovery.FULL, imports=())
    assert not [n for n in ("A", "B", "C") if t.va(n) in funcs], s["discovery"]
    assert s["discovery"]["refused"].get("packed table: run refused", 0) >= 1


def _packed_decoy_title(seed):
    """_decoy_title (test_static_discovery.py) plus data shaped like packed
    tables but naming no entry: runs of 3 to 8 words at a random stride and
    phase into the bytes of the code that are not entries, strings with an
    address inside, colours and floats packed at odd offsets."""
    t, words = _decoy_title(seed)
    rnd = random.Random(1000 + seed)
    r = t.sections[1]
    pool = sorted(words)
    for _ in range(6):
        stride = rnd.randrange(4, 17)
        while len(r.buf) % 4 == 0:
            r.raw(bytes([rnd.randrange(256)]))
        for _ in range(rnd.randrange(3, 9)):
            r.u32(rnd.choice(pool)).raw(bytes(rnd.randrange(32, 127) for _ in range(stride - 4)))
        r.raw(b"\xff\xff\xff\xff")
        r.raw(b"Menu\\").u32(rnd.choice(pool)).raw(b"Back\0")
        r.raw(b"\x00")
        for _ in range(8):
            r.u32(rnd.randrange(1 << 24) << 8 | rnd.randrange(256))     # colours
        r.raw(b"\x00\x00\x00")
        for _ in range(8):
            r.f32(rnd.uniform(-1000, 1000))
    t._layout()
    return t, words


def test_packed_decoy_corpus(tmp_path, capsys):
    """No packed-table decoy is seeded: across 40 decoy titles with packed
    decoy runs, level 2 adds no start or alias at a decoy address, and the
    pass is what it would be with it off (level 1 + the gap pass is not
    selectable alone, so the comparison is the run's packed_table count)."""
    seeded = decoys = runs_refused = taken = 0
    for seed in range(40):
        t, words = _packed_decoy_title(seed)
        funcs, s = analyse(t, str(tmp_path / str(seed)), discovery.FULL, imports=(), strict=True)
        bad = sorted(a for a in funcs if a in words)
        assert not bad, (seed, ["%08X %s" % (a, funcs[a]["detection_method"]) for a in bad])
        decoys += len(words)
        seeded += len(bad)
        taken += s["discovery"]["added"]["packed_table"]
        runs_refused += s["discovery"]["refused"].get("packed table: run refused", 0)
    assert taken == 0
    with capsys.disabled():
        print("\n  packed decoy corpus: 40 titles, %d decoy addresses, %d seeded, "
              "%d packed-table starts, %d runs refused" % (decoys, seeded, taken, runs_refused))


def test_growth_report(tmp_path, capsys):
    """Functions (starts and aliases) on the fixture, level 1 -> level 2:
    the growth is the routines only the packed words name."""
    rows = []
    for al, fill in LAYOUTS:
        t, _ = packed_table_title(al, fill)
        off, _ = analyse(t, str(tmp_path / ("1-%d%x" % (al, fill))), discovery.STRONG, imports=())
        on, s = analyse(t, str(tmp_path / ("2-%d%x" % (al, fill))), discovery.FULL, imports=())
        found = lambda fs: sum(1 for n in ROUTINES if t.va(n) in fs)         # noqa: E731
        rows.append((al, fill, len(off), len(on), found(off), found(on),
                     s["discovery"]["added"]["packed_table"]))
        assert len(on) - len(off) <= len(ROUTINES)
    with capsys.disabled():
        print("\n  packed tables, level 1 -> 2 (functions; routines found of %d; packed_table):"
              % len(ROUTINES))
        for al, fill, f0, f1, r0, r1, p in rows:
            print("    align %2d fill %02X: %2d -> %2d functions, routines %2d -> %2d, packed_table %d"
                  % (al, fill, f0, f1, r0, r1, p))
