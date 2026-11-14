"""tools.prepare for a PS1 disc (tools/prepare/ps1.py, docs/android/prepare.md « PS1 packs »): the title key, the boot
EXE (the source identity), the discovered code, a desktop pack built end to end with the host's cc -- with a fake
lifter (tests/fake_ps1_recomp.py) on a disc image made here, and with the real ps1_tools on our homebrew (built by
tools/consoles/build_homebrew.sh ps1; skipped without it) -- loaded back with ctypes; and, where the NDK is there, the
--android pack linked against a fake libmain.so."""

import ctypes
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import zlib

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.prepare import ps1                       # noqa: E402
from tools.prepare.progress import parse_line       # noqa: E402

FAKE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests")
CC = shutil.which(os.environ.get("CC", "cc"))
HOMEBREW = os.environ.get("PS1_HOMEBREW_DIR") or os.path.join(ROOT, "build", "homebrew", "ps1")


def ps1_chd():
    """consoles/ps1/tools/ps1_tools/chd.py (its writer makes the test CHDs)."""
    ps1._ps1_tools_exe()
    from ps1_tools import chd
    return chd


def make_exe(size=0x1800, pc0=0x80010000, fill=0x11):
    """A PS-X EXE: header, text at 0x80010000."""
    text = bytes([fill]) * (size - 0x800)
    head = bytearray(0x800)
    head[:8] = b"PS-X EXE"
    struct.pack_into("<10I", head, 0x10, pc0, 0, 0x80010000, len(text), 0, 0, 0, 0, 0x801FFFF0, 0)
    return bytes(head) + text


def _rec(name, lba, size, is_dir):
    n = name if isinstance(name, bytes) else name.encode()
    ln = 33 + len(n) + ((33 + len(n)) & 1)
    r = bytearray(ln)
    r[0] = ln
    struct.pack_into("<I", r, 2, lba)
    struct.pack_into(">I", r, 6, lba)
    struct.pack_into("<I", r, 10, size)
    struct.pack_into(">I", r, 14, size)
    r[25] = 2 if is_dir else 0
    r[32] = len(n)
    r[33:33 + len(n)] = n
    return bytes(r)


def make_iso(exe, cnf=b"BOOT = cdrom:\\SLUS_005.94;1\r\nTCB = 4\r\n", exe_name="SLUS_005.94;1", raw=False,
             system_id=b"PLAYSTATION"):
    """A PS1 disc image: ISO 9660, cooked (2048) or raw (2352, mode 2 form 1); SYSTEM.CNF (unless cnf is None) and the
    EXE in the root."""
    secs = {}
    pvd = bytearray(2048)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = system_id.ljust(32)
    pvd[156:156 + 34] = _rec(b"\x00", 18, 2048, True)
    secs[16] = bytes(pvd)
    root = _rec(b"\x00", 18, 2048, True) + _rec(b"\x01", 18, 2048, True)
    if cnf is not None:
        root += _rec("SYSTEM.CNF;1", 20, len(cnf), False)
        secs[20] = cnf.ljust(2048, b"\0")
    root += _rec(exe_name, 21, len(exe), False)
    secs[18] = root.ljust(2048, b"\0")
    for i in range(0, len(exe), 2048):
        secs[21 + i // 2048] = exe[i:i + 2048].ljust(2048, b"\0")
    n = max(secs) + 1
    out = bytearray()
    for lba in range(n):
        user = secs.get(lba, bytes(2048))
        if raw:
            s = bytearray(2352)
            s[0:12] = b"\x00" + b"\xff" * 10 + b"\x00"
            s[15] = 2
            s[24:24 + 2048] = user
            out += s
        else:
            out += user
    return bytes(out), bytes(pvd)


def env_fake(**kv):
    e = dict(os.environ, XBA_PS1_RECOMP="fake_ps1_recomp", XBA_PS1_TOOLS_DIR=FAKE_DIR)
    e.pop("FAKE_PS1_RECOMP_FAIL", None)
    e.update(kv)
    return e


def env_real():
    e = dict(os.environ)
    for k in ("XBA_PS1_RECOMP", "XBA_PS1_TOOLS_DIR", "FAKE_PS1_RECOMP_FAIL"):
        e.pop(k, None)
    return e


def prepare(args, env):
    p = subprocess.run([sys.executable, "-m", "tools.prepare"] + args, cwd=ROOT, env=env, capture_output=True,
                       text=True, timeout=600)
    lines = [parse_line(l) for l in p.stdout.splitlines()]
    lines = [l for l in lines if l]
    assert lines and lines[-1]["kind"] == "result", p.stdout + p.stderr
    return p.returncode, lines


# ── pure parts ──

def test_serial_matches_the_loader():
    assert ps1.serial("BOOT = cdrom:\\SLUS_005.94;1\r\nTCB = 4\r\n") == "SLUS-00594"
    assert ps1.serial("BOOT=cdrom:\\scus_944.26;1\n") == "SCUS-94426"
    assert ps1.serial("BOOT = cdrom:\\DIR\\SLES-123.45;1") == "SLES-12345"
    assert ps1.serial("BOOT = cdrom:\\PSX.EXE;1\n") is None
    assert ps1.serial("TCB = 4\n") is None
    pvd = bytes(range(256)) * 8
    assert ps1.disc_key(pvd, None) == "PSX-%08X" % (zlib.crc32(pvd) & 0xFFFFFFFF)
    assert ps1.disc_key(pvd, "BOOT=cdrom:\\PSX.EXE;1") == ps1.disc_key(pvd, None)
    exe = make_exe()
    assert ps1.exe_key(exe) == "EXE-%08X" % (zlib.crc32(exe) & 0xFFFFFFFF)


def test_compile_flags_and_jobs():
    """Lifted PS1 code compiles at -O1 for --opt 2 / 3 without the SLP vectorizer (XBA-161), and the job pool counts
    256 MB per compile with a 1 GB reserve."""
    from tools.prepare import compile as cc
    assert [ps1.ps1_opt(o) for o in ("0", "1", "2", "3", "s")] == ["0", "1", "1", "1", "s"]
    flags = ps1.compile_flags(ps1.ps1_opt("2"), ROOT, "/gen")
    assert "-O1" in flags and "-fno-tree-slp-vectorize" in flags and "-O2" not in flags
    assert cc.default_jobs(3 << 30, 8, ps1.PS1_JOB_RSS, ps1.PS1_RESERVED) == 5
    assert cc.default_jobs(int(1.6 * (1 << 30)), 8, ps1.PS1_JOB_RSS, ps1.PS1_RESERVED) == 2


def test_read_source(tmp_path):
    exe = make_exe()
    for raw in (False, True):
        img, _ = make_iso(exe, raw=raw)
        p = tmp_path / ("g%d.bin" % raw)
        p.write_bytes(img)
        assert ps1.is_ps1(str(p))
        assert ps1.read_source(str(p)) == ("SLUS-00594", "SLUS_005.94", exe)
    cue = tmp_path / "g.cue"
    cue.write_text('FILE "g1.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
                   'FILE "g1-audio.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n')
    assert ps1.is_ps1(str(cue)) and ps1.read_source(str(cue))[0] == "SLUS-00594"
    img, pvd = make_iso(exe, cnf=None, exe_name="PSX.EXE;1")
    (tmp_path / "n.iso").write_bytes(img)
    assert ps1.read_source(str(tmp_path / "n.iso")) == ("PSX-%08X" % (zlib.crc32(pvd) & 0xFFFFFFFF), "PSX.EXE", exe)
    (tmp_path / "x.exe").write_bytes(exe)
    assert ps1.is_ps1(str(tmp_path / "x.exe")) and ps1.read_source(str(tmp_path / "x.exe"))[0] == ps1.exe_key(exe)
    # Not a PS1 disc: another system ID, an Xbox-looking file, nothing.
    img, _ = make_iso(exe, system_id=b"WIN32")
    (tmp_path / "pc.iso").write_bytes(img)
    assert not ps1.is_ps1(str(tmp_path / "pc.iso"))
    (tmp_path / "default.xbe").write_bytes(b"XBEH" + bytes(300))
    assert not ps1.is_ps1(str(tmp_path / "default.xbe")) and not ps1.is_ps1(str(tmp_path))
    # Readable but wrong: a boot file that is not there, a .chd that is no CHD v5.
    img, _ = make_iso(exe, cnf=b"BOOT=cdrom:\\GONE.EXE;1\n")
    (tmp_path / "gone.bin").write_bytes(img)
    with pytest.raises(ValueError):
        ps1.read_source(str(tmp_path / "gone.bin"))
    (tmp_path / "d.chd").write_bytes(b"MComprHD" + bytes(200))
    assert ps1.is_ps1(str(tmp_path / "d.chd"))
    with pytest.raises(ValueError, match="chd"):
        ps1.read_source(str(tmp_path / "d.chd"))
    # A .chd of the same disc (cdlz, cdzl, uncompressed): the same key, boot name and EXE as its .bin.
    img, _ = make_iso(exe, raw=True)
    frames = [img[i:i + 2352] for i in range(0, len(img), 2352)]
    for codec in ("cdlz", "cdzl", "none"):
        p = tmp_path / ("g-%s.chd" % codec)
        ps1_chd().write_cd(str(p), [("MODE2_RAW", frames)], codec=codec)
        assert ps1.is_ps1(str(p))
        assert ps1.read_source(str(p)) == ("SLUS-00594", "SLUS_005.94", exe)


def test_records(tmp_path):
    p = tmp_path / "d.txt"
    p.write_text("# the runtime's\n80100000 00000000000000AA 0000E0AF\n80100000 00000000000000BB 1111E0AF\n"
                 "80100000 00000000000000aa 0000E0AF\n80010000\n\n8010000 1 00\nnot hex\n80100002 1 00\n"
                 "80020000 1 0G\n")
    rec = ps1.read_records(str(p))
    assert sorted(rec) == [(0x8010000, 1), (0x80010000, None), (0x80100000, 0xAA), (0x80100000, 0xBB)]
    assert ps1.sorted_lines(rec) == ["08010000 0000000000000001 00", "80010000",
                                     "80100000 00000000000000AA 0000E0AF", "80100000 00000000000000BB 1111E0AF"]
    assert ps1.read_records(str(tmp_path / "none")) == {}
    ps1.write_records(str(p), ["80010000"])
    assert p.read_text() == "80010000\n"


def test_check_refuses(tmp_path):
    d = tmp_path / "ps1-SLUS-00594"
    d.mkdir()
    (d / "libtitle.so").write_bytes(b"lib")
    meta = ps1.make_meta(str(d / "libtitle.so"), "SLUS-00594", "T", "ab" * 32, "SLUS_005.94", "x86_64", 1, "c",
                         "d" * 64, "clang 19 -O2", 0)
    (d / "pack.json").write_text(json.dumps(meta))
    assert ps1.check(str(d), "SLUS-00594", "ab" * 32, "x86_64") is None
    assert "another boot EXE" in ps1.check(str(d), "SLUS-00594", "cd" * 32, "x86_64")
    assert "title_key" in ps1.check(str(d), "SLUS-00595", "ab" * 32, "x86_64")
    assert "abi" in ps1.check(str(d), "SLUS-00594", "ab" * 32, "arm64-v8a")
    assert "shell_abi" in ps1.check(str(d), "SLUS-00594", "ab" * 32, "x86_64", shell_abi=2)
    (d / "libtitle.so").write_bytes(b"LIB")
    assert "sha256" in ps1.check(str(d), "SLUS-00594", "ab" * 32, "x86_64")
    assert ps1.check(str(tmp_path), "SLUS-00594", "ab" * 32, "x86_64") == "no pack.json"
    meta["console"] = "gba"
    (d / "pack.json").write_text(json.dumps(meta))
    assert ps1.read(str(d)) is None


def test_errors(tmp_path):
    (tmp_path / "d.chd").write_bytes(b"MComprHD" + bytes(200))
    rc, lines = prepare([str(tmp_path / "d.chd"), "--work", str(tmp_path / "w"), "--pack-dir", str(tmp_path / "p")],
                        env_fake())
    assert rc == 1 and lines[-1]["code"] == "BAD_DISC" and "chd: version 0" in lines[-1]["msg"]
    (tmp_path / "lost.cue").write_text('FILE "lost.bin" BINARY\n  TRACK 01 MODE2/2352\n')
    rc, lines = prepare([str(tmp_path / "lost.cue"), "--work", str(tmp_path / "w2")], env_fake())
    assert rc == 1 and lines[-1]["code"] == "NO_DISC"
    img, _ = make_iso(make_exe())
    (tmp_path / "g.bin").write_bytes(img)
    rc, lines = prepare([str(tmp_path / "g.bin"), "--android", "--work", str(tmp_path / "w3")], env_fake())
    assert rc == 1 and lines[-1]["code"] == "BAD_ARGS"
    rc, lines = prepare([str(tmp_path / "g.bin"), "--work", str(tmp_path / "w4"), "--pack-dir", str(tmp_path / "p")],
                        env_fake(XBA_PS1_RECOMP="no_such_ps1_lifter"))
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
    fn = so.ps1_recomp_title_table
    fn.restype = ctypes.POINTER(Table)
    return fn().contents


@pytest.mark.skipif(not CC, reason="no host C compiler")
def test_desktop_pack_end_to_end(tmp_path):
    exe = make_exe()
    img, _ = make_iso(exe, raw=True)
    (tmp_path / "Game.bin").write_bytes(img)
    cue = tmp_path / "Game.cue"
    cue.write_text('FILE "Game.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    work, packs = tmp_path / "work", tmp_path / "packs"
    args = [str(cue), "--work", str(work), "--pack-dir", str(packs), "--jobs", "2"]
    rc, lines = prepare(args, env_fake())
    assert rc == 0, lines[-1]
    final = packs / "ps1-SLUS-00594"
    assert lines[-1] == {"kind": "result", "ok": True, "pack": str(final)}
    assert {l["id"] for l in lines if l["kind"] == "progress"} == {"identify", "analyse", "lift", "compile", "pack"}
    meta = json.loads((final / "pack.json").read_text())
    sha = hashlib.sha256(exe).hexdigest()
    assert meta["console"] == "ps1" and meta["title_key"] == "SLUS-00594" and meta["source_sha256"] == sha
    assert meta["entry"] == "ps1_recomp_title_table" and meta["abi"] == ps1.gba.host_abi() and meta["shell_abi"] == 1
    assert meta["title"] == "Game" and meta["boot"] == "SLUS_005.94" and meta["discovered"] == 0
    assert meta["format"] == 1 and "title_id" not in meta and meta["prepare"]["files"] == 1
    assert ps1.check(str(final), "SLUS-00594", sha, ps1.gba.host_abi()) is None
    assert (work / "boot.exe").read_bytes() == exe
    t = load_table(str(final / "libtitle.so"))
    assert t.abi == 1 and t.count == 0 and t.source_sha256.decode() == sha and t.boot_pc == 0x80010000

    # Again: nothing to do.
    rc, lines = prepare(args, env_fake())
    assert rc == 0
    msgs = [l["msg"] for l in lines if l["kind"] == "progress"]
    assert "pack up to date" in msgs and any("lift up to date" in m for m in msgs)

    # The runtime found code (discovered.txt in the work folder, plus --discovered): lifted again with it, once each.
    (work / "discovered.txt").write_text("80100000 00000000000000AA 0000E0AF\n80100000 00000000000000BB 1111E0AF\n")
    (tmp_path / "more.txt").write_text("80100000 00000000000000AA 0000E0AF\n80020000 0000000000000001 00000000\n")
    rc, lines = prepare(args + ["--discovered", str(tmp_path / "more.txt")], env_fake())
    assert rc == 0
    assert json.loads((final / "pack.json").read_text())["discovered"] == 3
    assert (work / "discovered-seeds.txt").read_text().splitlines()[0] == "80020000 0000000000000001 00000000"
    assert "3 piece(s) of code found by earlier runs" in [l["msg"] for l in lines if l["kind"] == "progress"]
    assert load_table(str(final / "libtitle.so")).count == 3

    # A lifter that fails: LIFT_FAILED, the published pack stays; the next run lifts again (no stale stamp).
    with open(work / "discovered.txt", "a") as f:
        f.write("80100004 00000000000000CC 00000000\n")
    rc, lines = prepare(args, env_fake(FAKE_PS1_RECOMP_FAIL="1"))
    assert rc == 1 and lines[-1]["code"] == "LIFT_FAILED"
    assert json.loads((final / "pack.json").read_text())["discovered"] == 3
    rc, lines = prepare(args, env_fake())
    assert rc == 0 and json.loads((final / "pack.json").read_text())["discovered"] == 3   # --discovered not given


@pytest.mark.skipif(not CC, reason="no host C compiler")
def test_desktop_defaults_to_the_data_home(tmp_path):
    exe = make_exe(fill=0x22)
    (tmp_path / "g.exe").write_bytes(exe)
    rc, lines = prepare([str(tmp_path / "g.exe")], env_fake(XDG_DATA_HOME=str(tmp_path / "data")))
    assert rc == 0, lines[-1]
    name = "ps1-" + ps1.exe_key(exe)
    assert lines[-1]["pack"] == str(tmp_path / "data" / "xrecomp" / "packs" / name)
    assert (tmp_path / "data" / "xrecomp" / "work" / name / "stamp.json").is_file()


# ── the real lifter on our homebrew ──

@pytest.mark.skipif(not (CC and os.path.isfile(os.path.join(HOMEBREW, "overlay.cue"))),
                    reason="no host C compiler or no PS1 homebrew (tools/consoles/build_homebrew.sh ps1)")
def test_real_lifter_on_the_homebrew_disc(tmp_path):
    cue = os.path.join(HOMEBREW, "overlay.cue")
    with open(os.path.join(HOMEBREW, "overlay.exe"), "rb") as f:
        exe = f.read()
    work, packs = tmp_path / "work", tmp_path / "packs"
    args = [cue, "--work", str(work), "--pack-dir", str(packs), "--opt", "1"]
    rc, lines = prepare(args, env_real())
    assert rc == 0, lines[-1]
    final = lines[-1]["pack"]
    assert os.path.basename(final).startswith("ps1-PSX-")    # OVERLAY.EXE is not serial-shaped
    meta = json.loads(open(os.path.join(final, "pack.json")).read())
    assert meta["source_sha256"] == hashlib.sha256(exe).hexdigest() and meta["boot"] == "OVERLAY.EXE"
    # The lifted code calls the core (resolved against ps1_play / libmain.so): no dlopen here, its table's C instead.
    table = (work / "gen" / "ps1_rc_table.c").read_text()
    assert '"%s"' % meta["source_sha256"] in table
    first = table.count("ps1_rc_f_")
    assert first > 10
    nm = shutil.which("nm")
    if nm:
        syms = subprocess.run([nm, "-D", "--defined-only", os.path.join(final, "libtitle.so")], capture_output=True,
                              text=True, timeout=60).stdout
        assert " ps1_recomp_title_table" in syms
    # What a run with the pack records: an overlay's code, bytes included, lifted by the next Prepare.
    words = struct.unpack_from("<64I", exe, 0x800)
    h = 0xCBF29CE484222325
    for w in words:
        h = ((h ^ w) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    (work / "discovered.txt").write_text("80100000 %016X %s\n" % (h, exe[0x800:0x900].hex().upper()))
    rc, lines = prepare(args, env_real())
    assert rc == 0, lines[-1]
    assert json.loads(open(os.path.join(final, "pack.json")).read())["discovered"] == 1
    assert (work / "gen" / "ps1_rc_table.c").read_text().count("ps1_rc_f_") > first
    # The EXE alone: its own key, the same identity.
    rc, lines = prepare([os.path.join(HOMEBREW, "overlay.exe"), "--work", str(tmp_path / "w2"), "--pack-dir",
                         str(packs), "--opt", "1"], env_real())
    assert rc == 0 and os.path.basename(lines[-1]["pack"]) == "ps1-" + ps1.exe_key(exe)


@pytest.mark.skipif(not (CC and os.path.isfile(os.path.join(HOMEBREW, "overlay.cue"))),
                    reason="no host C compiler or no PS1 homebrew (tools/consoles/build_homebrew.sh ps1)")
def test_real_lifter_on_the_homebrew_chd(tmp_path):
    """The overlay disc as a .chd (chdman's own when it is on the PATH, else one ps1_tools.chd writes in chdman's
    shape, cdlz): the same pack as from its .cue (key, source identity, boot EXE)."""
    with open(os.path.join(HOMEBREW, "overlay.exe"), "rb") as f:
        exe = f.read()
    chd_path = tmp_path / "overlay.chd"
    chdman = shutil.which("chdman")
    if chdman:
        subprocess.run([chdman, "createcd", "-f", "-i", os.path.join(HOMEBREW, "overlay.cue"), "-o", str(chd_path)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    else:
        with open(os.path.join(HOMEBREW, "overlay.bin"), "rb") as f:
            img = f.read()
        ps1_chd().write_cd(str(chd_path), [("MODE2_RAW", [img[i:i + 2352] for i in range(0, len(img), 2352)])],
                           codec="cdlz")
    ref = ps1.read_source(os.path.join(HOMEBREW, "overlay.cue"))
    assert ps1.read_source(str(chd_path)) == ref and ref[2] == exe
    work, packs = tmp_path / "work", tmp_path / "packs"
    rc, lines = prepare([str(chd_path), "--work", str(work), "--pack-dir", str(packs), "--opt", "1"], env_real())
    assert rc == 0, lines[-1]
    final = lines[-1]["pack"]
    assert os.path.basename(final) == "ps1-" + ref[0]
    meta = json.loads(open(os.path.join(final, "pack.json")).read())
    assert meta["source_sha256"] == hashlib.sha256(exe).hexdigest() and meta["boot"] == "OVERLAY.EXE"
    assert (work / "boot.exe").read_bytes() == exe


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
    img, _ = make_iso(make_exe(fill=0x33))
    (tmp_path / "g.iso").write_bytes(img)
    (tmp_path / "w").mkdir()
    (tmp_path / "w" / "discovered.txt").write_text("80100000 00000000000000AA 0000E0AF\n")
    rc, lines = prepare([str(tmp_path / "g.iso"), "--android", "--work", str(tmp_path / "w"), "--pack-dir",
                         str(tmp_path / "p"), "--cc", clang, "--sysroot", sysroot, "--shell-lib", str(shell),
                         "--jobs", "1"], env_fake())
    assert rc == 0, lines[-1]
    final = tmp_path / "p" / "ps1-SLUS-00594"
    meta = json.loads((final / "pack.json").read_text())
    assert meta["abi"] == "arm64-v8a" and meta["discovered"] == 1 and meta["cc"].startswith("clang ")
    readelf = os.path.join(NDK_BIN, "bin", "llvm-readelf")
    dyn = subprocess.run([readelf, "-d", "--dyn-syms", str(final / "libtitle.so")], capture_output=True, text=True,
                         timeout=60).stdout
    assert "libmain.so" in dyn and "ps1_recomp_title_table" in dyn
