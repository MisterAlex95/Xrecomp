"""
The compile driver of `tools.prepare --android`: the title project's C into
objects with clang, then libtitle.so with ld.lld. No CMake (there is none on
the device): threads and subprocesses only, no multiprocessing (no sem_open
on Android). The host and CI run the same code with the NDK's clang, so the
path the tests cover is the one the device runs.

Objects live in a cache keyed by sha256(source path + source + flags +
compiler id + headers): an object whose key exists is not compiled again, so
a killed run resumes where it stopped and a second run compiles nothing.
The key names the toolkit's own directory as <toolkit> (the app extracts each
toolkit to a new files/toolkit-<version>) and the compiler by its bytes (an
app update extracts the same libxbacc.so again, with a new mtime): a Prepare
after an app update recompiles only what its C or headers changed.
<obj>/keys.json remembers each key with its inputs' stat (size, mtime,
ctime, inode): a second run re-reads and re-hashes only what changed.
"""

import errno
import hashlib
import json
import os
import re
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor

from tools.prepare import cpu

API = 30
TARGET = "aarch64-linux-android%d" % API
GB = 1 << 30
# Each clang job on the generated C: see "Measurements" in docs/android/prepare.md.
# 512 MB since the lift's chunks are at most ~1.5 MB of C (XBA-209): one peaks
# at ~170 MB, where a 5 MB chunk of the old 250-function split reached 0.6 GB.
JOB_RSS = GB // 2
RESERVED = 3 * GB
MAX_JOBS = 5

# The lift's chunks (tools.recomp --split-bytes, XBA-209): about 1 MB of C
# each, at boundaries a re-lift with a few more functions keeps. ~1-2 s of
# clang -O2 per chunk on a desktop core; a 250 MB title gives ~250 of them,
# enough to keep every job busy to the end.
LIFT_CHUNK_BYTES = 1000000

# The toolkit's header directories (what the CMake targets of src/ export).
TOOLKIT_INCLUDES = ("include", "src", "src/kernel", "src/platform", "src/audio", "src/apu",
                    "src/nv2a", "src/d3d", "src/input", "src/usb", "src/video", "src/selftest")


def sysroot_resource_dir(sysroot):
    """The clang resource directory carried inside the sysroot, or None.

    The app's sysroot.zip (docs/android/toolchain.md) holds lib/clang/<major>/
    next to usr/: the embedded clang's own headers (stddef.h, arm_neon.h, ...)
    and compiler-rt. That clang cannot find them by itself: it would look next
    to its binary, in nativeLibraryDir. The NDK's sysroot has no lib/clang (its
    clang knows its own resource directory): None, and nothing changes."""
    base = os.path.join(sysroot, "lib", "clang")
    try:
        names = os.listdir(base)
    except OSError:
        return None
    for name in sorted(names, key=lambda n: [int(x) if x.isdigit() else 0 for x in n.split(".")], reverse=True):
        d = os.path.join(base, name)
        if os.path.isfile(os.path.join(d, "include", "stddef.h")):
            return d
    return None


class StepError(Exception):
    def __init__(self, code, msg):
        Exception.__init__(self, msg)
        self.code = code
        self.msg = msg


class Cancelled(Exception):
    pass


# ── Child processes ─────────────────────────────────────────────

class _Flag:
    """threading.Event's set() / is_set() without its lock: set from a
    signal handler, which must not take a lock the thread it interrupted
    may hold. One attribute store or load is atomic under the GIL."""

    def __init__(self):
        self._set = False

    def set(self):
        self._set = True

    def is_set(self):
        return self._set


class Procs:
    """Every child process, so that a cancel kills them all at once.

    cancel() runs in the SIGTERM / SIGINT handlers (tools/prepare/android.py,
    gba.py), on the main thread, between two of its bytecodes: wherever it
    stopped, inside `with self.lock` too. So cancel() takes no lock: a
    non-reentrant lock held by the code it interrupted would never be
    released (the Prepare hung instead of cancelling)."""

    def __init__(self):
        self.lock = threading.Lock()
        self.live = set()
        self.cancelled = _Flag()

    def start(self, args, **kw):
        if self.cancelled.is_set():
            raise Cancelled()
        p = subprocess.Popen(args, **kw)
        try:
            with self.lock:
                self.live.add(p)
        except BaseException:   # a signal handler's Cancelled before p was in live: cancel() missed it
            p.kill()
            raise
        if self.cancelled.is_set():          # cancel() ran between the check and add()
            p.kill()
        return p

    def done(self, p):
        with self.lock:
            self.live.discard(p)

    def run(self, args, cwd=None, env=None, peak=None):
        """(returncode, stdout+stderr as text). peak: a list that gets the
        child's peak resident set in kB (wait4's ru_maxrss; 0 where the host
        has no wait4), for the Prepare's measurements (XBA-128)."""
        p = self.start(args, cwd=cwd, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        try:
            if peak is not None and hasattr(os, "wait4"):
                out = p.stdout.read()
                p.stdout.close()
                peak.append(_reap(p))
            else:
                out, _ = p.communicate()
                if peak is not None:
                    peak.append(0)
        finally:
            self.done(p)
        if self.cancelled.is_set():
            raise Cancelled()
        return p.returncode, out.decode("utf-8", "replace")

    def run_logged(self, args, log, cwd=None, on_tick=None, tick=0.5, peak=None):
        """Run with stdout+stderr appended to `log`; on_tick(elapsed seconds)
        while it runs. Returns (returncode, seconds). peak: a list that gets
        the child's own peak RSS in kB (wait_ticking)."""
        t0 = time.time()
        with open(log, "a") as f:
            f.write("\n$ %s\n" % " ".join(args))
            f.flush()
            p = self.start(args, cwd=cwd, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
            try:
                kb = wait_ticking(p, (lambda: on_tick(time.time() - t0)) if on_tick else None, tick)
                if peak is not None:
                    peak.append(kb)
            finally:
                if p.returncode is None:     # an exception while it ran (a cancel, a bug): it does not outlive us
                    try:
                        p.kill()
                        p.wait()
                    except OSError:
                        pass
                self.done(p)
        if self.cancelled.is_set():
            raise Cancelled()
        return p.returncode, time.time() - t0

    def cancel(self):
        """Kill every live child. Safe in a signal handler: no lock (see the
        class). The flag first: a start() that adds its child after the
        copy below sees it and kills that child itself."""
        self.cancelled.set()
        live = ()
        for _ in range(100):
            try:
                live = tuple(self.live)     # one C call: no other thread runs during it
                break
            except RuntimeError:            # "set changed size during iteration": once more
                continue
        for p in live:
            try:
                p.kill()
            except OSError:
                pass


def wait_ticking(p, on_tick=None, tick=0.5):
    """Wait for Popen p, calling on_tick() every `tick` seconds; its exit status
    into p.returncode. Returns its own peak RSS in kB (wait4's ru_maxrss: the
    lifter's alone, where RUSAGE_CHILDREN mixes in every compile reaped so
    far), 0 where the host has no wait4."""
    if not hasattr(os, "wait4"):
        while True:
            try:
                p.wait(timeout=tick)
                return 0
            except subprocess.TimeoutExpired:
                if on_tick:
                    on_tick()
    next_tick = time.time() + tick
    while True:
        try:
            pid, status, ru = os.wait4(p.pid, os.WNOHANG)
        except InterruptedError:
            continue
        except ChildProcessError:       # reaped elsewhere: Popen knows the code
            p.wait()
            return 0
        if pid:
            p.returncode = os.waitstatus_to_exitcode(status) if hasattr(os, "waitstatus_to_exitcode") else \
                (-(status & 0x7F) if status & 0x7F else (status >> 8) & 0xFF)
            return int(ru.ru_maxrss)
        time.sleep(0.05)
        if on_tick and time.time() >= next_tick:
            next_tick = time.time() + tick
            on_tick()


def _reap(p):
    """Wait for Popen p with wait4: its exit status into p.returncode, and its
    peak RSS (kB on Linux and Android) returned. Popen does not wait again
    once returncode is set."""
    while True:
        try:
            _, status, ru = os.wait4(p.pid, 0)
            break
        except InterruptedError:
            continue
        except ChildProcessError:       # reaped elsewhere: Popen knows the code
            p.wait()
            return 0
    p.returncode = os.waitstatus_to_exitcode(status) if hasattr(os, "waitstatus_to_exitcode") else \
        (-(status & 0x7F) if status & 0x7F else (status >> 8) & 0xFF)
    return int(ru.ru_maxrss)


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


def default_jobs(avail=None, cpus=None, job_rss=JOB_RSS, reserved=RESERVED):
    """min(5, (MemAvailable - 3 GB) / 512 MB), at least 1, at most the CPU
    count (job_rss / reserved: another console's figures, tools/prepare/gba.py).
    `avail` None = unknown memory: 1."""
    if avail is None:
        return 1
    n = int((avail - reserved) // job_rss)
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

    def compile_args(self, flags, src, out):
        """One compile: src into the object out (clang-cl spells it otherwise:
        tools/prepare/desktop_build.py)."""
        return self.cc_argv + flags + ["-c", src, "-o", out]

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
        size and sha256 of the binary (a rebuilt toolchain of the same
        version). Not its mtime: an app update installs the same libxbacc.so
        again, and every object would be compiled again."""
        return self.compiler_id_cached(None)

    def compiler_id_cached(self, cache_dir):
        """compiler_id(), the binary's sha256 remembered in
        cache_dir/compiler.json with its stat (hashed again when that changes)."""
        try:
            sig = stat_sig(self.cc)
        except OSError:
            return "%s|%s|?" % (self.version_text(), " ".join(self.cc_prefix))
        path = os.path.join(cache_dir, COMPILER_FILE) if cache_dir else None
        sha = None
        if path:
            try:
                with open(path) as f:
                    c = json.load(f)
                if c.get("cc") == os.path.abspath(self.cc) and c.get("sig") == sig:
                    sha = c.get("sha256")
            except (OSError, ValueError, AttributeError):
                pass
        if not isinstance(sha, str):
            h = hashlib.sha256()
            with open(self.cc, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            sha = h.hexdigest()
            if path and sig[1] < time.time_ns() - RACY_NS:
                try:
                    with open(path + ".tmp", "w") as f:
                        json.dump({"cc": os.path.abspath(self.cc), "sig": sig, "sha256": sha}, f)
                    os.replace(path + ".tmp", path)
                except OSError:
                    pass
        return "%s|%s|%d:%s" % (self.version_text(), " ".join(self.cc_prefix), sig[0], sha)

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
            rd = sysroot_resource_dir(self.sysroot)
            mine = rd and os.path.join(rd, "lib", "linux", "libclang_rt.builtins-aarch64-android.a")
            if mine and os.path.isfile(mine):
                self._builtins = mine
                return mine
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


def compile_flags(opt, toolkit, proj, sysroot, profile=False, include_dirs=(), cpu_features=()):
    flags = ["--target=" + TARGET, "--sysroot=" + sysroot]
    rd = sysroot_resource_dir(sysroot)
    if rd:
        flags.append("-resource-dir=" + rd)
    flags += ["-O%s" % opt, "-g0", "-fPIC", "-std=gnu11", "-w",
              "-DXBOXRECOMP_SHELL_TITLE=1", "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX", "-DNDEBUG"]
    # Lock-prefixed instructions (RECOMP_ATOMIC_*, recomp_types.h) inline as exclusive load/store loops (XBA-277),
    # which every ARMv8.0 core runs, instead of a call per atomic into libgcc's outline helpers (a feature test,
    # then the ARMv8.1 atomic or the same loop). tools.bench w_lock: -17 % instructions.
    flags.append("-mno-outline-atomics")
    # Math functions never set errno: what clang already does for an Android target, written out so that the same
    # flags compiled for another target (tools/bench's aarch64 glibc proxy, a desktop build) give the same code.
    # With errno, sqrt keeps a libm call on its NaN path and every libm call (frexp / ldexp in the x87 precision-24
    # rounding) may write memory, so the guest registers are reloaded after each x87 operation. Nothing generated
    # or in the title project reads errno.
    flags.append("-fno-math-errno")
    # cpu_features (tools/prepare/cpu.py, XBA-313): the device's own CPU, which the pack records and every reader
    # checks. "lse": the ARMv8.1 atomics inline instead of those loops.
    flags += cpu.flags(cpu_features)
    if profile:
        flags.append("-DXBA_TITLE_PROFILE=1")
    # src and src/recomp as in the template's CMakeLists; src/recomp/gen too, for
    # src/title and src/extra files that include "recomp_funcs.h".
    for d in [os.path.join(proj, "src"), os.path.join(proj, "src", "recomp"),
              os.path.join(proj, "src", "recomp", "gen")] + \
             [os.path.join(toolkit, d) for d in TOOLKIT_INCLUDES] + list(include_dirs):
        flags.append("-I" + d)
    return flags


FUNCS_H = "recomp_funcs.h"
# A line of recomp_funcs.h that only declares a lifted function (all of them
# are void(void)), with the comment a hand-written override's line carries;
# and its comments (the banner counts the functions).
_FUNC_DECL = re.compile(rb"^void [A-Za-z_]\w*\(void\);[^\n]*\n?", re.M)
_COMMENT = re.compile(rb"/\*.*?\*/", re.S)


TOOLKIT_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
TOOLKIT_TAG = "<toolkit>"


def portable(text, roots=None):
    """text with the toolkit's directory spelled TOOLKIT_TAG: what the cache keys see of a path or a flag. Two
    toolkits with the same headers compile the same objects, wherever they were extracted."""
    for root in roots or (TOOLKIT_ROOT, os.path.realpath(TOOLKIT_ROOT)):
        text = text.replace(root + os.sep, TOOLKIT_TAG + "/")
    return text


def _headers(dirs):
    """Every .h under `dirs` once, in digest order: (real path, file name)."""
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
                yield p, fn


def header_digest(dirs, generated=False):
    """sha256 over every .h under `dirs` (paths and contents): an object
    depends on its headers too, and a lift rewrites recomp_funcs.h.

    generated: the digest for the lifted C (src/recomp/gen/*.c). There
    recomp_funcs.h counts without its `void name(void);` lines and its
    comments (XBA-209): a
    generated file that compiled names only functions declared, all with that
    one signature, so a declaration added or dropped elsewhere cannot change
    its object, and a re-lift that found a few more functions recompiles the
    chunks that changed, not all of them. The project's own C (main.c,
    src/title, src/extra) keeps the whole header in its key."""
    h = hashlib.sha256()
    for p, fn in _headers(dirs):
        h.update(portable(p).encode() + b"\0")
        with open(p, "rb") as f:
            if generated and fn == FUNCS_H:
                h.update(_FUNC_DECL.sub(b"", _COMMENT.sub(b"", f.read())))
            else:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
        h.update(b"\0")
    return h.hexdigest()


# ── The key cache ───────────────────────────────────────────────
#
# A Prepare with nothing to compile still computed every key: a few hundred
# MB of lifted C and every header read and hashed (about a second on a
# desktop core with the built-in SHA-2 the device's Python has, more from
# the device's storage). <obj>/keys.json keeps each key with the stat of
# what it was computed from; a file whose stat is unchanged keeps its key
# without being read. The keys themselves are unchanged: an object cache
# made before keys.json existed stays valid.

KEYS_FILE = "keys.json"
COMPILER_FILE = "compiler.json"   # the compiler binary's sha256 with its stat (Toolchain.compiler_id_cached)
KEYS_VERSION = 2             # bump when header_digest() or cache_key() changes what it computes
# A file changed less than this before keys.json is written is not
# remembered: a second write within the file system's timestamp step, with
# the same size, would keep the stat (git's « racily clean » entries).
RACY_NS = 2 * 10 ** 9


def stat_sig(path):
    """What says a file is unchanged without reading it: [size, mtime_ns,
    ctime_ns, inode]. ctime and inode change on any write or replace, also
    one that puts the old mtime back (cp -p, an unzip)."""
    st = os.stat(path)
    # Windows' st_ctime is the creation time (and deprecated): not a change.
    return [st.st_size, st.st_mtime_ns, 0 if os.name == "nt" else st.st_ctime_ns, st.st_ino]


def _headers_sig(dirs):
    """(sha256 over every header's path and stat, newest mtime_ns)."""
    h = hashlib.sha256()
    newest = 0
    for p, _ in _headers(dirs):
        sig = stat_sig(p)
        newest = max(newest, sig[1])
        h.update(("%s\0%s\0" % (p, sig)).encode())
    return h.hexdigest(), newest


def _load_keys(obj_dir):
    try:
        with open(os.path.join(obj_dir, KEYS_FILE)) as f:
            k = json.load(f)
    except (OSError, ValueError):
        return {}
    if not isinstance(k, dict) or k.get("v") != KEYS_VERSION:
        return {}
    return k


def _save_keys(obj_dir, keys):
    """keys.json, atomically; a failure (no space) only costs the next run a re-hash."""
    path = os.path.join(obj_dir, KEYS_FILE)
    tmp = "%s.tmp%d" % (path, os.getpid())
    try:
        with open(tmp, "w") as f:
            json.dump(keys, f)
        os.replace(tmp, path)
    except OSError:
        try:
            os.remove(tmp)
        except OSError:
            pass


def plan_keys(sources, flags, cid, obj_dir):
    """[(source, size, object path)] for `sources`, each object named by its
    cache_key(); what keys.json remembers is reused, the rest is read and
    hashed, and keys.json is written again. Returns (plan, files hashed)."""
    old = _load_keys(obj_dir)
    now = time.time_ns()
    dirs = include_dirs_of(flags)
    hsig, hnewest = _headers_sig(dirs)
    hk = old.get("headers")
    if isinstance(hk, list) and len(hk) == 3 and hk[0] == hsig:
        headers, gen_headers = hk[1], hk[2]
    else:
        headers = header_digest(dirs)
        gen_headers = header_digest(dirs, generated=True)
    kflags = [portable(f) for f in flags]
    ctx = {}
    for name, hd in (("all", headers), ("gen", gen_headers)):
        h = hashlib.sha256()
        h.update(("\x1f".join(kflags) + "\0" + cid + "\0" + hd).encode())
        ctx[name] = h.hexdigest()
    files = old.get("files") if isinstance(old.get("files"), dict) else {}
    new_files = {}
    plan = []
    hashed = 0
    for s in sources:
        sig = stat_sig(s)
        which = "gen" if is_generated(s) else "all"
        got = files.get(s)
        if isinstance(got, list) and len(got) == 6 and got[:4] == sig and got[4] == ctx[which]:
            key = got[5]
        else:
            with open(s, "rb") as f:
                data = f.read()
            # sig is from before the read: a change during it is hashed again next time.
            key = cache_key(portable(s), data, kflags, cid, gen_headers if which == "gen" else headers)
            hashed += 1
        if sig[1] < now - RACY_NS:
            new_files[s] = sig + [ctx[which], key]
        plan.append((s, sig[0], os.path.join(obj_dir, key + ".o")))
    # The digests this plan used, whatever the headers' age: EarlyCompile's guess of a re-lift's keys (the lift
    # writes recomp_funcs.h just before the compile, too recently for "headers" to remember it).
    keys = {"v": KEYS_VERSION, "files": new_files, "digests": [headers, gen_headers]}
    if hnewest < now - RACY_NS:
        keys["headers"] = [hsig, headers, gen_headers]
    if keys != old:
        _save_keys(obj_dir, keys)
    return plan, hashed


def cache_key(path, source, flags, compiler_id, headers):
    """sha256(source path + source + flags + compiler id + headers digest)."""
    h = hashlib.sha256()
    for part in (path.encode(), b"\0", source, b"\0", "\x1f".join(flags).encode(), b"\0",
                 compiler_id.encode(), b"\0", headers.encode()):
        h.update(part)
    return h.hexdigest()


def is_generated(path):
    """A lifted C file: <project>/src/recomp/gen/*.c."""
    d = os.path.dirname(os.path.abspath(path))
    return os.path.basename(d) == "gen" and os.path.basename(os.path.dirname(d)) == "recomp"


def include_dirs_of(flags):
    return [f[2:] for f in flags if f.startswith("-I")]


# ── Compile ─────────────────────────────────────────────────────

def _first_error(out):
    for line in out.splitlines():
        if "error:" in line:
            return line.strip()
    lines = [l for l in out.splitlines() if l.strip()]
    return lines[-1].strip() if lines else "no output"


def compile_all(sources, flags, tc, obj_dir, jobs, log=None, on_progress=None, limit=None, stats=None, early=None):
    """Compile `sources` (largest first) into obj_dir/<key>.o.

    Returns (objects in the order of `sources`, number compiled now).
    on_progress(done, total, compiled_now, eta_or_None) after each file.
    limit(): how many compiles may run at once right now (the device log,
    tools/prepare/metrics.py: fewer when the device is hot, 0 holds); read
    before each compile starts, never above `jobs`. stats: a dict that gets
    "peak_kb" (each compile's peak RSS), "compiled" and "held_s" (wall time
    limit() held every compile, 0 jobs) and "hashed" (the sources read and
    hashed for their key: plan_keys()). early: the EarlyCompile that ran
    beside the lift (finish()ed): its objects that still match are adopted,
    its compiles still running are waited for (they take their slots until
    they end, then are adopted or compiled again), and they count in stats.
    Raises StepError (COMPILE_FAILED / NO_SPACE) or Cancelled."""
    try:
        os.makedirs(obj_dir, exist_ok=True)
        cached_id = getattr(tc, "compiler_id_cached", None)
        plan, hashed = plan_keys(sources, flags, cached_id(obj_dir) if cached_id else tc.compiler_id(), obj_dir)
        if stats is not None:
            stats["hashed"] = hashed
        return _compile_plan(plan, flags, tc, jobs, log, on_progress, limit, stats, early)
    finally:
        if early is not None:       # after a normal end nothing is left; after a failure or a cancel, its
            early.abort()           # compiles still running are killed and its objects deleted


def _compile_plan(plan, flags, tc, jobs, log, on_progress, limit, stats, early):
    busy = early.busy() if early is not None else set()      # before adopt(): one ending between is adopted
    adopted = early.adopt(plan) if early is not None else 0
    total = len(plan)
    missing = [p for p in plan if not os.path.isfile(p[2])]
    todo = [p for p in missing if p[0] not in busy]
    todo.sort(key=lambda p: -p[1])
    todo_bytes = sum(p[1] for p in todo) or 1
    state = {"done": total - len(missing), "compiled": 0, "bytes": 0}
    lock = threading.Lock()
    failed = []
    t0 = time.time()
    log_lock = threading.Lock()
    gate = threading.Condition()
    running = [0]
    peaks = []
    held = [0.0]
    held_since = [None]             # the device log holds every compile (jobs 0) since then
    n_early_peaks = len(early.peaks) if early is not None else 0
    if early is not None:
        peaks.extend(early.peaks[:n_early_peaks])
    if stats is not None:
        stats.update(peak_kb=peaks, compiled=0, held_s=0.0)
        if early is not None:
            stats.update(early=adopted, early_compiled=early.compiled)
    if on_progress:
        on_progress(state["done"], total, 0, None)

    def enter():
        """Wait for a free slot under limit() (re-read every second while held)."""
        with gate:
            while not (failed or tc.procs.cancelled.is_set()):
                allowed = jobs if limit is None else max(0, min(jobs, limit()))
                if allowed == 0 and held_since[0] is None:
                    held_since[0] = time.time()
                elif allowed > 0 and held_since[0] is not None:
                    held[0] += time.time() - held_since[0]
                    held_since[0] = None
                if early is not None:
                    allowed -= early.running()      # the early compiles still running hold their slots
                if running[0] < allowed:
                    running[0] += 1
                    return True
                gate.wait(0.25 if early is not None else 1.0)
            return False

    def leave():
        with gate:
            running[0] -= 1
            gate.notify_all()

    def one(item):
        src, size, obj = item
        if failed or tc.procs.cancelled.is_set():
            return
        if not enter():
            return
        tmp = "%s.tmp%d.%d" % (obj, os.getpid(), threading.get_ident())
        peak = []
        try:
            rc, out = tc.procs.run(tc.compile_args(flags, src, tmp), peak=peak)
        finally:
            leave()
        with lock:
            peaks.extend(peak)
        if rc != 0 or not os.path.isfile(tmp):
            try:
                os.remove(tmp)
            except OSError:
                pass
            code = "NO_SPACE" if "No space left on device" in out else "COMPILE_FAILED"
            failed.append((code, "%s: %s" % (os.path.basename(src), _first_error(out))))
            if log:
                with log_lock, open(log, "a") as f:
                    f.write("\n$ %s\n%s" % (" ".join(tc.compile_args(flags, src, obj)), out))
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

    def pool(items):
        if not items:
            return
        ex = ThreadPoolExecutor(max_workers=max(1, jobs))
        try:
            futs = [ex.submit(one, it) for it in items]
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

    if busy:
        early.kill([p[0] for p in plan if p[0] in busy and os.path.isfile(p[2])])
    pool(todo)
    if busy and not failed and not tc.procs.cancelled.is_set():
        # The early compiles that were running when the lift ended: adopted, or compiled again here.
        early.wait()
        with lock:
            peaks.extend(early.peaks[n_early_peaks:])
        late = early.adopt(plan)
        adopted += late
        state["done"] += late
        if on_progress and late:
            on_progress(state["done"], total, state["compiled"], None)
        if stats is not None:
            stats.update(early=adopted, early_compiled=early.compiled)
        pool([p for p in missing if p[0] in busy and not os.path.isfile(p[2])])
    if held_since[0] is not None:
        held[0] += time.time() - held_since[0]
    if stats is not None:
        stats.update(compiled=state["compiled"] + adopted, held_s=held[0])
    if tc.procs.cancelled.is_set():
        raise Cancelled()
    if failed:
        raise StepError(*failed[0])
    return [p[2] for p in plan], state["compiled"] + adopted


# ── Compiling while the lifter runs ─────────────────────────────
#
# The lifters write their C file by file (x360_tools.recomp one per bucket of guest code, tools.recomp one per
# chunk with --split-bytes), and the compile used to wait for the last one. EarlyCompile compiles the files that are
# finished while the lifter goes on, into temporary objects; compile_all then adopts each one under the key it
# computes after the lift, when the source and the headers it was compiled against are still those it would
# compile now, and compiles the rest as before. compile_all stays the only judge: an early object that is not
# adopted is deleted, an early compile that fails says nothing (compile_all compiles that file again and reports
# its error), and a lift that fails or is cancelled stops every early compile and leaves no temporary object.
#
# « Finished »: no lifter announces its files, and they write them in place (open, write, close). A file is taken
# once its stat (size, mtime, ctime, inode) is the same at two polls POLL_S apart; whatever happens later, an object
# is only adopted when the source's stat when compile_all hashes it is the one taken before that compile started,
# so an object is never kept under the key of other bytes than the ones clang read.
#
# Jobs: the lifter counts as one, so at most min(jobs, limit()) - 1 early compiles run (jobs 1, a severe device
# (limit 1) or a hold (0): none, as before), and one starts only while MemAvailable leaves `reserved` beside it.

EARLY_PREFIX = "early-"          # the temporary objects in obj/ (prune removes a crashed run's)


class EarlyCompile:
    POLL_S = 0.25

    def __init__(self, gen, flags, tc, obj_dir, jobs, limit=None, log=None, job_rss=JOB_RSS, reserved=RESERVED,
                 mem=mem_available):
        self.gen = gen
        self.flags = list(flags)
        self.tc = tc
        self.obj_dir = obj_dir
        self.jobs = max(1, jobs)
        self.limit = limit
        self.log = log
        self.job_rss, self.reserved, self.mem = job_rss, reserved, mem
        self.dirs = include_dirs_of(self.flags)
        self.funcs_h = os.path.join(os.path.abspath(gen), FUNCS_H)
        self.done = {}            # source -> (temporary object, source stat, headers' stat digest, includes FUNCS_H)
        self.peaks = []           # each early compile's peak RSS (kB)
        self.compiled = 0         # early compiles that succeeded (adopted or not)
        self.cached = 0           # files not compiled early: their object is in the cache already
        self._ctx = None          # what the cache keys need (_context)
        self.failed = 0           # early compiles that failed or whose source changed under them
        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)
        self._queue = []
        self._seen = {}           # source -> its stat at the last poll
        self._taken = {}          # source -> the stat it was queued with
        self._running = 0
        self._busy = set()
        self._proc_of = {}        # source -> its compile (Popen)
        self._live = set()        # the clang processes running now (Popen)
        self._threads = []
        self._stop = False
        self._watcher = None
        self._seq = 0

    # -- the lifter's side --

    def start(self):
        os.makedirs(self.obj_dir, exist_ok=True)
        self._watcher = threading.Thread(target=self._watch, name="early-compile", daemon=True)
        self._watcher.start()
        return self

    def count(self):
        with self._lock:
            return self.compiled

    def finish(self):
        """The lifter has exited: no early compile starts any more. The running ones go on: compile_all waits
        for them (busy(), running(), wait())."""
        self._halt()
        if self._watcher is not None:
            self._watcher.join()

    def busy(self):
        """The sources being compiled now."""
        with self._lock:
            return set(self._busy)

    def running(self):
        with self._lock:
            return self._running

    def wait(self):
        for t in list(self._threads):
            t.join()

    def abort(self):
        """The lift failed or was cancelled: the running compiles are killed, and every temporary object deleted."""
        self._halt()
        with self._lock:
            live = list(self._live)
        for p in live:
            try:
                p.kill()
            except OSError:
                pass
        self._join()
        self.discard()

    def discard(self):
        with self._lock:
            done, self.done = self.done, {}
        for tmp, _, _, _ in done.values():
            _remove(tmp)

    def _halt(self):
        with self._cv:
            self._stop = True
            self._queue = []
            self._cv.notify_all()

    def _join(self):
        if self._watcher is not None:
            self._watcher.join()
        for t in list(self._threads):
            t.join()

    # -- watching gen/ --

    def _allowed(self):
        n = self.jobs if self.limit is None else max(0, min(self.jobs, self.limit()))
        return n - 1                # the lifter is one of them

    def _poll(self):
        try:
            names = os.listdir(self.gen)
        except OSError:
            return
        for fn in sorted(names):
            if not fn.endswith(".c"):
                continue
            src = os.path.join(self.gen, fn)
            try:
                sig = stat_sig(src)
            except OSError:
                continue
            last = self._seen.get(src)
            self._seen[src] = sig
            if sig == last and self._taken.get(src) != sig:
                self._taken[src] = sig
                if self._in_cache(src):
                    with self._lock:
                        self.cached += 1
                    continue
                with self._lock:
                    self._queue.append(src)

    def _watch(self):
        try:
            while True:
                with self._cv:
                    if self._stop or self.tc.procs.cancelled.is_set():
                        return
                self._poll()
                with self._cv:
                    while self._queue and not self._stop and self._running < self._allowed() and self._room():
                        src = self._queue.pop(0)
                        self._running += 1
                        self._busy.add(src)
                        t = threading.Thread(target=self._one, args=(src,), daemon=True)
                        self._threads.append(t)
                        t.start()
                    self._cv.wait(self.POLL_S)
        except Exception:           # a bug here must not break the lift: compile_all does the work
            with self._cv:
                self._stop = True

    def _context(self):
        """(portable flags, compiler id, {"all": [header digests], "gen": [...]}) for the cache keys, or False when
        they cannot be had. The digests: the headers as they are now (right where the lift writes none: x360), and
        the ones keys.json remembers from the last compile (right for a re-lift whose recomp_funcs.h has the same
        shape: the key of the lifted C leaves its declarations out)."""
        if self._ctx is None:
            try:
                cached_id = getattr(self.tc, "compiler_id_cached", None)
                cid = cached_id(self.obj_dir) if cached_id else self.tc.compiler_id()
                digests = {"all": [header_digest(self.dirs)], "gen": [header_digest(self.dirs, generated=True)]}
                kj = _load_keys(self.obj_dir)
                for got in (kj.get("digests"), (kj.get("headers") or [None])[1:]):
                    if isinstance(got, list) and len(got) == 2 and all(isinstance(x, str) for x in got):
                        digests["all"].append(got[0])
                        digests["gen"].append(got[1])
                self._ctx = ([portable(f) for f in self.flags], cid, digests)
            except Exception:       # no key, no skip: the file is compiled early as any other
                self._ctx = False
        return self._ctx

    def _in_cache(self, src):
        """Whether src's object is in the cache under a key compile_all may compute (a re-lift that wrote the same
        C: an app update, the self-healing loop): no early compile for it, its slot goes to another file."""
        ctx = self._context()
        if not ctx:
            return False
        kflags, cid, digests = ctx
        try:
            with open(src, "rb") as f:
                data = f.read()
        except OSError:
            return False
        for hd in digests["gen" if is_generated(src) else "all"]:
            key = cache_key(portable(src), data, kflags, cid, hd)
            if os.path.isfile(os.path.join(self.obj_dir, key + ".o")):
                return True
        return False

    def kill(self, sources):
        """Stops the early compiles of these sources (compile_all found their objects in the cache)."""
        with self._lock:
            procs = [self._proc_of[s] for s in sources if s in self._proc_of]
        for p in procs:
            try:
                p.kill()
            except OSError:
                pass

    def _room(self):
        if self.mem is None:
            return True
        avail = self.mem()
        return avail is None or avail >= self.reserved + self.job_rss

    # -- one compile --

    def _headers(self, with_funcs_h):
        """sha256 over the stat of every header the compile sees; FUNCS_H left out for a file that does not include
        it (tools.recomp's self-contained chunks: the lift writes recomp_funcs.h last)."""
        h = hashlib.sha256()
        for p, _ in _headers(self.dirs):
            if not with_funcs_h and p == os.path.realpath(self.funcs_h):
                continue
            h.update(("%s\0%s\0" % (p, stat_sig(p))).encode())
        return h.hexdigest()

    def _one(self, src):
        tmp = None
        ok = False
        try:
            sig = stat_sig(src)
            with open(src, "rb") as f:
                with_funcs_h = b'"' + FUNCS_H.encode() + b'"' in f.read()
            hs = self._headers(with_funcs_h)
            with self._lock:
                self._seq += 1
                tmp = os.path.join(self.obj_dir, "%s%d.%d.o" % (EARLY_PREFIX, os.getpid(), self._seq))
            rc, peak = self._run(self.tc.compile_args(self.flags, src, tmp), src)
            if peak:
                with self._lock:
                    self.peaks.append(peak)
            ok = (rc == 0 and os.path.isfile(tmp) and stat_sig(src) == sig and self._headers(with_funcs_h) == hs)
            if ok:
                with self._lock:
                    old = self.done.get(src)
                    self.done[src] = (tmp, sig, hs, with_funcs_h)
                    self.compiled += 1
                if old:
                    _remove(old[0])
        except (OSError, Cancelled):
            ok = False
        finally:
            if not ok:
                if tmp:
                    _remove(tmp)
                with self._lock:
                    self.failed += 1
            with self._cv:
                self._running -= 1
                self._busy.discard(src)
                self._cv.notify_all()

    def _run(self, args, src=None):
        """(returncode, peak RSS kB) of one compile, its output dropped (compile_all reports a real error)."""
        procs = self.tc.procs
        with self._lock:
            if self._stop:
                return 1, 0
        p = procs.start(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        with self._lock:
            self._live.add(p)
            self._proc_of[src] = p
            stop = self._stop
        if stop:                    # abort() copied _live before this one was in it
            p.kill()
        try:
            if hasattr(os, "wait4"):
                peak = _reap(p)
            else:
                p.wait()
                peak = 0
        finally:
            procs.done(p)
            with self._lock:
                self._live.discard(p)
                if self._proc_of.get(src) is p:
                    del self._proc_of[src]
        if procs.cancelled.is_set():
            raise Cancelled()
        return p.returncode, peak

    # -- compile_all's side --

    def adopt(self, plan):
        """Moves each early object whose source and headers are still the ones it was compiled from to its key's
        object path in `plan` ([(source, size, object)]); deletes the others. Returns how many were adopted."""
        with self._lock:
            done, self.done = self.done, {}
        n = 0
        heads = {}
        for src, _, obj in plan:
            got = done.pop(src, None)
            if got is None:
                continue
            tmp, sig, hs, with_funcs_h = got
            try:
                if with_funcs_h not in heads:
                    heads[with_funcs_h] = self._headers(with_funcs_h)
                if stat_sig(src) == sig and heads[with_funcs_h] == hs and not os.path.isfile(obj):
                    os.replace(tmp, obj)
                    n += 1
                    continue
            except OSError:
                pass
            _remove(tmp)
        for tmp, _, _, _ in done.values():
            _remove(tmp)
        return n


def _remove(path):
    try:
        os.remove(path)
    except OSError:
        pass


def prune(obj_dir, keep):
    """Remove the cached objects not in `keep` (an old lift's, or an old -O):
    the device's storage is not free. keys.json and compiler.json stay."""
    keep = {os.path.abspath(k) for k in keep}
    keep.add(os.path.abspath(os.path.join(obj_dir, KEYS_FILE)))
    keep.add(os.path.abspath(os.path.join(obj_dir, COMPILER_FILE)))
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
