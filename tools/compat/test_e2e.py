"""End to end on a synthetic XBE of our own (no game data): prepare, and with
XBA_E2E=1 also build and boot it through the compat runner.

The XBE (tools.xbe_ident's make_xbe) has a retail-encoded entry point, an
empty kernel thunk table and a library table naming XAPILIB, D3D8 and
DSOUND, so the XDK profile layers apply and their C goes into the project;
its code is one function, `jmp $`.

- Always: tools.prepare --no-build lifts it: the generated C defines
  xbe_entry_point, and src/title holds the layers' C and xba_layers.c.
- XBA_E2E=1 (needs CMake, a compiler and SDL2, as in tools/docker's images):
  the full prepare builds the title against the runtime, and tools.compat
  boots it headless: the runtime starts and writes its status file. A loop
  that never flips is "prepares" with blocker "no flip".
"""

import json
import os
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("capstone")

from tools.xbe_ident.test_xbe_ident import make_xbe     # noqa: E402

LIBS = (("XAPILIB", 5849, 0), ("D3D8", 5849, 0), ("DSOUND", 5849, 0))
SPIN = b"\x55\x8b\xec\xeb\xfe"          # push ebp; mov ebp, esp; jmp $


def _xbe(tmp_path):
    d = tmp_path / "disc"
    d.mkdir()
    return make_xbe(d / "default.xbe", libs=LIBS, text=SPIN, title_id=0x58424101, title="Smoke")


def _prepare(xbe, work, *extra):
    return subprocess.run([sys.executable, "-m", "tools.prepare", xbe, "--work", str(work)] + list(extra),
                          cwd=ROOT, capture_output=True, text=True, timeout=900)


def test_prepare_lifts_a_synthetic_title(tmp_path):
    work = tmp_path / "work"
    r = _prepare(_xbe(tmp_path), work, "--no-build")
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    assert "profile layers: xdk/d3d8 xdk/dsound xdk/xapi" in r.stdout
    gen = work / "project" / "src" / "recomp" / "gen"
    text = "".join(p.read_text(errors="replace") for p in gen.glob("*.c"))
    assert "xbe_entry_point" in text
    title = sorted(os.listdir(work / "project" / "src" / "title"))
    assert "xba_layers.c" in title and "xdk_d3d8__d3d8.c" in title


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_build_and_boot_through_the_compat_runner(tmp_path):
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    xbe = _xbe(tmp_path)
    out = tmp_path / "results.jsonl"
    r = subprocess.run([sys.executable, "-m", "tools.compat", "run", os.path.dirname(xbe),
                        "--work-root", str(tmp_path / "cw"), "--seconds", "8", "--sustain", "4", "--out", str(out)],
                       cwd=ROOT, capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
    res = json.loads(out.read_text().splitlines()[-1])
    assert res["stage"] == "prepares" and res["blocker"].startswith("no flip"), res
    work = tmp_path / "cw" / "disc"
    status = (work / "status-0.jsonl").read_text().splitlines()
    assert status and json.loads(status[-1])["ms"] > 0          # the runtime started and reported


def _heal_xbe(tmp_path):
    """A title whose thread routine the static analysis cannot see: the entry
    computes its address (mov eax, 0x11000; add eax, 0x40) and hands it to
    PsCreateSystemThreadEx through the kernel thunk table (ordinal 255); the
    routine waits on a zero dword for ever. Then loops."""
    import struct
    entry = bytes.fromhex("558bec" "b800100100" "83c040" "50" + "6a00" * 9 + "ff1500080100" "ebfe")
    entry += b"\x90" * (0x40 - len(entry))
    routine = bytes.fromhex("833d0011010000" "74f7" "c3")   # L: cmp dword [0x11100], 0 / je L / ret
    text = entry + routine
    text += b"\x00" * (0x110 - len(text))
    d = tmp_path / "heal"
    d.mkdir()
    p = make_xbe(d / "default.xbe", libs=(("XAPILIB", 5849, 0),), text=text, title_id=0x58424102, title="Heal")
    b = bytearray(open(p, "rb").read())
    struct.pack_into("<II", b, 0x800, 0x80000000 | 255, 0)   # thunk table: PsCreateSystemThreadEx, end
    open(p, "wb").write(b)
    return str(d)


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1", reason="XBA_E2E=1: builds and boots a title (minutes)")
def test_the_self_healing_loop(tmp_path):
    """XBA-64 + XBA-97 + XBA-65 end to end: the first run meets a thread
    routine with no dispatch entry and records it; the next prepare seeds it
    from discovered.jsonl and rebuilds; the second run starts the thread."""
    if not shutil.which("cmake"):
        pytest.skip("no cmake")
    disc, out, cw = _heal_xbe(tmp_path), tmp_path / "results.jsonl", tmp_path / "cw"

    def run():
        r = subprocess.run([sys.executable, "-m", "tools.compat", "run", disc, "--work-root", str(cw),
                            "--seconds", "6", "--sustain", "3", "--out", str(out)],
                           cwd=ROOT, capture_output=True, text=True, timeout=1800)
        assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]
        return json.loads(out.read_text().splitlines()[-1])

    first = run()
    work = cw / "heal"
    disc_lines = [json.loads(l) for l in (work / "discovered.jsonl").read_text().splitlines()]
    assert {"t": "thread", "va": "0x00011040", "ctx1": "0x00000000"} in disc_lines, disc_lines
    assert first["runs"][0]["threads"] == 0
    second = run()
    seeds = json.loads((work / "discovered-seeds.json").read_text())
    assert [s["start"] for s in seeds] == ["0x00011040"]
    assert second["runs"][0]["threads"] >= 1 and second["stage"] == "boots", second
