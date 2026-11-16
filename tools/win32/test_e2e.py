"""A Windows PE through the x86 pipeline and the Win32 runtime (XBA-233, epic XBA-231). No third-party binary.

- Always: the synthetic hello world (tools/pe_parser/synth.py hello_pe) lifts: the generated C defines the entry
  point and calls kernel32 through the IAT slots.
- XBA_E2E=1 (needs a C compiler, as in tools/docker's images): it is compiled against src/win32 and run, and prints
  the hello world; a call to an import the runtime has no HLE for stops the program with exit status 127.
- XBA_E2E=1 and i686-w64-mingw32-gcc on the PATH (tools/docker/win32-mingw.Dockerfile), or XBA_WIN32_HELLO_DIR
  naming a folder tools/win32/build_hello.sh filled: our own mingw hello worlds (tests/win32_hello), without the CRT
  and through mingw's CRT startup and msvcrt, run the same way.
"""

import os
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

pytest.importorskip("capstone")
pytest.importorskip("pefile")

from tools.pe_parser.synth import HELLO_MESSAGE, Asm, build_pe, hello_pe   # noqa: E402
from tools.win32 import build as w32                                      # noqa: E402

E2E = os.environ.get("XBA_E2E") == "1"


def _cc():
    from tools.recomp.c_fixture import compiler, is_msvc
    cc = os.environ.get("CC") or compiler()
    return None if not cc or is_msvc(cc) else cc


def _run(prog, exe, *args):
    return subprocess.run([prog, str(exe), *args], capture_output=True, timeout=60)


def test_the_synthetic_hello_world_lifts(tmp_path):
    exe = tmp_path / "hello.exe"
    exe.write_bytes(hello_pe())
    gen = w32.lift(str(exe), str(tmp_path / "w"))
    text = "".join(open(os.path.join(gen, f)).read() for f in os.listdir(gen) if f.endswith(".c"))
    assert "void xbe_entry_point(void)" in text
    from tools.pe_parser.pe_parser import parse_pe
    for imp in parse_pe(str(exe)).imports:
        assert "MEM32(0x%X)" % imp.thunk_addr in text, imp.name     # call [slot]: through the IAT


@pytest.mark.skipif(not E2E, reason="XBA_E2E=1 builds and runs the recompiled program")
def test_the_synthetic_hello_world_runs(tmp_path):
    cc = _cc()
    if not cc:
        pytest.skip("no GCC/Clang-style C compiler")
    exe = tmp_path / "hello.exe"
    exe.write_bytes(hello_pe(extra_imports={"WS2_32.dll": [115]}))    # imported, never called: fine
    prog = w32.build(str(exe), str(tmp_path / "w"), cc)
    r = _run(prog, exe)
    assert r.returncode == 0, r.stderr.decode(errors="replace")
    assert r.stdout == HELLO_MESSAGE


@pytest.mark.skipif(not E2E, reason="XBA_E2E=1 builds and runs the recompiled program")
def test_an_import_without_hle_stops_the_program(tmp_path):
    cc = _cc()
    if not cc:
        pytest.skip("no GCC/Clang-style C compiler")

    def code(layout):
        a = Asm()
        a.emit(0xFF, 0x15).abs32(layout.iat[115])                       # call [WS2_32 #115]
        a.emit(0x6A, 0x00).emit(0xFF, 0x15).abs32(layout.iat["ExitProcess"])
        return a.done()
    exe = tmp_path / "missing.exe"
    exe.write_bytes(build_pe(code, imports={"KERNEL32.dll": ["ExitProcess"], "WS2_32.dll": [115]}))
    prog = w32.build(str(exe), str(tmp_path / "w"), cc)
    r = _run(prog, exe)
    assert r.returncode == 127
    assert b"WS2_32.dll!#115 called: no HLE for it" in r.stderr


def _mingw_hellos(tmp_path):
    d = os.environ.get("XBA_WIN32_HELLO_DIR")
    if d:
        return d
    if not shutil.which("i686-w64-mingw32-gcc"):
        return None
    out = tmp_path / "mingw"
    subprocess.run(["sh", os.path.join(ROOT, "tools", "win32", "build_hello.sh"), str(out)], check=True,
                   capture_output=True, timeout=300)
    return str(out)


@pytest.mark.skipif(not E2E, reason="XBA_E2E=1 builds and runs the recompiled program")
@pytest.mark.parametrize("name, args, expect", [
    ("hello", (), b"Hello, world! (a Win32 PE recompiled by XRecomp)\n"),
    ("hello_crt", ("one", "two"), b"Hello, world! (argc=3, through the CRT)\n"),
])
def test_the_mingw_hello_worlds_run(tmp_path, name, args, expect):
    cc = _cc()
    if not cc:
        pytest.skip("no GCC/Clang-style C compiler")
    d = _mingw_hellos(tmp_path)
    if not d:
        pytest.skip("no i686-w64-mingw32-gcc and no XBA_WIN32_HELLO_DIR (tools/win32/build_hello.sh)")
    exe = os.path.join(d, name + ".exe")
    prog = w32.build(exe, str(tmp_path / "w"), cc)
    r = _run(prog, exe, *args)
    assert r.returncode == 0, r.stderr.decode(errors="replace")
    assert r.stdout == expect


@pytest.mark.skipif(not E2E, reason="XBA_E2E=1 builds and runs the recompiled program")
def test_the_synthetic_hello_world_runs_on_arm64(tmp_path):
    """Cross-compiled for aarch64 and run under qemu-user, when both are there (tools/docker/linux-arm64.Dockerfile's
    packages: crossbuild-essential-arm64, qemu-user)."""
    cc, qemu = shutil.which("aarch64-linux-gnu-gcc"), shutil.which("qemu-aarch64")
    if not (cc and qemu):
        pytest.skip("no aarch64-linux-gnu-gcc / qemu-aarch64")
    exe = tmp_path / "hello.exe"
    exe.write_bytes(hello_pe())
    prog = w32.build(str(exe), str(tmp_path / "w"), cc)
    r = subprocess.run([qemu, "-L", "/usr/aarch64-linux-gnu", prog, str(exe)], capture_output=True, timeout=120)
    assert r.returncode == 0, r.stderr.decode(errors="replace")
    assert r.stdout == HELLO_MESSAGE
