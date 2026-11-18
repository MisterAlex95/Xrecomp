"""Reservations (XBA-283): lwarx / stwcx. and ldarx / stdcx. lifted through RC_LWARX / RC_STWCX (xenon_rt.h). The
store happens and CR0 is EQ when the word still holds what the reserving load read; a store to it in between (what
another thread does) makes the conditional store fail and leave memory alone; a stwcx. with no reservation fails."""

import shutil
import subprocess
import sys

import pytest

from . import CONSOLE  # noqa: F401
from tools.xex.synth import build_xex
from tools.xex.xex import parse_xex

from .recomp import RUNTIME_DIRS, recompile
from .test_x360_mem import BASE, BLR, HARNESS, MFCR_R8, PATTERN, TEXT, d, ds, x

LWARX = x(7, 0, 3, 20)          # lwarx r7, 0, r3
STWCX = x(5, 0, 3, 150, 1)      # stwcx. r5, 0, r3
LDARX = x(7, 0, 3, 84)          # ldarx r7, 0, r3
STDCX = x(5, 0, 3, 214, 1)      # stdcx. r5, 0, r3
STW_R4 = d(36, 4, 3, 0)         # stw r4, 0(r3): another writer
STD_R4 = ds(62, 4, 3, 0, 0)     # std r4, 0(r3)

CASES = [  # name, body, r4, what the word at BASE holds after, CR0 EQ
    ("lwarx+stwcx", [LWARX, STWCX], 0, "new4", 1),
    ("lwarx+other store+stwcx", [LWARX, STW_R4, STWCX], 0x0BADF00D, "r4w", 0),
    ("lwarx+same value+stwcx", [LWARX, d(32, 4, 3, 0), STW_R4, STWCX], 0, "new4", 1),  # lwz r4, 0(r3); stw r4 back
    ("stwcx alone", [STWCX], 0, "old", 0),
    ("ldarx+stdcx", [LDARX, STDCX], 0, "new8", 1),
    ("ldarx+other store+stdcx", [LDARX, STD_R4, STDCX], 0x0BADF00D0BADF00D, "r4d", 0),
]
NEW = 0x1122334455667788


@pytest.mark.skipif(not shutil.which("cc") or not sys.platform.startswith("linux"), reason="no C compiler / not Linux")
def test_reservations(tmp_path):
    words, funcs = [], []
    for _, body, _, _, _ in CASES:
        funcs.append((4 * len(words), 4 * (len(body) + 2)))
        words.extend(body + [MFCR_R8 & ~(31 << 21) | (8 << 21), BLR])
    data, _ = build_xex(words, functions=funcs, compression="none")
    src = tmp_path / "src"
    recompile(parse_xex(data), str(src))
    text = "".join(p.read_text() for p in sorted(src.glob("x360_rc_[0-9]*.c")))
    assert "RC_LWARX" in text and "RC_STWCX" in text and "RC_LDARX" in text and "RC_STDCX" in text
    assert "xe_rc_features = 1u" in (src / "x360_rc_table.c").read_text()
    calls = []
    for (off, _), (_, _, r4, _, _) in zip(funcs, CASES):
        # r3 = BASE, r5 = NEW; the harness prints r7, r3, CR, f7 and memory 0xF0..0x130 (BASE is at 0x100)
        calls.append("    c(0x%08Xu, 0x%XULL, 0x%XULL, 0x%XULL, 0, 0);" % (TEXT + off, BASE, r4, NEW))
    (tmp_path / "h.c").write_text(HARNESS % {"pattern": ",".join(str(b) for b in PATTERN), "calls": "\n".join(calls)})
    inc = []
    for dd in RUNTIME_DIRS:
        inc += ["-I", dd]
    exe = str(tmp_path / "t")
    subprocess.run(["cc", "-std=gnu11", "-O1", "-w", "-ffp-contract=off"] + inc + ["-o", exe, str(tmp_path / "h.c")] +
                   sorted(str(p) for p in src.glob("*.c")) + ["-lm"], check=True)
    out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout.split("\n")
    for (name, _, r4, after, eq), line in zip(CASES, out):
        rc, _, _, cr, _, memhex = line.split()
        assert rc == "0", (name, line)
        word = bytes.fromhex(memhex)[0x10:0x18]
        old = PATTERN[0x100:0x108]
        want = {"new4": NEW.to_bytes(8, "big")[4:] + old[4:], "new8": NEW.to_bytes(8, "big"),
                "r4w": (r4 & 0xFFFFFFFF).to_bytes(4, "big") + old[4:], "r4d": r4.to_bytes(8, "big"),
                "old": old}[after]
        assert word == want, (name, word.hex(), want.hex())
        assert (int(cr, 16) >> 29) & 1 == eq, (name, cr)
