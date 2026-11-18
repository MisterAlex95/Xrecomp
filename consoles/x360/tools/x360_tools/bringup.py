"""One command from a title's XEX to a recompiled run (XBA-270), for a dump of your own:

    python3 -m x360_tools.bringup DEFAULT.XEX WORKDIR [--key-file KEY] [--disc IMAGE|DIR] [--x360-run PATH]
                                  [--rounds N] [--insns N] [--cc CC] [-j N]

1. parse the XEX (with your key: none is in the repository) and write it plain (decrypted, expanded) to
   WORKDIR/plain.xex, the file x360_run loads;
2. the imports the runtime's HLE covers, and the ones it does not (by name);
3. the decoder / lifter coverage of the code (unknown words, not-lifted mnemonics, helpers, jump tables);
4. recompile, build the pack (WORKDIR/libtitle.so, -DXE_RUNTIME), run it under x360_run, and when execution reaches
   code the pack lacks, recompile with those addresses and run again (up to --rounds);
   the shaders it loads are dumped (WORKDIR/shaders) and translated to GLSL ES (xenos_glsl), counted, and a
   last run draws with them (x360_run --render; --frames N keeps the first N frames as PPM in WORKDIR/frames);
5. with --app-pack DIR (XBA-280), an app pack: DIR/x360-<KEY>/ with the pack built again by --app-cc (the NDK's
   clang for the AYN Thor: aarch64-linux-android30-clang), default.xex (the plain title), the shader cache (shaders/)
   and pack.json (console x360, title_key, abi, shell_abi 1, lib, lib_size), to copy into the app's files/packs/;
6. a summary with no title data in it (counts, export names, the runtime's last message), also written to
   WORKDIR/summary.txt: what to paste in the ticket.

What the title printed through DbgPrint stays in WORKDIR/dbg.txt (not in the summary: it is the title's text).
"""

import argparse
import collections
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

from . import CONSOLE  # noqa: F401
from tools.xex.xex import COMPRESSION_NAMES, XexError, load_keys, parse_xex, plain_xex

from .hle_coverage import coverage
from .recomp import REPO, RUNTIME_DIRS, recompile, sweep, sweep_shapes
from .xenos_glsl import Unsupported, translate_cached as translate


def find_x360_run(explicit):
    if explicit:
        return explicit
    for pattern in ("build/**/x360_run", "build*/**/x360_run"):
        for p in sorted(glob.glob(os.path.join(REPO, pattern), recursive=True)):
            if os.access(p, os.X_OK) and os.path.isfile(p):
                return p
    return shutil.which("x360_run")


def build_pack(src, out, cc, jobs):
    """Compile every C file of src into a shared library (parallel objects, then one link)."""
    inc = []
    for d in RUNTIME_DIRS:
        inc += ["-I", d]
    flags = ["-O1", "-fPIC", "-ffp-contract=off", "-DXE_RUNTIME=1", "-w"]
    files = sorted(glob.glob(os.path.join(src, "*.c")))
    objs, procs = [], []
    for f in files:
        o = f[:-2] + ".o"
        objs.append(o)
        procs.append(subprocess.Popen([cc, "-c"] + flags + inc + [f, "-o", o]))
        if len(procs) >= jobs:
            if procs.pop(0).wait():
                raise RuntimeError("compiling the pack failed")
    for p in procs:
        if p.wait():
            raise RuntimeError("compiling the pack failed")
    subprocess.run([cc, "-shared", "-o", out] + objs + ["-lm"], check=True)


def render_run(a, run, plain, pack, shader_dir, say):
    """One more run of the title with the renderer on its translated shaders (x360_run --render): the render stats
    line goes in the summary, the first --frames frames stay in WORKDIR/frames."""
    frames = os.path.join(a.work, "frames")
    shutil.rmtree(frames, ignore_errors=True)
    os.makedirs(frames)
    log = os.path.join(a.work, "render_log.txt")
    cmd = [run, plain, "--pack", pack, "--insns", str(a.insns), "--render", shader_dir,
           "--dbg", os.devnull, "--log", log]
    if a.frames > 0:  # frames are written only when asked for, at most N (the disk is shared)
        cmd += ["--frames", frames, "--frame-limit", str(a.frames)]
    if a.disc:
        cmd += ["--disc", a.disc]
    try:
        subprocess.run(cmd, timeout=3600)
    except subprocess.TimeoutExpired:
        pass
    lines = open(log, errors="replace").read().splitlines() if os.path.exists(log) else []
    for line in lines:
        if line.startswith("render:") or "no renderer" in line:
            say("  %s" % line)
    names = sorted(os.listdir(frames))
    if names:
        say("  frames kept: %d (in %s)" % (len(names), frames))


def shader_report(shader_dir):
    """The shaders x360_run dumped (vs_*.bin / ps_*.bin), each translated by xenos_glsl: counts and what stops the
    others (the instruction kinds only, no title data). The GLSL lands next to each .bin."""
    rep = {"total": 0, "translated": 0, "unsupported": {}}
    for name in sorted(os.listdir(shader_dir)) if os.path.isdir(shader_dir) else []:
        if not name.endswith(".bin"):
            continue
        rep["total"] += 1
        raw = open(os.path.join(shader_dir, name), "rb").read()
        words = list(struct.unpack(">%dI" % (len(raw) // 4), raw[:len(raw) // 4 * 4]))
        try:
            glsl, _ = translate(words, "pixel" if name.startswith("ps") else "vertex",
                                interpolators=16 if name.startswith("ps") else 0)
        except Unsupported as ex:
            why = re.sub(r"^(alu|fetch) \d+: ", "", str(ex))
            rep["unsupported"][why] = rep["unsupported"].get(why, 0) + 1
            continue
        rep["translated"] += 1
        with open(os.path.join(shader_dir, name[:-4] + (".frag" if name.startswith("ps") else ".vert")), "w") as f:
            f.write(glsl)
    return rep


def title_key(raw):
    """The app's key of an XEX (recomp_pack_rom_identify, x360_runtime_key): X360-<title id>-<CRC32 of the execution
    info's 24 bytes>, from optional header 0x00040006 (a plain rewrite keeps it); None without one."""
    import zlib
    if len(raw) < 24 or raw[:4] != b"XEX2":
        return None
    count = struct.unpack(">I", raw[20:24])[0]
    for i in range(min(count, 64)):
        k, v = struct.unpack(">II", raw[24 + 8 * i:32 + 8 * i])
        if k == 0x00040006 and v + 24 <= len(raw):
            return "X360-%08X-%08X" % (struct.unpack(">I", raw[v + 12:v + 16])[0], zlib.crc32(raw[v:v + 24]))
    return None


def app_pack(out_dir, raw, plain, src, shader_dir, cc, jobs, abi):
    """DIR/x360-<KEY>/: the pack built by cc, default.xex, shaders/ (GLSL ES and microcode), pack.json."""
    key = title_key(raw)
    if not key:
        raise RuntimeError("the XEX has no execution info: no title key")
    d = os.path.join(out_dir, "x360-" + key)
    os.makedirs(os.path.join(d, "shaders"), exist_ok=True)
    lib = os.path.join(d, "libtitle.so")
    build_pack(src, lib, cc, jobs)
    shutil.copyfile(plain, os.path.join(d, "default.xex"))
    for f in glob.glob(os.path.join(shader_dir, "*")):
        if f.endswith((".vert", ".frag", ".bin")):
            shutil.copyfile(f, os.path.join(d, "shaders", os.path.basename(f)))
    meta = {"format": 1, "console": "x360", "title_key": key, "title": key, "abi": abi, "shell_abi": 1,
            "lib": "libtitle.so", "lib_size": os.path.getsize(lib)}
    with open(os.path.join(d, "pack.json"), "w") as f:
        json.dump(meta, f, indent=1)
    return d, key


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python3 -m x360_tools.bringup")
    ap.add_argument("xex")
    ap.add_argument("work")
    ap.add_argument("--key-file", action="append", default=[])
    ap.add_argument("--disc", help="the title's disc: an image or a directory of its files")
    ap.add_argument("--x360-run", help="the x360_run binary (default: looked for under build/)")
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--insns", type=int, default=2000000000)
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
    ap.add_argument("-j", type=int, default=3)
    ap.add_argument("--no-render", action="store_true",
                    help="skip the last run that draws with the translated shaders (EGL / GLES 3.0)")
    ap.add_argument("--frames", type=int, default=0,
                    help="that run's frames: keep the first N as PPM in WORKDIR/frames (default 0: count only)")
    ap.add_argument("--app-pack", help="also write an app pack into DIR/x360-<KEY>/ (pack.json, the pack, shaders)")
    ap.add_argument("--app-cc", default=os.environ.get("APP_CC", ""),
                    help="the app pack's compiler (default: --cc; the AYN Thor: aarch64-linux-android30-clang)")
    ap.add_argument("--app-abi", default="arm64-v8a", help="the app pack's ABI in pack.json (default arm64-v8a)")
    a = ap.parse_args(argv)
    os.makedirs(a.work, exist_ok=True)
    lines = []

    def say(s):
        print(s)
        lines.append(s)

    t0 = time.time()
    try:
        with open(a.xex, "rb") as f:
            raw = f.read()
        img = parse_xex(raw, load_keys(a.key_file))
    except (OSError, XexError) as ex:
        print("error: %s" % ex, file=sys.stderr)
        return 1
    say("xex: image %d bytes, %s, compression %s, %d .pdata functions, %d import libraries" % (
        img.image_size, "encrypted" if img.encryption else "not encrypted",
        COMPRESSION_NAMES.get(img.compression, img.compression), len(img.pdata), len(img.libraries)))
    if img.image is None:
        say("stop: the image is not readable: %s" % img.image_note)
        return 1
    plain = os.path.join(a.work, "plain.xex")
    with open(plain, "wb") as f:
        f.write(plain_xex(raw, img))

    rows = coverage(img)
    missing = [r for r in rows if not r[4]]
    by_lib = collections.Counter(r[0] for r in rows)
    say("imports: %d (%s), covered by the HLE: %d, missing: %d" % (
        len(rows), ", ".join("%s %d" % kv for kv in by_lib.items()), len(rows) - len(missing), len(missing)))
    for lib, o, n, kind, _ in missing:
        say("  missing %s %d %s (%s)" % (lib, o, n, kind))

    total, unknown, unlifted = sweep(img)
    kinds, jt_ok, jt_bad = sweep_shapes(img)
    say("sweep: %d instruction words, %d unknown (%.2f %%), %d decoded but not lifted" % (
        total, sum(unknown.values()), 100.0 * sum(unknown.values()) / max(total, 1), sum(unlifted.values())))
    if unlifted:
        say("  not lifted: %s" % ", ".join("%s %d" % kv for kv in unlifted.most_common(15)))
    say("  helpers: %s; bctr: %d tables resolved, %d not" % (
        ", ".join("%s %d" % kv for kv in sorted(kinds.items())) or "none", jt_ok, jt_bad))

    run = find_x360_run(a.x360_run)
    if not run:
        say("stop: no x360_run (build consoles/x360, or pass --x360-run)")
        return 1
    discovered = set()
    disc_file = os.path.join(a.work, "discovered.txt")
    outcome = "not run"
    shader_dir = os.path.join(a.work, "shaders")
    os.makedirs(shader_dir, exist_ok=True)
    for rnd in range(1, a.rounds + 1):
        src = os.path.join(a.work, "src")
        shutil.rmtree(src, ignore_errors=True)
        t = time.time()
        s = recompile(img, src, extra_seeds=discovered, comments=False)
        rt = time.time() - t
        t = time.time()
        pack = os.path.join(a.work, "libtitle.so")
        build_pack(src, pack, a.cc, a.j)
        bt = time.time() - t
        say("round %d: %d functions, %d instructions, %d to the interpreter, %d jump tables, %d bctr unresolved; "
            "recompiled in %.1f s, pack built in %.1f s" % (rnd, s["functions"], s["instructions"],
                                                           s["interp_fallback_insns"], s["jump_tables"],
                                                           s["unresolved_bctr"], rt, bt))
        missed = os.path.join(a.work, "missed.txt")
        cmd = [run, plain, "--pack", pack, "--insns", str(a.insns), "--stats", "--missed", missed,
               "--dbg", os.path.join(a.work, "dbg.txt"), "--log", os.path.join(a.work, "log.txt"),
               "--dump-shaders", shader_dir]
        if a.disc:
            cmd += ["--disc", a.disc]
        try:
            rc = subprocess.run(cmd, timeout=3600).returncode
        except subprocess.TimeoutExpired:
            rc = -1
        log = open(os.path.join(a.work, "log.txt")).read().splitlines() if os.path.exists(
            os.path.join(a.work, "log.txt")) else []
        last = [x for x in log if x.startswith("x360_run:")]
        stats = [x for x in log if x.startswith("stats:")]
        unimpl = sorted({x.split(" ")[1] for x in log if "has no HLE" in x and x.startswith("x360: ")})
        outcome = last[-1] if last else "x360_run exit %d" % rc
        say("  run: %s" % outcome)
        if stats:
            say("  %s" % stats[-1])
        if unimpl:
            say("  called without an HLE: %s" % ", ".join(unimpl))
        new = set()
        if os.path.exists(missed):
            for line in open(missed):
                line = line.split("#")[0].strip()
                if line:
                    new.add(int(line, 16))
        new -= discovered
        if not new:
            break
        discovered |= new
        with open(disc_file, "w") as f:
            f.write("".join("%08X\n" % v for v in sorted(discovered)))
        say("  %d address(es) reached without lifted code: recompiling with them" % len(new))
    shaders = shader_report(shader_dir)
    todo = sorted(shaders["unsupported"].items(), key=lambda kv: -kv[1])
    say("shaders: %d loaded, %d translated to GLSL ES%s" % (
        shaders["total"], shaders["translated"],
        "; not yet: " + ", ".join("%s (%d)" % kv for kv in todo) if todo else ""))
    if shaders["translated"] and not a.no_render:
        render_run(a, run, plain, pack, shader_dir, say)
    dbg = os.path.join(a.work, "dbg.txt")
    say("DbgPrint: %d lines (in %s, not copied here)" % (
        sum(1 for _ in open(dbg, errors="replace")) if os.path.exists(dbg) else 0, dbg))
    if a.app_pack:
        try:
            d, key = app_pack(a.app_pack, raw, plain, os.path.join(a.work, "src"), shader_dir, a.app_cc or a.cc,
                              a.j, a.app_abi)
            say("app pack: x360-%s (copy %s into the app's files/packs/)" % (key, d))
        except (OSError, RuntimeError, subprocess.CalledProcessError) as ex:
            say("app pack: not written: %s" % ex)
    say("total %.1f s" % (time.time() - t0))
    with open(os.path.join(a.work, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(a.work, "summary.json"), "w") as f:
        json.dump({"missing_imports": [list(r[:4]) for r in missing], "outcome": outcome,
                   "discovered": len(discovered), "shaders": shaders}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
