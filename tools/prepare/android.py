"""
`python -m tools.prepare SOURCE --android ...`: prepare a title pack for the
Android app, on the device (embedded CPython + clang/lld) or on a host with
the NDK. docs/android/prepare.md is the contract with the app.

Steps: 1 identify, 2 analyse, 3 lift, 4 compile, 5 pack. Every sub-step
leaves a stamp in <work>/stamp.json (and every object its cache entry), so a
run that is killed starts again where it stopped.
"""

import copy
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
from tools.prepare import metrics
from tools.prepare import pack
from tools.prepare import toolkit
from tools.prepare.progress import Reporter

REQUIRED = ("work", "pack_dir", "cc", "sysroot", "shell_lib")
# The disc's other images a pack carries (src/platform/recomp_pack.h RECOMP_PACK_IMAGES_MAX).
IMAGES_MAX = 8
# <work>/images.want: the images the app asked for (« Prepare <image> », PendingImage.java), one disc path a line.
IMAGES_WANT = "images.want"


def image_norm(p):
    """A disc path for comparison: lowercase, '/', no leading separator (recomp_pack.c image_norm)."""
    return p.strip().replace("\\", "/").lstrip("/").lower()


class _ImageReporter:
    """Another image's steps, shown inside step 4: « game.xbe: analysing: disasm »."""

    def __init__(self, rep, label):
        self.rep, self.label = rep, label

    def progress(self, step, pct, msg, eta=None):
        self.rep.progress(4, min(99, ((step - 1) * 100 + pct) / 4.0), "%s: %s" % (self.label, msg), eta)


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
        self.title_layer = None
        self.toolkit_sha = ""         # tools/prepare/toolkit.py: what a stale pack is judged by
        self.data_dir = None
        self.image = None             # a disc image source (XBA-126): the other images come out of it
        self.images = []              # [(disc path, objects, work)], other_images(): the images built
        self.lazy_images = []         # [disc path]: named by default.xbe, built on their first launch
        self.steps = metrics.Steps()  # what this run cost (XBA-128): pack.json "prepare"
        self.compile_stats = {}
        self.jobs = 0
        self.sources = []
        self.tools_peak_kb = 0

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
            xbe, self.data_dir, self.image = self.m.find_xbe(self.a.source, self.work)
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
        # The toolkit's content, not its commit: an app update that changed
        # only Java or the shell's .c files keeps the steps done (XBA-127).
        self.toolkit_sha = toolkit.digest()
        self.stamp = {} if self.a.force else self.m.read_stamp(self.stamp_path)
        if self.stamp.get("xbe") != h or self.stamp.get("toolkit_sha256") != self.toolkit_sha:
            self.stamp = {}
            self.save_stamp(xbe=h, toolkit=self.toolkit, toolkit_sha256=self.toolkit_sha)
        # Steps a killed or repeated Prepare finds done already (pack.json "prepare" "skipped", XBA-128).
        self.steps.skipped = [k for k in ("parse", "disasm", "func_id", "abi", "lift", "link") if self.stamp.get(k)]
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
        from tools.prepare import profiles
        for line in profiles.xdk_report(self.ident, self.m.PROFILES):     # XBA-106
            self.rep.progress(1, 99, line)
        self.title_layer = self.m.title_layer_state(self.tid)              # WP6: the app's Enhancements switch
        en = profiles.title_line(self.tid, self.m.PROFILES, left_out=self.title_layer == "off")
        if en:
            self.rep.progress(1, 99, en)
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
        # Also when the seeds file went away (removed to drop a bad seed): the
        # analysis made with it must not be kept.
        # The static discovery switches too (XBA-137): "" for the default.
        key = discovered.digest(disc) + discovered.mode_suffix()
        if self.stamp.get("discovered", "") != key:
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
            if name == "disasm":        # how many functions, static discovery's share (XBA-137)
                counts = discovered.counts_line(discovered.analysis_counts(an))
                if counts:
                    self.rep.progress(2, hi, counts)
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
            # Drop the old stamp before the old C (XBA-138): a run killed in
            # the lift leaves gen/ half written, and a stamp still naming an
            # earlier key would pass it as up to date once the key comes
            # back (the « Enhancements » switch turned off, then on again).
            if "lift" in self.stamp:
                self.stamp.pop("lift")
                self.save_stamp()
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
            log_at = os.path.getsize(self.log) if os.path.exists(self.log) else 0
            self.py_step(args, 3, 2, 100, "lifting to C", 30 * mb, "LIFT_FAILED")
            # Bodies the analysis cut short (WP2): reaching one stops the title.
            cut = self.m.lift_cut_bodies(self.log, log_at)
            # What the title cannot reach, by class (XBA-139).
            health = self.m.lift_health(self.log, log_at)
            words = self.m.health_line(health)
            if health is not None:
                self.save_stamp(lift=key, lift_health=health)
            else:
                self.save_stamp(lift=key)
            self.rep.progress(3, 100, "%d C files" % len(os.listdir(gen))
                              + ("; cut bodies " + cut if cut else "")
                              + ("; lift health: " + words if words else ""))
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
        self.jobs, self.sources = jobs, sources
        self.tools_peak_kb = metrics.children_peak_kb()     # the pipeline tools so far (disasm, lift, ...)
        device_log = getattr(a, "device_log", None)
        limit = (lambda: metrics.jobs_now(device_log, jobs)) if device_log else None
        self.rep.progress(4, 0, "compiling %d files, %d jobs, -O%s" % (len(sources), jobs, a.opt))

        def prog(done, total, now, eta):
            self.rep.progress(4, done * 100 // max(1, total), "%d/%d files (%d compiled)" % (done, total, now),
                              eta)
        self.objects, n = cc.compile_all(sources, flags, self.tc, os.path.join(self.work, "obj"), jobs,
                                         log=self.log, on_progress=prog, limit=limit, stats=self.compile_stats)
        self.rep.progress(4, 100, "%d files, %d compiled" % (len(sources), n))
        self.compiled = n

    def wanted_images(self):
        """The other images this run builds (normalised disc paths), or None for
        all of them (--all-images). Lazy by default (docs/android/prepare.md
        « Images »): those asked with --image, those the app asked for in
        <work>/images.want, and those the current pack has a library for (a
        Prepare again keeps them)."""
        if getattr(self.a, "all_images", False):
            return None
        want = {image_norm(x) for x in (getattr(self.a, "image", None) or [])}
        try:
            with open(os.path.join(self.work, IMAGES_WANT)) as f:
                want |= {image_norm(x) for x in f if x.strip()}
        except OSError:
            pass
        if getattr(self, "tid", None):
            meta = pack.read(pack.final_dir(os.path.abspath(self.a.pack_dir), self.tid))
            for rel, lib in ((meta or {}).get("images") or {}).items():
                if lib:
                    want.add(image_norm(rel))
        return want

    def other_images(self):
        """XBA-82: the disc's other images that default.xbe names (tools.triage's
        test). The wanted ones (wanted_images) are each analysed, lifted and
        compiled in <work>/images/<path>/ with this run's toolchain; make_pack
        links one library each. The others are only named (lazy_images: library
        "" in pack.json), prepared when the title first launches them. An image
        that fails is left out (default.xbe still runs; its launch then exits as
        before). From a folder, or from a disc image (XBA-126: the named XBEs
        are copied out of it, nothing else); a lone default.xbe has no others."""
        disc = self.data_dir or self.image
        if not disc or getattr(self.a, "no_images", False):
            return
        import re
        from tools.triage import _named_xbes, disc_xbes
        with open(self.xbe, "rb") as f:
            named = _named_xbes(f.read())
        others = [(rel, p) for rel, p in disc_xbes(disc, self.work, named) if rel.lower() != "default.xbe"]
        want = self.wanted_images()
        for rel, p in others[:IMAGES_MAX]:
            if want is not None and image_norm(rel) not in want:
                self.lazy_images.append(rel)
                continue
            sub_a = copy.copy(self.a)
            sub_a.source = p
            sub_a.work = os.path.join(self.work, "images", re.sub(r"[^A-Za-z0-9._-]", "_", rel))
            os.makedirs(sub_a.work, exist_ok=True)
            r = Run(sub_a, self.m)
            r.rep = _ImageReporter(self.rep, rel)
            r.procs = self.procs        # a cancel reaches its compilers too
            try:
                r.identify()
                r.analyse()
                r.lift()
                r.compile()
            except cc.StepError as e:
                self.rep.progress(4, 99, "%s left out: %s %s" % (rel, e.code, e.msg))
                continue
            self.images.append((rel, r.objects, r.work))
        if self.lazy_images:
            self.rep.progress(4, 99, "%d more image(s) prepared on their first launch: %s"
                              % (len(self.lazy_images), ", ".join(self.lazy_images)))
        if others[IMAGES_MAX:]:
            self.rep.progress(4, 99, "%d more image(s) left out (at most %d)" % (len(others) - IMAGES_MAX, IMAGES_MAX))

    def image_libs(self):
        """{disc path: library file name} for pack.json "images", "" for an
        image not built yet (prepared on its first launch)."""
        libs = {rel: "libimage-%d.so" % (i + 1) for i, (rel, _, _) in enumerate(self.images)}
        for rel in self.lazy_images:
            libs.setdefault(rel, "")
        return libs

    def make_pack(self):
        a = self.a
        packs = os.path.abspath(a.pack_dir)
        os.makedirs(packs, exist_ok=True)
        self.rep.progress(5, 0, "linking libtitle.so")
        h = hashlib.sha256()
        for o in self.objects:
            h.update(os.path.basename(o).encode() + b"\n")
        h.update(pack.sha256_file(a.shell_lib).encode())
        h.update(("|%s|%s" % (a.shell_abi, self.toolkit_sha)).encode())
        # pack.json's env_force comes from title.json: a change there alone
        # (same objects) still writes the pack again.
        h.update(("|env_force=%s" % json.dumps(self.env_force, sort_keys=True)).encode())
        h.update(("|layers=%s" % ",".join(self.layers)).encode())
        for rel, objects, _ in self.images:
            h.update(("|image=%s:" % rel).encode())
            for o in objects:
                h.update(os.path.basename(o).encode() + b"\n")
        key = h.hexdigest()
        images = self.image_libs()
        pack.recover(packs, self.tid)
        final = pack.final_dir(packs, self.tid)
        meta = pack.read(final)
        if (self.stamp.get("link") == key and meta and meta["xbe_sha256"] == self.xbe_sha
                and meta.get("env_force", {}) == self.env_force and meta.get("layers", []) == self.layers
                and meta.get("title_layer") == self.title_layer
                and meta.get("images", {}) == images
                and meta.get("toolkit_sha256", "") == self.toolkit_sha
                and pack.check(final, a.shell_abi) is None):
            self.rep.progress(5, 100, "pack up to date")
        else:
            try:
                part = pack.begin(packs, self.tid)
            except OSError as e:
                raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "PACK_FAILED", str(e))
            lib = os.path.join(part, pack.LIB)
            cc.link(self.objects, lib, self.tc, a.shell_lib, log=self.log)
            for i, (rel, objects, _) in enumerate(self.images):
                self.rep.progress(5, 40 + 30 * i // max(1, len(self.images)), "linking %s" % rel)
                cc.link(objects, os.path.join(part, images[rel]), self.tc, a.shell_lib, log=self.log)
            self.rep.progress(5, 70, "writing pack.json")
            meta = pack.make_meta(lib, self.tid, self.info.get("title", ""), self.xbe_sha, self.toolkit,
                                  "%s -O%s" % (self.tc.short_version(), a.opt), shell_abi=a.shell_abi,
                                  env_force=self.env_force, layers=self.layers,
                                  toolkit_sha256=self.toolkit_sha, title_layer=self.title_layer)
            if images:
                meta["images"] = images
            # The analysis' function count and static discovery's share per
            # pass (XBA-137), for the owner's library report.
            from tools.prepare import discovered
            counts = discovered.analysis_counts(self.an) if getattr(self, "an", None) else None
            if counts:
                meta["analysis"] = counts
            # What this run cost (XBA-128): step times, compile peak RSS, sizes, the device's thermal samples.
            meta["prepare"] = metrics.summary(self.steps, self.compile_stats, self.sources, self.objects, self.jobs,
                                              a.opt, part, getattr(a, "device_log", None), self.tools_peak_kb)
            try:
                pack.publish(packs, self.tid, meta)
            except OSError as e:
                raise cc.StepError("NO_SPACE" if e.errno == errno.ENOSPC else "PACK_FAILED", str(e))
            self.save_stamp(link=key)
            self.rep.progress(5, 100, "pack %s, %d bytes" % (self.tid, meta["lib_size"]))
        cc.prune(os.path.join(self.work, "obj"), self.objects)
        for _, objects, work in self.images:
            cc.prune(os.path.join(work, "obj"), objects)
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
        st = r.steps
        st.run("identify", r.identify)
        st.run("analyse", r.analyse)
        st.run("lift", r.lift)
        st.run("compile", r.compile)
        st.run("images", r.other_images)
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


def _log_exc(r):
    try:
        with open(r.log, "a") as f:
            traceback.print_exc(file=f)
    except OSError:
        pass
