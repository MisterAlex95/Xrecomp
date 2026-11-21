"""
`python -m tools.prepare GAME.gba`: a Game Boy Advance title pack (XBA-157 / XBA-158; docs/android/prepare.md « GBA
packs », consoles/gba/runtime/gba_recomp.h).

The same five steps and progress lines as an Xbox pack, for a ROM:

  1 identify  the ROM's title key (game code + CRC32, as gba_runtime_key / recomp_pack_rom_identify), its SHA-256,
              a copy in <work>/rom.gba (the app may hand over a document: the copy is what the steps read)
  2 analyse   the addresses earlier runs had to interpret (<work>/discovered.txt, written by the runtime at exit:
              gba_recomp_missed), merged into <work>/discovered-seeds.txt
  3 lift      `python -m gba_tools.recomp <rom> <work>/gen [--discovered seeds]` (consoles/gba/tools)
  4 compile   every generated C file with -I consoles/gba/runtime, by the Xbox pack's compile driver (compile.py: the
              on-device clang multicall and sysroot with --android, the host's cc otherwise), object cache included
  5 pack      libtitle.so exporting gba_recomp_title_table, pack.json ("console": "gba", "title_key", "rom_sha256",
              "entry": "gba_recomp_title_table"), the same atomic publish as an Xbox pack under packs/gba-<KEY>/

With --android it is the device's (or a host with the NDK's) build for the app: linked with ld.lld against libmain.so
(--no-undefined: the core's symbols must be exported by the shell). Without it, a desktop pack for gba_play: built with
the host's cc (or --cc), its core symbols resolved from the executable that loads it; work and packs default to
$XDG_DATA_HOME/xrecomp/{work,packs} (beside gba_play's saves).
"""

import errno
import hashlib
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import traceback
import zlib

from tools.prepare import compile as cc
from tools.prepare import metrics
from tools.prepare import pack
from tools.prepare import toolkit
from tools.prepare.progress import Reporter

CONSOLE = "gba"
ENTRY = "gba_recomp_title_table"           # gba_recomp.h GBA_RECOMP_ENTRY_SYMBOL
ROM_MAX = 32 << 20
ROM_COPY = "rom.gba"
DISCOVERED = "discovered.txt"              # the runtime's (gba_runtime_opts.discovered_path)
SEEDS = "discovered-seeds.txt"             # what this Prepare handed to the lifter
# The lifter; overridable for the tests (a fake generator), and its package's directory added to PYTHONPATH.
RECOMP_MODULE_ENV, TOOLS_DIR_ENV = "XBA_GBA_RECOMP", "XBA_GBA_TOOLS_DIR"
DEFAULT_MODULE = "gba_tools.recomp"
FIELDS = ("format", "console", "title_key", "title", "rom_sha256", "abi", "shell_abi", "toolkit", "lib",
          "lib_sha256", "lib_size", "entry", "cc", "built_at")


def runtime_dir(root):
    return os.path.join(root, "consoles", "gba", "runtime")


def tools_dir(root):
    return os.path.join(root, "consoles", "gba", "tools")


# ── The ROM ─────────────────────────────────────────────────────

def header_ok(rom):
    """The cartridge header's fixed byte (0x96 at 0xB2) and complement check (0xBD), as gba_runtime_key."""
    if len(rom) < 0xC0 or rom[0xB2] != 0x96:
        return False
    s = 0
    for i in range(0xA0, 0xBD):
        s = (s - rom[i]) & 0xFF
    return (s - 0x19) & 0xFF == rom[0xBD]


def is_gba(path):
    """A .gba file, or any file whose header is a GBA cartridge's."""
    if not os.path.isfile(path):
        return False
    if path.lower().endswith(".gba"):
        return True
    try:
        with open(path, "rb") as f:
            return header_ok(f.read(0xC0))
    except OSError:
        return False


def title_key(rom):
    """« AXVE-1A2B3C4D »: game code ([A-Z0-9], else '_'; '____' for a bad header) + CRC32 of the whole image."""
    code = "____"
    if header_ok(rom):
        code = "".join(chr(b) if (0x41 <= b <= 0x5A or 0x30 <= b <= 0x39) else "_" for b in rom[0xAC:0xB0])
    return "%s-%08X" % (code, zlib.crc32(rom) & 0xFFFFFFFF)


def rom_title(rom, fallback):
    """The header's 12-character game title (printable ASCII), else fallback."""
    if len(rom) >= 0xAC:
        t = "".join(chr(b) for b in rom[0xA0:0xAC] if 0x20 <= b <= 0x7E).strip()
        if t:
            return t
    return fallback


def host_abi():
    """pack.json's "abi" for a desktop pack: what recomp_pack_host_abi says on this host."""
    m = platform.machine().lower()
    if m in ("aarch64", "arm64"):
        return "arm64-v8a"
    if m in ("x86_64", "amd64"):
        return "x86_64"
    if re.match(r"i[3-6]86\Z", m):
        return "x86"
    if m.startswith("arm"):
        return "armeabi-v7a"
    return "unknown"


def data_home():
    base = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")
    return os.path.join(base, "xrecomp")


# ── Discovered addresses ────────────────────────────────────────

def read_addresses(path):
    """The addresses of a discovered file (one hex address per line, 0x optional, bit 0 = Thumb; blank lines and
    # comments ignored), as a sorted list without repeats; [] when there is no file."""
    out = set()
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                try:
                    v = int(line, 16)
                except ValueError:
                    continue
                if 0 <= v <= 0xFFFFFFFF:
                    out.add(v)
    except OSError:
        pass
    return sorted(out)


def write_addresses(path, addrs):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        for a in addrs:
            f.write("0x%08X\n" % a)
    os.replace(tmp, path)


# ── The run ─────────────────────────────────────────────────────

class HostToolchain(cc.Toolchain):
    """The host's C compiler for a desktop pack (no sysroot, no ld.lld: `cc -shared`)."""

    def __init__(self, compiler, procs):
        cc.Toolchain.__init__(self, compiler, "", procs=procs)

    def short_version(self):
        v = self.version_text()
        m = re.search(r"clang version (\S+)", v)
        if m:
            return "clang %s" % m.group(1)
        m = re.search(r"\(.*\)\s+(\d+\.\S+)", v) or re.search(r"(\d+\.\d+\S*)", v)
        return "%s %s" % (os.path.basename(self.cc), m.group(1) if m else "?")


# Lifted GBA code (XBA-151, docs/android/prepare.md « GBA packs »): -O2 runs it no faster than -O1 and costs ~10 %
# more compile time, so -O2 / -O3 compile at -O1. One clang job peaks at ~150 MB: 256 MB per job and a 1 GB reserve
# (an Xbox title's files need 1 GB and 3 GB) give a phone with little free memory its cores all the same.
GBA_JOB_RSS = 256 << 20
GBA_RESERVED = 1 << 30


def gba_opt(opt):
    """The -O level lifted GBA code compiles at for a requested --opt."""
    return "1" if opt in ("2", "3") else opt


def lifted_cflags(opt):
    """The optimisation flags of lifted GBA code (also tools/consoles/gba_recomp_build.py's). No SLP vectorizer: there
    is nothing to vectorize, and it was the costliest pass on big functions (half the optimiser's time for aarch64).
    (clang's basic register allocator would save another 16 % of compile time, for 2-3 % of run time in tight loops:
    not taken.)"""
    return ["-O%s" % opt, "-fno-tree-slp-vectorize"]


def compile_flags(opt, root, gen, sysroot=None, include_dirs=()):
    """The generated C's flags: -I the GBA runtime (gba_recomp.h, gba_core.h and the lifted code's own header) and
    the generated directory; with a sysroot, the Android target (compile.py's)."""
    flags = []
    if sysroot:
        flags += ["--target=" + cc.TARGET, "--sysroot=" + sysroot]
        rd = cc.sysroot_resource_dir(sysroot)
        if rd:
            flags.append("-resource-dir=" + rd)
    flags += lifted_cflags(opt) + ["-g0", "-fPIC", "-std=gnu11", "-w", "-DNDEBUG"]
    for d in [gen, runtime_dir(root)] + list(include_dirs):
        flags.append("-I" + d)
    return flags


def host_link_args(objects, out, tc):
    args = tc.cc_argv + ["-shared", "-fPIC", "-o", out] + list(objects)
    if sys.platform == "darwin":
        args += ["-undefined", "dynamic_lookup"]
    else:
        args += ["-Wl,-Bsymbolic", "-Wl,-soname," + pack.LIB]
    return args


def make_meta(lib_path, key, title, rom_sha256, abi, shell_abi, toolkit_commit, toolkit_sha256, cc_text,
              discovered, now=None):
    import datetime
    now = now or datetime.datetime.now(datetime.timezone.utc)
    return {
        "format": pack.FORMAT,
        "console": CONSOLE,
        "title_key": key,
        "title": title,
        "rom_sha256": rom_sha256,
        "abi": abi,
        "shell_abi": int(shell_abi),
        "toolkit": toolkit_commit,
        "toolkit_sha256": toolkit_sha256,
        "lib": pack.LIB,
        "lib_sha256": pack.sha256_file(lib_path),
        "lib_size": os.path.getsize(lib_path),
        "entry": ENTRY,
        "cc": cc_text,
        "built_at": now.replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "discovered": int(discovered),
    }


def read(pack_dir):
    """A GBA pack.json as a dict, or None (no pack, not a whole one, not a GBA pack)."""
    try:
        with open(os.path.join(pack_dir, "pack.json")) as f:
            meta = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(meta, dict) or any(k not in meta for k in FIELDS) or meta.get("console") != CONSOLE:
        return None
    return meta


def check(pack_dir, key, rom_sha256, abi, shell_abi=pack.SHELL_ABI, verify_hash=True):
    """None when the GBA pack is loadable for this ROM (the runtime's checks, gba_pack.c), else why not."""
    meta = read(pack_dir)
    if meta is None:
        return "no pack.json"
    if meta["format"] != pack.FORMAT:
        return "format %r" % meta["format"]
    if meta["title_key"] != key:
        return "title_key %r" % meta["title_key"]
    if meta["rom_sha256"] != rom_sha256:
        return "built from another ROM"
    if meta["abi"] != abi:
        return "abi %r" % meta["abi"]
    if meta["shell_abi"] != shell_abi:
        return "shell_abi %r, this shell is %d" % (meta["shell_abi"], shell_abi)
    if meta["entry"] != ENTRY:
        return "entry %r" % meta["entry"]
    lib = os.path.join(pack_dir, meta["lib"])
    if "/" in meta["lib"] or not os.path.isfile(lib) or os.path.getsize(lib) != meta["lib_size"]:
        return "%s missing or of the wrong size" % meta["lib"]
    if verify_hash and pack.sha256_file(lib) != meta["lib_sha256"]:
        return "%s sha256 mismatch" % meta["lib"]
    return None


class Run:
    def __init__(self, a, host, android):
        self.a, self.m, self.android = a, host, android
        self.rep = Reporter()
        self.procs = cc.Procs()
        self.py = a.python or os.environ.get("XBA_PYTHON") or sys.executable
        self.source = os.path.abspath(a.source)
        self.steps = metrics.Steps()
        self.compile_stats = {}
        self.stamp = {}
        self.jobs = 0
        self.opt = None
        self.sources = []
        self.tools_peak_kb = 0
        self.work = None
        self.log = None

    def save_stamp(self, **kv):
        self.stamp.update(kv)
        pack.write_json_atomic(self.stamp_path, self.stamp)

    # ── 1 identify ──

    def identify(self):
        self.rep.progress(1, 0, "reading the ROM")
        try:
            size = os.path.getsize(self.source)
            if size == 0 or size > ROM_MAX:
                raise cc.StepError("BAD_ROM", "%s: %d bytes, a GBA ROM is 1 byte to 32 MB" % (self.source, size))
            with open(self.source, "rb") as f:
                rom = f.read()
        except OSError as e:
            raise cc.StepError("NO_ROM", "%s: %s" % (self.source, e.strerror or e))
        self.key = title_key(rom)
        self.name = "%s-%s" % (CONSOLE, self.key)
        self.rom_sha = hashlib.sha256(rom).hexdigest()
        self.title = rom_title(rom, os.path.splitext(os.path.basename(self.source))[0])
        a = self.a
        self.work = os.path.abspath(a.work or os.path.join(data_home(), "work", self.name))
        self.packs = os.path.abspath(a.pack_dir or os.path.join(data_home(), "packs"))
        try:
            os.makedirs(self.work, exist_ok=True)
        except OSError as e:
            raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "BAD_ARGS", "--work %s: %s" % (self.work, e))
        self.log = os.path.join(self.work, "prepare.log")
        self.stamp_path = os.path.join(self.work, "stamp.json")
        self.rom = os.path.join(self.work, ROM_COPY)
        if os.path.abspath(self.source) != self.rom:
            have = None
            if os.path.isfile(self.rom):
                have = pack.sha256_file(self.rom)
            if have != self.rom_sha:
                with open(self.rom + ".tmp", "wb") as f:
                    f.write(rom)
                os.replace(self.rom + ".tmp", self.rom)
        self.toolkit = self.m.toolkit_version()
        self.toolkit_sha = toolkit.digest()
        self.stamp = {} if a.force else self.m.read_stamp(self.stamp_path)
        if self.stamp.get("rom") != self.rom_sha or self.stamp.get("toolkit_sha256") != self.toolkit_sha:
            self.stamp = {}
            self.save_stamp(console=CONSOLE, rom=self.rom_sha, toolkit=self.toolkit, toolkit_sha256=self.toolkit_sha)
        self.steps.skipped = [k for k in ("lift", "link") if self.stamp.get(k)]
        self.rep.progress(1, 100, "%s (GBA, key %s, %d KB)" % (self.title, self.key, len(rom) // 1024))

    # ── 2 analyse: the discovered addresses ──

    def analyse(self):
        found = read_addresses(os.path.join(self.work, DISCOVERED))
        extra = getattr(self.a, "discovered", None)
        if extra:
            found = sorted(set(found) | set(read_addresses(extra)))
        seeds = os.path.join(self.work, SEEDS)
        if found:
            write_addresses(seeds, found)
        elif os.path.exists(seeds):
            os.remove(seeds)
        self.seeds = seeds if found else None
        self.discovered = len(found)
        self.disc_key = hashlib.sha256(("".join("%08X\n" % v for v in found)).encode()).hexdigest()
        self.rep.progress(2, 100, "%d address(es) found by earlier runs" % len(found) if found
                          else "no addresses from earlier runs")

    # ── 3 lift ──

    def lift(self):
        gen = os.path.join(self.work, "gen")
        key = "%s|%s" % (self.disc_key, os.environ.get(RECOMP_MODULE_ENV, DEFAULT_MODULE))
        if self.stamp.get("lift") == key and os.path.isdir(gen) and any(f.endswith(".c") for f in os.listdir(gen)):
            self.rep.progress(3, 100, "lift up to date (%d C files)" % len([f for f in os.listdir(gen)
                                                                            if f.endswith(".c")]))
            self.gen = gen
            return
        module = os.environ.get(RECOMP_MODULE_ENV, DEFAULT_MODULE)
        args = [self.py, "-m", module, self.rom, gen]
        if self.seeds:
            args += ["--discovered", self.seeds]
        env = dict(os.environ)
        paths = [p for p in (os.environ.get(TOOLS_DIR_ENV), tools_dir(self.m.ROOT), self.m.ROOT) if p]
        if env.get("PYTHONPATH"):
            paths.append(env["PYTHONPATH"])
        env["PYTHONPATH"] = os.pathsep.join(paths)
        files = self.run_lifter(module, args, env, gen, key, "lifting to C", stdout_to_log=True)
        self.rep.progress(3, 100, "%d C files" % len(files))

    def run_lifter(self, module, args, env, gen, key, note, stdout_to_log):
        """The lifter into gen/ (emptied first), the files it has finished compiled meanwhile (cc.EarlyCompile:
        compile() adopts them). The lift stamp `key` is dropped first and saved only once the lifter succeeded, so a
        half-written gen/ never passes as a finished lift (XBA-138). The C files written; StepError LIFT_FAILED or
        Cancelled, every early compile stopped."""
        if "lift" in self.stamp:
            self.stamp.pop("lift")
            self.save_stamp()
        # In place, not in gen.new: an object's key and bytes hold its source's path (compile.py cache_key), so the
        # early compiles must see the path compile() will.
        shutil.rmtree(gen + ".new", ignore_errors=True)      # an older toolkit's
        shutil.rmtree(gen, ignore_errors=True)
        os.makedirs(gen)
        self.gen = gen
        tc, flags, jobs, limit = self.compile_setup()
        self.tools_peak_kb = metrics.children_peak_kb()     # the tools so far, before any early compile is reaped
        early = cc.EarlyCompile(gen, flags, tc, os.path.join(self.work, "obj"), jobs, limit=limit, log=self.log,
                                job_rss=self.job_rss, reserved=self.reserved).start()
        self.rep.progress(3, 2, note)
        rc = None
        try:
            with open(self.log, "a") as f:
                f.write("\n$ %s\n" % " ".join(args))
                f.flush()
                p = self.procs.start(args, cwd=self.m.ROOT, env=env, stdout=f if stdout_to_log else subprocess.DEVNULL,
                                     stderr=subprocess.STDOUT if stdout_to_log else f, stdin=subprocess.DEVNULL)
                shown = [0]

                def tick():
                    n = early.count()
                    if n != shown[0]:
                        shown[0] = n
                        self.rep.progress(3, 2, "%s, %d file(s) compiled meanwhile" % (note, n))
                try:
                    peak = cc.wait_ticking(p, tick, 1.0)
                    rc = p.returncode
                    self.lift_peak_kb = peak
                finally:
                    if rc is None:          # an exception (a cancel, a bug) while it ran: it does not outlive us
                        try:
                            p.kill()
                            p.wait()
                        except OSError:
                            pass
                    self.procs.done(p)
            if self.procs.cancelled.is_set():
                raise cc.Cancelled()
            files = [fn for fn in os.listdir(gen) if fn.endswith(".c")]
            if rc != 0 or not files:
                raise cc.StepError("LIFT_FAILED", "%s failed (exit %d%s), see %s"
                                   % (module, rc, ", no C written" if rc == 0 else "", self.log))
        except BaseException:
            early.abort()
            raise
        early.finish()
        self.early = early
        self.save_stamp(lift=key)
        return files

    # ── 4 compile ──

    job_rss, reserved = GBA_JOB_RSS, GBA_RESERVED

    def lifted_opt(self):
        return gba_opt(self.a.opt)

    def flags_for(self, opt, sysroot):
        return compile_flags(opt, self.m.ROOT, self.gen, sysroot, self.a.include_dir)

    def compile_setup(self):
        """(toolchain, flags, jobs, limit) of the compile, made once: the lift's early compiles use the same."""
        if getattr(self, "_setup", None):
            return self._setup
        a = self.a
        self.opt = opt = self.lifted_opt()
        if self.android:
            self.tc = cc.Toolchain(a.cc, os.path.abspath(a.sysroot), cc_prefix=a.cc_prefix_arg, ld=a.ld,
                                   ld_prefix=a.ld_prefix_arg, builtins=a.builtins, procs=self.procs)
            flags = self.flags_for(opt, self.tc.sysroot)
        else:
            self.tc = HostToolchain(a.cc or os.environ.get("CC") or "cc", self.procs)
            flags = self.flags_for(opt, None)
        jobs = a.jobs or cc.default_jobs(cc.mem_available(), os.cpu_count(), self.job_rss, self.reserved)
        device_log = getattr(a, "device_log", None)
        limit = (lambda: metrics.jobs_now(device_log, jobs)) if device_log else None
        self._setup = (self.tc, flags, jobs, limit)
        return self._setup

    def compile(self):
        a = self.a
        tc, flags, jobs, limit = self.compile_setup()
        opt = self.opt
        sources = [os.path.join(self.gen, fn) for fn in sorted(os.listdir(self.gen)) if fn.endswith(".c")]
        sources += [os.path.abspath(s) for s in a.extra_src]
        self.jobs, self.sources = jobs, sources
        self.tools_peak_kb = self.tools_peak()
        self.rep.progress(4, 0, "compiling %d files, %d jobs, -O%s" % (len(sources), jobs, opt))

        def prog(done, total, now, eta):
            self.rep.progress(4, done * 100 // max(1, total), "%d/%d files (%d compiled)" % (done, total, now), eta)
        self.objects, n = cc.compile_all(sources, flags, tc, os.path.join(self.work, "obj"), jobs,
                                         log=self.log, on_progress=prog, limit=limit, stats=self.compile_stats,
                                         early=getattr(self, "early", None))
        self.early = None
        self.rep.progress(4, 100, "%d files, %d compiled%s" % (len(sources), n, self.early_note()))

    def tools_peak(self):
        """The pipeline tools' peak RSS (pack.json tools_peak_mb): the children reaped before the lift, and the
        lifter's own (the early compiles are the compile's, not the tools')."""
        if getattr(self, "lift_peak_kb", None) is None:
            return metrics.children_peak_kb()
        return max(self.tools_peak_kb or 0, self.lift_peak_kb)

    def early_note(self):
        e = self.compile_stats.get("early")
        return ", %d of them while lifting" % e if e else ""

    # ── 5 pack ──

    def link(self, out):
        if self.android:
            cc.link(self.objects, out, self.tc, self.a.shell_lib, log=self.log)
            return
        args = host_link_args(self.objects, out, self.tc)
        rc, text = self.procs.run(args)
        with open(self.log, "a") as f:
            f.write("\n$ %s\n%s" % (" ".join(args[:30]) + " ...", text))
        if rc != 0:
            raise cc.StepError("NO_SPACE" if "No space left on device" in text else "LINK_FAILED",
                               cc._first_error(text))

    def make_pack(self):
        a = self.a
        abi = pack.ABI if self.android else host_abi()
        os.makedirs(self.packs, exist_ok=True)
        self.rep.progress(5, 0, "linking libtitle.so")
        h = hashlib.sha256()
        for o in self.objects:
            h.update(os.path.basename(o).encode() + b"\n")
        if self.android:
            h.update(pack.sha256_file(a.shell_lib).encode())
        h.update(("|%s|%s|%s|%s|%d" % (a.shell_abi, self.toolkit_sha, abi, self.rom_sha, self.discovered)).encode())
        key = h.hexdigest()
        pack.recover(self.packs, self.name)
        final = pack.final_dir(self.packs, self.name)
        if self.stamp.get("link") == key and check(final, self.key, self.rom_sha, abi, a.shell_abi) is None:
            self.rep.progress(5, 100, "pack up to date")
        else:
            try:
                part = pack.begin(self.packs, self.name)
            except OSError as e:
                raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "PACK_FAILED", str(e))
            lib = os.path.join(part, pack.LIB)
            self.link(lib)
            self.rep.progress(5, 70, "writing pack.json")
            meta = make_meta(lib, self.key, self.title, self.rom_sha, abi, a.shell_abi, self.toolkit,
                             self.toolkit_sha, "%s -O%s" % (self.tc.short_version(), self.opt), self.discovered)
            meta["prepare"] = metrics.summary(self.steps, self.compile_stats, self.sources, self.objects, self.jobs,
                                              self.opt, part, getattr(a, "device_log", None), self.tools_peak_kb)
            try:
                pack.publish(self.packs, self.name, meta)
            except OSError as e:
                raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "PACK_FAILED", str(e))
            self.save_stamp(link=key)
            self.rep.progress(5, 100, "pack %s, %d bytes" % (self.name, meta["lib_size"]))
        cc.prune(os.path.join(self.work, "obj"), self.objects)
        return final


def run(a, host, android, run_class=None):
    """The five steps; run_class: Run or a subclass (tools/prepare/ps1.py's)."""
    rep = Reporter()
    if android:
        from tools.prepare.android import REQUIRED
        missing = [k for k in REQUIRED if not getattr(a, k)]
        if missing:
            rep.error("BAD_ARGS", "--android needs " + ", ".join("--" + k.replace("_", "-") for k in missing))
            return 1
    r = (run_class or Run)(a, host, android)
    r.rep = rep

    def on_signal(signum, frame):
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        r.procs.cancel()
        raise cc.Cancelled("signal %d" % signum)
    old = signal.signal(signal.SIGINT, on_signal), signal.signal(signal.SIGTERM, on_signal)
    try:
        st = r.steps
        st.run("identify", r.identify)
        st.run("analyse", r.analyse)
        st.run("lift", r.lift)
        st.run("compile", r.compile)
        final = st.run("pack", r.make_pack)
        rep.ok(final)
        return 0
    except cc.Cancelled as e:
        rep.error("CANCELLED", "cancelled (%s)" % (e.args[0] if e.args else "signal"))
        return 130
    except KeyboardInterrupt:
        rep.error("CANCELLED", "cancelled")
        return 130
    except cc.StepError as e:
        rep.error(e.code, e.msg)
        return 1
    except OSError as e:
        _log_exc(r)
        rep.error("NO_SPACE" if e.errno == errno.ENOSPC else "INTERNAL", str(e))
        return 1
    except Exception as e:      # a bug: the traceback goes to the log
        _log_exc(r)
        rep.error("INTERNAL", "%s: %s (see %s)" % (type(e).__name__, e, r.log))
        return 1
    finally:
        signal.signal(signal.SIGINT, old[0])
        signal.signal(signal.SIGTERM, old[1])


def _log_exc(r):
    if not r.log:
        traceback.print_exc()
        return
    try:
        with open(r.log, "a") as f:
            traceback.print_exc(file=f)
    except OSError:
        pass
