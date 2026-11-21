"""tools.prepare's desktop build without CMake (XBA-86): toolchain discovery,
the command lines per host, the route, the incremental rebuild, the runtime
manifest; and with XBA_E2E=1 the real thing on this host (runtime built with
CMake, then a title prepared with a fake failing cmake first on PATH, and run)."""

import json
import os
import shutil
import stat
import subprocess
import sys
import textwrap

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.prepare import compile as cc            # noqa: E402
from tools.prepare import desktop_build as db      # noqa: E402
from tools.prepare import runtime as rt            # noqa: E402

TK = "/tk"


def _files(*paths):
    s = set(paths)
    return lambda p: p in s


def _which(**tools):
    return lambda name: tools.get(name)


def _man(rdir="/tk/runtime/linux-x86_64", lang="C", link=None, **kv):
    m = {"format": rt.FORMAT, "host": "linux-x86_64", "runtime_sha256": "abc", "dir": rdir,
         "defines": ["NOMINMAX", "WIN32_LEAN_AND_MEAN"], "includes": ["{toolkit}/src/kernel", "/usr/include/SDL2"],
         "compile_flags": ["-DNDEBUG"], "link_language": lang,
         "link": link if link is not None else [
             {"arg": "-Wl,--start-group", "role": "libraries"},
             {"arg": "{runtime}/lib/libxbox_kernel.a", "role": "libraries"},
             {"arg": "/usr/lib/x86_64-linux-gnu/libSDL2.so", "role": "libraries"},
             {"arg": "-lm", "role": "libraries"},
             {"arg": "-Wl,--end-group", "role": "libraries"}]}
    m.update(kv)
    return m


# ── host and toolchain discovery ──

@pytest.mark.parametrize("system,machine,want", [
    ("Linux", "x86_64", "linux-x86_64"), ("Linux", "aarch64", "linux-arm64"), ("Darwin", "arm64", "macos-arm64"),
    ("Windows", "AMD64", "windows-x86_64"), ("MSYS_NT-10.0", "x86_64", "windows-x86_64")])
def test_host_id(system, machine, want):
    assert rt.host_id(system, machine) == want


def test_host_id_from_the_environment(monkeypatch):
    """$XBA_HOST: another host's runtime (a cross build, XBA-87), when no host is given."""
    monkeypatch.setenv("XBA_HOST", "linux-arm64")
    assert rt.host_id() == "linux-arm64"
    assert rt.runtime_dir("/tk").endswith(os.path.join("runtime", "linux-arm64"))
    assert rt.host_id("Linux", "x86_64") == "linux-x86_64"


def test_explicit_compiler_wins():
    env = {"XBA_CC": "/env/clang"}
    tc = db.find_toolchain(TK, "linux-x86_64", cc_path="/my/clang", env=env,
                           which=_which(clang="/usr/bin/clang"), isfile=_files("/tk/toolchain/bin/clang"))
    assert (tc.cc, tc.source, tc.kind) == ("/my/clang", "explicit", "clang")
    tc = db.find_toolchain(TK, "linux-x86_64", env=env, which=_which(), isfile=_files("/tk/toolchain/bin/clang"))
    assert (tc.cc, tc.source) == ("/env/clang", "explicit")


def test_toolkit_clang_before_the_system():
    isfile = _files("/tk/toolchain/bin/clang", "/tk/toolchain/bin/ld.lld")
    tc = db.find_toolchain(TK, "linux-x86_64", env={}, which=_which(clang="/usr/bin/clang", cc="/usr/bin/cc"),
                           isfile=isfile)
    assert (tc.cc, tc.source, tc.lld) == ("/tk/toolchain/bin/clang", "toolkit", "/tk/toolchain/bin/ld.lld")
    assert tc.isysroot is None


def test_system_clang_then_versioned_then_cc(tmp_path):
    tc = db.find_toolchain(TK, "linux-x86_64", env={}, which=_which(clang="/usr/bin/clang", cc="/usr/bin/cc"),
                           isfile=_files())
    assert (tc.cc, tc.source, tc.lld) == ("/usr/bin/clang", "system", None)
    tc = db.find_toolchain(TK, "linux-x86_64", env={}, which=_which(clang="/usr/bin/clang",
                                                                      **{"ld.lld": "/usr/bin/ld.lld"}),
                           isfile=_files())
    assert tc.lld == "/usr/bin/ld.lld"
    d = tmp_path / "bin"
    d.mkdir()
    for n in ("clang-14", "clang-19", "clang-format"):
        (d / n).write_text("")
    tc = db.find_toolchain(TK, "linux-x86_64", env={"PATH": str(d)}, which=_which(cc="/usr/bin/cc"),
                           isfile=os.path.isfile)
    assert tc.cc == str(d / "clang-19") and tc.kind == "clang"
    tc = db.find_toolchain(TK, "linux-x86_64", env={"PATH": ""}, which=_which(gcc="/usr/bin/gcc"), isfile=_files())
    assert (tc.cc, tc.kind, tc.lld) == ("/usr/bin/gcc", "gnu", None)
    with pytest.raises(cc.StepError) as e:
        db.find_toolchain(TK, "linux-x86_64", env={"PATH": ""}, which=_which(), isfile=_files())
    assert e.value.code == "NO_COMPILER"


def test_macos_toolkit_clang_gets_an_sdk():
    isfile = _files("/tk/toolchain/bin/clang", "/tk/toolchain/bin/ld64.lld")
    tc = db.find_toolchain(TK, "macos-arm64", env={"SDKROOT": "/sdk/MacOSX.sdk"}, which=_which(), isfile=isfile)
    assert (tc.lld, tc.isysroot) == ("/tk/toolchain/bin/ld64.lld", "/sdk/MacOSX.sdk")
    # Apple's clang knows its SDK, and links with ld64 (no lld from PATH).
    tc = db.find_toolchain(TK, "macos-arm64", env={}, which=_which(clang="/usr/bin/clang",
                                                                    **{"ld64.lld": "/opt/ld64.lld"}),
                           isfile=_files())
    assert (tc.lld, tc.isysroot) == (None, None)
    # Homebrew's LLVM first on PATH: not Apple's, so the SDK too.
    tc = db.find_toolchain(TK, "macos-arm64", env={"SDKROOT": "/sdk/MacOSX.sdk"},
                           which=_which(clang="/opt/homebrew/opt/llvm/bin/clang"), isfile=_files())
    assert tc.isysroot == "/sdk/MacOSX.sdk"


def test_windows_clang_cl_and_lld_link():
    isfile = _files("/tk/toolchain/bin/clang-cl.exe", "/tk/toolchain/bin/lld-link.exe")
    tc = db.find_toolchain(TK, "windows-x86_64", env={}, which=_which(), isfile=isfile)
    assert (tc.kind, tc.lld) == ("msvc", "/tk/toolchain/bin/lld-link.exe")
    tc = db.find_toolchain(TK, "windows-x86_64", env={}, which=_which(**{"clang-cl": "C:/LLVM/bin/clang-cl.exe"}),
                           isfile=_files())
    assert (tc.cc, tc.lld) == ("C:/LLVM/bin/clang-cl.exe", "lld-link.exe")


# ── command lines ──

def _tc(host="linux-x86_64", cc_path="/usr/bin/clang", **kv):
    return db.DesktopToolchain(cc_path, host, kv.pop("source", "system"), **kv)


def test_linux_compile_and_link():
    tc = _tc()
    man = _man()
    flags = db.compile_flags(tc, man, TK, "/p", "2", ['XBA_DISCOVERY_FILE="/w/d.jsonl"'])
    assert flags[:5] == ["-O2", "-g0", "-std=gnu11", "-w", "-fno-math-errno"]
    assert "-DNDEBUG" in flags and "-DNOMINMAX" in flags and '-DXBA_DISCOVERY_FILE="/w/d.jsonl"' in flags
    incs = [f[2:] for f in flags if f.startswith("-I")]
    assert incs == ["/p/src", "/p/src/recomp", "/p/src/recomp/gen", "/tk/include", "/tk/src/kernel",
                    "/usr/include/SDL2"]
    assert tc.compile_args(flags, "a.c", "a.o")[-4:] == ["-c", "a.c", "-o", "a.o"]
    args = db.link_args(tc, man, TK, ["/o/1.o", "/o/2.o"], "/p/build/t", exists=lambda p: True)
    assert args == ["/usr/bin/clang", "-o", "/p/build/t", "/o/1.o", "/o/2.o", "-Wl,--start-group",
                    "/tk/runtime/linux-x86_64/lib/libxbox_kernel.a", "/usr/lib/x86_64-linux-gnu/libSDL2.so",
                    "-lm", "-Wl,--end-group"]


def test_compile_flags_cpu_features():
    """XBA-313: the host CPU's ARMv8.1 atomics inline on an aarch64 Linux, unless the runtime picks a -march."""
    tc = _tc("linux-arm64")
    assert "-march=armv8-a+lse" in db.compile_flags(tc, _man(), TK, "/p", cpu_features=("lse",))
    assert not any(f.startswith("-march") for f in db.compile_flags(tc, _man(), TK, "/p"))
    man = _man(compile_flags=["-DNDEBUG", "-mcpu=cortex-a76"])
    assert "-march=armv8-a+lse" not in db.compile_flags(tc, man, TK, "/p", cpu_features=("lse",))


def test_linux_cxx_runtime_links_with_lld_and_the_cxx_driver():
    tc = _tc(cc_path="/tk/toolchain/bin/clang", source="toolkit", lld="/tk/toolchain/bin/ld.lld")
    args = db.link_args(tc, _man(lang="CXX"), TK, ["/o/1.o"], "/p/build/t", exists=lambda p: False)
    assert args[:3] == ["/tk/toolchain/bin/clang", "--driver-mode=g++", "--ld-path=/tk/toolchain/bin/ld.lld"]
    # Another distribution keeps SDL2 elsewhere: by name.
    assert "-lSDL2" in args and "/usr/lib/x86_64-linux-gnu/libSDL2.so" not in args
    gcc = _tc(cc_path="/usr/bin/gcc", kind="gnu")
    assert db.link_args(gcc, _man(lang="CXX"), TK, [], "t", exists=lambda p: True)[0] == "/usr/bin/g++"
    assert _tc(cc_path="/usr/bin/x86_64-linux-gnu-gcc-14", kind="gnu").link_driver("CXX") == \
        ["/usr/bin/x86_64-linux-gnu-g++-14"]


def test_macos_link_line():
    tc = _tc("macos-arm64", "/tk/toolchain/bin/clang", source="toolkit", lld="/tk/toolchain/bin/ld64.lld",
             isysroot="/sdk/MacOSX.sdk")
    man = _man("/tk/runtime/macos-arm64", lang="CXX", link=[
        {"arg": "{runtime}/lib/libxbox_kernel.a", "role": "libraries"},
        {"arg": "/opt/homebrew/lib/libSDL2.dylib", "role": "libraries"},
        {"arg": "-framework", "role": "libraries"}, {"arg": "OpenGL", "role": "libraries"}])
    flags = db.compile_flags(tc, man, TK, "/p")
    assert flags[5:7] == ["-isysroot", "/sdk/MacOSX.sdk"]
    args = db.link_args(tc, man, TK, ["/o/1.o"], "/p/build/t", exists=lambda p: True)
    assert args == ["/tk/toolchain/bin/clang", "--driver-mode=g++", "--ld-path=/tk/toolchain/bin/ld64.lld",
                    "-isysroot", "/sdk/MacOSX.sdk", "-o", "/p/build/t", "/o/1.o",
                    "/tk/runtime/macos-arm64/lib/libxbox_kernel.a", "/opt/homebrew/lib/libSDL2.dylib",
                    "-framework", "OpenGL"]


def test_windows_clang_cl_and_lld_link_lines():
    tc = _tc("windows-x86_64", "C:/LLVM/bin/clang-cl.exe", lld="C:/LLVM/bin/lld-link.exe")
    man = _man("C:/tk/runtime/windows-x86_64", lang="C", compile_flags=["/DNDEBUG", "/MD"], link=[
        {"arg": "/machine:x64", "role": "flags"}, {"arg": "/subsystem:windows", "role": "flags"},
        {"arg": "{runtime}/lib/xbox_kernel.lib", "role": "libraries"},
        {"arg": "d3d11.lib", "role": "libraries"}, {"arg": "kernel32.lib", "role": "libraries"}])
    flags = db.compile_flags(tc, man, "C:/tk", "C:/p", "2", ["XBA_TITLE_PROFILE=1"])
    assert flags[:4] == ["/nologo", "/O2", "/w", "/std:c11"]
    assert "/DNDEBUG" in flags and "/MD" in flags and "/DXBA_TITLE_PROFILE=1" in flags
    assert "/IC:/tk/include" in flags and not any(f.startswith("-") for f in flags)
    assert tc.compile_args(flags, "a.c", "a.o")[-3:] == ["/c", "a.c", "/Foa.o"]
    args = db.link_args(tc, man, "C:/tk", ["a.o"], "C:/p/build/t.exe", exists=lambda p: True)
    assert args == ["C:/LLVM/bin/lld-link.exe", "/nologo", "/OUT:C:/p/build/t.exe", "a.o", "/machine:x64",
                    "/subsystem:windows", "C:/tk/runtime/windows-x86_64/lib/xbox_kernel.lib", "d3d11.lib",
                    "kernel32.lib"]
    assert db.exe_name("t", "windows-x86_64") == "t.exe" and db.exe_name("t", "linux-arm64") == "t"


class _FakeProcs:
    def __init__(self, out, rc=2):
        self.out, self.rc, self.calls = out, rc, []

    def run(self, args, **kw):
        self.calls.append(list(args))
        return self.rc, self.out


def test_windows_msvc_cl_and_link():
    """XBA-89: Visual Studio's cl when there is no clang-cl, linked by the link.exe beside it."""
    cl = "C:/VS/VC/Tools/MSVC/14.44/bin/Hostx64/x64/cl.exe"
    procs = _FakeProcs("Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35211 for x64\n"
                       "usage: cl [ option... ] filename... [ /link linkoption... ]\n")
    tc = db.find_toolchain(TK, "windows-x86_64", env={}, which=_which(cl=cl), isfile=_files(), procs=procs)
    assert (tc.kind, tc.lld, tc.source) == ("msvc", None, "system")
    assert tc.link_driver("C") == ["C:/VS/VC/Tools/MSVC/14.44/bin/Hostx64/x64/link.exe"]
    assert tc.short_version() == "cl 19.44.35211" and procs.calls == [[cl]]
    assert db.compiler_kind("C:\\VS\\bin\\CL.EXE") == "msvc"
    # clang-cl first when both are there.
    tc = db.find_toolchain(TK, "windows-x86_64", env={}, isfile=_files(),
                           which=_which(cl=cl, **{"clang-cl": "C:/LLVM/bin/clang-cl.exe",
                                                  "lld-link.exe": "C:/LLVM/bin/lld-link.exe"}))
    assert (tc.cc, tc.lld) == ("C:/LLVM/bin/clang-cl.exe", "C:/LLVM/bin/lld-link.exe")
    with pytest.raises(cc.StepError) as e:
        db.find_toolchain(TK, "windows-x86_64", env={}, which=_which(), isfile=_files())
    assert e.value.code == "NO_COMPILER" and "developer prompt" in e.value.msg


def test_windows_c_runtime_matches_the_libraries():
    """CMake writes MSVC's C runtime as -MD: kept, and /MD added when the manifest names none."""
    tc = _tc("windows-x86_64", "C:/LLVM/bin/clang-cl.exe", lld="C:/LLVM/bin/lld-link.exe")
    man = _man("C:/rt", compile_flags=["/DWIN32", "/DNDEBUG", "-MD", "/bigobj"])
    flags = db.compile_flags(tc, man, "C:/tk", "C:/p")
    assert "-MD" in flags and "/MD" not in flags and "/DWIN32" in flags
    flags = db.compile_flags(tc, _man("C:/rt", compile_flags=["/DNDEBUG"]), "C:/tk", "C:/p")
    assert "/MD" in flags and "/bigobj" in flags


def test_windows_mingw_gcc_against_a_mingw_runtime():
    """The Linux check of the Windows route: x86_64-w64-mingw32-gcc (--cc) links like any GNU driver."""
    tc = db.find_toolchain(TK, "windows-x86_64", cc_path="/usr/bin/x86_64-w64-mingw32-gcc", env={},
                           which=_which(), isfile=_files())
    assert (tc.kind, tc.lld) == ("gnu", None)
    man = _man("/tk/runtime/windows-x86_64", link=[{"arg": "-mwindows", "role": "flags"},
                                                   {"arg": "{runtime}/lib/libxbox_kernel.a", "role": "libraries"},
                                                   {"arg": "-ld3d11", "role": "libraries"}])
    args = db.link_args(tc, man, TK, ["/o/1.o"], "/p/build/t.exe", exists=lambda p: True)
    assert args == ["/usr/bin/x86_64-w64-mingw32-gcc", "-o", "/p/build/t.exe", "/o/1.o", "-mwindows",
                    "/tk/runtime/windows-x86_64/lib/libxbox_kernel.a", "-ld3d11"]


def test_xba_host_names_a_cross_build_host():
    assert rt.host_id(env={"XBA_HOST": "windows-x86_64"}) == "windows-x86_64"
    assert rt.host_id("Linux", "x86_64", env={"XBA_HOST": "windows-x86_64"}) == "linux-x86_64"
    assert rt.host_id(env={}) == rt.host_id("Linux", "x86_64") or sys.platform != "linux"


def test_cmake_parallel_leaves_cmake_build_parallel_level_alone():
    assert rt.cmake_parallel(3, env={"CMAKE_BUILD_PARALLEL_LEVEL": "1"}) == ["--parallel", "3"]
    assert rt.cmake_parallel(env={"CMAKE_BUILD_PARALLEL_LEVEL": "1"}) == []     # CMake reads it
    assert rt.cmake_parallel(env={"CMAKE_BUILD_PARALLEL_LEVEL": ""}) == ["--parallel"]
    assert rt.cmake_parallel(env={}) == ["--parallel"]


def test_release_configuration_of_a_multi_config_generator():
    assert rt.pick_configuration({"configurations": [{"name": "Debug"}, {"name": "Release"}]})["name"] == "Release"
    assert rt.pick_configuration({"configurations": [{"name": ""}]})["name"] == ""


def test_game_data_link_replaces_an_old_one(tmp_path):
    from tools.prepare import __main__ as prep
    a, b, link = tmp_path / "a", tmp_path / "b", str(tmp_path / "game")
    a.mkdir()
    b.mkdir()
    prep.link_game_data(str(a), link)
    prep.link_game_data(str(b), link)
    assert os.path.realpath(link) == str(b)


def test_system_lib():
    assert db.system_lib("/nope/libSDL2-2.0.so.0", exists=lambda p: False) == "-lSDL2-2.0"
    assert db.system_lib("/nope/libepoxy.so", exists=lambda p: False) == "-lepoxy"
    assert db.system_lib("/nope/libGL.tbd", exists=lambda p: False) == "-lGL"
    assert db.system_lib("/here/libSDL2.so", exists=lambda p: True) == "/here/libSDL2.so"
    assert db.system_lib("-lm", exists=lambda p: False) == "-lm"


def test_title_defines(tmp_path):
    p = tmp_path / "proj"
    (p / "src" / "title").mkdir(parents=True)
    w = {"XBA_DISCOVERY_FILE": "/w/discovered.jsonl", "XBA_IMAGE_MAP": "/w/images.map", "XBA_IMAGE_NAME": ""}
    assert db.title_defines(str(p), w) == ['XBA_DISCOVERY_FILE="/w/discovered.jsonl"', 'XBA_IMAGE_MAP="/w/images.map"']
    (p / "src" / "title" / "a.c").write_text("")
    (p / "src" / "title" / "env_force.h").write_text("")
    d = db.title_defines(str(p), dict(w, XBA_IMAGE_NAME="game.xbe"))
    assert d[0] == "XBA_TITLE_PROFILE=1" and d[-1] == "XBA_TITLE_ENV_FORCE=1" and 'XBA_IMAGE_NAME="game.xbe"' in d


# ── route ──

def _runtime(tmp_path, sha="abc", host=None):
    d = tmp_path / "runtime" / (host or rt.host_id())
    d.mkdir(parents=True)
    (d / rt.MANIFEST).write_text(json.dumps(dict(_man(), host=host or rt.host_id(), runtime_sha256=sha)))
    return str(tmp_path)


def test_route(tmp_path):
    digest = lambda root: "abc"              # noqa: E731
    with_cmake, without = _which(cmake="/usr/bin/cmake"), _which()
    empty = str(tmp_path / "empty")
    assert db.route(empty, "auto", which=with_cmake, runtime_digest=digest, env={})[0] == "cmake"
    with pytest.raises(cc.StepError) as e:
        db.route(empty, "auto", which=without, runtime_digest=digest, env={})
    assert e.value.code == "NO_BUILDER" and "no cmake" in e.value.msg
    tk = _runtime(tmp_path)
    how, man, why = db.route(tk, "auto", which=without, runtime_digest=digest, env={})
    assert how == "driver" and man["dir"].endswith(rt.host_id())
    assert db.route(tk, "cmake", which=without, runtime_digest=digest, env={})[0] == "cmake"
    assert db.route(tk, None, which=without, runtime_digest=digest, env={"XBA_BUILDER": "cmake"})[0] == "cmake"
    # A runtime built from another tree: CMake when there is one, the driver only when asked for.
    stale = lambda root: "other"             # noqa: E731
    assert db.route(tk, "auto", which=with_cmake, runtime_digest=stale, env={})[0] == "cmake"
    assert db.route(tk, "driver", which=without, runtime_digest=stale, env={})[0] == "driver"
    with pytest.raises(cc.StepError):
        db.route(empty, "driver", which=with_cmake, runtime_digest=digest, env={})
    # $XBA_RUNTIME: a runtime folder elsewhere.
    env = {"XBA_RUNTIME": os.path.join(tk, "runtime", rt.host_id())}
    assert db.route(empty, "auto", which=without, runtime_digest=digest, env=env)[0] == "driver"


def test_another_hosts_runtime_is_not_used(tmp_path):
    tk = _runtime(tmp_path, host="plan9-mips")
    assert db.load_manifest(tk, env={"XBA_RUNTIME": os.path.join(tk, "runtime", "plan9-mips")}) is None


# ── incremental build, with a fake clang ──

FAKE_CLANG = r'''#!%s
import hashlib, os, sys
a = sys.argv[1:]
log = os.environ["FAKE_CC_LOG"]
if "--version" in a:
    print("clang version 99.1.0 (fake)"); sys.exit(0)
out = a[a.index("-o") + 1]
with open(log, "a") as f:
    f.write(("compile " if "-c" in a else "link ") + (a[a.index("-c") + 1] if "-c" in a else out) + "\n")
if "-c" in a:
    with open(a[a.index("-c") + 1], "rb") as s, open(out, "w") as o:
        o.write(hashlib.sha256(s.read()).hexdigest())
else:
    with open(out, "w") as o:
        o.write(" ".join(x for x in a if x.endswith(".o")))
'''


def _project(tmp_path):
    p = tmp_path / "project"
    gen = p / "src" / "recomp" / "gen"
    gen.mkdir(parents=True)
    (p / "src" / "main.c").write_text("int main(void) { return 0; }\n")
    (p / "src" / "recomp_manual.c").write_text("/* */\n")
    for i in range(3):
        (gen / ("gen_%04d.c" % i)).write_text("void f%d(void) {}\n" % i)
    (gen / "recomp_funcs.h").write_text("void f0(void);\n")
    return p


def test_incremental_rebuild(tmp_path, monkeypatch):
    fake = tmp_path / "bin" / "clang"
    fake.parent.mkdir()
    fake.write_text(FAKE_CLANG % sys.executable)
    fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    log = tmp_path / "cc.log"
    monkeypatch.setenv("FAKE_CC_LOG", str(log))
    rdir = tmp_path / "rt"
    (rdir / "lib").mkdir(parents=True)
    (rdir / "lib" / "libxbox_kernel.a").write_text("v1")
    man = _man(str(rdir), link=[{"arg": "{runtime}/lib/libxbox_kernel.a", "role": "libraries"}], includes=[])
    proj = _project(tmp_path)
    tc = db.DesktopToolchain(str(fake), "linux-x86_64", "explicit")

    def build():
        log.write_text("")
        exe, n, linked = db.build(str(proj), "title", man, tc, ROOT, {}, jobs=2)
        return n, linked, log.read_text().split("\n")[:-1]

    n, linked, calls = build()
    assert (n, linked) == (5, True) and sum(c.startswith("compile") for c in calls) == 5
    assert os.path.isfile(proj / "build" / "title") and not os.path.exists(proj / "build" / "title.tmp")
    assert build() == (0, False, [])                       # nothing changed: no compile, no link
    (proj / "src" / "recomp" / "gen" / "gen_0001.c").write_text("void f1(void) { for (;;); }\n")
    n, linked, calls = build()
    assert (n, linked) == (1, True)
    assert calls == ["compile " + str(proj / "src" / "recomp" / "gen" / "gen_0001.c"),
                     "link " + str(proj / "build" / "title.tmp")]
    # A new prebuilt runtime: a link, no compile.
    os.utime(rdir / "lib" / "libxbox_kernel.a", (1, 1))
    n, linked, calls = build()
    assert (n, linked) == (0, True) and calls == ["link " + str(proj / "build" / "title.tmp")]
    # The old object of gen_0001.c is gone from the cache.
    assert len([fn for fn in os.listdir(proj / "build" / "obj") if fn.endswith(".o")]) == 5


# ── the runtime manifest from CMake's file API ──

def test_manifest_from_target(tmp_path):
    tk, build, probe, out = (tmp_path / n for n in ("tk", "build", "probe", "out"))
    for d in (tk / "src" / "kernel", build / "xboxrecomp" / "src" / "kernel", build / "gen" / "inc", probe / "src",
              out):
        d.mkdir(parents=True)
    (build / "xboxrecomp" / "src" / "kernel" / "libxbox_kernel.a").write_text("lib")
    (build / "gen" / "inc" / "config.h").write_text("#define X 1\n")
    (build / "gen" / "inc" / "junk.o").write_text("")
    target = {
        "compileGroups": [{"language": "C", "defines": [{"define": "NOMINMAX"}],
                           "includes": [{"path": str(probe / "src")}, {"path": str(tk / "src" / "kernel")},
                                        {"path": str(build / "gen" / "inc")}, {"path": "/usr/include/SDL2"}],
                           "compileCommandFragments": [{"fragment": "-O3 -DNDEBUG -std=gnu11 -fno-strict-aliasing"}]}],
        "link": {"language": "CXX", "commandFragments": [
            {"fragment": "-O3 -DNDEBUG", "role": "flags"},
            {"fragment": "-rdynamic", "role": "flags"},
            {"fragment": "-Wl,-rpath,%s:/opt/lib" % build, "role": "libraries"},
            {"fragment": "-Wl,-rpath,%s" % build, "role": "libraries"},
            {"fragment": "-Wl,--start-group", "role": "libraries"},
            {"fragment": "xboxrecomp/src/kernel/libxbox_kernel.a", "role": "libraries"},
            {"fragment": "/usr/lib/libSDL2.so", "role": "libraries"},
            {"fragment": "-Wl,--end-group", "role": "libraries"}]}}
    reloc = rt.Relocator(str(tk), str(build), str(probe), str(out))
    m = rt.manifest_from_target(target, reloc, "sha", "GNU 14", "linux-x86_64")
    assert m["includes"] == ["{toolkit}/src/kernel", "{runtime}/include/0", "/usr/include/SDL2"]
    assert m["compile_flags"] == ["-DNDEBUG", "-fno-strict-aliasing"]
    assert [a["arg"] for a in m["link"]] == ["-rdynamic", "-Wl,-rpath,/opt/lib", "-Wl,--start-group",
                                             "{runtime}/lib/libxbox_kernel.a", "/usr/lib/libSDL2.so",
                                             "-Wl,--end-group"]
    assert m["link_language"] == "CXX" and m["runtime_sha256"] == "sha" and m["defines"] == ["NOMINMAX"]
    assert (out / "lib" / "libxbox_kernel.a").read_text() == "lib"
    assert sorted(os.listdir(out / "include" / "0")) == ["config.h"]


def test_mkxbe_retail_names_an_entry_point(tmp_path):
    from tools.conformance import mkxbe
    from tools.xbe_parser.xbe_parser import XBEParser
    with open(os.path.join(ROOT, "tools", "conformance", "test.xbe"), "rb") as f:
        assert mkxbe.build() == f.read()          # the committed one is unchanged
    p = tmp_path / "default.xbe"
    p.write_bytes(mkxbe.build(retail=True))
    x = XBEParser(str(p)).parse()
    assert x.header.entry_point == mkxbe.TEXT_VA and x.header.entry_key == "retail"


# ── end to end (XBA_E2E=1): the runtime built once with CMake, then no CMake ──

def _host_compiler():
    return shutil.which("clang") or shutil.which("cc")


@pytest.mark.skipif(os.environ.get("XBA_E2E") != "1" or sys.platform not in ("linux", "darwin"),
                    reason="XBA_E2E=1 on Linux or macOS (needs CMake once, a C compiler and SDL2)")
def test_e2e_prepare_without_cmake(tmp_path):
    pytest.importorskip("capstone")
    if not (shutil.which("cmake") and _host_compiler()):
        pytest.skip("needs cmake (for the runtime) and a C compiler")
    rdir = tmp_path / "runtime"
    r = subprocess.run([sys.executable, "-m", "tools.prepare.runtime", "--out", str(rdir), "--no-players"], cwd=ROOT,
                       capture_output=True, text=True, timeout=1800)
    assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-3000:]
    # From here on, a cmake or ninja that runs fails the prepare.
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    for tool in ("cmake", "ninja"):
        (bin_dir / tool).write_text("#!/bin/sh\necho \"$0 must not run\" >&2\nexit 99\n")
        (bin_dir / tool).chmod(0o755)
    env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ.get("PATH", ""), XBA_RUNTIME=str(rdir),
               SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    game = tmp_path / "game"
    game.mkdir()
    from tools.conformance import mkxbe
    (game / "default.xbe").write_bytes(mkxbe.build(retail=True))
    work = tmp_path / "work"
    prep = [sys.executable, "-m", "tools.prepare", str(game / "default.xbe"), "--work", str(work)]
    r = subprocess.run(prep + ["--progress"], cwd=ROOT, env=env, capture_output=True, text=True, timeout=900)
    assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-3000:]
    assert "compile driver:" in r.stdout and "must not run" not in r.stderr
    exe = work / "project" / "build" / "title_12345678"
    assert "XBA_RESULT ok exe=%s" % exe in r.stdout
    # Again: nothing to compile, nothing to link.
    r = subprocess.run(prep, cwd=ROOT, env=env, capture_output=True, text=True, timeout=900)
    assert r.returncode == 0 and "(0 compiled, up to date)" in r.stdout, r.stdout[-2000:]
    chk = subprocess.run(prep + ["--check", "--json"], cwd=ROOT, env=env, capture_output=True, text=True, timeout=300)
    assert json.loads(chk.stdout)["state"] == "ready" and json.loads(chk.stdout)["exe"] == str(exe)
    # It starts: the runtime loads the XBE and runs the lifted entry point (add3 returns).
    p = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True)
    try:
        out, _ = p.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
    assert "=== SYNTH - Static Recompilation ===" in out and "XBE loaded" in out, out[-3000:]
