"""
`python -m tools.prepare SOURCE --android ...`: prepare a title pack for the
Android app, on the device (embedded CPython + clang/lld) or on a host with
the NDK. docs/android/prepare.md is the contract with the app.

Steps: 1 identify, 2 analyse, 3 lift, 4 compile, 5 pack. Every sub-step
leaves a stamp in <work>/stamp.json (and every object its cache entry), so a
run that is killed starts again where it stopped.
"""

import errno
import hashlib
import json
import math
import os
import shutil
import signal
import sys
import traceback

from tools.prepare import compile as cc
from tools.prepare import pack
from tools.prepare.progress import Reporter

REQUIRED = ("work", "pack_dir", "cc", "sysroot", "shell_lib")


class Run:
    def __init__(self, a, host):
        self.a = a
        self.m = host                 # tools/prepare/__main__.py: find_xbe, assemble_project, ...
        self.rep = Reporter()
        self.procs = cc.Procs()
        self.py = a.python or os.environ.get("XBA_PYTHON") or sys.executable
        self.work = os.path.abspath(a.work)
        self.log = os.path.join(self.work, "prepare.log")
        self.stamp_path = os.path.join(self.work, "stamp.json")
        self.stamp = {}
        self.env_force = {}
        self.layers = []

    # ── helpers ──

    def save_stamp(self, **kv):
        self.stamp.update(kv)
        pack.write_json_atomic(self.stamp_path, self.stamp)

    def py_step(self, args, step, lo, hi, msg, expect, code):
        """A pipeline tool as `python -m ...`, its progress a time-based guess
        from lo to just under hi (the tools print no counts)."""
        def tick(t):
            frac = 1.0 - math.exp(-t / max(expect, 1.0))
            self.rep.progress(step, min(hi - 1, lo + (hi - lo) * frac), msg)
        self.rep.progress(step, lo, msg)
        rc, secs = self.procs.run_logged([self.py, "-m"] + args, self.log, cwd=self.m.ROOT, on_tick=tick)
        if rc != 0:
            raise cc.StepError(code, "%s failed (exit %d), see %s" % (args[0], rc, self.log))
        self.rep.progress(step, hi, "%s done in %.0f s" % (args[0].split(".")[-1], secs))
        return secs

    # ── the steps ──

    def identify(self):
        self.rep.progress(1, 0, "finding default.xbe")
        try:
            xbe, _, _ = self.m.find_xbe(self.a.source, self.work)
        except SystemExit as e:
            raise cc.StepError("NO_XBE", str(e))
        # Our own copy, next to its analysis JSON (the tools look for
        # <stem>_analysis.json beside the XBE): nothing is written into the game folder.
        mine = os.path.join(self.work, "xbe", "default.xbe")
        os.makedirs(os.path.dirname(mine), exist_ok=True)
        h = self.m.sha256(xbe)
        if os.path.abspath(xbe) != mine and not (os.path.isfile(mine) and self.m.sha256(mine) == h):
            shutil.copyfile(xbe, mine + ".tmp")
            os.replace(mine + ".tmp", mine)
        self.xbe = mine
        self.xbe_sha = h
        self.toolkit = self.m.toolkit_version()
        self.stamp = {} if self.a.force else self.m.read_stamp(self.stamp_path)
        if self.stamp.get("xbe") != h or self.stamp.get("toolkit") != self.toolkit:
            self.stamp = {}
            self.save_stamp(xbe=h, toolkit=self.toolkit)
        self.xjson = os.path.join(self.work, "xbe", "default_analysis.json")
        if not (self.stamp.get("parse") and os.path.isfile(self.xjson)):
            rc, _ = self.procs.run_logged([self.py, "-m", "tools.xbe_parser", mine, "--json", self.xjson],
                                          self.log, cwd=self.m.ROOT)
            if rc != 0:
                raise cc.StepError("BAD_XBE", "xbe_parser failed (exit %d), see %s" % (rc, self.log))
            self.save_stamp(parse=True)
        with open(self.xjson) as f:
            self.info = json.load(f)
        self.tid = self.info["title_id"][2:].upper()
        self.ident = self.m.identify_xbe(self.xbe, self.work)
        try:
            self.profile = self.m.load_profile(self.tid, self.ident)
            self.env_force = pack.env_force_of(self.profile)
        except ValueError as e:
            raise cc.StepError("BAD_PROFILE", "profiles: %s" % e)
        self.layers = self.profile["keys"] if self.profile else []
        self.rep.progress(1, 100, "%s (title ID %s)%s%s" % (self.info.get("title", "?"), self.tid,
                                                           ", title profile" if self.profile and self.profile["title"]
                                                           else "",
                                                           ", " + self.ident["summary"] if self.ident else ""))

    def analyse(self):
        an = os.path.join(self.work, "analysis")
        mb = max(1.0, os.path.getsize(self.xbe) / 1e6)
        d, fid = os.path.join(an, "disasm"), os.path.join(an, "func_id")
        disasm = ["tools.disasm", self.xbe, "-o", d, "--force"]
        # Function starts the analysis cannot find (the layers' "seed_functions").
        for fn in (self.profile or {}).get("seed_functions", []):
            disasm += ["--seed-functions", fn]
        # And what earlier runs of the title found (discovered.jsonl, XBA-97):
        # new seeds redo the analysis from disasm on.
        from tools.prepare import discovered
        disc = None
        try:
            disc = discovered.refresh(self.work, self.xbe, os.path.join(d, "functions.json"),
                                      self.xjson, log=lambda m: self.rep.progress(2, 0, m.strip()))
        except Exception as e:
            self.rep.progress(2, 0, "discovered.jsonl not used: %s" % e)
        if disc:
            disasm += ["--seed-functions", disc]
            key = discovered.digest(disc)
            if self.stamp.get("discovered") != key:
                for name in ("disasm", "func_id", "abi", "lift"):
                    self.stamp.pop(name, None)
                self.save_stamp(discovered=key)
        steps = (
            ("disasm", 0, 55, 20 * mb, disasm),
            ("func_id", 55, 70, 5 * mb, ["tools.func_id", self.xbe,
                                         "--functions", os.path.join(d, "functions.json"),
                                         "--strings", os.path.join(d, "strings.json"),
                                         "--xrefs", os.path.join(d, "xrefs.json"), "-o", fid]),
            ("abi", 70, 100, 10 * mb, ["tools.abi_analysis", self.xbe, "--disasm-dir", d,
                                       "--func-id-dir", fid, "--output-dir", os.path.join(an, "abi")]),
        )
        for i, (name, lo, hi, expect, args) in enumerate(steps):
            if self.stamp.get(name):
                self.rep.progress(2, hi, "%s up to date" % name)
                continue
            # A redone sub-step invalidates the ones after it.
            for later in [s[0] for s in steps[i + 1:]] + ["lift"]:
                self.stamp.pop(later, None)
            self.py_step(args, 2, lo, hi, "analysing: %s" % name, expect, "ANALYSE_FAILED")
            self.save_stamp(**{name: True})
        self.an = an
        self.rep.progress(2, 100, "analysis done")

    def lift(self):
        self.rep.progress(3, 0, "assembling the title project")
        proj = os.path.join(self.work, "project")
        self.m.assemble_project(proj, self.profile, self.info)
        extra = os.path.join(proj, "src", "extra")
        shutil.rmtree(extra, ignore_errors=True)
        if self.a.extra_src:
            os.makedirs(extra)
            for s in self.a.extra_src:
                shutil.copy(s, os.path.join(extra, os.path.basename(s)))
        gen = os.path.join(proj, "src", "recomp", "gen")
        key = self.m.profile_digest(self.profile)
        if self.stamp.get("lift") == key and os.listdir(gen):
            self.rep.progress(3, 100, "lift up to date (%d C files)" % len(os.listdir(gen)))
        else:
            for fn in os.listdir(gen):
                os.remove(os.path.join(gen, fn))
            an = self.an
            args = ["tools.recomp", self.xbe, "--all", "--split", "250",
                    "--game-name", self.info.get("title", self.tid),
                    "--disasm-dir", os.path.join(an, "disasm"), "--func-id-dir", os.path.join(an, "func_id"),
                    "--abi-dir", os.path.join(an, "abi"), "--gen-dir", gen]
            for fn in (self.profile or {}).get("exclude_manual", []):
                args += ["--exclude-manual", os.path.join(proj, "src", "title", fn)]
            mb = max(1.0, os.path.getsize(self.xbe) / 1e6)
            self.py_step(args, 3, 2, 100, "lifting to C", 30 * mb, "LIFT_FAILED")
            self.save_stamp(lift=key)
            self.rep.progress(3, 100, "%d C files" % len(os.listdir(gen)))
        self.proj = proj

    def compile(self):
        a = self.a
        self.tc = cc.Toolchain(a.cc, os.path.abspath(a.sysroot), cc_prefix=a.cc_prefix_arg, ld=a.ld,
                               ld_prefix=a.ld_prefix_arg, builtins=a.builtins, procs=self.procs)
        sources = cc.collect_sources(self.proj)
        has_profile = os.path.isdir(os.path.join(self.proj, "src", "title")) and \
            any(fn.endswith(".c") for fn in os.listdir(os.path.join(self.proj, "src", "title")))
        flags = cc.compile_flags(a.opt, self.m.ROOT, self.proj, self.tc.sysroot, has_profile, a.include_dir)
        jobs = a.jobs or cc.default_jobs(cc.mem_available(), os.cpu_count())
        self.rep.progress(4, 0, "compiling %d files, %d jobs, -O%s" % (len(sources), jobs, a.opt))

        def prog(done, total, now, eta):
            self.rep.progress(4, done * 100 // max(1, total), "%d/%d files (%d compiled)" % (done, total, now),
                              eta)
        self.objects, n = cc.compile_all(sources, flags, self.tc, os.path.join(self.work, "obj"), jobs,
                                         log=self.log, on_progress=prog)
        self.rep.progress(4, 100, "%d files, %d compiled" % (len(sources), n))
        self.compiled = n

    def make_pack(self):
        a = self.a
        packs = os.path.abspath(a.pack_dir)
        os.makedirs(packs, exist_ok=True)
        self.rep.progress(5, 0, "linking libtitle.so")
        h = hashlib.sha256()
        for o in self.objects:
            h.update(os.path.basename(o).encode() + b"\n")
        h.update(pack.sha256_file(a.shell_lib).encode())
        h.update(("|%s|%s" % (a.shell_abi, self.toolkit)).encode())
        # pack.json's env_force comes from title.json: a change there alone
        # (same objects) still writes the pack again.
        h.update(("|env_force=%s" % json.dumps(self.env_force, sort_keys=True)).encode())
        h.update(("|layers=%s" % ",".join(self.layers)).encode())
        key = h.hexdigest()
        pack.recover(packs, self.tid)
        final = pack.final_dir(packs, self.tid)
        meta = pack.read(final)
        if (self.stamp.get("link") == key and meta and meta["xbe_sha256"] == self.xbe_sha
                and meta.get("env_force", {}) == self.env_force and meta.get("layers", []) == self.layers
                and pack.check(final, a.shell_abi) is None):
            self.rep.progress(5, 100, "pack up to date")
        else:
            try:
                part = pack.begin(packs, self.tid)
            except OSError as e:
                raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "PACK_FAILED", str(e))
            lib = os.path.join(part, pack.LIB)
            cc.link(self.objects, lib, self.tc, a.shell_lib, log=self.log)
            self.rep.progress(5, 70, "writing pack.json")
            meta = pack.make_meta(lib, self.tid, self.info.get("title", ""), self.xbe_sha, self.toolkit,
                                  "%s -O%s" % (self.tc.short_version(), a.opt), shell_abi=a.shell_abi,
                                  env_force=self.env_force, layers=self.layers)
            try:
                pack.publish(packs, self.tid, meta)
            except OSError as e:
                raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "PACK_FAILED", str(e))
            self.save_stamp(link=key)
            self.rep.progress(5, 100, "pack %s, %d bytes" % (self.tid, meta["lib_size"]))
        cc.prune(os.path.join(self.work, "obj"), self.objects)
        return final


def run(a, host):
    rep = Reporter()
    missing = [k for k in REQUIRED if not getattr(a, k)]
    if missing:
        rep.error("BAD_ARGS", "--android needs " + ", ".join("--" + k.replace("_", "-") for k in missing))
        return 1
    try:
        os.makedirs(a.work, exist_ok=True)
    except OSError as e:
        rep.error("NO_SPACE" if e.errno == errno.ENOSPC else "BAD_ARGS", "--work %s: %s" % (a.work, e))
        return 1
    r = Run(a, host)
    r.rep = rep

    def on_signal(signum, frame):
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        r.procs.cancel()
        raise cc.Cancelled("signal %d" % signum)
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    try:
        r.identify()
        r.analyse()
        r.lift()
        r.compile()
        final = r.make_pack()
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


def _log_exc(r):
    try:
        with open(r.log, "a") as f:
            traceback.print_exc(file=f)
    except OSError:
        pass
