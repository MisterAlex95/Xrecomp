"""tools.prepare --android: the progress grammar, the jobs formula, the object
cache key, pack.json and the atomic publish (docs/android/prepare.md)."""

import datetime
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.prepare import compile as cc            # noqa: E402
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
    (4.9, 8, 1),
    (5.0, 8, 2),
    (6.0, 8, 3),
    (7.5, 8, 4),
    (12, 8, 5),          # (12 - 3) / 1 = 9 -> capped at 5
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


def test_compile_flags():
    f = cc.compile_flags("1", "/tk", "/w/project", "/sys", profile=True)
    for want in ("--target=aarch64-linux-android30", "--sysroot=/sys", "-O1", "-g0", "-fPIC", "-std=gnu11",
                 "-w", "-DXBOXRECOMP_SHELL_TITLE=1", "-DXBA_TITLE_PROFILE=1", "-I/w/project/src",
                 "-I/w/project/src/recomp", "-I/tk/include", "-I/tk/src", "-I/tk/src/kernel"):
        assert want in f
    assert "-DXBA_TITLE_PROFILE=1" not in cc.compile_flags("1", "/tk", "/w/p", "/sys")


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
