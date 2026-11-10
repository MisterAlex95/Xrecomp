"""
The compile driver of `tools.prepare --android`: the title project's C into
objects with clang, then libtitle.so with ld.lld. No CMake (there is none on
the device): threads and subprocesses only, no multiprocessing (no sem_open
on Android). The host and CI run the same code with the NDK's clang, so the
path the tests cover is the one the device runs.

Objects live in a cache keyed by sha256(source path + source + flags +
compiler id + headers): an object whose key exists is not compiled again, so
a killed run resumes where it stopped and a second run compiles nothing.
"""

import errno
import hashlib
import os
import re
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor

API = 30
TARGET = "aarch64-linux-android%d" % API
GB = 1 << 30
# Each clang job on the generated C: see "Measurements" in docs/android/prepare.md.
JOB_RSS = 1 * GB
RESERVED = 3 * GB
MAX_JOBS = 5

# The toolkit's header directories (what the CMake targets of src/ export).
TOOLKIT_INCLUDES = ("include", "src", "src/kernel", "src/platform", "src/audio", "src/apu",
                    "src/nv2a", "src/d3d", "src/input", "src/usb", "src/video", "src/selftest")


class StepError(Exception):
    def __init__(self, code, msg):
        Exception.__init__(self, msg)
        self.code = code
        self.msg = msg


class Cancelled(Exception):
    pass


# ── Child processes ─────────────────────────────────────────────

class Procs:
    """Every child process, so that a cancel kills them all at once."""

    def __init__(self):
        self.lock = threading.Lock()
        self.live = set()
        self.cancelled = threading.Event()

    def start(self, args, **kw):
        if self.cancelled.is_set():
            raise Cancelled()
        p = subprocess.Popen(args, **kw)
        with self.lock:
            self.live.add(p)
        if self.cancelled.is_set():          # cancel() ran between the check and add()
            p.kill()
        return p

    def done(self, p):
        with self.lock:
            self.live.discard(p)

    def run(self, args, cwd=None, env=None):
        """(returncode, stdout+stderr as text)."""
        p = self.start(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        try:
            out, _ = p.communicate()
        finally:
            self.done(p)
        if self.cancelled.is_set():
            raise Cancelled()
        return p.returncode, out.decode("utf-8", "replace")

    def run_logged(self, args, log, cwd=None, on_tick=None, tick=0.5):
        """Run with stdout+stderr appended to `log`; on_tick(elapsed seconds)
        while it runs. Returns (returncode, seconds)."""
        t0 = time.time()
        with open(log, "a") as f:
            f.write("\n$ %s\n" % " ".join(args))
            f.flush()
            p = self.start(args, cwd=cwd, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            try:
                while True:
                    try:
                        p.wait(timeout=tick)
                        break
                    except subprocess.TimeoutExpired:
                        if on_tick:
                            on_tick(time.time() - t0)
            finally:
                self.done(p)
        if self.cancelled.is_set():
            raise Cancelled()
        return p.returncode, time.time() - t0

    def cancel(self):
        self.cancelled.set()
        with self.lock:
            live = list(self.live)
        for p in live:
            try:
                p.kill()
            except OSError:
                pass


# ── Jobs ────────────────────────────────────────────────────────

def mem_available(meminfo="/proc/meminfo"):
    """MemAvailable in bytes, or None."""
    try:
        with open(meminfo) as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    return int(line.split()[1]) * 1024
    except (OSError, ValueError, IndexError):
        pass
    return None


def default_jobs(avail=None, cpus=None):
    """min(5, (MemAvailable - 3 GB) / 1 GB), at least 1, at most the CPU
    count. `avail` None = unknown memory: 1."""
    if avail is None:
        return 1
    n = int((avail - RESERVED) // JOB_RSS)
    n = min(MAX_JOBS, n)
    if cpus:
        n = min(n, cpus)
    return max(1, n)


# ── Toolchain ───────────────────────────────────────────────────

class Toolchain:
    """clang and ld.lld for aarch64-linux-android30.

    cc / cc_prefix: the compiler is `cc *cc_prefix ...` -- `libxbacc.so clang`
    for the device's multicall LLVM, or a plain clang (cc_prefix empty).
    The linker: `ld *ld_prefix` when given; `cc ld.lld` when the compiler is
    the multicall binary; else ld.lld next to cc."""

    def __init__(self, cc, sysroot, cc_prefix=(), ld=None, ld_prefix=(), builtins=None, procs=None):
        self.cc = cc
        self.cc_prefix = list(cc_prefix)
        self.sysroot = sysroot
        if ld:
            self.ld_argv = [ld] + list(ld_prefix)
        elif self.cc_prefix:
            self.ld_argv = [cc, "ld.lld"]
        else:
            self.ld_argv = [os.path.join(os.path.dirname(cc), "ld.lld")]
        self._builtins = builtins
        self.procs = procs or Procs()
        self._version = None

    @property
    def cc_argv(self):
        return [self.cc] + self.cc_prefix

    def version_text(self):
        if self._version is None:
            rc, out = self.procs.run(self.cc_argv + ["--version"])
            if rc != 0:
                raise StepError("COMPILE_FAILED", "%s --version failed: %s"
                                % (" ".join(self.cc_argv), out.strip()[:300]))
            self._version = out.splitlines()[0].strip() if out.strip() else "clang"
        return self._version

    def compiler_id(self):
        """What goes into the object cache key: the version line, and the
        size and mtime of the binary (a rebuilt toolchain of the same version)."""
        try:
            st = os.stat(self.cc)
            stamp = "%d:%d" % (st.st_size, int(st.st_mtime))
        except OSError:
            stamp = "?"
        return "%s|%s|%s" % (self.version_text(), " ".join(self.cc_prefix), stamp)

    def short_version(self):
        """'clang 19.0.1'."""
        m = re.search(r"clang version (\S+)", self.version_text())
        return "clang %s" % (m.group(1) if m else "?")

    def libdir(self):
        return os.path.join(self.sysroot, "usr", "lib", "aarch64-linux-android")

    def api_libdir(self):
        return os.path.join(self.libdir(), str(API))

    def builtins(self):
        if not self._builtins:
            rc, out = self.procs.run(self.cc_argv + ["--target=" + TARGET, "-rtlib=compiler-rt",
                                                     "-print-libgcc-file-name"])
            path = out.strip().splitlines()[-1] if out.strip() else ""
            if rc != 0 or not os.path.isfile(path):
                raise StepError("LINK_FAILED", "compiler-rt builtins not found (%s); pass --builtins" % path)
            self._builtins = path
        return self._builtins


# ── Sources, flags, cache keys ──────────────────────────────────

def collect_sources(proj):
    """The title project's C: what templates/new-game/CMakeLists.txt builds
    (main.c, recomp_manual.c, src/title/*.c, the generated src/recomp/gen/*.c)
    plus src/extra/*.c (--extra-src)."""
    src = os.path.join(proj, "src")
    out = []
    for fn in ("main.c", "recomp_manual.c"):
        if os.path.isfile(os.path.join(src, fn)):
            out.append(os.path.join(src, fn))
    for sub in ("title", "extra", os.path.join("recomp", "gen")):
        d = os.path.join(src, sub)
        if os.path.isdir(d):
            out += [os.path.join(d, fn) for fn in sorted(os.listdir(d)) if fn.endswith(".c")]
    return out


def compile_flags(opt, toolkit, proj, sysroot, profile=False, include_dirs=()):
    flags = ["--target=" + TARGET, "--sysroot=" + sysroot,
             "-O%s" % opt, "-g0", "-fPIC", "-std=gnu11", "-w",
             "-DXBOXRECOMP_SHELL_TITLE=1", "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX", "-DNDEBUG"]
    if profile:
        flags.append("-DXBA_TITLE_PROFILE=1")
    # src and src/recomp as in the template's CMakeLists; src/recomp/gen too, for
    # src/title and src/extra files that include "recomp_funcs.h".
    for d in [os.path.join(proj, "src"), os.path.join(proj, "src", "recomp"),
              os.path.join(proj, "src", "recomp", "gen")] + \
             [os.path.join(toolkit, d) for d in TOOLKIT_INCLUDES] + list(include_dirs):
        flags.append("-I" + d)
    return flags


def header_digest(dirs):
    """sha256 over every .h under `dirs` (paths and contents): an object
    depends on its headers too, and a lift rewrites recomp_funcs.h."""
    h = hashlib.sha256()
    seen = set()
    for top in dirs:
        for d, subs, files in os.walk(top):
            subs.sort()
            for fn in sorted(files):
                if not fn.endswith(".h"):
                    continue
                p = os.path.realpath(os.path.join(d, fn))
                if p in seen:
                    continue
                seen.add(p)
                h.update(p.encode() + b"\0")
                with open(p, "rb") as f:
                    for chunk in iter(lambda: f.read(1 << 20), b""):
                        h.update(chunk)
                h.update(b"\0")
    return h.hexdigest()


def cache_key(path, source, flags, compiler_id, headers):
    """sha256(source path + source + flags + compiler id + headers digest)."""
    h = hashlib.sha256()
    for part in (path.encode(), b"\0", source, b"\0", "\x1f".join(flags).encode(), b"\0",
                 compiler_id.encode(), b"\0", headers.encode()):
        h.update(part)
    return h.hexdigest()


def include_dirs_of(flags):
    return [f[2:] for f in flags if f.startswith("-I")]


# ── Compile ─────────────────────────────────────────────────────

def _first_error(out):
    for line in out.splitlines():
        if "error:" in line:
            return line.strip()
    lines = [l for l in out.splitlines() if l.strip()]
    return lines[-1].strip() if lines else "no output"


def compile_all(sources, flags, tc, obj_dir, jobs, log=None, on_progress=None):
    """Compile `sources` (largest first) into obj_dir/<key>.o.

    Returns (objects in the order of `sources`, number compiled now).
    on_progress(done, total, compiled_now, eta_or_None) after each file.
    Raises StepError (COMPILE_FAILED / NO_SPACE) or Cancelled."""
    os.makedirs(obj_dir, exist_ok=True)
    cid = tc.compiler_id()
    headers = header_digest(include_dirs_of(flags))
    plan = []
    for s in sources:
        with open(s, "rb") as f:
            data = f.read()
        plan.append((s, len(data), os.path.join(obj_dir, cache_key(s, data, flags, cid, headers) + ".o")))
    total = len(plan)
    todo = [p for p in plan if not os.path.isfile(p[2])]
    todo.sort(key=lambda p: -p[1])
    todo_bytes = sum(p[1] for p in todo) or 1
    state = {"done": total - len(todo), "compiled": 0, "bytes": 0}
    lock = threading.Lock()
    failed = []
    t0 = time.time()
    log_lock = threading.Lock()
    if on_progress:
        on_progress(state["done"], total, 0, None)

    def one(item):
        src, size, obj = item
        if failed or tc.procs.cancelled.is_set():
            return
        tmp = "%s.tmp%d.%d" % (obj, os.getpid(), threading.get_ident())
        rc, out = tc.procs.run(tc.cc_argv + flags + ["-c", src, "-o", tmp])
        if rc != 0 or not os.path.isfile(tmp):
            try:
                os.remove(tmp)
            except OSError:
                pass
            code = "NO_SPACE" if "No space left on device" in out else "COMPILE_FAILED"
            failed.append((code, "%s: %s" % (os.path.basename(src), _first_error(out))))
            if log:
                with log_lock, open(log, "a") as f:
                    f.write("\n$ %s\n%s" % (" ".join(tc.cc_argv + flags + ["-c", src]), out))
            return
        os.replace(tmp, obj)
        with lock:
            state["done"] += 1
            state["compiled"] += 1
            state["bytes"] += size
            el = time.time() - t0
            eta = el / state["bytes"] * (todo_bytes - state["bytes"]) if state["bytes"] else None
            if on_progress:
                on_progress(state["done"], total, state["compiled"], eta)

    if todo:
        ex = ThreadPoolExecutor(max_workers=max(1, jobs))
        try:
            futs = [ex.submit(one, it) for it in todo]
            for fu in futs:
                fu.result()
        except OSError as e:
            if e.errno == errno.ENOSPC:
                raise StepError("NO_SPACE", str(e))
            raise
        finally:
            # A cancel has killed the running compiles already (Procs.cancel);
            # queued ones return at once (failed / cancelled checked first).
            ex.shutdown(wait=True, cancel_futures=True)
    if tc.procs.cancelled.is_set():
        raise Cancelled()
    if failed:
        raise StepError(*failed[0])
    return [p[2] for p in plan], state["compiled"]


def prune(obj_dir, keep):
    """Remove the cached objects not in `keep` (an old lift's, or an old -O):
    the device's storage is not free."""
    keep = {os.path.abspath(k) for k in keep}
    for fn in os.listdir(obj_dir):
        p = os.path.abspath(os.path.join(obj_dir, fn))
        if p not in keep:
            try:
                os.remove(p)
            except OSError:
                pass


# ── Link ────────────────────────────────────────────────────────

def link_args(objects, out, tc, shell_lib, soname="libtitle.so"):
    """ld.lld for a shared libtitle.so that needs libmain.so (the shell, which
    exports the toolkit). What the NDK's clang -shared passes, plus -Bsymbolic
    (the title's own calls bind inside the title) and --no-undefined (every
    symbol the title uses must be in libmain.so or the system libraries:
    checked at build time, not at dlopen)."""
    api = tc.api_libdir()
    builtins = tc.builtins()
    args = tc.ld_argv + [
        "-EL", "--fix-cortex-a53-843419", "-z", "now", "-z", "relro", "-z", "max-page-size=16384",
        "-z", "noexecstack", "--hash-style=gnu", "--eh-frame-hdr", "--build-id=sha1",
        "-m", "aarch64linux", "-shared", "-soname", soname, "-Bsymbolic", "--no-undefined",
        "-o", out, os.path.join(api, "crtbegin_so.o"),
        "-L" + api, "-L" + tc.libdir(), "-L" + os.path.dirname(os.path.abspath(shell_lib)),
    ] + list(objects) + [
        "-l:" + os.path.basename(shell_lib), "-lm", "-ldl", "-llog", "-lc", builtins,
    ]
    unwind = os.path.join(os.path.dirname(builtins), "aarch64", "libunwind.a")
    if os.path.isfile(unwind):
        args.append(unwind)
    args += ["-ldl", os.path.join(api, "crtend_so.o")]
    return args


def link(objects, out, tc, shell_lib, log=None):
    if not os.path.isfile(shell_lib):
        raise StepError("LINK_FAILED", "%s: no such shell library" % shell_lib)
    args = link_args(objects, out, tc, shell_lib)
    rc, text = tc.procs.run(args)
    if log:
        with open(log, "a") as f:
            f.write("\n$ %s\n%s" % (" ".join(args[:30]) + " ...", text))
    if rc != 0:
        code = "NO_SPACE" if "No space left on device" in text else "LINK_FAILED"
        undef = [l.strip() for l in text.splitlines() if "undefined symbol" in l]
        raise StepError(code, "; ".join(undef[:3]) if undef else _first_error(text))
