"""tools/android/profile_device.py (XBA-36): the host side of profile.sh, on synthetic inputs (no device, no NDK)."""
import os
import struct
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profile_device as pd  # noqa: E402

PKG = "fr.adserver.xrecomp.shell"


# ── A synthetic ELF64 (what ld.lld leaves in a libtitle.so that matters here) ──

def make_elf(path, symbols, build_id=b"\x12\x34\xab\xcd", symtab=True):
    """symbols: (name, value, size, type) with type 2 = STT_FUNC, 1 = STT_OBJECT; shndx 1 when value != 0."""
    shstr = b"\0.shstrtab\0.strtab\0.symtab\0.note.gnu.build-id\0"
    names = {".shstrtab": 1, ".strtab": 11, ".symtab": 19, ".note.gnu.build-id": 27}
    strtab = b"\0"
    syms = struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)
    for name, value, size, typ in symbols:
        off = len(strtab)
        strtab += name.encode() + b"\0"
        syms += struct.pack("<IBBHQQ", off, (1 << 4) | typ, 0, 1 if value else 0, value, size)
    note = struct.pack("<III", 4, len(build_id), 3) + b"GNU\0" + build_id
    blobs = [shstr, strtab, syms, note]
    offs, pos = [], 64
    for b in blobs:
        offs.append(pos)
        pos += len(b)
    shoff = (pos + 7) & ~7
    sh = [struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
          struct.pack("<IIQQQQIIQQ", names[".shstrtab"], 3, 0, 0, offs[0], len(shstr), 0, 0, 1, 0),
          struct.pack("<IIQQQQIIQQ", names[".strtab"], 3, 0, 0, offs[1], len(strtab), 0, 0, 1, 0)]
    if symtab:
        sh.append(struct.pack("<IIQQQQIIQQ", names[".symtab"], 2, 0, 0, offs[2], len(syms), 2, 1, 8, 24))
    sh.append(struct.pack("<IIQQQQIIQQ", names[".note.gnu.build-id"], 7, 2, 0, offs[3], len(note), 0, 0, 4, 0))
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8
    hdr = ident + struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 0, shoff, 0, 64, 0, 0, 64, len(sh), 1)
    body = hdr + b"".join(blobs)
    body += b"\0" * (shoff - len(body))
    with open(path, "wb") as f:
        f.write(body + b"".join(sh))
    return path


# ── Arguments ──

def test_args_defaults():
    a = pd.parse_args(["30"])
    assert a.seconds == 30 and a.package == PKG and a.freq == 1000 and not a.dwarf and a.lib == []
    assert a.out is None and a.serial is None and not a.no_report


def test_args_options():
    a = pd.parse_args(["5", "-s", "ABC", "-p", "fr.adserver.xrecomp.dev", "-o", "/x", "-f", "4000", "--dwarf",
                       "--lib", "/l1", "--lib", "/l2", "--no-report", "--ndk", "/ndk"])
    assert (a.seconds, a.serial, a.package, a.out, a.freq, a.dwarf, a.lib, a.no_report, a.ndk) == \
        (5, "ABC", "fr.adserver.xrecomp.dev", "/x", 4000, True, ["/l1", "/l2"], True, "/ndk")


@pytest.mark.parametrize("argv", [[], ["0"], ["-3"], ["thirty"], ["3601"], ["30", "-f", "5"], ["30", "-f", "x"]])
def test_args_refused(argv, capsys):
    with pytest.raises(SystemExit) as e:
        pd.parse_args(argv)
    assert e.value.code == 2


def test_default_out():
    import datetime
    p = pd.default_out(PKG, datetime.datetime(2026, 11, 16, 21, 5, 9), home="/h")
    assert p == "/h/xrecomp-profiles/%s-20261116-210509" % PKG


# ── The NDK ──

def _ndk(root, version):
    sp = root / "ndk" / version / "simpleperf"
    for f in ("report_html.py", "bin/linux/x86_64/simpleperf", "bin/android/arm64/simpleperf"):
        (sp / f).parent.mkdir(parents=True, exist_ok=True)
        (sp / f).write_text("")
    return root / "ndk" / version


def test_find_simpleperf(tmp_path):
    old = _ndk(tmp_path, "27.0.12077973")
    new = _ndk(tmp_path, "28.2.13676358")
    assert pd.find_simpleperf(env={"ANDROID_HOME": str(tmp_path)}) == str(new / "simpleperf")
    assert pd.find_simpleperf(env={"ANDROID_NDK_HOME": str(old), "ANDROID_HOME": str(tmp_path)}) == \
        str(old / "simpleperf")
    assert pd.find_simpleperf(ndk=str(old), env={}) == str(old / "simpleperf")
    with pytest.raises(pd.ProfileError, match="no NDK"):
        pd.find_simpleperf(env={})
    with pytest.raises(pd.ProfileError, match="no NDK"):
        pd.find_simpleperf(ndk=str(tmp_path), env={})


def test_simpleperf_binaries(tmp_path):
    sp = str(_ndk(tmp_path, "28") / "simpleperf")
    assert pd.host_simpleperf(sp, "linux").endswith("bin/linux/x86_64/simpleperf")
    with pytest.raises(pd.ProfileError):
        pd.host_simpleperf(sp, "darwin")
    assert pd.device_simpleperf(sp, "arm64-v8a\r\n").endswith("bin/android/arm64/simpleperf")
    with pytest.raises(pd.ProfileError):
        pd.device_simpleperf(sp, "mips")
    with pytest.raises(pd.ProfileError):
        pd.device_simpleperf(sp, "x86_64")


# ── adb, faked ──

class FakeRun:
    """subprocess.run for adb: answers by the shell command's text."""

    def __init__(self, answers):
        self.answers = answers
        self.calls = []

    def __call__(self, argv, **kw):
        self.calls.append(argv)
        key = " ".join(argv[argv.index("shell") + 1:]) if "shell" in argv else " ".join(argv[1:])
        for prefix, (rc, out) in self.answers.items():
            if key.startswith(prefix):
                return subprocess.CompletedProcess(argv, rc, out.encode(), b"")
        return subprocess.CompletedProcess(argv, 1, b"", b"not found")


def test_access_mode_run_as():
    run = FakeRun({"pm path": (0, "package:/data/app/x/base.apk\n"), "run-as %s id" % PKG: (0, "uid=10123(u0_a123)")})
    assert pd.access_mode(pd.Adb("SER", run=run), PKG) == "run-as"
    assert run.calls[0][:3] == ["adb", "-s", "SER"]


def test_access_mode_su_and_refused():
    run = FakeRun({"pm path": (0, "package:/x.apk"), "run-as": (1, "run-as: package not debuggable"),
                   "su 0 id": (0, "uid=0(root) gid=0(root)")})
    assert pd.access_mode(pd.Adb(run=run), PKG) == "su"
    run = FakeRun({"pm path": (0, "package:/x.apk"), "run-as": (1, "run-as: package not debuggable")})
    with pytest.raises(pd.ProfileError, match="not debuggable.*xrecomp-latest"):
        pd.access_mode(pd.Adb(run=run), PKG)
    with pytest.raises(pd.ProfileError, match="not installed"):
        pd.access_mode(pd.Adb(run=FakeRun({})), PKG)


def test_find_pid():
    assert pd.find_pid(pd.Adb(run=FakeRun({"pidof": (0, "4242\n")})), PKG) == 4242
    with pytest.raises(pd.ProfileError, match="not running"):
        pd.find_pid(pd.Adb(run=FakeRun({"pidof": (1, "")})), PKG)


def test_adb_missing():
    def run(*a, **k):
        raise FileNotFoundError("adb")
    with pytest.raises(pd.ProfileError, match="adb not found"):
        pd.Adb(run=run).shell(["id"])


def test_shell_quotes():
    run = FakeRun({})
    pd.Adb(run=run).shell(["run-as", PKG, "find", "/d", "-name", "*_dispatch.c"])
    assert run.calls[0][-1] == "run-as %s find /d -name '*_dispatch.c'" % PKG


def test_record_command():
    c = pd.record_command(PKG, 77, 30, 1000, False, "run-as")
    assert c[:2] == [pd.DEVICE_SIMPLEPERF, "record"] and c[-2:] == ["--app", PKG]
    assert "fp" in c and "30" in c and "-p" not in c
    c = pd.record_command(PKG, 77, 10, 4000, True, "su")
    assert c[:2] == ["su", "0"] and c[-2:] == ["-p", "77"] and "dwarf" in c and "--app" not in c
    assert pd.as_app("run-as", PKG, ["cat", "x"]) == ["run-as", PKG, "cat", "x"]
    assert pd.as_app("su", PKG, ["cat", "x"]) == ["su", "0", "cat", "x"]


# ── Maps and the pack ──

MAPS = """\
5d1c000000-5d1c001000 r--p 00000000 fd:05 123  /system/bin/app_process64
7a10000000-7a10400000 r--p 00000000 fd:30 456  /data/user/0/%(p)s/files/packs/4D530004/libtitle.so
7a10400000-7a12000000 r-xp 00400000 fd:30 456  /data/user/0/%(p)s/files/packs/4D530004/libtitle.so
7a20000000-7a20100000 r-xp 00000000 fd:30 789  /data/app/~~a==/%(p)s-b==/lib/arm64/libmain.so
7a30000000-7a30001000 r-xp 00000000 fd:30 790  /data/data/%(p)s/files/packs/gba-ABCD/libtitle.so
7a40000000-7a40001000 r-xp 00000000 fd:30 791  /data/user/0/other.app/files/libx.so
7a50000000-7a50001000 r-xp 00000000 fd:30 792  /data/user/0/%(p)s/files/old/libgone.so (deleted)
7a60000000-7a60001000 rw-p 00000000 00:00 0    [anon:scudo:primary]
7a70000000-7a70001000 r--p 00000000 fd:30 793  /data/user/0/%(p)s/files/save/state.bin
""" % {"p": PKG}


def test_app_libraries():
    assert pd.app_libraries(MAPS, PKG) == [
        "/data/user/0/%s/files/packs/4D530004/libtitle.so" % PKG,
        "/data/data/%s/files/packs/gba-ABCD/libtitle.so" % PKG,
    ]
    assert pd.app_libraries("", PKG) == []


def test_pack_dir_name(tmp_path):
    assert pd.pack_dir_name("/data/user/0/%s/files/packs/4D530004/libtitle.so" % PKG) == "4D530004"
    assert pd.pack_dir_name("/data/user/0/%s/files/packs/gba-ABCD/libtitle.so" % PKG) == "gba-ABCD"
    assert pd.pack_dir_name("/data/app/x/lib/arm64/libmain.so") is None
    assert pd.local_lib_path(str(tmp_path), "/data/user/0/p/libtitle.so") == \
        os.path.join(str(tmp_path), "data/user/0/p/libtitle.so")


# ── ELF symbols ──

SYMS = [("sub_00011000", 0x4000, 0x40, 2), ("sub_00011040_gen", 0x4040, 0x80, 2), ("XapiInitProcess", 0x4100, 0x20, 2),
        ("recomp_title_main", 0x4200, 0x10, 2), ("g_recomp_flat", 0x9000, 8, 1), ("extern_undef", 0, 0, 2),
        ("gba_rc_08000123", 0x5000, 4, 2), ("n64_rc_f_80001234", 0x5100, 4, 2), ("ps1_rc_80010000", 0x5200, 4, 2)]


def test_read_elf(tmp_path):
    info = pd.read_elf(make_elf(str(tmp_path / "libtitle.so"), SYMS))
    assert info.has_symtab and info.build_id == "1234abcd"
    names = [n for _v, _s, n in info.functions]
    assert names == ["sub_00011000", "sub_00011040_gen", "XapiInitProcess", "recomp_title_main", "gba_rc_08000123",
                     "n64_rc_f_80001234", "ps1_rc_80010000"]   # functions with a value, sorted; no object, no undef
    assert info.functions[0] == (0x4000, 0x40, "sub_00011000")


def test_read_elf_stripped_and_not_elf(tmp_path):
    info = pd.read_elf(make_elf(str(tmp_path / "s.so"), SYMS, symtab=False))
    assert not info.has_symtab and info.functions == [] and info.build_id == "1234abcd"
    (tmp_path / "x.so").write_bytes(b"not an elf at all")
    with pytest.raises(pd.ProfileError, match="not an ELF"):
        pd.read_elf(str(tmp_path / "x.so"))
    (tmp_path / "32.so").write_bytes(b"\x7fELF\x01\x01" + b"\0" * 60)
    with pytest.raises(pd.ProfileError, match="64-bit"):
        pd.read_elf(str(tmp_path / "32.so"))


DISPATCH = """\
static const recomp_entry_t g_recomp_table[] = {
    { 0x00011000u, (recomp_func_t)sub_00011000 },
    { 0x00011040u, (recomp_func_t)sub_00011040 },
    { 0x00012345u, (recomp_func_t)XapiInitProcess },
};
"""


def test_guest_address_and_dispatch():
    assert pd.guest_address("sub_0001ABCD") == ("xbox", 0x1ABCD)
    assert pd.guest_address("sub_0001abcd_gen") == ("xbox", 0x1ABCD)
    assert pd.guest_address("gba_rc_08000123") == ("gba", 0x08000123)
    assert pd.guest_address("n64_rc_f_80001234") == ("n64", 0x80001234)
    assert pd.guest_address("ps1_rc_80010000") == ("ps1", 0x80010000)
    assert pd.guest_address("sub_123") is None and pd.guest_address("memcpy") is None
    d = pd.parse_dispatch(DISPATCH)
    assert d == {"sub_00011000": 0x11000, "sub_00011040": 0x11040, "XapiInitProcess": 0x12345}
    assert pd.guest_address("XapiInitProcess", d) == ("xbox", 0x12345)


def test_guest_symbols_tsv(tmp_path):
    info = pd.read_elf(make_elf(str(tmp_path / "libtitle.so"), SYMS))
    rows = pd.guest_symbols(info, pd.parse_dispatch(DISPATCH))
    assert [(r[2], r[3], r[4]) for r in rows] == [
        ("sub_00011000", "xbox", 0x11000), ("sub_00011040_gen", "xbox", 0x11040),
        ("XapiInitProcess", "xbox", 0x12345), ("gba_rc_08000123", "gba", 0x08000123),
        ("n64_rc_f_80001234", "n64", 0x80001234), ("ps1_rc_80010000", "ps1", 0x80010000)]
    assert len(pd.guest_symbols(info)) == 5        # without the table: the names that carry their address
    out = tmp_path / "guest_symbols.tsv"
    pd.write_guest_symbols(str(out), rows)
    lines = out.read_text().splitlines()
    assert lines[0].startswith("# host_offset") and lines[1] == "0x4000\t64\tsub_00011000\txbox\t0x00011000"


# ── Reports ──

REPORT = """\
Cmdline: /data/local/tmp/simpleperf record -e task-clock -f 1000 --call-graph fp --duration 30
Samples: 29876 of event 'task-clock'

Overhead  Sample  Shared Object                                              Symbol
21.40%    6393    /data/user/0/fr.adserver.xrecomp.shell/files/packs/4D530004/libtitle.so  sub_0004F1C0
 9.02%    2695    /data/user/0/fr.adserver.xrecomp.shell/files/packs/4D530004/libtitle.so  XapiInitProcess
 7.77%    2321    /data/app/x/lib/arm64/libmain.so                           nv2a_pb_exec_walk
 1.00%    299     /apex/com.android.runtime/lib64/bionic/libc.so             memcpy
"""


def test_annotate_report():
    out = pd.annotate_report(REPORT, {"XapiInitProcess": 0x12345}).splitlines()
    assert out[4].endswith("sub_0004F1C0  [xbox 0x0004F1C0]")
    assert out[5].endswith("XapiInitProcess  [xbox 0x00012345]")
    assert out[6].endswith("nv2a_pb_exec_walk") and out[7].endswith("memcpy")
    assert out[0] == REPORT.splitlines()[0]
    assert pd.annotate_report("") == ""


def test_perf_lines():
    lc = ("11-16 21:00:00.000  4242  4300 I xboxrecomp: xbox: perf speed ? (29.9 fps); render 3.0 ...\n"
          "11-16 21:00:01.000  4242  4300 I xboxrecomp: [GPU] frame cap 30\n"
          "11-16 21:00:10.000  4242  4300 I xboxrecomp: n64: perf over the run: speed 25 % ...\n"
          "11-16 21:00:11.000  4242  4300 I xboxrecomp: n64: perf detail 500 VIs\n")
    got = pd.perf_lines(lc)
    assert len(got) == 3 and "[GPU]" not in "".join(got)


def test_main_reports_errors(monkeypatch, capsys):
    for var in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK", "ANDROID_HOME", "ANDROID_SDK_ROOT"):
        monkeypatch.delenv(var, raising=False)
    assert pd.main(["10", "--ndk", "/nonexistent"]) == 2
    assert "no NDK" in capsys.readouterr().err
