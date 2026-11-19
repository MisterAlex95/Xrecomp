"""tools.prepare --android: the progress grammar, the jobs formula, the object
cache key, pack.json and the atomic publish (docs/android/prepare.md)."""

import datetime
import json
import os
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.prepare import compile as cc            # noqa: E402
from tools.prepare import cpu                       # noqa: E402
from tools.prepare import pack                      # noqa: E402
from tools.prepare.progress import (                # noqa: E402
    STEP_IDS, parse_line, progress_line, result_error, result_ok, Reporter)

GB = 1 << 30


# ── progress grammar ──

def test_progress_line_exact():
    assert progress_line(4, "compile", 37, "61/165 files", eta=120.4) == \
        "XBA_PROGRESS step=4/5 id=compile pct=37 eta=120 msg=61/165 files"
    assert progress_line(1, "identify", 0, "finding default.xbe") == \
        "XBA_PROGRESS step=1/5 id=identify pct=0 msg=finding default.xbe"


def test_step_ids_in_order():
    assert STEP_IDS == ("identify", "analyse", "lift", "compile", "pack")


def test_progress_clamps_and_one_line():
    line = progress_line(2, "analyse", 140.7, "two\nlines  and\ttabs")
    assert "pct=100" in line and "\n" not in line
    assert line.endswith("msg=two lines and tabs")
    assert "pct=0 " in progress_line(2, "analyse", -3, "x")


def test_msg_last_and_greedy():
    d = parse_line("XBA_PROGRESS step=3/5 id=lift pct=12 msg=a=b c=d pct=99")
    assert d == {"kind": "progress", "step": 3, "steps": 5, "id": "lift", "pct": 12, "msg": "a=b c=d pct=99"}


def test_unknown_keys_ignored_by_reader():
    d = parse_line("XBA_PROGRESS step=4/5 id=compile pct=50 eta=7 speed=3 msg=x")
    assert d["pct"] == 50 and d["eta"] == 7 and d["msg"] == "x" and d["speed"] == "3"


def test_results_roundtrip():
    assert result_ok("/data/x/packs/4D5300D1") == "XBA_RESULT ok pack=/data/x/packs/4D5300D1"
    assert parse_line(result_ok("/p/4D5300D1")) == {"kind": "result", "ok": True, "pack": "/p/4D5300D1"}
    line = result_error("COMPILE_FAILED", "gen_0001.c: error: x")
    assert line == "XBA_RESULT error code=COMPILE_FAILED msg=gen_0001.c: error: x"
    assert parse_line(line) == {"kind": "result", "ok": False, "code": "COMPILE_FAILED",
                                "msg": "gen_0001.c: error: x"}
    with pytest.raises(ValueError):
        result_error("compile failed", "x")


def test_not_ours():
    assert parse_line("[1/5] finding default.xbe") is None
    assert parse_line("XBA_RESULT maybe") is None
    assert parse_line("XBA_PROGRESS step=x id=lift pct=1 msg=") is None


def test_reporter_dedupes(capsys):
    r = Reporter(sys.stdout)
    r.progress(4, 10, "a")
    r.progress(4, 10, "a")
    r.progress(4, 11, "a")
    r.ok("/p")
    out = capsys.readouterr().out.splitlines()
    assert out == ["XBA_PROGRESS step=4/5 id=compile pct=10 msg=a",
                   "XBA_PROGRESS step=4/5 id=compile pct=11 msg=a", "XBA_RESULT ok pack=/p"]


# ── jobs ──

@pytest.mark.parametrize("avail_gb,cpus,want", [
    (None, 8, 1),        # unknown memory
    (2, 8, 1),           # below the reserve: still one job
    (3.9, 8, 1),
    (4.0, 8, 2),         # (4 - 3) / 0.5
    (4.5, 8, 3),
    (5.0, 8, 4),
    (5.5, 8, 5),
    (12, 8, 5),          # (12 - 3) / 0.5 = 18 -> capped at 5
    (64, 8, 5),
    (12, 2, 2),          # no more jobs than cores
])
def test_default_jobs(avail_gb, cpus, want):
    avail = None if avail_gb is None else int(avail_gb * GB)
    assert cc.default_jobs(avail, cpus) == want


def test_mem_available(tmp_path):
    p = tmp_path / "meminfo"
    p.write_text("MemTotal:       16000000 kB\nMemFree:  100 kB\nMemAvailable:    9437184 kB\n")
    assert cc.mem_available(str(p)) == 9437184 * 1024
    assert cc.mem_available(str(tmp_path / "none")) is None


# ── cache key ──

def test_cache_key_inputs():
    base = cc.cache_key("/w/a.c", b"int x;", ["-O1"], "clang 19", "h0")
    assert base == cc.cache_key("/w/a.c", b"int x;", ["-O1"], "clang 19", "h0")
    others = {
        cc.cache_key("/w/b.c", b"int x;", ["-O1"], "clang 19", "h0"),
        cc.cache_key("/w/a.c", b"int y;", ["-O1"], "clang 19", "h0"),
        cc.cache_key("/w/a.c", b"int x;", ["-O2"], "clang 19", "h0"),
        cc.cache_key("/w/a.c", b"int x;", ["-O1"], "clang 20", "h0"),
        cc.cache_key("/w/a.c", b"int x;", ["-O1"], "clang 19", "h1"),
        # flags are joined with a separator, not concatenated
        cc.cache_key("/w/a.c", b"int x;", ["-O", "1"], "clang 19", "h0"),
    }
    assert base not in others and len(others) == 6


def test_header_digest(tmp_path):
    (tmp_path / "a").mkdir()
    (tmp_path / "a" / "x.h").write_text("#define A 1\n")
    (tmp_path / "a" / "x.c").write_text("int a;\n")
    d0 = cc.header_digest([str(tmp_path / "a")])
    (tmp_path / "a" / "x.c").write_text("int b;\n")          # not a header
    assert cc.header_digest([str(tmp_path / "a")]) == d0
    (tmp_path / "a" / "x.h").write_text("#define A 2\n")
    assert cc.header_digest([str(tmp_path / "a")]) != d0


def test_header_digest_of_the_lifted_c(tmp_path):
    """XBA-209: for the lifted C, recomp_funcs.h counts without its `void name(void);` lines, so a re-lift that
    found one more function keeps the other chunks' objects; anything else in it still counts, and the project's
    own C keeps the whole header."""
    gen = tmp_path / "src" / "recomp" / "gen"
    gen.mkdir(parents=True)
    h = gen / "recomp_funcs.h"
    h.write_text('/**\n * T - Recompiled Function Declarations\n * 2 functions, auto-generated\n */\n'
                 '#include "recomp_types.h"\nvoid sub_00011000(void);\nvoid memcpy_hook(void);  /* 0x00012000 */\n')
    d, g = cc.header_digest([str(gen)]), cc.header_digest([str(gen)], generated=True)
    h.write_text('/**\n * T - Recompiled Function Declarations\n * 3 functions, auto-generated\n */\n'
                 '#include "recomp_types.h"\nvoid sub_00011000(void);\nvoid sub_00011040(void);\n')
    assert cc.header_digest([str(gen)], generated=True) == g
    assert cc.header_digest([str(gen)]) != d
    h.write_text('#include "recomp_types.h"\n#define X 1\nvoid sub_00011000(void);\n')
    assert cc.header_digest([str(gen)], generated=True) != g
    assert cc.is_generated(str(gen / "recomp_0003.c"))
    assert not cc.is_generated(str(tmp_path / "src" / "main.c"))
    assert not cc.is_generated(str(tmp_path / "src" / "title" / "fix.c"))


def test_compile_flags():
    f = cc.compile_flags("1", "/tk", "/w/project", "/sys", profile=True)
    for want in ("--target=aarch64-linux-android30", "--sysroot=/sys", "-O1", "-g0", "-fPIC", "-std=gnu11",
                 "-w", "-DXBOXRECOMP_SHELL_TITLE=1", "-DXBA_TITLE_PROFILE=1", "-I/w/project/src",
                 "-I/w/project/src/recomp", "-I/tk/include", "-I/tk/src", "-I/tk/src/kernel",
                 "-mno-outline-atomics"):         # lock-prefixed instructions inline (XBA-277)
        assert want in f
    assert "-DXBA_TITLE_PROFILE=1" not in cc.compile_flags("1", "/tk", "/w/p", "/sys")
    assert not any(x.startswith("-march") for x in f)                     # the ARMv8.0 baseline by default


def test_compile_flags_cpu_features():
    """XBA-313: the device's ARMv8.1 atomics inline when Prepare found them (only the atomics: not armv8.1-a)."""
    f = cc.compile_flags("2", "/tk", "/w/p", "/sys", cpu_features=("lse",))
    assert "-march=armv8-a+lse" in f and "-mno-outline-atomics" in f
    assert cc.compile_flags("2", "/tk", "/w/p", "/sys", cpu_features=()) == cc.compile_flags("2", "/tk", "/w/p", "/sys")


def _auxv(path, pairs):
    import struct
    with open(path, "wb") as f:
        for k, v in list(pairs) + [(0, 0)]:
            f.write(struct.pack("<QQ", k, v))
    return str(path)


def test_cpu_detect(tmp_path):
    """The features come from AT_HWCAP (/proc/self/auxv), only on an aarch64 Linux / Android: the CPU that runs
    the pack. Anywhere else, or unreadable: none (ARMv8.0)."""
    lse = _auxv(tmp_path / "a", [(6, 4096), (cpu.AT_HWCAP, 0xFF | cpu.HWCAP_ATOMICS), (26, 0)])
    plain = _auxv(tmp_path / "b", [(cpu.AT_HWCAP, 0xFF)])
    assert cpu.hwcap(lse) == 0xFF | cpu.HWCAP_ATOMICS
    assert cpu.detect("aarch64", "Linux", lse) == ("lse",)
    assert cpu.detect("aarch64", "Android", lse) == ("lse",)
    assert cpu.detect("aarch64", "Linux", plain) == ()
    assert cpu.detect("x86_64", "Linux", lse) == ()
    assert cpu.detect("arm64", "Darwin", lse) == ()
    none = str(tmp_path / "none")
    assert cpu.detect("aarch64", "Linux", none, none) == ()
    assert cpu.hwcap(_auxv(tmp_path / "c", [(6, 4096)])) is None
    # No auxv: /proc/cpuinfo's Features line, the same bit by name.
    info = tmp_path / "cpuinfo"
    info.write_text("processor\t: 0\nFeatures\t: fp asimd evtstrm aes crc32 atomics fphp\n")
    assert cpu.detect("aarch64", "Linux", none, str(info)) == ("lse",)
    info.write_text("processor\t: 0\nFeatures\t: fp asimd evtstrm aes crc32\n")
    assert cpu.detect("aarch64", "Linux", none, str(info)) == ()
    assert cpu.detect("aarch64", "Linux", lse, str(info)) == ("lse",)   # auxv first


def test_cpu_resolve_and_missing():
    assert cpu.resolve("none") == () and cpu.resolve("LSE") == ("lse",) and cpu.resolve("lse,lse") == ("lse",)
    assert cpu.resolve("auto", machine="x86_64") == ()
    with pytest.raises(ValueError):
        cpu.resolve("sve9")
    assert cpu.text(("lse",)) == "lse" and cpu.text(()) == ""
    assert cpu.missing("lse", ()) == ("lse",) and cpu.missing("lse", ("lse",)) == ()
    assert cpu.missing("", ()) == () and cpu.missing("lse,sve9", ("lse",)) == ("sve9",)
    assert cpu.flags(("lse",)) == ["-march=armv8-a+lse"] and cpu.flags(()) == []


def test_sysroot_resource_dir(tmp_path):
    """The app's sysroot.zip carries the embedded clang's resource directory: -resource-dir and the builtins come
    from there. The NDK's sysroot has none: no flag, the compiler is asked for its builtins."""
    sys_ = str(tmp_path)
    assert cc.sysroot_resource_dir(sys_) is None
    assert not any(f.startswith("-resource-dir") for f in cc.compile_flags("2", "/tk", "/w/p", sys_))
    rd = tmp_path / "lib" / "clang" / "19"
    (rd / "include").mkdir(parents=True)
    (rd / "include" / "stddef.h").write_text("")
    (tmp_path / "lib" / "clang" / "18").mkdir()            # no headers: not a resource directory
    rt = rd / "lib" / "linux" / "libclang_rt.builtins-aarch64-android.a"
    rt.parent.mkdir(parents=True)
    rt.write_bytes(b"!<arch>\n")
    assert cc.sysroot_resource_dir(sys_) == str(rd)
    assert "-resource-dir=" + str(rd) in cc.compile_flags("2", "/tk", "/w/p", sys_)
    tc = cc.Toolchain("/data/app/lib/libxbacc.so", sys_, cc_prefix=["clang"])
    assert tc.builtins() == str(rt)                           # without running the compiler


def test_collect_sources(tmp_path):
    for rel in ("src/main.c", "src/recomp_manual.c", "src/title/t.c", "src/extra/e.c",
                "src/recomp/gen/recomp_0001.c", "src/recomp/gen/recomp_0000.c", "src/recomp/gen/recomp_funcs.h"):
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("")
    got = [os.path.relpath(s, str(tmp_path)) for s in cc.collect_sources(str(tmp_path))]
    assert got == ["src/main.c", "src/recomp_manual.c", "src/title/t.c", "src/extra/e.c",
                   "src/recomp/gen/recomp_0000.c", "src/recomp/gen/recomp_0001.c"]


def test_link_args(tmp_path):
    tc = cc.Toolchain("/ndk/bin/clang", "/sys", builtins="/rt/libclang_rt.builtins-aarch64-android.a")
    args = cc.link_args(["/o/a.o", "/o/b.o"], "/p/libtitle.so", tc, "/shell/libmain.so")
    assert args[0] == "/ndk/bin/ld.lld"
    for want in ("-shared", "-Bsymbolic", "--no-undefined", "-l:libmain.so", "-L/shell", "-lm", "-ldl",
                 "-llog", "/sys/usr/lib/aarch64-linux-android/30/crtbegin_so.o",
                 "/sys/usr/lib/aarch64-linux-android/30/crtend_so.o", "/rt/libclang_rt.builtins-aarch64-android.a"):
        assert want in args
    assert args.index("/o/b.o") < args.index("-l:libmain.so")
    assert args[-1].endswith("crtend_so.o")
    multi = cc.Toolchain("/data/app/lib/libxbacc.so", "/sys", cc_prefix=["clang"], builtins="/rt/b.a")
    assert multi.cc_argv == ["/data/app/lib/libxbacc.so", "clang"]
    assert multi.ld_argv == ["/data/app/lib/libxbacc.so", "ld.lld"]


# ── pack.json and publish ──

def _lib(d, data=b"\x7fELF fake"):
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, pack.LIB)
    with open(p, "wb") as f:
        f.write(data)
    return p


def test_make_meta_fields(tmp_path):
    lib = _lib(str(tmp_path))
    now = datetime.datetime(2026, 11, 10, 12, 30, 5, 123, tzinfo=datetime.timezone.utc)
    m = pack.make_meta(lib, "4d5300d1", "Some title", "ab" * 32, "9bcb508", "clang 19.0.1 -O1", now=now)
    assert tuple(m) == pack.FIELDS
    assert m["format"] == 1 and m["title_id"] == "4D5300D1" and m["abi"] == "arm64-v8a"
    assert m["shell_abi"] == 1 and m["lib"] == "libtitle.so" and m["entry"] == "recomp_title_main"
    assert m["lib_size"] == 9 and m["lib_sha256"] == pack.sha256_file(lib)
    assert m["built_at"] == "2026-11-10T12:30:05Z"
    json.dumps(m)


def test_publish_atomic_and_replace(tmp_path):
    packs = str(tmp_path / "packs")
    part = pack.begin(packs, "4d5300d1")
    assert part.endswith("4D5300D1.partial")
    lib = _lib(part, b"one")
    # Before publish: no pack, and the partial one has no pack.json yet.
    assert pack.read(pack.final_dir(packs, "4D5300D1")) is None
    assert not os.path.exists(os.path.join(part, "pack.json"))
    final = pack.publish(packs, "4D5300D1", pack.make_meta(lib, "4D5300D1", "t", "x", "k", "c"))
    assert os.listdir(packs) == ["4D5300D1"]
    assert sorted(os.listdir(final)) == ["libtitle.so", "pack.json"]
    assert pack.check(final) is None
    # A second build replaces it whole.
    part = pack.begin(packs, "4D5300D1")
    lib = _lib(part, b"two two")
    pack.publish(packs, "4D5300D1", pack.make_meta(lib, "4D5300D1", "t", "x", "k", "c"))
    assert os.listdir(packs) == ["4D5300D1"]
    assert pack.read(final)["lib_size"] == 7 and pack.check(final) is None


def test_killed_build_leaves_published_pack(tmp_path):
    packs = str(tmp_path / "packs")
    lib = _lib(pack.begin(packs, "AA"), b"good")
    final = pack.publish(packs, "AA", pack.make_meta(lib, "AA", "t", "x", "k", "c"))
    _lib(pack.begin(packs, "AA"), b"half")        # killed before publish
    assert pack.check(final) is None and pack.read(final)["lib_size"] == 4
    pack.begin(packs, "AA")                        # the next run starts a fresh partial
    assert os.listdir(os.path.join(packs, "AA.partial")) == []


def test_recover_between_renames(tmp_path):
    packs = str(tmp_path / "packs")
    lib = _lib(pack.begin(packs, "AA"), b"good")
    final = pack.publish(packs, "AA", pack.make_meta(lib, "AA", "t", "x", "k", "c"))
    os.rename(final, final + ".old")               # killed between the two renames
    pack.recover(packs, "AA")
    assert pack.check(final) is None and not os.path.exists(final + ".old")


def test_check_refuses(tmp_path):
    packs = str(tmp_path / "packs")
    lib = _lib(pack.begin(packs, "AA"), b"good")
    final = pack.publish(packs, "AA", pack.make_meta(lib, "AA", "t", "x", "k", "c", shell_abi=1))
    assert "shell_abi" in pack.check(final, shell_abi=2)
    with open(os.path.join(final, pack.LIB), "wb") as f:
        f.write(b"evil")
    assert "sha256" in pack.check(final)
    os.remove(os.path.join(final, "pack.json"))
    assert pack.check(final) == "no pack.json"


# ── toolkit version without git (the device) ──

def test_toolkit_version_from_version_file(tmp_path, monkeypatch):
    import tools.prepare.__main__ as prep

    def no_git(*a, **k):
        raise OSError("no git")
    monkeypatch.setattr(prep.subprocess, "check_output", no_git)
    monkeypatch.setattr(prep, "VERSION_FILE", str(tmp_path / "VERSION"))
    assert prep.toolkit_version() == "unknown"
    (tmp_path / "VERSION").write_text("9bcb508deadbeef\n")
    assert prep.toolkit_version() == "9bcb508deadbeef"


def test_android_bad_args(capsys):
    import tools.prepare.__main__ as prep
    assert prep.main(["x.xbe", "--android"]) == 1
    out = capsys.readouterr().out.strip()
    assert out.startswith("XBA_RESULT error code=BAD_ARGS msg=--android needs --work")


# ── title.json "env_force" → pack.json, and the desktop project ──

BO3_LIKE = {"title_id": "4541005B", "project": "synthetic", "env_force": {"RECOMP_LSFG": "0", "RECOMP_GPU_FPS_CAP": 60}}
BO3_LIKE_NO_FORCE = {"title_id": "4541005B", "project": "synthetic"}


def test_env_force_of():
    assert pack.env_force_of(None) == {}
    assert pack.env_force_of({"title_id": "X"}) == {}
    assert pack.env_force_of(BO3_LIKE) == {"RECOMP_LSFG": "0", "RECOMP_GPU_FPS_CAP": "60"}
    assert list(pack.env_force_of(BO3_LIKE)) == ["RECOMP_LSFG", "RECOMP_GPU_FPS_CAP"]     # title.json order
    for bad in ({"LD_PRELOAD": "x"}, {"RECOMP_": "1"}, {"recomp_lsfg": "0"}, {"RECOMP_A-B": "0"},
                {"RECOMP_" + "A" * 57: "0"}, {"RECOMP_LSFG": True}, {"RECOMP_LSFG": None},
                {"RECOMP_LSFG": ["0"]}, {"RECOMP_LSFG": "a\nb"}, {"RECOMP_LSFG": "é"},
                {"RECOMP_LSFG": "x" * 256}, ["RECOMP_LSFG"],
                {"RECOMP_T%d" % i: "1" for i in range(pack.ENV_FORCE_MAX + 1)}):
        with pytest.raises(ValueError):
            pack.env_force_of({"env_force": bad})
    assert pack.env_force_of({"env_force": {"RECOMP_" + "A" * 56: "x" * 255}})


def test_make_meta_env_force(tmp_path):
    lib = _lib(str(tmp_path))
    m = pack.make_meta(lib, "4541005b", "t", "x", "k", "c", env_force=pack.env_force_of(BO3_LIKE))
    assert tuple(m) == pack.FIELDS + ("env_force",)
    assert m["env_force"] == {"RECOMP_LSFG": "0", "RECOMP_GPU_FPS_CAP": "60"}
    assert "env_force" not in pack.make_meta(lib, "AA", "t", "x", "k", "c", env_force={})
    packs = str(tmp_path / "packs")
    lib = _lib(pack.begin(packs, "4541005B"), b"good")
    final = pack.publish(packs, "4541005B", pack.make_meta(lib, "4541005B", "t", "x", "k", "c",
                                                          env_force={"RECOMP_LSFG": "0"}))
    with open(os.path.join(final, "pack.json")) as f:
        text = f.read()
    assert json.loads(text)["env_force"] == {"RECOMP_LSFG": "0"}
    assert pack.read(final)["env_force"] == {"RECOMP_LSFG": "0"} and pack.check(final) is None


def _profile(tmp_path, obj):
    from tools.prepare import profiles
    d = tmp_path / "titles" / obj["title_id"].lower()
    d.mkdir(parents=True, exist_ok=True)
    (d / "title.json").write_text(json.dumps(obj))
    (d / "title.c").write_text("/* no hooks */\n")
    return profiles.resolve(obj["title_id"], None, str(tmp_path))


def test_assemble_project_env_force_header(tmp_path):
    import tools.prepare.__main__ as prep
    proj = str(tmp_path / "project")
    prof = _profile(tmp_path, dict(BO3_LIKE, env_force={"RECOMP_LSFG": "0", "RECOMP_X": 'a"b\\c??='}))
    prep.assemble_project(proj, prof, {"title_id": "0x4541005B"})
    with open(os.path.join(proj, "src", "title", "env_force.h")) as f:
        h = f.read()
    assert '#define XBA_TITLE_ENV_FORCE_TID "4541005B"' in h
    assert '    { "RECOMP_LSFG", "0" },' in h
    assert r'    { "RECOMP_X", "a\"b\\c\?\?=" },' in h
    with open(os.path.join(proj, "CMakeLists.txt")) as f:
        assert "src/title/env_force.h" in f.read()
    # Without env_force (or without a profile) there is no header: XBA_TITLE_ENV_FORCE stays undefined.
    prep.assemble_project(proj, _profile(tmp_path, BO3_LIKE_NO_FORCE), {"title_id": "0x4541005B"})
    assert not os.path.exists(os.path.join(proj, "src", "title", "env_force.h"))
    prep.assemble_project(proj, None, {"title_id": "0x4541005B"})
    assert not os.path.exists(os.path.join(proj, "src", "title"))
    with pytest.raises(ValueError):
        _profile(tmp_path, dict(BO3_LIKE, env_force={"PATH": "/"}))


def test_env_force_header_compiles(tmp_path):
    """The generated header with the loop main.c runs over it, built by the host's cc."""
    import shutil
    import subprocess
    import tools.prepare.__main__ as prep
    cc_bin = shutil.which("cc")
    if not cc_bin:
        pytest.skip("no cc")
    (tmp_path / "env_force.h").write_text(prep.env_force_header("4541005b", {"RECOMP_LSFG": "0",
                                                                            "RECOMP_Q": 'x"?\\'}))
    (tmp_path / "t.c").write_text(
        '#include <stdio.h>\n#include "env_force.h"\nint main(void) {\n'
        '    for (unsigned i = 0; i < sizeof xba_title_env_force / sizeof xba_title_env_force[0]; i++)\n'
        '        printf("%s %s=%s\\n", XBA_TITLE_ENV_FORCE_TID, xba_title_env_force[i][0], xba_title_env_force[i][1]);\n'
        '    return 0;\n}\n')
    exe = str(tmp_path / "t")
    subprocess.check_call([cc_bin, "-std=c11", "-Wall", "-Werror", "-o", exe, str(tmp_path / "t.c")])
    assert subprocess.check_output([exe], text=True) == '4541005B RECOMP_LSFG=0\n4541005B RECOMP_Q=x"?\\\n'


class _Args:
    def __init__(self, tmp_path, shell_lib):
        self.work = str(tmp_path / "work")
        self.pack_dir = str(tmp_path / "packs")
        self.shell_lib = shell_lib
        self.shell_abi = 1
        self.opt = "2"
        self.python = None


class _Tc:
    def short_version(self):
        return "clang 19"


def _make_pack(tmp_path, monkeypatch, env_force, layers=(), images=(), toolkit_sha="cd" * 32, title_layer=None,
               cpu_features=()):
    from tools.prepare import android
    links = []

    def fake_link(objects, lib, tc, shell_lib, log=None):
        links.append(lib)
        with open(lib, "wb") as f:
            f.write(b"\x7fELF title")
    monkeypatch.setattr(android.cc, "link", fake_link)
    monkeypatch.setattr(android.cc, "prune", lambda *a, **k: None)
    shell = tmp_path / "libmain.so"
    if not shell.exists():
        shell.write_bytes(b"shell")
    a = _Args(tmp_path, str(shell))
    os.makedirs(a.work, exist_ok=True)
    r = android.Run(a, None)
    r.rep = Reporter()
    if os.path.exists(r.stamp_path):
        with open(r.stamp_path) as f:
            r.stamp = json.load(f)
    r.tid, r.objects, r.toolkit, r.xbe_sha, r.tc = "4541005B", [], "k", "ab" * 32, _Tc()
    r.toolkit_sha = toolkit_sha
    r.info = {"title": "Synthetic"}
    r.env_force = env_force
    r.layers = list(layers)
    r.title_layer = title_layer
    r.cpu_features = tuple(cpu_features)
    r.images = [(rel, ["/o/%s.o" % rel], str(tmp_path / "work" / "images" / rel)) for rel in images]
    final = r.make_pack()
    return pack.read(final), links


def test_android_pack_json_env_force(tmp_path, monkeypatch, capsys):
    meta, links = _make_pack(tmp_path, monkeypatch, pack.env_force_of(BO3_LIKE))
    assert meta["env_force"] == {"RECOMP_LSFG": "0", "RECOMP_GPU_FPS_CAP": "60"} and len(links) == 1
    # Up to date: not linked again.
    meta, links = _make_pack(tmp_path, monkeypatch, pack.env_force_of(BO3_LIKE))
    assert links == [] and meta["env_force"]["RECOMP_LSFG"] == "0"
    # Only title.json's env_force changed (same objects): pack.json is written again.
    meta, links = _make_pack(tmp_path, monkeypatch, {})
    assert len(links) == 1 and "env_force" not in meta and "layers" not in meta


def test_android_pack_json_layers(tmp_path, monkeypatch):
    """pack.json says which profile layers went in; a change of layers alone writes it again."""
    layers = ["xdk/d3d8", "titles/4541005b"]
    meta, links = _make_pack(tmp_path, monkeypatch, {}, layers)
    assert meta["layers"] == layers and len(links) == 1
    meta, links = _make_pack(tmp_path, monkeypatch, {}, layers)
    assert links == []
    meta, links = _make_pack(tmp_path, monkeypatch, {}, layers[1:])
    assert meta["layers"] == layers[1:] and len(links) == 1


def test_android_pack_json_title_layer(tmp_path, monkeypatch):
    """WP6: pack.json says whether the title's optional enhancements went in ("on") or were left out ("off"), for
    the app's « Enhancements » switch; nothing for a title the toolkit has none for. A flip of the switch alone
    writes the pack again."""
    meta, links = _make_pack(tmp_path, monkeypatch, {})
    assert "title_layer" not in meta and len(links) == 1
    meta, links = _make_pack(tmp_path, monkeypatch, {}, ["titles/58424104"], title_layer="on")
    assert meta["title_layer"] == "on" and len(links) == 1
    meta, links = _make_pack(tmp_path, monkeypatch, {}, [], title_layer="off")
    assert meta["title_layer"] == "off" and "layers" not in meta and len(links) == 1
    meta, links = _make_pack(tmp_path, monkeypatch, {}, [], title_layer="off")
    assert links == []


def test_android_pack_json_toolkit_sha256(tmp_path, monkeypatch):
    """XBA-127: pack.json carries the toolkit digest the app compares with its own; another digest links again."""
    meta, links = _make_pack(tmp_path, monkeypatch, {})
    assert meta["toolkit_sha256"] == "cd" * 32 and meta["toolkit"] == "k" and len(links) == 1
    meta, links = _make_pack(tmp_path, monkeypatch, {})
    assert links == []
    meta, links = _make_pack(tmp_path, monkeypatch, {}, toolkit_sha="ef" * 32)
    assert meta["toolkit_sha256"] == "ef" * 32 and len(links) == 1


def test_android_pack_json_cpu_features(tmp_path, monkeypatch):
    """XBA-313: pack.json records the CPU features the objects were compiled for; a CPU without them refuses the
    pack (pack.check, like the loader and the app), and another feature set links again."""
    meta, links = _make_pack(tmp_path, monkeypatch, {}, cpu_features=("lse",))
    assert meta["cpu_features"] == "lse" and len(links) == 1
    final = pack.final_dir(str(tmp_path / "packs"), "4541005B")
    assert pack.check(final, cpu_features=("lse",)) is None
    assert "lacks lse" in pack.check(final, cpu_features=())
    meta, links = _make_pack(tmp_path, monkeypatch, {}, cpu_features=("lse",))
    assert links == []
    meta, links = _make_pack(tmp_path, monkeypatch, {})
    assert "cpu_features" not in meta and len(links) == 1
    assert pack.check(final, cpu_features=()) is None


def test_android_stamps_follow_the_toolkit_digest(tmp_path, monkeypatch):
    """A new commit with the same toolkit tree keeps the steps done; another tree starts over (XBA-127)."""
    from tools.prepare import android
    xbe = tmp_path / "default.xbe"
    xbe.write_bytes(b"XBEH synthetic")

    class Host:
        ROOT = str(tmp_path)
        commit = "c1"

        def find_xbe(self, source, work):
            return str(xbe), None, None

        def sha256(self, p):
            return pack.sha256_file(p)

        def toolkit_version(self):
            return self.commit

        def read_stamp(self, p):
            try:
                with open(p) as f:
                    return json.load(f)
            except OSError:
                return {}

    class Stop(Exception):
        pass

    def no_parse(*a, **k):
        raise Stop()
    host = Host()
    digest = ["11" * 32]
    monkeypatch.setattr(android.toolkit, "digest", lambda: digest[0])
    a = _Args(tmp_path, "libmain.so")
    a.source, a.force = str(tmp_path), False
    os.makedirs(a.work, exist_ok=True)

    def identify():
        r = android.Run(a, host)
        monkeypatch.setattr(r.procs, "run_logged", no_parse)
        try:
            r.identify()
        except Stop:
            pass
        return r
    r = identify()
    r.save_stamp(parse=True, disasm=True)
    host.commit = "c2"                          # an app update that changed only Java
    r = identify()
    assert r.stamp.get("disasm") and r.stamp["toolkit_sha256"] == "11" * 32
    digest[0] = "22" * 32                       # the lifter changed
    r = identify()
    assert "disasm" not in r.stamp and r.stamp["toolkit_sha256"] == "22" * 32 and r.stamp["toolkit"] == "c2"


def test_android_pack_json_images(tmp_path, monkeypatch):
    """XBA-82: one library per other image of the disc, named in pack.json "images"."""
    meta, links = _make_pack(tmp_path, monkeypatch, {}, images=["game.xbe", "Sub/Menu.xbe"])
    assert meta["images"] == {"game.xbe": "libimage-1.so", "Sub/Menu.xbe": "libimage-2.so"}
    assert [os.path.basename(x) for x in links] == ["libtitle.so", "libimage-1.so", "libimage-2.so"]
    final = pack.final_dir(str(tmp_path / "packs"), "4541005B")
    assert pack.check(final) is None
    meta, links = _make_pack(tmp_path, monkeypatch, {}, images=["game.xbe", "Sub/Menu.xbe"])
    assert links == []
    os.remove(os.path.join(final, "libimage-2.so"))
    assert "Sub/Menu.xbe" in pack.check(final)
    meta, links = _make_pack(tmp_path, monkeypatch, {})
    assert "images" not in meta and len(links) == 1


def test_android_other_images(tmp_path, monkeypatch):
    """The images default.xbe names are prepared (a sub-run each), the others not; a failing one is left out."""
    from tools.prepare import android
    game = tmp_path / "game"
    (game / "sub").mkdir(parents=True)
    (game / "default.xbe").write_bytes(b"XBEH\0d:\\game.xbe\0menu.xbe\0")
    (game / "game.xbe").write_bytes(b"XBEH")
    (game / "sub" / "menu.xbe").write_bytes(b"XBEH")
    (game / "unrelated.xbe").write_bytes(b"XBEH")
    ran = []

    def step(self, name):
        def f():
            ran.append((os.path.basename(self.a.source), name))
            if name == "compile":
                if self.a.source.endswith("menu.xbe"):
                    raise android.cc.StepError("COMPILE_FAILED", "x")
                self.objects = ["/o/a.o"]
        return f
    for name in ("identify", "analyse", "lift", "compile"):
        monkeypatch.setattr(android.Run, name, lambda self, n=name: step(self, n)())
    a = _Args(tmp_path, "libmain.so")
    a.source = str(game)
    a.all_images = True                         # up front (the app prepares them lazily)
    os.makedirs(a.work, exist_ok=True)
    r = android.Run(a, None)
    lines = []
    r.rep = type("R", (), {"progress": lambda self, s, p, m, e=None: lines.append(m)})()
    r.xbe, r.data_dir = str(game / "default.xbe"), str(game)
    r.other_images()
    assert [rel for rel, _, _ in r.images] == ["game.xbe"]
    assert ("unrelated.xbe", "identify") not in ran and ("menu.xbe", "compile") in ran
    assert any("sub/menu.xbe left out: COMPILE_FAILED" in m for m in lines)
    assert os.path.isdir(os.path.join(a.work, "images", "game.xbe"))


def test_android_other_images_lazy(tmp_path, monkeypatch):
    """Lazy by default: the first Prepare builds default.xbe only and names the other images (library ""); an image
    the app asked for (images.want), asked with --image, or already in the pack is built, the rest stays named."""
    from tools.prepare import android
    game = tmp_path / "game"
    (game / "sub").mkdir(parents=True)
    (game / "default.xbe").write_bytes(b"XBEH\0d:\\game.xbe\0menu.xbe\0intro.xbe\0")
    for n in ("game.xbe", "sub/menu.xbe", "intro.xbe"):
        (game / n).write_bytes(b"XBEH")
    ran = []

    def step(self, name):
        def f():
            ran.append((os.path.basename(self.a.source), name))
            if name == "compile":
                self.objects = ["/o/a.o"]
        return f
    for name in ("identify", "analyse", "lift", "compile"):
        monkeypatch.setattr(android.Run, name, lambda self, n=name: step(self, n)())
    a = _Args(tmp_path, "libmain.so")
    a.source = str(game)
    os.makedirs(a.work, exist_ok=True)

    def run(**kw):
        for k, v in kw.items():
            setattr(a, k, v)
        del ran[:]
        r = android.Run(a, None)
        lines = []
        r.rep = type("R", (), {"progress": lambda self, s, p, m, e=None: lines.append(m)})()
        r.xbe, r.data_dir, r.tid = str(game / "default.xbe"), str(game), "4541005B"
        r.other_images()
        return r, lines
    r, lines = run()
    assert r.images == [] and ran == []
    assert sorted(r.lazy_images) == ["game.xbe", "intro.xbe", "sub/menu.xbe"]
    assert r.image_libs() == {"game.xbe": "", "intro.xbe": "", "sub/menu.xbe": ""}
    assert any("3 more image(s) prepared on their first launch" in m for m in lines)
    # « Prepare game.xbe » in the app: images.want (any case, '\\').
    (tmp_path / "work" / android.IMAGES_WANT).write_text("\\GAME.XBE\n")
    r, _ = run()
    assert [rel for rel, _, _ in r.images] == ["game.xbe"] and ("game.xbe", "compile") in ran
    assert r.image_libs() == {"game.xbe": "libimage-1.so", "intro.xbe": "", "sub/menu.xbe": ""}
    # --image on a host, and the pack's built images kept by a later Prepare (images.want gone).
    os.remove(str(tmp_path / "work" / android.IMAGES_WANT))
    final = tmp_path / "packs" / "4541005B"
    final.mkdir(parents=True)
    meta = dict({k: "" for k in pack.FIELDS}, images={"game.xbe": "libimage-1.so", "intro.xbe": ""})
    (final / "pack.json").write_text(json.dumps(meta))
    r, _ = run(image=["Sub/Menu.xbe"])
    assert sorted(rel for rel, _, _ in r.images) == ["game.xbe", "sub/menu.xbe"] and r.lazy_images == ["intro.xbe"]
    r, _ = run(image=[], all_images=True)
    assert len(r.images) == 3 and r.lazy_images == []


def test_android_pack_json_lazy_images(tmp_path, monkeypatch):
    """An image named but not built: library "" in pack.json, no file, the pack still valid."""
    from tools.prepare import android
    monkeypatch.setattr(android.Run, "image_libs",
                        lambda self: dict({rel: "libimage-%d.so" % (i + 1)
                                           for i, (rel, _, _) in enumerate(self.images)}, **{"intro.xbe": ""}))
    meta, links = _make_pack(tmp_path, monkeypatch, {}, images=["game.xbe"])
    assert meta["images"] == {"game.xbe": "libimage-1.so", "intro.xbe": ""}
    assert [os.path.basename(x) for x in links] == ["libtitle.so", "libimage-1.so"]
    assert pack.check(pack.final_dir(str(tmp_path / "packs"), "4541005B")) is None


@pytest.mark.parametrize("fmt", ["iso", "cso", "split"])
def test_android_other_images_from_an_image(tmp_path, monkeypatch, fmt):
    """XBA-126: the same from a disc image (plain, compressed, split): the named XBEs are copied out of it into
    <work>/disc-xbe/ and prepared, nothing else is extracted."""
    import sys as _sys
    _sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "xiso"))
    import mkcso
    import mkxiso
    from tools.prepare import android
    from tools.prepare import __main__ as host
    game = tmp_path / "game"
    (game / "sub").mkdir(parents=True)
    (game / "default.xbe").write_bytes(b"XBEH\0d:\\game.xbe\0menu.xbe\0")
    (game / "game.xbe").write_bytes(b"XBEH game")
    (game / "sub" / "menu.xbe").write_bytes(b"XBEH menu")
    (game / "unrelated.xbe").write_bytes(b"XBEH")
    (game / "big.bin").write_bytes(b"\x07" * 200000)
    iso = tmp_path / "Game.iso"
    mkxiso.build(str(game), str(iso))
    src = str(iso)
    if fmt == "cso":
        src = str(tmp_path / "Game.cso")
        with open(src, "wb") as f:
            f.write(mkcso.compress(iso.read_bytes(), "cso2"))
    elif fmt == "split":
        parts = mkcso.write_split(iso.read_bytes(), str(tmp_path / "Disc.iso"), 70000, "dot")
        src = parts[1]                          # any part names the image
    ran = []

    def step(self, name):
        def f():
            ran.append((os.path.basename(self.a.source), name, open(self.a.source, "rb").read()))
            if name == "compile":
                self.objects = ["/o/a.o"]
        return f
    for name in ("identify", "analyse", "lift", "compile"):
        monkeypatch.setattr(android.Run, name, lambda self, n=name: step(self, n)())
    a = _Args(tmp_path, "libmain.so")
    a.source = src
    a.all_images = True
    os.makedirs(a.work, exist_ok=True)
    r = android.Run(a, host)
    r.rep = type("R", (), {"progress": lambda self, s, p, m, e=None: None})()
    xbe, r.data_dir, r.image = host.find_xbe(src, a.work)
    assert r.data_dir is None and r.image
    r.xbe = xbe
    r.other_images()
    assert sorted(rel for rel, _, _ in r.images) == ["game.xbe", "sub/menu.xbe"]
    assert ("game.xbe", "identify", b"XBEH game") in ran and ("menu.xbe", "identify", b"XBEH menu") in ran
    assert not any(n == "unrelated.xbe" for n, _, _ in ran)
    copied = sorted(os.path.relpath(os.path.join(d, f), os.path.join(a.work, "disc-xbe"))
                    for d, _, fs in os.walk(os.path.join(a.work, "disc-xbe")) for f in fs)
    assert copied == ["game.xbe", os.path.join("sub", "menu.xbe")]


def test_android_killed_lift_never_passes_as_up_to_date(tmp_path, monkeypatch):
    """A lift killed half way, then the earlier profile key back, lifts again (XBA-138).

    The lift wipes gen/ and writes it in one process; its stamp is the profile
    digest. If the old stamp outlived the wipe, a run killed in the lift with
    key B, followed by a run with key A again (the « Enhancements » switch
    turned off, then on), would take the half-written gen/ as A's lift.
    """
    from tools.prepare import android

    class Killed(Exception):
        pass

    keys = {"profile": "A"}

    class Host:
        ROOT = str(tmp_path)

        def assemble_project(self, proj, profile, info):
            os.makedirs(os.path.join(proj, "src", "recomp", "gen"), exist_ok=True)

        def profile_digest(self, profile):
            return keys["profile"]

        def lift_cut_bodies(self, log, at):
            return ""

        def lift_health(self, log, at):     # XBA-139: an older lift's answer
            return None

        def health_line(self, health):
            return None

    a = _Args(tmp_path, "libmain.so")
    a.extra_src = []
    os.makedirs(a.work)
    gen = os.path.join(a.work, "project", "src", "recomp", "gen")
    lifts = []

    def run(kill):
        r = android.Run(a, Host())
        if os.path.exists(r.stamp_path):
            with open(r.stamp_path) as f:
                r.stamp = json.load(f)
        r.an, r.xbe, r.tid, r.profile, r.info = str(tmp_path), str(tmp_path / "x.xbe"), "T", None, {}

        def py_step(args, *rest):
            lifts.append(keys["profile"])
            for i in range(3):
                with open(os.path.join(gen, "chunk_%d.c" % i), "w") as f:
                    f.write("/* %s */" % keys["profile"])
                if kill:
                    raise Killed()
        monkeypatch.setattr(r, "py_step", py_step)
        r.lift()

    (tmp_path / "x.xbe").write_bytes(b"XBEH")
    run(kill=False)                             # A, complete
    assert lifts == ["A"] and len(os.listdir(gen)) == 3
    keys["profile"] = "B"
    with pytest.raises(Killed):
        run(kill=True)                          # B, killed after one file
    assert os.listdir(gen) == ["chunk_0.c"]
    with open(os.path.join(a.work, "stamp.json")) as f:
        assert "lift" not in json.load(f)
    keys["profile"] = "A"
    run(kill=False)                             # A again: lifted, not « up to date »
    assert lifts == ["A", "B", "A"] and len(os.listdir(gen)) == 3
    run(kill=False)                             # and now it is
    assert lifts == ["A", "B", "A"]


def test_resume_entries(tmp_path):
    """XBA-258: pack.json "resume" comes from recomp_dispatch.c's call-site table (XBA-250's resume entries)."""
    def disp(name, text):
        p = tmp_path / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        return str(p)

    other = disp("src/game/recomp_0001.c", "static const uint32_t g_recomp_call_site_count = 9u;\n")
    with_sites = disp("a/recomp_dispatch.c", "static const uint32_t g_recomp_call_site_count = 1234u;\n")
    none = disp("b/recomp_dispatch.c",
                "    0u, 0u, 0u   /* none */\nstatic const uint32_t g_recomp_call_site_count = 0u;\n")
    old = disp("c/recomp_dispatch.c", "int recomp_dispatch_init(void) { return 1; }\n")
    assert pack.resume_entries([other, with_sites])
    assert not pack.resume_entries([none])
    assert not pack.resume_entries([old])                  # a lifter from before XBA-250: no table
    assert not pack.resume_entries([other])                # the table is only read from recomp_dispatch.c
    assert not pack.resume_entries([str(tmp_path / "gone/recomp_dispatch.c")])


def test_android_analysis_stamp_follows_the_profile_seeds(tmp_path, monkeypatch):
    """The analysis is redone when the profile's seed_functions change (a
    layer with seeds added or dropped, as the « Enhancements » switch does,
    or a seeds file edited), and kept when they do not; a stamp made without
    profile seeds stays valid."""
    from tools.prepare import android, discovered
    monkeypatch.setattr(discovered, "refresh", lambda *a, **k: None)
    monkeypatch.setattr(discovered, "analysis_counts", lambda an: None)
    for k in ("RECOMP_STATIC_DISCOVERY", "RECOMP_STATIC_DISCOVERY_CAP", "RECOMP_STATIC_DISCOVERY_STRICT"):
        monkeypatch.delenv(k, raising=False)
    a = _Args(tmp_path, str(tmp_path / "libmain.so"))
    os.makedirs(a.work, exist_ok=True)
    xbe = tmp_path / "default.xbe"
    xbe.write_bytes(b"XBEH" + b"\0" * 60)
    seeds = tmp_path / "seeds.txt"
    seeds.write_text("0x00011000\n")
    ran = []

    def analyse(profile):
        r = android.Run(a, None)
        r.rep = Reporter()
        with open(r.stamp_path) as f:
            r.stamp = json.load(f)
        r.xbe, r.xjson, r.profile = str(xbe), str(tmp_path / "x.json"), profile
        del ran[:]
        monkeypatch.setattr(r, "py_step", lambda args, *rest: ran.append(args[0]))
        r.analyse()
        return r.stamp

    every = ["tools.disasm", "tools.func_id", "tools.abi_analysis"]
    # A stamp from before the profile seeds counted, everything done: kept.
    pack.write_json_atomic(os.path.join(a.work, "stamp.json"),
                           {"disasm": True, "func_id": True, "abi": True, "lift": "k", "discovered": ""})
    analyse(None)
    assert ran == []
    analyse({"seed_functions": []})
    assert ran == []
    st = analyse({"seed_functions": [str(seeds)]})      # a layer with seeds comes in
    assert ran == every and "lift" not in st
    analyse({"seed_functions": [str(seeds)]})
    assert ran == []
    seeds.write_text("0x00011000\n0x00012000\n")      # its seeds file changes
    analyse({"seed_functions": [str(seeds)]})
    assert ran == every
    analyse(None)                                       # the layer goes out
    assert ran == every


def test_android_seeds_suffix(tmp_path):
    from tools.prepare import android
    assert android.seeds_suffix([]) == ""
    f = tmp_path / "s.txt"
    f.write_text("0x1000\n")
    one = android.seeds_suffix([str(f)])
    assert one.startswith("|seeds=") and one == android.seeds_suffix([str(f)])
    assert android.seeds_suffix([str(f), str(f)]) != one
    assert android.seeds_suffix(["/nonexistent/a"]) != android.seeds_suffix(["/nonexistent/b"])


@pytest.mark.skipif(os.name == "nt", reason="no SIGUSR1 on Windows")
def test_procs_cancel_in_a_signal_handler_does_not_deadlock(tmp_path):
    """The SIGTERM handler calls Procs.cancel() on the main thread, wherever
    it was interrupted: inside `with procs.lock` (start() / done()) too. It
    must still kill the children and return, not wait forever for the lock
    the interrupted code holds. In a child process, so a hang fails the
    test instead of stopping the suite."""
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    code = ("import os, signal, sys; sys.path.insert(0, %r)\n"
            "from tools.prepare import compile as cc\n"
            "procs = cc.Procs()\n"
            "p = procs.start([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
            "signal.signal(signal.SIGUSR1, lambda n, f: procs.cancel())\n"
            "with procs.lock:\n"
            "    os.kill(os.getpid(), signal.SIGUSR1)\n"
            "    for _ in range(1000):\n"
            "        pass\n"
            "assert procs.cancelled.is_set()\n"
            "p.wait(timeout=10)\n"
            "print('rc', p.returncode)\n") % root
    out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=30)
    assert out.returncode == 0, out.stderr
    assert out.stdout.strip() == "rc -9"


def test_procs_start_kills_a_child_a_cancel_interrupts(monkeypatch):
    """A signal handler's Cancelled raised in start() after Popen but before
    the child is in `live` (cancel() could not see it): start() kills it."""
    procs = cc.Procs()
    started = []
    real = subprocess.Popen

    class Lock:
        def __enter__(self):
            raise cc.Cancelled("signal 15")

        def __exit__(self, *exc):
            return False

    def popen(*a, **k):
        started.append(real(*a, **k))
        return started[-1]
    procs.lock = Lock()
    monkeypatch.setattr(cc.subprocess, "Popen", popen)
    with pytest.raises(cc.Cancelled):
        procs.start([sys.executable, "-c", "import time; time.sleep(60)"])
    assert started[0].wait(timeout=10) != 0


# ── The key cache (keys.json): a Prepare with nothing to compile reads nothing ──

class _TouchTc:
    """A toolchain whose "compile" only creates the object."""

    def __init__(self):
        self.procs = cc.Procs()

    def compiler_id(self):
        return "clang version 19 (test)"

    def compile_args(self, flags, src, out):
        return [sys.executable, "-c", "import sys; open(sys.argv[1], 'w').close()", out]


def _key_project(tmp_path):
    """A title project with a generated chunk, main.c and a header, all
    dated a minute ago (older than keys.json's racy window)."""
    proj = tmp_path / "proj"
    gen = proj / "src" / "recomp" / "gen"
    gen.mkdir(parents=True)
    (gen / "recomp_funcs.h").write_text("void sub_1(void);\n")
    (gen / "recomp_0000.c").write_text('#include "recomp_funcs.h"\nvoid sub_1(void) {}\n')
    (proj / "src" / "main.c").write_text("int main(void) { return 0; }\n")
    _age(proj)
    flags = ["-O2", "-I" + str(proj / "src"), "-I" + str(gen)]
    return proj, flags


def _age(top, seconds=60):
    t = __import__("time").time() - seconds
    for d, _, files in os.walk(str(top)):
        for fn in files:
            os.utime(os.path.join(d, fn), (t, t))


def _compile(proj, flags, obj):
    stats = {}
    objs, n = cc.compile_all(cc.collect_sources(str(proj)), flags, _TouchTc(), str(obj), 2, stats=stats)
    return objs, n, stats["hashed"]


def test_keys_cache_skips_the_rehash_and_keeps_the_keys(tmp_path):
    proj, flags = _key_project(tmp_path)
    obj = tmp_path / "obj"
    objs, n, hashed = _compile(proj, flags, obj)
    assert (n, hashed) == (2, 2) and (obj / cc.KEYS_FILE).is_file()
    # Nothing changed: nothing read, nothing compiled, the same objects.
    assert _compile(proj, flags, obj) == (objs, 0, 0)
    # The keys are the ones computed from the bytes: without keys.json, the same objects.
    os.remove(str(obj / cc.KEYS_FILE))
    assert _compile(proj, flags, obj) == (objs, 0, 2)
    # Other flags: every key again.
    assert _compile(proj, flags + ["-DX"], obj)[1:] == (2, 2)
    # A stale or broken keys.json is not trusted.
    (obj / cc.KEYS_FILE).write_text("{not json")
    assert _compile(proj, flags, obj) == (objs, 0, 2)
    (obj / cc.KEYS_FILE).write_text('{"v": 0, "files": {}}')
    assert _compile(proj, flags, obj) == (objs, 0, 2)


def test_keys_cache_sees_a_change_that_keeps_size_and_mtime(tmp_path):
    """A source rewritten with the same size and its old mtime put back (cp -p,
    an unzip) is still read again: ctime and the inode are in the stat."""
    proj, flags = _key_project(tmp_path)
    obj = tmp_path / "obj"
    objs, _, _ = _compile(proj, flags, obj)
    main = proj / "src" / "main.c"
    st = os.stat(str(main))
    main.write_text("int main(void) { return 1; }\n")
    os.utime(str(main), ns=(st.st_atime_ns, st.st_mtime_ns))
    assert os.stat(str(main)).st_size == st.st_size
    objs2, n, hashed = _compile(proj, flags, obj)
    assert (n, hashed) == (1, 1) and objs2[0] != objs[0] and objs2[1] == objs[1]      # main.c, the chunk


@pytest.mark.skipif(os.name == "nt", reason="ctime is the creation time on Windows")
def test_keys_cache_sees_a_header_change(tmp_path):
    proj, flags = _key_project(tmp_path)
    obj = tmp_path / "obj"
    objs, _, _ = _compile(proj, flags, obj)
    h = proj / "src" / "recomp" / "gen" / "recomp_funcs.h"
    st = os.stat(str(h))
    h.write_text("void sub_2(void);\n")             # same size, mtime put back
    os.utime(str(h), ns=(st.st_atime_ns, st.st_mtime_ns))
    _age(proj)
    objs2, n, hashed = _compile(proj, flags, obj)
    # Every key computed again; main.c includes the whole header, the chunk not its declarations.
    assert hashed == 2 and n == 1 and objs2[0] != objs[0] and objs2[1] == objs[1]


def test_keys_cache_does_not_remember_a_racy_file(tmp_path):
    """A file written just now could be written again within the same
    timestamp step and size: keys.json leaves it out, it is read next time."""
    proj, flags = _key_project(tmp_path)
    obj = tmp_path / "obj"
    _compile(proj, flags, obj)
    (proj / "src" / "main.c").write_text("int main(void) { return 2; }\n")     # now
    assert _compile(proj, flags, obj)[1:] == (1, 1)
    assert _compile(proj, flags, obj)[1:] == (0, 1)       # still racy: read again
    _age(proj)
    assert _compile(proj, flags, obj)[1:] == (0, 2)       # aged (utime moves ctime too): read once more
    assert _compile(proj, flags, obj)[1:] == (0, 0)


def test_prune_keeps_keys_json(tmp_path):
    proj, flags = _key_project(tmp_path)
    obj = tmp_path / "obj"
    objs, _, _ = _compile(proj, flags, obj)
    (obj / "old.o").write_bytes(b"x")
    cc.prune(str(obj), objs)
    assert sorted(os.listdir(str(obj))) == sorted([os.path.basename(o) for o in objs] + [cc.KEYS_FILE])
