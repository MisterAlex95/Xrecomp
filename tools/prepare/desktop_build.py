"""
The desktop build of `python3 -m tools.prepare` without CMake (XBA-86): the
title project's C compiled by compile.py's driver (the Android pack's: object
cache, largest file first, parallel jobs) and linked into one executable per
title against the toolkit package's prebuilt runtime (runtime/<host>/, made by
tools/prepare/runtime.py when the package is built).

Hosts: Linux (ELF) and macOS (Mach-O) with clang or the system cc, linked by
lld when the toolchain has it; Windows x86-64 (PE, XBA-89) with clang-cl and
lld-link, or MSVC's cl and link, against a runtime built by MSVC (the Windows
download, built and smoke-tested on GitHub's windows runner); a MinGW gcc
(--cc) against a MinGW-built runtime, which is how the Windows route is
checked from Linux (docs/desktop/windows.md).

The compiler, in this order (find_toolchain):
  1. --cc, else $XBA_CC
  2. the toolkit's own: <toolkit>/toolchain/bin/clang (clang-cl.exe on Windows)
  3. clang on PATH (else the newest clang-NN); clang-cl on Windows
  4. the system's cc / gcc (XBA-85: the system compiler during 0.x); MSVC's
     cl on Windows (a Visual Studio developer prompt, or xrecomp.cmd, which
     opens one)

The route (route()): the driver when runtime/<host>/runtime.json is there and
was built from this toolkit tree (its runtime_sha256), else CMake as before
(templates/new-game), which a checkout without a prebuilt runtime uses.
--builder driver / cmake (or $XBA_BUILDER) forces one.
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

from tools.prepare import compile as cc
from tools.prepare import cpu
from tools.prepare import runtime as rt

ROUTES = ("auto", "driver", "cmake")


# ── Host and toolchain ──────────────────────────────────────────

def host_os(host):
    return host.split("-", 1)[0]


def exe_name(name, host):
    return name + ".exe" if host_os(host) == "windows" else name


class DesktopToolchain(cc.Toolchain):
    """A host compiler. kind: "clang", "gnu" (gcc / cc) or "msvc" (clang-cl,
    cl). lld: the lld binary to link with (ld.lld, ld64.lld or lld-link), or
    None for the compiler driver's default linker (cl: link.exe beside it). source: "explicit", "toolkit",
    "system". isysroot: the macOS SDK a non-Apple clang needs."""

    def __init__(self, compiler, host, source, kind=None, lld=None, isysroot=None, procs=None):
        cc.Toolchain.__init__(self, compiler, "", ld=lld or compiler, procs=procs)
        self.host = host
        self.source = source
        self.kind = kind or compiler_kind(compiler)
        self.lld = lld
        self.isysroot = isysroot

    def compile_args(self, flags, src, out):
        if self.kind == "msvc":
            return self.cc_argv + flags + ["/c", src, "/Fo" + out]
        return cc.Toolchain.compile_args(self, flags, src, out)

    def is_cl(self):
        """MSVC's own cl.exe (not clang-cl)."""
        return is_msvc_cl(self.cc)

    def version_text(self):
        if not self.is_cl():
            return cc.Toolchain.version_text(self)
        if self._version is None:
            # cl has no --version: alone, it prints its banner and fails for want of a file.
            rc, out = self.procs.run([self.cc])
            m = re.search(r"Version (\S+)", out)
            if not m:
                raise cc.StepError("COMPILE_FAILED", "%s: no version banner: %s" % (self.cc, out.strip()[:300]))
            self._version = "cl version %s" % m.group(1)
        return self._version

    def link_driver(self, language):
        """The command that links: the compiler driver (its C++ mode when the
        runtime links C++: frame generation), or lld-link / MSVC's link on
        Windows."""
        if self.kind == "msvc":
            return [self.lld or _sibling(self.cc, "link.exe" if self.is_cl() else "lld-link")]
        if language != "CXX":
            return [self.cc]
        if self.kind == "clang":
            return [self.cc, "--driver-mode=g++"]
        base = os.path.basename(self.cc)
        cxx = {"cc": "c++", "gcc": "g++"}.get(base) or re.sub(r"gcc", "g++", base)
        return [_sibling(self.cc, cxx) if os.path.dirname(self.cc) else cxx]

    def short_version(self):
        v = self.version_text()
        m = re.search(r"(clang|cl) version (\S+)", v)
        if m:
            return "%s %s" % m.groups()
        m = re.search(r"\)\s+(\d+\.\S+)", v) or re.search(r"(\d+\.\d+\S*)", v) or re.search(r"\)\s+(\d+\S*)", v)
        return "%s %s" % (os.path.basename(self.cc), m.group(1) if m else "?")

    def describe(self):
        linker = os.path.basename(self.lld) if self.lld else "the default linker"
        return "%s (%s, %s), %s" % (self.short_version(), self.source, self.cc, linker)


def _sibling(path, name):
    d = os.path.dirname(path)
    return os.path.join(d, name) if d else name


def _base(path):
    base = os.path.basename(path.replace("\\", "/")).lower()
    return base[:-4] if base.endswith(".exe") else base


def is_msvc_cl(path):
    return _base(path) == "cl"


def compiler_kind(path):
    base = _base(path)
    if base.startswith("clang-cl") or base == "cl":
        return "msvc"
    if base.startswith("clang"):
        return "clang"
    return "gnu"


def _versioned_clang(path_env, isfile):
    """The newest clang-NN on PATH (Debian / Ubuntu name it so without the
    clang alternative), or None."""
    best = None
    for d in (path_env or "").split(os.pathsep):
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for n in names:
            m = re.match(r"^clang-(\d+)$", n)
            if m and isfile(os.path.join(d, n)) and (best is None or int(m.group(1)) > best[0]):
                best = (int(m.group(1)), os.path.join(d, n))
    return best[1] if best else None


def find_toolchain(toolkit, host=None, cc_path=None, env=None, which=shutil.which, isfile=os.path.isfile,
                   procs=None, sdk=None):
    """The DesktopToolchain for host (module docstring for the order).
    Raises cc.StepError("NO_COMPILER") when there is none."""
    host = host or rt.host_id()
    env = os.environ if env is None else env
    win = host_os(host) == "windows"
    tc_bin = os.path.join(toolkit, "toolchain", "bin")
    compiler, source = None, None
    if cc_path or env.get("XBA_CC"):
        given = cc_path or env.get("XBA_CC")
        compiler, source = (given if os.path.dirname(given) else (which(given) or given)), "explicit"
    else:
        mine = os.path.join(tc_bin, exe_name("clang-cl" if win else "clang", host))
        if isfile(mine):
            compiler, source = mine, "toolkit"
        else:
            for name in (("clang-cl", "cl") if win else ("clang",)):
                if which(name):
                    compiler, source = which(name), "system"
                    break
            if compiler is None and not win:
                compiler = _versioned_clang(env.get("PATH", ""), isfile)
                source = "system" if compiler else None
            if compiler is None and not win:
                for name in ("cc", "gcc"):
                    if which(name):
                        compiler, source = which(name), "system"
                        break
    if compiler is None:
        raise cc.StepError("NO_COMPILER", "no C compiler: install clang (or gcc), or pass --cc"
                           if not win else "no C compiler: install Visual Studio's Build Tools (C++) or LLVM, "
                           "and run from a developer prompt (xrecomp.cmd opens one), or pass --cc")
    kind = compiler_kind(compiler)
    if kind == "gnu" and os.path.basename(compiler) in ("cc", "c99") and procs is not None:
        # cc may be clang (macOS, FreeBSD): what it says it is.
        try:
            rc, out = procs.run([compiler, "--version"])
            if rc == 0 and "clang" in out:
                kind = "clang"
        except OSError:
            pass
    lld = None
    if kind != "gnu" and not is_msvc_cl(compiler):
        lld_name = exe_name({"windows": "lld-link", "macos": "ld64.lld"}.get(host_os(host), "ld.lld"), host)
        for cand in (os.path.join(os.path.dirname(compiler), lld_name), os.path.join(tc_bin, lld_name)):
            if os.path.dirname(compiler) and isfile(cand):
                lld = cand
                break
        if lld is None and source == "system":
            lld = which(lld_name) if host_os(host) != "macos" else None   # Apple's ld64 is the system's
        if lld is None and win and kind == "msvc":
            lld = which(lld_name) or lld_name
    isysroot = None
    apple = os.path.realpath(compiler).startswith(APPLE_DIRS) or compiler.startswith(APPLE_DIRS)
    if host_os(host) == "macos" and (not apple or sdk):
        isysroot = sdk or macos_sdk(toolkit, env, isfile)
    return DesktopToolchain(compiler, host, source, kind=kind, lld=lld, isysroot=isysroot, procs=procs)


# Apple's clang (Xcode, the Command Line Tools) finds its SDK by itself; another clang (the toolkit's, Homebrew's
# LLVM) needs -isysroot.
APPLE_DIRS = ("/usr/bin/", "/Library/Developer/", "/Applications/Xcode")


def macos_sdk(toolkit, env, isfile=os.path.isfile):
    """The SDK a clang that is not Apple's compiles against: $SDKROOT, a
    toolchain/sdk/*.sdk shipped with the toolkit, else xcrun's (the Command
    Line Tools), else None."""
    if env.get("SDKROOT"):
        return env["SDKROOT"]
    d = os.path.join(toolkit, "toolchain", "sdk")
    try:
        for n in sorted(os.listdir(d)):
            if n.endswith(".sdk"):
                return os.path.join(d, n)
    except OSError:
        pass
    try:
        out = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, timeout=30)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return None


# ── The prebuilt runtime ────────────────────────────────────────

def manifest_dir(toolkit, host=None, env=None):
    """$XBA_RUNTIME (a runtime folder made elsewhere: a test, a build
    machine), else <toolkit>/runtime/<host>."""
    env = os.environ if env is None else env
    return os.path.abspath(env["XBA_RUNTIME"]) if env.get("XBA_RUNTIME") else rt.runtime_dir(toolkit, host)


def load_manifest(toolkit, host=None, env=None):
    """runtime.json of manifest_dir() as a dict (with "dir"), or None (none,
    another format, or another host's)."""
    d = manifest_dir(toolkit, host, env)
    try:
        with open(os.path.join(d, rt.MANIFEST)) as f:
            man = json.load(f)
    except (OSError, ValueError):
        return None
    if man.get("format") != rt.FORMAT or man.get("host") != (host or rt.host_id()):
        return None
    man["dir"] = d
    return man


def expand(arg, toolkit, rdir):
    return arg.replace("{toolkit}", toolkit).replace("{runtime}", rdir)


_LIBNAME = re.compile(r"^lib(.+?)(\.so(\.\d+)*|\.dylib|\.tbd|\.a)$")


def system_lib(arg, exists=os.path.exists):
    """An absolute system library the package was built against: as is when
    this machine has it there, else -l<name> (another distribution's lib dir)."""
    if not os.path.isabs(arg) or exists(arg):
        return arg
    m = _LIBNAME.match(os.path.basename(arg))
    return "-l" + m.group(1) if m else arg


# ── Flags and link line ─────────────────────────────────────────

def title_defines(proj, work_files):
    """What templates/new-game/CMakeLists.txt defines for the title on top of
    the runtime's: XBA_TITLE_PROFILE, XBA_DISCOVERY_FILE, XBA_IMAGE_MAP,
    XBA_IMAGE_NAME (when set) and XBA_TITLE_ENV_FORCE."""
    out = []
    tdir = os.path.join(proj, "src", "title")
    if os.path.isdir(tdir) and any(fn.endswith(".c") for fn in os.listdir(tdir)):
        out.append("XBA_TITLE_PROFILE=1")
    for key in ("XBA_DISCOVERY_FILE", "XBA_IMAGE_MAP", "XBA_IMAGE_NAME"):
        v = work_files.get(key)
        if v:
            out.append('%s="%s"' % (key, v))
    if os.path.isfile(os.path.join(tdir, "env_force.h")):
        out.append("XBA_TITLE_ENV_FORCE=1")
    return out


def compile_flags(tc, man, toolkit, proj, opt="2", defines=(), include_dirs=(), cpu_features=()):
    """cpu_features (tools/prepare/cpu.py, XBA-313): the host CPU's, the one the executable is built for and runs
    on; their flags go in unless the runtime's manifest already picks a -march / -mcpu (and never for MSVC)."""
    rdir = man["dir"]
    incs = [os.path.join(proj, "src"), os.path.join(proj, "src", "recomp"), os.path.join(proj, "src", "recomp", "gen"),
            os.path.join(toolkit, "include")]
    for i in man.get("includes", []):
        p = expand(i, toolkit, rdir)
        if p not in incs:
            incs.append(p)
    incs += list(include_dirs)
    defs = list(man.get("defines", [])) + list(defines)
    if tc.kind == "msvc":
        flags = ["/nologo", "/O%s" % ("2" if opt in ("2", "3") else opt), "/w", "/std:c11", "/bigobj"]
        # MSVC's options start with / or - (CMake writes the C runtime as -MD); a GNU runtime's do not apply.
        mflags = [f for f in man.get("compile_flags", []) if f.startswith(("/", "-"))]
        if not any(re.match(r"^[/-]M[DT]d?$", f) for f in mflags):
            mflags.insert(0, "/MD")     # CMake's default C runtime: the runtime's libraries were built with it
        flags += mflags
        flags += ["/D" + d for d in defs] + ["/I" + i for i in incs]
        return flags
    # -fno-math-errno: as the Android Prepare (compile.py compile_flags), whose target has it by default.
    flags = ["-O%s" % opt, "-g0", "-std=gnu11", "-w", "-fno-math-errno"]
    if tc.isysroot:
        flags += ["-isysroot", tc.isysroot]
    flags += [f for f in man.get("compile_flags", []) if not f.startswith("/")]
    if not any(f.startswith(("-march=", "-mcpu=")) for f in flags):
        flags += cpu.flags(cpu_features)
    flags += ["-D" + d for d in defs] + ["-I" + i for i in incs]
    return flags


def link_args(tc, man, toolkit, objects, out, exists=os.path.exists):
    """The link command of the title executable: objects, then the runtime's
    libraries and system libraries in the order CMake gave them."""
    rdir = man["dir"]
    lang = man.get("link_language", "C")
    rest = []
    for item in man.get("link", []):
        arg = expand(item["arg"], toolkit, rdir)
        if item.get("role") == "libraries" and "{runtime}" not in item["arg"] and "{toolkit}" not in item["arg"]:
            arg = system_lib(arg, exists)
        rest.append(arg)
    if tc.kind == "msvc":
        return tc.link_driver(lang) + ["/nologo", "/OUT:" + out] + list(objects) + rest
    args = list(tc.link_driver(lang))
    if tc.lld:
        args.append("--ld-path=" + tc.lld)
    if tc.isysroot:
        args += ["-isysroot", tc.isysroot]
    return args + ["-o", out] + list(objects) + rest


# ── The route ───────────────────────────────────────────────────

def route(toolkit, builder="auto", host=None, which=shutil.which, runtime_digest=None, env=None):
    """("driver", manifest, note) or ("cmake", None, note): how step 5 builds.
    Raises cc.StepError("NO_BUILDER") when neither can."""
    host = host or rt.host_id()
    env = os.environ if env is None else env
    builder = builder or env.get("XBA_BUILDER") or "auto"
    if builder not in ROUTES:
        raise cc.StepError("BAD_ARGS", "--builder %s: one of %s" % (builder, ", ".join(ROUTES)))
    if builder == "cmake":
        return "cmake", None, "CMake (asked for)"
    man = load_manifest(toolkit, host, env)
    where = manifest_dir(toolkit, host, env)
    if runtime_digest is None:
        from tools.prepare import toolkit as tk
        runtime_digest = tk.runtime_digest
    current = man is not None and man.get("runtime_sha256") == runtime_digest(toolkit)
    if builder == "driver":
        if man is None:
            raise cc.StepError("NO_BUILDER", "--builder driver: no prebuilt runtime for %s in %s"
                               % (host, where))
        return "driver", man, "prebuilt runtime %s%s" % (host, "" if current else
                                                          " (built from another runtime tree: asked for)")
    if current:
        return "driver", man, "prebuilt runtime %s" % host
    why = ("the prebuilt runtime in %s was built from another runtime tree" % where
           if man is not None else "no prebuilt runtime for %s" % host)
    if which("cmake"):
        return "cmake", None, "CMake (%s)" % why
    raise cc.StepError("NO_BUILDER", "%s and no cmake on PATH: install CMake, or make the runtime with "
                       "`python3 -m tools.prepare.runtime` where CMake is (docs: docs/desktop/cli.md « Install »)" % why)


# ── The build ───────────────────────────────────────────────────

def windows_mem_available():
    """The available physical memory on Windows (no /proc/meminfo), else None."""
    if os.name != "nt":
        return None
    try:
        import ctypes

        class MemoryStatus(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
        st = MemoryStatus()
        st.dwLength = ctypes.sizeof(MemoryStatus)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
            return int(st.ullAvailPhys)
    except (AttributeError, OSError, ValueError):
        pass
    return None


def _link_key(args, man):
    h = hashlib.sha256()
    h.update("\x1f".join(args).encode())
    h.update(b"\0" + (man.get("runtime_sha256") or "").encode())
    for a in args:
        if a.endswith((".a", ".lib")) and os.path.isfile(a):
            st = os.stat(a)
            h.update(("%s:%d:%d" % (a, st.st_size, int(st.st_mtime))).encode())
    return h.hexdigest()


def build(proj, name, man, tc, toolkit, work_files, opt="2", jobs=0, log=None, on_progress=None,
          include_dirs=(), extra_sources=(), cpu_arg="auto"):
    """Compile and link <proj>/build/<name>. Returns (exe, compiled now, linked:
    False when the executable was up to date)."""
    bdir = os.path.join(proj, "build")
    os.makedirs(bdir, exist_ok=True)
    sources = cc.collect_sources(proj) + list(extra_sources)
    # The executable is built for this machine (XBA-313): for a linux-arm64 host whose CPU has the ARMv8.1 atomics
    # (cpu_arg auto, --cpu-features), they are inline; another host (a cross build too) gets none (macOS arm64
    # compilers target them already). The flags are in every object's key and the link key, so the same project
    # prepared on a CPU without them is rebuilt; an executable copied there is not guarded.
    try:
        feats = cpu.resolve(cpu_arg) if tc.host == "linux-arm64" else ()
    except ValueError as e:
        raise cc.StepError("BAD_ARGS", str(e))
    flags = compile_flags(tc, man, toolkit, proj, opt, title_defines(proj, work_files), include_dirs,
                          cpu_features=feats)
    jobs = jobs or cc.default_jobs(cc.mem_available() or windows_mem_available(), os.cpu_count())
    obj_dir = os.path.join(bdir, "obj")
    objects, n = cc.compile_all(sources, flags, tc, obj_dir, jobs, log=log, on_progress=on_progress)
    cc.prune(obj_dir, objects)
    exe = os.path.join(bdir, exe_name(name, tc.host))
    args = link_args(tc, man, toolkit, objects, exe)
    key = _link_key(args, man)
    stamp = exe + ".link"
    try:
        with open(stamp) as f:
            same = f.read().strip() == key
    except OSError:
        same = False
    if same and n == 0 and os.path.isfile(exe):
        return exe, n, False
    tmp = exe + ".tmp"
    args_tmp = [a if a != exe and a != "/OUT:" + exe else a.replace(exe, tmp) for a in args]
    rc, text = tc.procs.run(args_tmp)
    if log:
        with open(log, "a") as f:
            f.write("\n$ %s\n%s" % (" ".join(args_tmp), text))
    if rc != 0 or not os.path.isfile(tmp):
        try:
            os.remove(tmp)
        except OSError:
            pass
        code = "NO_SPACE" if "No space left on device" in text else "LINK_FAILED"
        undef = [l.strip() for l in text.splitlines() if "undefined" in l]
        raise cc.StepError(code, "; ".join(undef[:3]) if undef else cc._first_error(text))
    os.replace(tmp, exe)
    with open(stamp, "w") as f:
        f.write(key + "\n")
    return exe, n, True
