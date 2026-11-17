"""
`python -m tools.prepare GAME.iso`: a GameCube title pack (XBA-264; docs/android/prepare.md « GameCube packs »,
consoles/gc/runtime/gc_pack.h and gc_recomp.h).

The same five steps and progress lines as a PS1 pack (tools/prepare/ps1.py, whose Run this is), for a GameCube disc
or a homebrew DOL:

  1 identify  the title key (game code + maker + "-" + revision, « GALE01-0 », "-D2" from a set's second disc, as
              gc_disc_key_from_header; a DOL: « DOL-<CRC32 of the file> », as gc_runtime_key), the main DOL (read
              through the disc's header) copied to <work>/main.dol, its SHA-256 the source identity, and every REL
              module of the FST (*.rel) copied to <work>/rel/. Images: ISO / GCM (also the sparse GCM the app or
              `gc_run --extract` writes), CISO, GCZ read here (consoles/gc/tools/gc_tools/gcm.py); RVZ / WIA through
              the runtime's reader ($XBA_GC_RUN or gc_run on PATH: `gc_run --extract`), else BAD_DISC with how
  2 analyse   what earlier runs found (<work>/discovered.txt, written by the runtime at exit, at every pause and every
              few minutes: gc_recomp_write_discovered): the addresses the interpreter had to start at, and the REL
              modules it saw loaded (`REL <id> <base> <bss> <hash> <co-loaded...>`), merged without repeats into
              <work>/discovered-seeds.txt
  3 lift      `python -m gc_tools.recomp <work>/main.dol <work>/gen --discovered seeds --rel <work>/rel/...`
              (consoles/gc/tools): the DOL's text, and each recorded REL module relocated at its load address when
              its bytes match the run's (gc_tools/rel.py « per load address »)
  4 compile   every generated C file with -I consoles/gc/runtime -ffp-contract=off -frounding-math (as the core: fmul
              then fadd round twice; guest code runs under the rounding mode FPSCR[RN] asks for), by the Xbox pack's
              compile driver (compile.py: the on-device clang multicall and sysroot with --android, the host's cc
              otherwise), object cache included
  5 pack      libtitle.so exporting gc_recomp_title_table, pack.json ("console": "gc", "title_key", "source_sha256",
              "entry": "gc_recomp_title_table", "discovered", "rels"), the same atomic publish under packs/gc-<KEY>/;
              then the title's GX shader list (<work>/gx_shaders.txt, XBA-262 layer 2) copied next to it

The source identity is the main DOL's SHA-256, not the image's: a 1.4 GB image would take tens of seconds to hash on a
phone at every start, the DOL is a few MB, and it is what the table itself records (gc_recomp_table.dol_sha256). The
same disc as ISO, RVZ, CISO or GCZ has the same identity. REL modules and code copied to RAM are not part of it: the
table's spans are content-checked in memory (gc_recomp.h), so code the disc changed never runs from the pack.

A GameCube game always starts in the interpreter without a pack (PACK_NONE) or with a stale one (PACK_STALE); each run
grows discovered.txt, and the next Prepare lifts what it found (the discovery rounds).
"""

import errno
import hashlib
import os
import shutil
import subprocess
import sys
import zlib

from tools.prepare import compile as cc
from tools.prepare import gba
from tools.prepare import pack
from tools.prepare import ps1
from tools.prepare import toolkit

CONSOLE = "gc"
ENTRY = "gc_recomp_title_table"            # gc_recomp.h GC_RECOMP_ENTRY_SYMBOL
DOL_MAX = 16 << 20                         # gc_runtime.c GC_DOL_MAX
DOL_COPY = "main.dol"
REL_DIR = "rel"
DISCOVERED = "discovered.txt"              # the runtime's (gc_runtime_set_work_dir)
SEEDS = "discovered-seeds.txt"             # what this Prepare handed to the lifter
SHADERS = "gx_shaders.txt"                 # the runtime's GX shader list (XBA-262), copied into the pack
DISCOVERED_MAX = 65536                     # addresses kept (gc_rel.c GC_DISCOVERED_MAX)
RECORDS_MAX = 4096                         # REL records kept (gc_rel.c GC_REL_RECORDS_MAX)
DISC_EXT = (".iso", ".gcm", ".ciso", ".gcz", ".rvz", ".wia")
# The lifter; overridable for the tests (a fake generator), and its package's directory added to PYTHONPATH.
RECOMP_MODULE_ENV, TOOLS_DIR_ENV = "XBA_GC_RECOMP", "XBA_GC_TOOLS_DIR"
DEFAULT_MODULE = "gc_tools.recomp"
GC_RUN_ENV = "XBA_GC_RUN"
FIELDS = ps1.FIELDS


def runtime_dir(root):
    return os.path.join(root, "consoles", "gc", "runtime")


def tools_dir(root):
    return os.path.join(root, "consoles", "gc", "tools")


def _gc_tools():
    """consoles/gc/tools/gc_tools: the lifter's own readers, so what is hashed here is what is lifted."""
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    d = tools_dir(root)
    if d not in sys.path:
        sys.path.insert(0, d)
    import gc_tools.dol
    import gc_tools.gcm
    return gc_tools


# ── The source ──────────────────────────────────────────────────

def is_dol(path):
    """A DOL by its header: sections inside the file and in MEM1, the entry in a text section (gc_create's checks)."""
    try:
        if os.path.getsize(path) > DOL_MAX:
            return False
        with open(path, "rb") as f:
            raw = f.read(DOL_MAX + 1)
        _gc_tools().dol.Image(raw)
        return True
    except Exception:
        return False


def is_gc(path):
    """A GameCube disc image (by content) or a DOL."""
    if not os.path.isfile(path):
        return False
    return _gc_tools().gcm.is_disc(path) or is_dol(path)


def dol_key(data):
    """gc_runtime_key: « DOL-<CRC32 of the whole file> »."""
    return "DOL-%08X" % (zlib.crc32(data) & 0xFFFFFFFF)


def _native_extract(path, out):
    """An RVZ / WIA image through the runtime's reader: `gc_run --extract out path`. The sparse GCM, else ValueError."""
    exe = os.environ.get(GC_RUN_ENV) or shutil.which("gc_run")
    if not exe:
        raise ValueError("an RVZ / WIA image needs the runtime's reader: set %s to a gc_run, or convert the image "
                         "to ISO" % GC_RUN_ENV)
    r = subprocess.run([exe, "--extract", out, path], capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        raise ValueError("gc_run --extract: %s" % (r.stderr.strip() or "exit %d" % r.returncode))
    return out


def read_source(path, work=None):
    """(key, title, main DOL bytes, {REL path: bytes}) of a disc image or a DOL. ValueError with why when it is not
    one."""
    g = _gc_tools()
    if not g.gcm.is_disc(path):
        with open(path, "rb") as f:
            data = f.read(DOL_MAX + 1)
        if len(data) > DOL_MAX:
            raise ValueError("a DOL of over 16 MB")
        try:
            g.dol.Image(data)
        except g.dol.DolError as e:
            raise ValueError("not a DOL: %s" % e)
        base = os.path.basename(path)
        return dol_key(data), os.path.splitext(base)[0] or base, data, {}
    src = path
    if g.gcm.needs_native(path):
        if not work:
            raise ValueError("an RVZ / WIA image needs a work folder")
        src = _native_extract(path, os.path.join(work, "disc.gcm"))
    try:
        d = g.gcm.Disc(src)
    except g.gcm.DiscError as e:
        raise ValueError(str(e))
    try:
        dol = d.dol()
        g.dol.Image(dol)
        rels = dict(d.rels())
        title = d.name or os.path.splitext(os.path.basename(path))[0]
        return d.key, title, dol, rels
    except (g.gcm.DiscError, g.dol.DolError) as e:
        raise ValueError(str(e))
    finally:
        d.close()


# ── Discovered code ─────────────────────────────────────────────

def read_discovered(path):
    """(set of addresses, [REL record lines]) of a discovered file; empty when there is none."""
    addrs, recs = set(), []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.split("#", 1)[0].strip()
                if not s:
                    continue
                if s.startswith("REL "):
                    t = " ".join(s.split())
                    if t not in recs and len(recs) < RECORDS_MAX:
                        recs.append(t)
                    continue
                try:
                    a = int(s.split()[0], 16)
                except ValueError:
                    continue
                if 0 <= a <= 0xFFFFFFFF and not a & 3 and len(addrs) < DISCOVERED_MAX:
                    addrs.add(a)
    except OSError:
        pass
    return addrs, recs


def merged_lines(*paths):
    addrs, recs = set(), []
    for p in paths:
        if not p:
            continue
        a, r = read_discovered(p)
        addrs |= a
        recs += [x for x in r if x not in recs]
    return ["%08X" % a for a in sorted(addrs)][:DISCOVERED_MAX] + recs[:RECORDS_MAX]


# ── pack.json ───────────────────────────────────────────────────

def make_meta(lib_path, key, title, source_sha256, boot, abi, shell_abi, toolkit_commit, toolkit_sha256, cc_text,
              discovered, now=None):
    meta = ps1.make_meta(lib_path, key, title, source_sha256, boot, abi, shell_abi, toolkit_commit, toolkit_sha256,
                         cc_text, discovered, now)
    meta["console"] = CONSOLE
    meta["entry"] = ENTRY
    return meta


def read(pack_dir):
    """A GameCube pack.json as a dict, or None (no pack, not a whole one, not a GameCube pack)."""
    import json
    try:
        with open(os.path.join(pack_dir, "pack.json")) as f:
            meta = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(meta, dict) or any(k not in meta for k in FIELDS) or meta.get("console") != CONSOLE:
        return None
    return meta


def check(pack_dir, key, source_sha256, abi, shell_abi=pack.SHELL_ABI, verify_hash=True):
    """None when the GameCube pack is loadable for this game (the runtime's checks, gc_pack.c), else why not."""
    meta = read(pack_dir)
    if meta is None:
        return "no pack.json"
    if meta["format"] != pack.FORMAT:
        return "format %r" % meta["format"]
    if meta["title_key"] != key:
        return "title_key %r" % meta["title_key"]
    if meta["source_sha256"] != source_sha256:
        return "built from another main DOL"
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


# Lifted GameCube code compiles at -O2 at most (a heavier console than the PS1: the run time matters more than the
# compile time); -O3 compiles at -O2. A generated file holds 150 functions; one clang job peaks a little above a PS1 one.
JOB_RSS = 384 << 20
RESERVED = 1 << 30


def lifted_opt(opt):
    """The -O level lifted GameCube code compiles at for a requested --opt."""
    return "2" if opt == "3" else opt


def lifted_cflags(opt):
    """The optimisation and floating-point flags of lifted GameCube code (gc_core's: no contraction, the rounding mode
    honoured), no SLP vectorizer (as GBA / PS1 code)."""
    return gba.lifted_cflags(opt) + ["-ffp-contract=off", "-frounding-math"]


def compile_flags(opt, root, gen, sysroot=None, include_dirs=()):
    """The generated C's flags: -I the GameCube runtime (gc_recomp.h, gc_recomp_rt.h, ppc_ops.h) and the generated
    directory; with a sysroot, the Android target (compile.py's)."""
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


# ── The run: the PS1's (ps1.Run), with a GameCube disc ──

class Run(ps1.Run):
    cm = sys.modules[__name__]

    # ── 1 identify ──

    def identify(self):
        self.rep.progress(1, 0, "reading the disc")
        a = self.a
        early_work = os.path.abspath(a.work or os.path.join(gba.data_home(), "work", "gc-extract"))
        try:
            os.makedirs(early_work, exist_ok=True)
            self.key, self.title, dol, rels = read_source(self.source, early_work)
        except (FileNotFoundError, PermissionError) as e:
            raise cc.StepError("NO_DISC", "%s: %s" % (self.source, e.strerror or e))
        except subprocess.TimeoutExpired:
            raise cc.StepError("BAD_DISC", "%s: gc_run --extract took too long" % self.source)
        except (OSError, ValueError) as e:
            raise cc.StepError("BAD_DISC", "%s: %s" % (self.source, getattr(e, "strerror", None) or e))
        self.name = "%s-%s" % (CONSOLE, self.key)
        self.rom_sha = hashlib.sha256(dol).hexdigest()       # the source identity (the main DOL's)
        self.boot = DOL_COPY
        self.work = os.path.abspath(a.work or os.path.join(gba.data_home(), "work", self.name))
        self.packs = os.path.abspath(a.pack_dir or os.path.join(gba.data_home(), "packs"))
        try:
            os.makedirs(os.path.join(self.work, REL_DIR), exist_ok=True)
        except OSError as e:
            raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "BAD_ARGS", "--work %s: %s" % (self.work, e))
        self.log = os.path.join(self.work, "prepare.log")
        self.stamp_path = os.path.join(self.work, "stamp.json")
        self.rom = os.path.join(self.work, DOL_COPY)
        _write_if_changed(self.rom, dol)
        # the REL modules, by a flat name (a path's '/' as '_'): what the lifter relocates
        rel_dir = os.path.join(self.work, REL_DIR)
        want = {}
        for p, data in rels.items():
            want[p.replace("/", "_")] = data
        for old in os.listdir(rel_dir):
            if old not in want:
                os.remove(os.path.join(rel_dir, old))
        h = hashlib.sha256()
        for n in sorted(want):
            _write_if_changed(os.path.join(rel_dir, n), want[n])
            h.update(n.encode() + b"\0" + hashlib.sha256(want[n]).digest())
        self.rels = [os.path.join(rel_dir, n) for n in sorted(want)]
        self.rels_sha = h.hexdigest()
        self.toolkit = self.m.toolkit_version()
        self.toolkit_sha = toolkit.digest()
        self.stamp = {} if a.force else self.m.read_stamp(self.stamp_path)
        if self.stamp.get("source") != self.rom_sha or self.stamp.get("toolkit_sha256") != self.toolkit_sha \
                or self.stamp.get("rels") != self.rels_sha:
            self.stamp = {}
            self.save_stamp(console=CONSOLE, source=self.rom_sha, toolkit=self.toolkit,
                            toolkit_sha256=self.toolkit_sha, rels=self.rels_sha)
        self.steps.skipped = [k for k in ("lift", "link") if self.stamp.get(k)]
        self.rep.progress(1, 100, "%s (GameCube, key %s, main DOL %d KB, %d REL module(s))"
                          % (self.title, self.key, len(dol) // 1024, len(self.rels)))

    # ── 2 analyse: the code earlier runs found ──

    def analyse(self):
        lines = merged_lines(os.path.join(self.work, DISCOVERED), getattr(self.a, "discovered", None))
        seeds = os.path.join(self.work, SEEDS)
        if lines:
            ps1.write_records(seeds, lines)
        elif os.path.exists(seeds):
            os.remove(seeds)
        self.seeds = seeds if lines else None
        self.discovered = len(lines)
        nrec = sum(1 for x in lines if x.startswith("REL "))
        self.disc_key = hashlib.sha256(("".join(x + "\n" for x in lines) + self.rels_sha).encode()).hexdigest()
        self.rep.progress(2, 100, "%d address(es) and %d REL module load(s) found by earlier runs"
                          % (len(lines) - nrec, nrec) if lines else "no code from earlier runs")

    # ── 3 lift: ps1.Run's, with the REL modules ──

    def lift_args(self, module, out):
        args = [self.py, "-m", module, self.rom, out, "--no-comments", "--report", os.path.join(out, "report.json")]
        if self.seeds:
            args += ["--discovered", self.seeds]
        for r in self.rels:
            args += ["--rel", r]
        return args

    def lift(self):
        gen = os.path.join(self.work, "gen")
        module = os.environ.get(RECOMP_MODULE_ENV, DEFAULT_MODULE)
        key = "%s|%s" % (self.disc_key, module)
        if self.stamp.get("lift") == key and os.path.isdir(gen) and any(f.endswith(".c") for f in os.listdir(gen)):
            self.rep.progress(3, 100, "lift up to date (%d C files)" % len([f for f in os.listdir(gen)
                                                                            if f.endswith(".c")]))
            self.gen = gen
            return
        if "lift" in self.stamp:            # a half-written gen/ never passes as a finished lift (XBA-138)
            self.stamp.pop("lift")
            self.save_stamp()
        new = gen + ".new"
        shutil.rmtree(new, ignore_errors=True)
        os.makedirs(new)
        args = self.lift_args(module, new)
        env = dict(os.environ)
        paths = [p for p in (os.environ.get(TOOLS_DIR_ENV), tools_dir(self.m.ROOT), self.m.ROOT) if p]
        if env.get("PYTHONPATH"):
            paths.append(env["PYTHONPATH"])
        env["PYTHONPATH"] = os.pathsep.join(paths)
        self.rep.progress(3, 2, "lifting to C (main DOL, %d REL module(s) on the disc)" % len(self.rels))
        with open(self.log, "a") as f:
            f.write("\n$ %s\n" % " ".join(args))
            f.flush()
            p = self.procs.start(args, cwd=self.m.ROOT, env=env, stdout=subprocess.DEVNULL, stderr=f,
                                 stdin=subprocess.DEVNULL)
            try:
                rc = p.wait()
            finally:
                self.procs.done(p)
        if self.procs.cancelled.is_set():
            raise cc.Cancelled()
        files = [fn for fn in os.listdir(new) if fn.endswith(".c")]
        if rc != 0 or not files:
            raise cc.StepError("LIFT_FAILED", "%s failed (exit %d%s), see %s"
                               % (module, rc, ", no C written" if rc == 0 else "", self.log))
        shutil.rmtree(gen, ignore_errors=True)
        os.rename(new, gen)
        self.save_stamp(lift=key)
        self.gen = gen
        self.rep.progress(3, 100, "%d C files%s" % (len(files), self._lift_summary(gen)))

    def _lift_summary(self, gen):
        import json
        try:
            with open(os.path.join(gen, "report.json")) as f:
                s = json.load(f)
        except (OSError, ValueError):
            return ""
        with open(self.log, "a") as f:
            for why in s.get("rel_skipped", []):
                f.write("REL not lifted: %s\n" % why)
        return ", %d functions, %d REL module load(s) lifted, %d not" % (
            s.get("functions", 0) + sum(r.get("functions", 0) for r in s.get("rels", [])),
            len(s.get("rels", [])), len(s.get("rel_skipped", [])))

    # ── 5 pack: ps1.Run's, then the shader list next to it ──

    def make_pack(self):
        final = super().make_pack()
        src = os.path.join(self.work, SHADERS)
        if os.path.isfile(src) and os.path.isdir(final):
            tmp = os.path.join(final, SHADERS + ".tmp")
            shutil.copyfile(src, tmp)
            os.replace(tmp, os.path.join(final, SHADERS))
        return final


def _write_if_changed(path, data):
    if os.path.isfile(path) and os.path.getsize(path) == len(data):
        with open(path, "rb") as f:
            if f.read() == data:
                return
    with open(path + ".tmp", "wb") as f:
        f.write(data)
    os.replace(path + ".tmp", path)


def run(a, host, android):
    """gba.run with this module's Run."""
    return gba.run(a, host, android, run_class=Run)
