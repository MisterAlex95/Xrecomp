"""tools.prepare for an N64 ROM (tools/prepare/n64.py, docs/android/prepare.md « N64 packs »): the title key and the
source identity (the same for the z64 / v64 / n64 byte orders), the discovered code, a desktop pack built end to end
with the host's cc -- with a fake lifter (tests/fake_n64_recomp.py) on a ROM made here, and with the real n64_tools on
our homebrew (built by tools/consoles/build_homebrew.sh n64; skipped without it), run by n64_run --pack --lockstep
when it is built -- loaded back with ctypes; and, where the NDK is there, the --android pack linked against a fake
libmain.so."""

import ctypes
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.prepare import n64                       # noqa: E402
from tools.prepare.progress import parse_line       # noqa: E402

FAKE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests")
CC = shutil.which(os.environ.get("CC", "cc"))
HOMEBREW = os.environ.get("N64_HOMEBREW_DIR") or os.path.join(ROOT, "build", "homebrew", "n64")


def n64_run():
    """A built n64_run (consoles/n64/tests or the root build), or None."""
    env = os.environ.get("N64_RUN")
    if env:
        return env if os.access(env, os.X_OK) else None
    for p in sorted(glob.glob(os.path.join(ROOT, "build", "*", "n64_runtime", "n64_run"))
                    + glob.glob(os.path.join(ROOT, "build", "*", "consoles", "n64", "runtime", "n64_run"))):
        if os.access(p, os.X_OK):
            return p
    return None


def make_rom(size=0x10000, code=b"NSME", crc1=0x635A2BFF, title=b"XBA PREPARE TEST"):
    """A big-endian (z64) ROM: header (game code, CRC1, title), libdragon's IPL3 marker, some bytes."""
    rom = bytearray(size)
    rom[0:4] = b"\x80\x37\x12\x40"
    rom[8:12] = (0x80000400).to_bytes(4, "big")
    rom[0x10:0x14] = crc1.to_bytes(4, "big")
    rom[0x20:0x34] = title.ljust(20)
    rom[0x3B:0x3F] = code
    rom[0x340:0x350] = b" Libdragon IPL3 "
    for i in range(0x1000, size):
        rom[i] = (i * 13 + (i >> 8)) & 0xFF
    return bytes(rom)


def reorder(be, order):
    """be (z64) as a v64 (pairs swapped) or n64 (words reversed) file; a trailing part stays."""
    out = bytearray(be)
    if order == "v64":
        n = len(be) & ~1
        out[0:n:2], out[1:n:2] = be[1:n:2], be[0:n:2]
    elif order == "n64":
        n = len(be) & ~3
        out[0:n:4], out[1:n:4], out[2:n:4], out[3:n:4] = be[3:n:4], be[2:n:4], be[1:n:4], be[0:n:4]
    return bytes(out)


def env_fake(**kv):
    e = dict(os.environ, XBA_N64_RECOMP="fake_n64_recomp", XBA_N64_TOOLS_DIR=FAKE_DIR)
    e.pop("FAKE_N64_RECOMP_FAIL", None)
    e.update(kv)
    return e


def env_real():
    e = dict(os.environ)
    for k in ("XBA_N64_RECOMP", "XBA_N64_TOOLS_DIR", "FAKE_N64_RECOMP_FAIL"):
        e.pop(k, None)
    return e


def prepare(args, env):
    p = subprocess.run([sys.executable, "-m", "tools.prepare"] + args, cwd=ROOT, env=env, capture_output=True,
                       text=True, timeout=600)
    lines = [parse_line(l) for l in p.stdout.splitlines()]
    lines = [l for l in lines if l]
    assert lines and lines[-1]["kind"] == "result", p.stdout + p.stderr
    return p.returncode, lines


def progress(lines):
    return [l["msg"] for l in lines if l["kind"] == "progress"]


# ── pure parts ──

def test_key_and_identity_in_every_byte_order(tmp_path):
    """The key (recomp_pack.c n64_key: game code [A-Z0-9] else '_', CRC1) and the SHA-256 of the big-endian ROM, the
    same for the three byte orders, a trailing part included and over several 1 MB pieces; n64_tools agrees."""
    for size in (0x10000, (2 << 20) + 0x1003):
        be = make_rom(size)
        want = hashlib.sha256(be).hexdigest()
        for order in ("z64", "v64", "n64"):
            p = tmp_path / ("g%d.%s" % (size, order))
            p.write_bytes(reorder(be, order))
            assert n64.is_n64(str(p))
            assert n64.read_source(str(p)) == ("NSME-635A2BFF", "XBA PREPARE TEST", order, want, size)
    sys.path.insert(0, n64.tools_dir(ROOT))
    from n64_tools import rom as n64rom
    r = n64rom.Rom(reorder(make_rom(), "n64"))
    assert r.sha256 == hashlib.sha256(make_rom()).hexdigest() and r.title_key() == "NSME-635A2BFF"
    assert n64.title_key(make_rom(code=b"n\x00E1")) == "__E1-635A2BFF"
    (tmp_path / "untitled.z64").write_bytes(make_rom(title=b""))
    assert n64.read_source(str(tmp_path / "untitled.z64"))[1] == "untitled"


def test_not_an_n64_rom(tmp_path):
    (tmp_path / "x.z64").write_bytes(b"\x00" * 0x2000)
    assert not n64.is_n64(str(tmp_path / "x.z64")) and not n64.is_n64(str(tmp_path))
    with pytest.raises(ValueError, match="byte order"):
        n64.read_source(str(tmp_path / "x.z64"))
    (tmp_path / "short.z64").write_bytes(make_rom()[:0x800])
    assert n64.is_n64(str(tmp_path / "short.z64"))
    with pytest.raises(ValueError, match="4 KB"):
        n64.read_source(str(tmp_path / "short.z64"))
    # A GBA ROM or a PS1 disc never passes for one (the first word is the N64's magic in one of three orders).
    (tmp_path / "g.gba").write_bytes(b"\x2e\x00\x00\xea" + bytes(0x200))
    assert not n64.is_n64(str(tmp_path / "g.gba"))


def test_compile_flags_and_jobs():
    """Lifted N64 code compiles at -O2 for --opt 2 / 3 (XBA-279) without the SLP vectorizer, -I consoles/n64/runtime,
    and the job pool counts 256 MB per compile with a 1 GB reserve."""
    from tools.prepare import compile as cc
    assert [n64.lifted_opt(o) for o in ("0", "1", "2", "3", "s")] == ["0", "1", "2", "2", "s"]
    flags = n64.compile_flags(n64.lifted_opt("2"), ROOT, "/gen")
    assert "-O2" in flags and "-fno-tree-slp-vectorize" in flags and "-O1" not in flags
    assert "-I" + os.path.join(ROOT, "consoles", "n64", "runtime") in flags and "-I/gen" in flags
    assert cc.default_jobs(3 << 30, 8, n64.JOB_RSS, n64.RESERVED) == 5


def test_check_refuses(tmp_path):
    d = tmp_path / "n64-NSME-635A2BFF"
    d.mkdir()
    (d / "libtitle.so").write_bytes(b"lib")
    meta = n64.make_meta(str(d / "libtitle.so"), "NSME-635A2BFF", "T", "ab" * 32, "v64", "x86_64", 1, "c",
                         "d" * 64, "clang 19 -O1", 0)
    assert meta["console"] == "n64" and meta["entry"] == "n64_recomp_title_table" and meta["rom_order"] == "v64"
    assert "boot" not in meta
    (d / "pack.json").write_text(json.dumps(meta))
    assert n64.check(str(d), "NSME-635A2BFF", "ab" * 32, "x86_64") is None
    assert "another ROM" in n64.check(str(d), "NSME-635A2BFF", "cd" * 32, "x86_64")
    assert "title_key" in n64.check(str(d), "NSME-00000000", "ab" * 32, "x86_64")
    assert "abi" in n64.check(str(d), "NSME-635A2BFF", "ab" * 32, "arm64-v8a")
    assert "shell_abi" in n64.check(str(d), "NSME-635A2BFF", "ab" * 32, "x86_64", shell_abi=2)
    (d / "libtitle.so").write_bytes(b"LIB")
    assert "sha256" in n64.check(str(d), "NSME-635A2BFF", "ab" * 32, "x86_64")
    meta["console"] = "ps1"
    (d / "pack.json").write_text(json.dumps(meta))
    assert n64.read(str(d)) is None


def test_errors(tmp_path):
    (tmp_path / "short.z64").write_bytes(make_rom()[:0x800])
    rc, lines = prepare([str(tmp_path / "short.z64"), "--work", str(tmp_path / "w1")], env_fake())
    assert rc == 1 and lines[-1]["code"] == "BAD_ROM"
    (tmp_path / "g.z64").write_bytes(make_rom())
    rc, lines = prepare([str(tmp_path / "g.z64"), "--android", "--work", str(tmp_path / "w2")], env_fake())
    assert rc == 1 and lines[-1]["code"] == "BAD_ARGS"
    rc, lines = prepare([str(tmp_path / "g.z64"), "--work", str(tmp_path / "w3"), "--pack-dir", str(tmp_path / "p")],
                        env_fake(XBA_N64_RECOMP="no_such_n64_lifter"))
    assert rc == 1 and lines[-1]["code"] == "LIFT_FAILED"


# ── a desktop pack, end to end ──

class Table(ctypes.Structure):
    _fields_ = [("abi", ctypes.c_uint32), ("count", ctypes.c_uint32), ("entries", ctypes.c_void_p),
                ("source_sha256", ctypes.c_char_p), ("boot_pc", ctypes.c_uint32)]


def load_table(lib, n=[0]):
    n[0] += 1                                   # a fresh path each time: dlopen would hand back the first library
    copy = "%s.%d.so" % (lib, n[0])
    shutil.copy(lib, copy)
    so = ctypes.CDLL(copy)
    fn = so.n64_recomp_title_table
    fn.restype = ctypes.POINTER(Table)
    return fn().contents


@pytest.mark.skipif(not CC, reason="no host C compiler")
def test_desktop_pack_end_to_end(tmp_path):
    be = make_rom()
    rom = tmp_path / "Game.v64"
    rom.write_bytes(reorder(be, "v64"))
    work, packs = tmp_path / "work", tmp_path / "packs"
    args = [str(rom), "--work", str(work), "--pack-dir", str(packs), "--jobs", "2"]
    rc, lines = prepare(args, env_fake())
    assert rc == 0, lines[-1]
    final = packs / "n64-NSME-635A2BFF"
    assert lines[-1] == {"kind": "result", "ok": True, "pack": str(final)}
    assert {l["id"] for l in lines if l["kind"] == "progress"} == {"identify", "analyse", "lift", "compile", "pack"}
    meta = json.loads((final / "pack.json").read_text())
    sha = hashlib.sha256(be).hexdigest()
    assert meta["console"] == "n64" and meta["title_key"] == "NSME-635A2BFF" and meta["source_sha256"] == sha
    assert meta["entry"] == "n64_recomp_title_table" and meta["abi"] == n64.gba.host_abi() and meta["shell_abi"] == 1
    assert meta["title"] == "XBA PREPARE TEST" and meta["rom_order"] == "v64" and meta["discovered"] == 0
    assert meta["format"] == 1 and "title_id" not in meta and meta["prepare"]["files"] == 1
    assert n64.check(str(final), "NSME-635A2BFF", sha, n64.gba.host_abi()) is None
    assert not any(f.endswith((".z64", ".v64", ".n64")) for f in os.listdir(work))   # no copy of the ROM
    t = load_table(str(final / "libtitle.so"))
    assert t.abi == 1 and t.count == 0 and t.source_sha256.decode() == sha and t.boot_pc == 0x80000400

    # Again: nothing to do. The same cartridge in another byte order: the same pack, up to date.
    rc, lines = prepare(args, env_fake())
    assert rc == 0 and "pack up to date" in progress(lines) and any("lift up to date" in m for m in progress(lines))
    (tmp_path / "Game.z64").write_bytes(be)
    rc, lines = prepare([str(tmp_path / "Game.z64")] + args[1:], env_fake())
    assert rc == 0 and lines[-1]["pack"] == str(final) and "pack up to date" in progress(lines)

    # The runtime found code (discovered.txt in the work folder, plus --discovered): lifted again with it, once each.
    (work / "discovered.txt").write_text("80300000 00000000000000AA 03E0000800000000\n"
                                         "80300000 00000000000000BB 03E0000824020001\n")
    (tmp_path / "more.txt").write_text("80300000 00000000000000AA 03E0000800000000\n"
                                       "80200000 0000000000000001 03E0000800000000\n")
    rc, lines = prepare(args + ["--discovered", str(tmp_path / "more.txt")], env_fake())
    assert rc == 0
    assert json.loads((final / "pack.json").read_text())["discovered"] == 3
    assert (work / "discovered-seeds.txt").read_text().splitlines()[0] == "80200000 0000000000000001 03E0000800000000"
    assert "3 piece(s) of code found by earlier runs" in progress(lines)
    assert load_table(str(final / "libtitle.so")).count == 3

    # A lifter that fails: LIFT_FAILED, the published pack stays; the next run lifts again (no stale stamp).
    with open(work / "discovered.txt", "a") as f:
        f.write("80300004 00000000000000CC 00000000\n")
    rc, lines = prepare(args, env_fake(FAKE_N64_RECOMP_FAIL="1"))
    assert rc == 1 and lines[-1]["code"] == "LIFT_FAILED"
    assert json.loads((final / "pack.json").read_text())["discovered"] == 3
    rc, lines = prepare(args, env_fake())
    assert rc == 0 and json.loads((final / "pack.json").read_text())["discovered"] == 3   # --discovered not given


@pytest.mark.skipif(not CC, reason="no host C compiler")
def test_desktop_defaults_to_the_data_home(tmp_path):
    (tmp_path / "g.n64").write_bytes(reorder(make_rom(crc1=0x12345678), "n64"))
    rc, lines = prepare([str(tmp_path / "g.n64")], env_fake(XDG_DATA_HOME=str(tmp_path / "data")))
    assert rc == 0, lines[-1]
    assert lines[-1]["pack"] == str(tmp_path / "data" / "xrecomp" / "packs" / "n64-NSME-12345678")
    assert (tmp_path / "data" / "xrecomp" / "work" / "n64-NSME-12345678" / "stamp.json").is_file()


# ── the real lifter on our homebrew ──

def fnv(words):
    h = 0xCBF29CE484222325
    for w in words:
        h = ((h ^ w) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


@pytest.mark.skipif(not (CC and os.path.isfile(os.path.join(HOMEBREW, "gfx.z64"))),
                    reason="no host C compiler or no N64 homebrew (tools/consoles/build_homebrew.sh n64)")
def test_real_lifter_on_the_homebrew(tmp_path):
    rom = os.path.join(HOMEBREW, "gfx.z64")
    with open(rom, "rb") as f:
        data = f.read()
    work, packs = tmp_path / "work", tmp_path / "packs"
    args = [rom, "--work", str(work), "--pack-dir", str(packs), "--opt", "1"]
    rc, lines = prepare(args, env_real())
    assert rc == 0, lines[-1]
    final = lines[-1]["pack"]
    key = n64.title_key(data[:0x40])
    assert os.path.basename(final) == "n64-" + key
    meta = json.loads(open(os.path.join(final, "pack.json")).read())
    assert meta["source_sha256"] == hashlib.sha256(data).hexdigest() and meta["rom_order"] == "z64"
    table = (work / "gen" / "n64_rc_table.c").read_text()
    assert '"%s"' % meta["source_sha256"] in table          # the lifter's identity is the Prepare's
    files = len([f for f in os.listdir(work / "gen") if f.endswith(".c")])
    assert files > 5
    nm = shutil.which("nm")
    if nm:
        syms = subprocess.run([nm, "-D", "--defined-only", os.path.join(final, "libtitle.so")], capture_output=True,
                              text=True, timeout=60).stdout
        assert " n64_recomp_title_table" in syms
    lib = os.path.join(final, "libtitle.so")

    # The pack runs natively under n64_run, in lockstep with the interpreter; what it misses goes to discovered.txt.
    run = n64_run()
    if run:
        p = subprocess.run([run, rom, "--pack", lib, "--lockstep", "--frames", "120", "--missed",
                            str(work / "discovered.txt")], capture_output=True, text=True, timeout=300)
        assert p.returncode == 0 and "LOCKSTEP OK" in p.stdout + p.stderr, p.stdout + p.stderr

    # Code that is in no static image (as the runtime records it, bytes included): the next Prepare lifts it, and
    # only the files that gained functions compile again (stable files, the object cache).
    words = [0x03E00008, 0x24020007]                         # jr ra; li v0, 7
    with open(work / "discovered.txt", "a") as f:
        f.write("80700000 %016X %s\n" % (fnv(words), "".join("%08X" % w for w in words)))
    rc, lines = prepare(args, env_real())
    assert rc == 0, lines[-1]
    disc = json.loads(open(os.path.join(final, "pack.json")).read())["discovered"]
    assert disc >= 1
    assert "80700000" in (work / "gen" / "n64_rc_table.c").read_text().upper()
    done = [m for m in progress(lines) if m.endswith(" compiled") and " files, " in m][-1]
    compiled = int(done.split(", ")[1].split()[0])
    assert compiled <= 3 < files, done                       # the table, the file with the new function

    # The same cartridge as a .v64: the same key, identity and pack (up to date).
    v64 = tmp_path / "gfx.v64"
    v64.write_bytes(reorder(data, "v64"))
    rc, lines = prepare([str(v64)] + args[1:], env_real())
    assert rc == 0 and lines[-1]["pack"] == final and "pack up to date" in progress(lines)
    if run:
        p = subprocess.run([run, str(v64), "--pack", lib, "--lockstep", "--frames", "60"], capture_output=True,
                           text=True, timeout=300)
        assert p.returncode == 0 and "LOCKSTEP OK" in p.stdout + p.stderr, p.stdout + p.stderr


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
    (tmp_path / "g.z64").write_bytes(make_rom())
    (tmp_path / "w").mkdir()
    (tmp_path / "w" / "discovered.txt").write_text("80300000 00000000000000AA 03E0000800000000\n")
    rc, lines = prepare([str(tmp_path / "g.z64"), "--android", "--work", str(tmp_path / "w"), "--pack-dir",
                         str(tmp_path / "p"), "--cc", clang, "--sysroot", sysroot, "--shell-lib", str(shell),
                         "--jobs", "1"], env_fake())
    assert rc == 0, lines[-1]
    final = tmp_path / "p" / "n64-NSME-635A2BFF"
    meta = json.loads((final / "pack.json").read_text())
    assert meta["abi"] == "arm64-v8a" and meta["discovered"] == 1 and meta["cc"].startswith("clang ")
    readelf = os.path.join(NDK_BIN, "bin", "llvm-readelf")
    dyn = subprocess.run([readelf, "-d", "--dyn-syms", str(final / "libtitle.so")], capture_output=True, text=True,
                         timeout=60).stdout
    assert "libmain.so" in dyn and "n64_recomp_title_table" in dyn
