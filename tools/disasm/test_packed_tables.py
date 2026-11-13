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
from tools.disasm.synthetic import (CRT_FALL_INTO, CRT_ROUTINES,     # noqa: E402
                                    DATA, X87_ROUTINES, Title,
                                    crt_math_table_title, packed_table_title)
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


# -- WP5: a CRT math table with no anchor (no seed, no other reference) -------

def _instruction_starts(t):
    """Every instruction boundary of the title's code, by a linear decode
    from each label (the fixture has no data in .text)."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    x = t.sections[0]
    t.va("entry")
    out = set()
    for off in x.labels.values():
        for i in md.disasm(bytes(x.buf[off:]), x.va + off):
            out.add(i.address)
            if i.mnemonic in ("ret", "jmp"):
                break
    return out


@pytest.mark.parametrize("al,fill", LAYOUTS)
@pytest.mark.parametrize("strict", (False, True))
def test_crt_math_table_without_seeds(tmp_path, al, fill, strict):
    """Every routine the table names is an entry (a start or an alias) at the
    first analysis, with no seed: the fall-into ones through the table's
    completion. No decoy is, and nothing starts inside an instruction."""
    t, decoys = crt_math_table_title(al, fill)
    funcs, s = analyse(t, str(tmp_path), discovery.FULL, imports=(), strict=strict)
    missing = [n for n in CRT_ROUTINES if t.va(n) not in funcs]
    assert not missing, (missing, s["discovery"])
    seeded = sorted(k for k, va in decoys.items() if va in funcs)
    assert not seeded, seeded
    starts = _instruction_starts(t)
    text = t.sections[0]
    inside = sorted("%08X" % a for a in funcs
                    if text.va <= a < text.va + len(text.buf) and a not in starts)
    assert not inside, inside
    assert s["discovery"]["refused"].get("packed table completion: inside an instruction", 0) >= 1
    # The completion made aliases (the code around them stays whole); a
    # fall-into entry the older gate already takes (a dword-aligned word in a
    # gap) is a start, as before.
    assert s["discovery"]["packed_completed"] >= 2, s["discovery"]
    for n in CRT_FALL_INTO:
        assert funcs[t.va(n)]["detection_method"] in ("tail_jump_alias", "packed_table"), \
            (n, funcs[t.va(n)])


def test_crt_math_table_needs_the_pass(tmp_path):
    """Level 1 (no packed-table pass) finds none of the fall-into routines:
    nothing else references them, so the test above measures the pass."""
    t, _ = crt_math_table_title()
    funcs, s = analyse(t, str(tmp_path), discovery.STRONG, imports=())
    assert not [n for n in CRT_FALL_INTO if t.va(n) in funcs]


def test_spread_run_is_not_trimmed(tmp_path):
    """A run whose passing words name code 64 KB apart (words read across a
    table of short strings, `"+3\\0\\0"` one byte in) keeps its refused end
    words: trimming is for a table whose handlers sit together, so this run
    stays refused (4 of 6) and nothing it names is added."""
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    x.label("entry").hex("558bec ebfe")
    for k in range(4):
        x.pad(0x10000).align(16)
        x.label("P%d" % k).hex("558bec 33c0 5d c3")
        x.label("T%d" % k).hex("d9fe c3")              # fsin; ret, packed after P's ret
    x.pad(0x10000).align(16)
    x.label("tail").hex("558bec 33c0 5d c3")
    x.label("last").hex("c3")
    r.raw(b"\x00\x00\x00")                             # every word at phase 3
    r.abs("entry", 1)                                  # refused (inside an instruction), far
    for k in range(4):
        r.abs("T%d" % k)
    r.abs("tail", 2)                                   # refused, far
    r.u32(0)
    t._layout()
    funcs, s = analyse(t, str(tmp_path), discovery.FULL, imports=(), strict=True)
    found = {k: funcs[t.va("T%d" % k)]["detection_method"] for k in range(4) if t.va("T%d" % k) in funcs}
    assert not found, (found, s["discovery"])
    assert s["discovery"]["refused"].get("packed table: run refused", 0) >= 1


def test_decoy_table_alone_is_not_completed(tmp_path):
    """A table of the same shape whose words all name the middle of paths
    has no established word: no run of it is taken, nothing is completed."""
    from tools.disasm.synthetic import CRT_DECOY_WORDS
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    x.label("entry").hex("558bec ebfe").align(16)
    x.label("M_helper").hex("d9ea dec9 33db f6d3 c3").align(16)
    x.label("M_a").hex("c68570ffffffff").call("M_helper").hex("0adb 7402 d9e8 c3 d9e0 c3")
    x.align(16)
    x.label("M_b").hex("c68570fffffffe").call("M_helper").hex("0adb 7502 d9e1 c3 d9ee c3")
    x.align(16)
    x.label("M_c").hex("dbbd62ffffff dbad62ffffff c3").align(16)
    x.label("M_e").hex("d9c0 d9e1").call("M_helper").hex("0adb 7403 dee9 c3 ddd8 c3")
    x.align(16)
    x.label("M_or").hex("0ac9 0f8500000000 ddd8 d9ee c3").align(16)
    x.label("last").hex("c3")
    r.raw(b"\x00\x00")
    for n, row in enumerate(CRT_DECOY_WORDS):
        r.raw(b"\x04dec%d" % n + b"\0" * 8).raw(b"\x01").raw(bytes([n, 0]))
        for name, add in row:
            r.abs(name, add)
    r.u32(0)
    t._layout()
    words = {t.va(name) + add for row in CRT_DECOY_WORDS for name, add in row}
    funcs, s = analyse(t, str(tmp_path), discovery.FULL, imports=(), strict=True)
    assert not sorted("%08X" % a for a in funcs if a in words)
    assert s["discovery"]["added"]["packed_table"] == 0
