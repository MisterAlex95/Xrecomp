"""tools.prepare for a GBA ROM (tools/prepare/gba.py, docs/android/prepare.md « GBA packs »): the title key, the
discovered addresses, a desktop pack built end to end with the host's cc and a fake lifter (tests/fake_gba_recomp.py),
loaded back with ctypes; and, where the NDK is there, the --android pack linked against a fake libmain.so."""

import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import zlib

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.prepare import gba                       # noqa: E402
from tools.prepare.progress import parse_line       # noqa: E402

FAKE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests")
CC = shutil.which(os.environ.get("CC", "cc"))


def make_rom(code=b"XBAT", size=4096, fill=0x11):
    """A homebrew-shaped ROM: game code, 0x96 at 0xB2, the complement check."""
    rom = bytearray([fill]) * size
    rom[0xA0:0xAC] = b"PREPARE TEST"
    rom[0xAC:0xB0] = code
    rom[0xB2] = 0x96
    s = 0
    for i in range(0xA0, 0xBD):
        s = (s - rom[i]) & 0xFF
    rom[0xBD] = (s - 0x19) & 0xFF
    return bytes(rom)


def env_fake(**kv):
    e = dict(os.environ, XBA_GBA_RECOMP="fake_gba_recomp", XBA_GBA_TOOLS_DIR=FAKE_DIR)
    e.pop("FAKE_GBA_RECOMP_FAIL", None)
    e.update(kv)
    return e


def prepare(args, env):
    p = subprocess.run([sys.executable, "-m", "tools.prepare"] + args, cwd=ROOT, env=env, capture_output=True,
                       text=True, timeout=300)
    lines = [parse_line(l) for l in p.stdout.splitlines()]
    lines = [l for l in lines if l]
    assert lines and lines[-1]["kind"] == "result", p.stdout + p.stderr
    return p.returncode, lines


# ── pure parts ──

def test_title_key_matches_the_runtime():
    rom = make_rom()
    assert gba.header_ok(rom)
    assert gba.title_key(rom) == "XBAT-%08X" % (zlib.crc32(rom) & 0xFFFFFFFF)
    bad = bytearray(rom)
    bad[0xBD] ^= 1
    assert gba.title_key(bytes(bad)).startswith("____-")
    odd = make_rom(code=b"a\x00 1")
    assert gba.title_key(odd).startswith("____-") is False and gba.title_key(odd)[:4] == "___1"
    assert gba.rom_title(rom, "x") == "PREPARE TEST" and gba.rom_title(b"", "x") == "x"


def test_is_gba(tmp_path):
    p = tmp_path / "game.bin"
    p.write_bytes(make_rom())
    assert gba.is_gba(str(p))
    q = tmp_path / "x.gba"
    q.write_bytes(b"anything")
    assert gba.is_gba(str(q))
    r = tmp_path / "default.xbe"
    r.write_bytes(b"XBEH" + bytes(300))
    assert not gba.is_gba(str(r)) and not gba.is_gba(str(tmp_path))


def test_read_addresses(tmp_path):
    p = tmp_path / "d.txt"
    p.write_text("0x08000101\n# a comment\n\n080001F0\n0x08000101  # again\nnot hex\n0x1FFFFFFFF\n")
    assert gba.read_addresses(str(p)) == [0x08000101, 0x080001F0]
    assert gba.read_addresses(str(tmp_path / "none")) == []
    gba.write_addresses(str(p), [1, 0x08000000])
    assert p.read_text() == "0x00000001\n0x08000000\n"


def test_pack_dir_names():
    from tools.prepare import pack
    assert pack.dir_name("gba-XBAT-0011AABB") == "gba-XBAT-0011AABB"
    assert pack.dir_name("4d5300d1") == "4D5300D1"
    assert pack.final_dir("/p", "gba-____-00000001") == "/p/gba-____-00000001"


def test_host_abi():
    assert gba.host_abi() in ("x86_64", "arm64-v8a", "x86", "armeabi-v7a", "unknown")


def test_check_refuses(tmp_path):
    d = tmp_path / "gba-XBAT-1"
    d.mkdir()
    (d / "libtitle.so").write_bytes(b"lib")
    meta = gba.make_meta(str(d / "libtitle.so"), "XBAT-1", "T", "ab" * 32, "x86_64", 1, "c", "d" * 64,
                         "clang 19 -O2", 0)
    (d / "pack.json").write_text(json.dumps(meta))
    assert gba.check(str(d), "XBAT-1", "ab" * 32, "x86_64") is None
    assert "another ROM" in gba.check(str(d), "XBAT-1", "cd" * 32, "x86_64")
    assert "title_key" in gba.check(str(d), "XBAT-2", "ab" * 32, "x86_64")
    assert "abi" in gba.check(str(d), "XBAT-1", "ab" * 32, "arm64-v8a")
    assert "shell_abi" in gba.check(str(d), "XBAT-1", "ab" * 32, "x86_64", shell_abi=2)
    (d / "libtitle.so").write_bytes(b"LIB")
    assert "sha256" in gba.check(str(d), "XBAT-1", "ab" * 32, "x86_64")
    assert gba.check(str(tmp_path), "XBAT-1", "ab" * 32, "x86_64") == "no pack.json"


def test_android_bad_args(tmp_path):
    rom = tmp_path / "g.gba"
    rom.write_bytes(make_rom())
    rc, lines = prepare([str(rom), "--android", "--work", str(tmp_path / "w")], env_fake())
    assert rc == 1 and lines[-1]["code"] == "BAD_ARGS"


# ── a desktop pack, end to end ──

class Table(ctypes.Structure):
    _fields_ = [("abi", ctypes.c_uint32), ("count", ctypes.c_uint32), ("entries", ctypes.c_void_p),
                ("rom_sha256", ctypes.c_char_p)]


def load_table(lib):
    so = ctypes.CDLL(lib)
    fn = so.gba_recomp_title_table
    fn.restype = ctypes.POINTER(Table)
    return fn().contents


@pytest.mark.skipif(not CC, reason="no host C compiler")
def test_desktop_pack_end_to_end(tmp_path):
    rom_bytes = make_rom()
    rom = tmp_path / "Game.gba"
    rom.write_bytes(rom_bytes)
    work, packs = tmp_path / "work", tmp_path / "packs"
    args = [str(rom), "--work", str(work), "--pack-dir", str(packs), "--jobs", "2"]
    rc, lines = prepare(args, env_fake())
    assert rc == 0, lines[-1]
    key = gba.title_key(rom_bytes)
    final = packs / ("gba-" + key)
    assert lines[-1] == {"kind": "result", "ok": True, "pack": str(final)}
    assert [l["id"] for l in lines if l["kind"] == "progress"][0] == "identify"
    assert {l["id"] for l in lines if l["kind"] == "progress"} == {"identify", "analyse", "lift", "compile", "pack"}
    meta = json.loads((final / "pack.json").read_text())
    sha = hashlib.sha256(rom_bytes).hexdigest()
    assert meta["console"] == "gba" and meta["title_key"] == key and meta["rom_sha256"] == sha
    assert meta["entry"] == "gba_recomp_title_table" and meta["abi"] == gba.host_abi() and meta["shell_abi"] == 1
    assert meta["title"] == "PREPARE TEST" and meta["discovered"] == 0 and meta["format"] == 1
    assert "title_id" not in meta and meta["toolkit_sha256"] and meta["prepare"]["files"] == 1
    assert gba.check(str(final), key, sha, gba.host_abi()) is None
    assert (work / "rom.gba").read_bytes() == rom_bytes
    t = load_table(str(final / "libtitle.so"))
    assert t.abi == 1 and t.count == 0 and t.rom_sha256.decode() == sha

    # Again: nothing to do.
    rc, lines = prepare(args, env_fake())
    assert rc == 0
    msgs = [l["msg"] for l in lines if l["kind"] == "progress"]
    assert "pack up to date" in msgs and any("lift up to date" in m for m in msgs)
    assert "1 files, 0 compiled" in msgs

    # The runtime found addresses (discovered.txt in the work folder): lifted again with them.
    (work / "discovered.txt").write_text("0x08000101\n0x080001F0\n")
    rc, lines = prepare(args, env_fake())
    assert rc == 0
    meta = json.loads((final / "pack.json").read_text())
    assert meta["discovered"] == 2
    assert (work / "discovered-seeds.txt").read_text() == "0x08000101\n0x080001F0\n"
    msgs = [l["msg"] for l in lines if l["kind"] == "progress"]
    assert "2 address(es) found by earlier runs" in msgs

    # A lifter that fails: LIFT_FAILED, the published pack stays.
    (work / "discovered.txt").write_text("0x08000101\n0x080001F0\n0x08000301\n")
    rc, lines = prepare(args, env_fake(FAKE_GBA_RECOMP_FAIL="1"))
    assert rc == 1 and lines[-1]["code"] == "LIFT_FAILED"
    assert json.loads((final / "pack.json").read_text())["discovered"] == 2
    rc, lines = prepare(args, env_fake())        # and the next run lifts again (no stale stamp)
    assert rc == 0 and json.loads((final / "pack.json").read_text())["discovered"] == 3


@pytest.mark.skipif(not CC, reason="no host C compiler")
def test_desktop_defaults_to_the_data_home(tmp_path):
    rom = tmp_path / "g.gba"
    rom.write_bytes(make_rom(fill=0x22))
    rc, lines = prepare([str(rom)], env_fake(XDG_DATA_HOME=str(tmp_path / "data")))
    assert rc == 0
    key = gba.title_key(rom.read_bytes())
    assert lines[-1]["pack"] == str(tmp_path / "data" / "xrecomp" / "packs" / ("gba-" + key))
    assert (tmp_path / "data" / "xrecomp" / "work" / ("gba-" + key) / "stamp.json").is_file()


def test_no_lifter_is_lift_failed(tmp_path):
    rom = tmp_path / "g.gba"
    rom.write_bytes(make_rom())
    rc, lines = prepare([str(rom), "--work", str(tmp_path / "w"), "--pack-dir", str(tmp_path / "p")],
                        env_fake(XBA_GBA_RECOMP="no_such_gba_lifter"))
    assert rc == 1 and lines[-1]["code"] == "LIFT_FAILED"


# ── --android, with the NDK ──

NDK = os.environ.get("ANDROID_NDK_HOME") or os.environ.get("ANDROID_NDK_ROOT")
NDK_BIN = NDK and os.path.join(NDK, "toolchains", "llvm", "prebuilt", "linux-x86_64")


@pytest.mark.skipif(not (NDK_BIN and os.path.isdir(NDK_BIN)), reason="no Android NDK")
def test_android_pack_with_the_ndk(tmp_path):
    clang = os.path.join(NDK_BIN, "bin", "clang")
    sysroot = os.path.join(NDK_BIN, "sysroot")
    shell = tmp_path / "shell" / "libmain.so"
    shell.parent.mkdir()
    (tmp_path / "shell.c").write_text("int xba_shell_abi_stub(void) { return 1; }\n")
    subprocess.run([clang, "--target=aarch64-linux-android30", "-shared", "-fPIC", "-o", str(shell),
                    str(tmp_path / "shell.c")], check=True, timeout=120)
    rom = tmp_path / "g.gba"
    rom.write_bytes(make_rom(fill=0x33))
    (tmp_path / "w").mkdir()
    (tmp_path / "w" / "discovered.txt").write_text("0x08000000\n")
    rc, lines = prepare([str(rom), "--android", "--work", str(tmp_path / "w"), "--pack-dir", str(tmp_path / "p"),
                         "--cc", clang, "--sysroot", sysroot, "--shell-lib", str(shell), "--jobs", "1"], env_fake())
    assert rc == 0, lines[-1]
    key = gba.title_key(rom.read_bytes())
    final = tmp_path / "p" / ("gba-" + key)
    meta = json.loads((final / "pack.json").read_text())
    assert meta["abi"] == "arm64-v8a" and meta["discovered"] == 1 and meta["cc"].startswith("clang ")
    readelf = os.path.join(NDK_BIN, "bin", "llvm-readelf")
    dyn = subprocess.run([readelf, "-d", "--dyn-syms", str(final / "libtitle.so")], capture_output=True, text=True,
                         timeout=60).stdout
    assert "libmain.so" in dyn and "gba_recomp_title_table" in dyn
