"""Find one-frame glitches in a headless run: frame dumps, batch records,
signature tables. See docs/technical/gpu-backend.md, "Checking for one-frame glitches".

  python3 -m tools.frame_outliers frames  <dump dir>   [--crop x0,y0,x1,y1]
  python3 -m tools.frame_outliers batches <batch log>
  python3 -m tools.frame_outliers sigs    <run log>
  python3 -m tools.frame_outliers run --exe <game> --pad-script <file> --start 94 --length 20 --out <dir>

Exit status: 0, or 1 when the outlier frames (of the frame dumps when there
are any, else of the records) exceed --max-outliers; 2 on a usage error.
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time

from . import images, records


def _crop(s):
    if not s:
        return None
    v = [int(x) for x in s.split(",")]
    if len(v) != 4:
        raise argparse.ArgumentTypeError("--crop wants x0,y0,x1,y1")
    return tuple(v)


def _image_args(p):
    g = p.add_argument_group("frame dumps")
    g.add_argument("--crop", type=_crop, help="x0,y0,x1,y1: only this part of each frame (e.g. the 3D viewport)")
    g.add_argument("--threshold", type=int, default=24, help="a pixel changed: some channel differs by more (24)")
    g.add_argument("--ratio", type=float, default=3.0,
                   help="flash: a frame differs from both neighbours this many times more than they differ (3)")
    g.add_argument("--floor", type=float, default=0.001,
                   help="flash: least difference counted between the neighbours, fraction of the area (0.001)")
    g.add_argument("--max-run", type=int, default=2, help="flash: longest wrong run, in frames (2)")
    g.add_argument("--black-excess", type=float, default=0.01,
                   help="black: spike above the neighbours' median, fraction of the area (0.01)")
    g.add_argument("--black-window", type=int, default=6, help="black: neighbours each side (6)")
    g.add_argument("--steady", type=float, default=0.1,
                   help="black: the frames around a spike differ by less than this fraction of the area (0.1)")
    g.add_argument("--detectors", default="flash,black", help="flash,black (both)")
    g.add_argument("--prefix", default=None, help="only dumps whose name starts with this")


def _common(p):
    p.add_argument("--max-outliers", type=int, default=0, help="exit 1 above this many outlier frames (0)")
    p.add_argument("--json", metavar="FILE", help="also write the full result as JSON")


def _analyse_dir(args, d):
    fr = images.list_frames(d, args.prefix)
    return images.analyse_frames(
        fr, crop=args.crop, threshold=args.threshold, ratio=args.ratio, floor=args.floor,
        max_run=args.max_run, black_excess=args.black_excess, black_window=args.black_window,
        steady=args.steady,
        detectors=tuple(x for x in args.detectors.split(",") if x))


def _jsonable(res):
    def conv(o):
        if isinstance(o, dict):
            return {str(k): conv(v) for k, v in o.items() if k != "events"}
        if isinstance(o, (list, tuple)):
            return [conv(v) for v in o]
        return o
    return conv(res)


def _finish(args, results, primary):
    if args.json:
        with open(args.json, "w") as f:
            json.dump(_jsonable(results), f, indent=1)
    n = primary["count"] if primary else 0
    verdict = "FAIL" if n > args.max_outliers else "ok"
    print("verdict: %s (%d outlier frames, allowed %d)" % (verdict, n, args.max_outliers))
    return 1 if n > args.max_outliers else 0


def cmd_frames(args):
    res = _analyse_dir(args, args.dir)
    if not res["frames"]:
        print("no numbered .bmp frames in %s" % args.dir, file=sys.stderr)
        return 2
    print(images.format_report(res))
    return _finish(args, {"frames": res}, res)


def cmd_batches(args):
    res = records.analyse_records(records.read_records(args.log), with_constants=args.with_constants)
    print(records.format_records_report(res))
    return _finish(args, {"batches": res}, res)


def cmd_sigs(args):
    with open(args.log, errors="replace") as f:
        res = records.analyse_sig_tables(records.read_sig_tables(f))
    print(records.format_sig_report(res))
    return _finish(args, {"sigs": res}, res)


def cmd_run(args):
    out = os.path.abspath(args.out)
    if os.path.exists(out) and os.listdir(out) and not args.force:
        print("%s exists and is not empty (--force to reuse it)" % out, file=sys.stderr)
        return 2
    fb = os.path.join(out, "fb")
    shutil.rmtree(fb, ignore_errors=True)
    os.makedirs(fb)
    flag = os.path.join(out, "window.flag")
    if os.path.exists(flag):
        os.remove(flag)
    env = dict(os.environ)
    env.update({"SDL_AUDIODRIVER": "dummy", "RECOMP_FB_WINDOW": "0", "RECOMP_GPU": "1",
                "RECOMP_FB_DUMP": os.path.join(fb, "f"), "RECOMP_FB_DUMP_FLIPS": flag,
                "RECOMP_DISCOVERY": os.path.join(out, "discovered.jsonl")})
    if args.pad_script:
        with open(args.pad_script) as f:
            env["RECOMP_PAD_SCRIPT"] = f.read().strip()
    if args.save_from:
        save = os.path.join(out, "save")
        shutil.rmtree(save, ignore_errors=True)
        shutil.copytree(args.save_from, save, symlinks=True)
        env["RECOMP_SAVE_DIR"] = save
    blog = os.path.join(out, "batches.blg")
    if args.batchlog:
        env["RECOMP_GPU_BATCHLOG"] = blog
        env["RECOMP_GPU_BATCHLOG_FLIPS"] = flag
    if args.sigs:
        env["RECOMP_GPU_SIGDUMP"] = flag
        env["RECOMP_GPU_SIGDUMP_KEEP"] = "1"
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v
    total = args.total or (args.start + args.length + 4)
    logp = os.path.join(out, "run.log")

    def window():
        time.sleep(args.start)
        open(flag, "w").close()
        time.sleep(args.length)
        if os.path.exists(flag):
            os.remove(flag)
    threading.Thread(target=window, daemon=True).start()
    print("running %s for %ds, dumping %d-%ds, log %s" % (args.exe, total, args.start,
                                                         args.start + args.length, logp))
    with open(logp, "wb") as log:
        p = subprocess.Popen([os.path.abspath(args.exe)], cwd=out, env=env, stdout=log,
                             stderr=subprocess.STDOUT, start_new_session=True)
        try:
            p.wait(timeout=total)
        except subprocess.TimeoutExpired:
            p.send_signal(signal.SIGTERM)       # by PID, as every headless run here
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
    if os.path.exists(flag):
        os.remove(flag)
    if args.save_from and not args.keep_save:
        shutil.rmtree(os.path.join(out, "save"), ignore_errors=True)
    print("exit %d" % p.returncode)

    results, primary = {}, None
    res = _analyse_dir(args, fb)
    if res["frames"]:
        print(images.format_report(res))
        results["frames"], primary = res, res
    else:
        print("no frames dumped (the run ended before the window?)")
    if args.batchlog and os.path.exists(blog):
        rb = records.analyse_records(records.read_records(blog))
        print(records.format_records_report(rb))
        results["batches"] = rb
        primary = primary or rb
    if args.sigs:
        with open(logp, errors="replace") as f:
            rs = records.analyse_sig_tables(records.read_sig_tables(f))
        print(records.format_sig_report(rs))
        results["sigs"] = rs
    if not args.keep_frames and res["frames"]:
        keep = set()
        for n in res["outliers"]:
            keep.update((n - 1, n, n + 1))
        for n, path in images.list_frames(fb):
            if n not in keep:
                os.remove(path)
        print("kept the outlier frames and their neighbours in %s" % fb)
    if primary is None:
        print("verdict: no data")
        return 2
    return _finish(args, results, primary)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python3 -m tools.frame_outliers",
                                 description="One-frame glitches in a headless run.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("frames", help="analyse a directory of RECOMP_FB_DUMP frames")
    p.add_argument("dir")
    _image_args(p)
    _common(p)
    p.set_defaults(fn=cmd_frames)

    p = sub.add_parser("batches", help="analyse a RECOMP_GPU_BATCHLOG file")
    p.add_argument("log")
    p.add_argument("--with-constants", action="store_true",
                   help="vertex constants count as look too (noisy with a moving camera)")
    _common(p)
    p.set_defaults(fn=cmd_batches)

    p = sub.add_parser("sigs", help="analyse the [GPUSIG] tables of a run log")
    p.add_argument("log")
    _common(p)
    p.set_defaults(fn=cmd_sigs)

    p = sub.add_parser("run", help="run a game headless, dump a window, analyse it")
    p.add_argument("--exe", required=True, help="the game executable")
    p.add_argument("--pad-script", help="file holding a RECOMP_PAD_SCRIPT")
    p.add_argument("--start", type=float, required=True, help="window start, seconds after launch")
    p.add_argument("--length", type=float, required=True, help="window length, seconds")
    p.add_argument("--total", type=float, help="kill the run after this many seconds (start+length+4)")
    p.add_argument("--out", required=True, help="run directory (frames, log, records)")
    p.add_argument("--save-from", help="copy this save dir into <out>/save and run on the copy")
    p.add_argument("--keep-save", action="store_true", help="do not delete the save copy after the run")
    p.add_argument("--batchlog", action="store_true", help="also record batches (RECOMP_GPU_BATCHLOG) in the window")
    p.add_argument("--sigs", action="store_true", help="also print signature tables (RECOMP_GPU_SIGDUMP) in the window")
    p.add_argument("--env", action="append", default=[], metavar="K=V", help="extra environment (repeatable)")
    p.add_argument("--keep-frames", action="store_true", help="keep every dumped frame (default: outliers and neighbours)")
    p.add_argument("--force", action="store_true", help="reuse a non-empty --out")
    _image_args(p)
    _common(p)
    p.set_defaults(fn=cmd_run)

    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
