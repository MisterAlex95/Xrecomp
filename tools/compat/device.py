"""The compat runner's grading for a run it did not start itself: the app's
« Check my library » on the device (XBA-130), which boots each title headed,
on screen, and hands the files of the run to

    python -m tools.compat grade --out compat-results.jsonl --work WORK \\
        --source TITLE --status status.jsonl --log run.log --presses 8000,10000 \\
        --exit none --run-s 180 [--field device=...] ...

It grades with the same rules as `tools.compat run` (classify, one
implementation: the app runs this module with the CPython it embeds for
Prepare) and appends one line with the same schema, plus what the device
adds (device, android, app_build, renderer, prepare_s, peak_mem_mb, ...).
Nothing from the game: the title ID, the certificate's title name, the XDK
library builds, engine names, our stage names and the first line of an error.
"""

import json
import os
import re
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))



def check_env(run_dir, cfg_usb=None):
    """What the app sets for a check run (LibraryCheck.runEnv, applied by
    ShellActivity before the runtime starts), as tools.compat's boot() does on
    a desktop minus the headless and screenshot switches: on the device the
    game is on screen and nothing is dumped. RECOMP_USB: recomp.cfg's when it
    has one, else 1 (the pad script goes through the USB pad)."""
    return {"RECOMP_STATUS": os.path.join(run_dir, "status.jsonl"), "RECOMP_STATUS_MS": "1000",
            "RECOMP_PAD_LIVE": os.path.join(run_dir, "pad-live.txt"),
            "RECOMP_USB": cfg_usb if cfg_usb is not None else "1"}

GPUGL_RE = re.compile(r"\[GPUGL\] ([^|\n]+?) \|")
NO_GL_RE = re.compile(r"\[GPUGL\] no GL context")
FATAL_SIGNAL_RE = re.compile(r"Fatal signal (\d+)")

# --exit values besides none / exit:N / signal:N: how Android said the
# process ended (ApplicationExitInfo), and what the app saw itself.
# --field cannot replace what the grading says.
PROTECTED = ("source", "title_id", "title", "stage", "blocker", "best", "runs", "host")

EXIT_WORDS = {
    "crash": "native crash",
    "java": "Java exception in the game process",
    "anr": "not responding (ANR)",
    "lowmem": "ended by the system: low memory",
    "window": "the game window closed",
    "lost": "the check was interrupted during this boot (the app or the device restarted)",
    "away": "interrupted: the game left the screen (the app was left or the screen turned off)",
    "nostart": "the game's process did not start",
}


def read_jsonl(path):
    from tools.compat.__main__ import read_jsonl as r
    return r(path)


def monotonic(status):
    """The status lines with one clock. A quick reboot (the title restarting
    into itself) runs a new process whose status clock starts at 0 again in
    the same file: its lines are moved after the previous ones."""
    out, base, last = [], 0, -1
    for s in status:
        ms = s.get("ms", 0) + base
        if ms < last:
            base = last
            ms = s.get("ms", 0) + base
        last = ms
        out.append(dict(s, ms=ms))
    return out


def parse_exit(arg, log_text=""):
    """(exit_info for classify, blocker override or None) from --exit."""
    arg = (arg or "none").strip()
    if arg == "none":
        return None, None
    kind, _, n = arg.partition(":")
    if kind in ("exit", "signal") and n.lstrip("-").isdigit():
        return (kind, int(n)), None
    if kind == "crash":
        m = FATAL_SIGNAL_RE.search(log_text or "")
        sig = int(n) if n.isdigit() and int(n) else int(m.group(1)) if m else 0
        return ("signal", sig), None if sig else EXIT_WORDS["crash"]
    if kind in ("java", "anr", "lowmem"):
        return ("signal", 9 if kind != "java" else 6), EXIT_WORDS[kind]
    if kind == "window":
        return ("exit", 0), None
    if kind in ("lost", "away", "nostart"):
        return None, EXIT_WORDS[kind]
    raise ValueError("--exit: %s" % arg)


def renderer_of(log_text):
    """The GPU path the runtime took, from its log: the GL renderer's name, or the CPU rasteriser."""
    m = GPUGL_RE.search(log_text or "")
    if m:
        return "GL: " + m.group(1).strip()
    if NO_GL_RE.search(log_text or ""):
        return "CPU rasteriser (no GL context)"
    return None


def ident_fields(work):
    """From tools.prepare's ident.json: title ID, the certificate's title name,
    the XDK build and every library's build, the engine names, the layers."""
    try:
        with open(os.path.join(work, "ident.json"), encoding="utf-8") as f:
            ident = json.load(f)
    except (OSError, ValueError):
        return {}
    xdk = ident.get("xdk") or {}
    out = {"title_id": ident.get("title_id"), "title": ident.get("title"), "xdk": xdk.get("build"),
           "layers": ident.get("layers")}
    if xdk.get("libraries"):
        out["xdk_libraries"] = xdk["libraries"]
    out["engines"] = [e.get("name") for e in ident.get("engines") or [] if e.get("name")]
    return {k: v for k, v in out.items() if v is not None}


def toolkit():
    """The toolkit's commit: tools/VERSION in the app's assets (no git there), else git."""
    try:
        with open(os.path.join(ROOT, "tools", "VERSION")) as f:
            v = f.read().split()
        if v:
            return v[0][:12]
    except OSError:
        pass
    from tools.compat.__main__ import toolkit_commit
    return toolkit_commit()


def _field(s):
    k, sep, v = s.partition("=")
    if not sep or not re.match(r"^[a-z][a-z0-9_]*$", k):
        raise ValueError("--field: key=value with a lower-case key, not %r" % s)
    try:
        return k, json.loads(v)            # numbers, true/false, objects
    except ValueError:
        return k, v


def record(a):
    """The results line for one title, as tools.compat run writes it, from the
    files of a run the app made (or a Prepare that failed, or a title the
    check could not boot)."""
    from tools.compat.__main__ import classify
    res = {"source": a.source, "toolkit": a.toolkit or toolkit(),
           "time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "host": "android"}
    if a.work:
        res.update(ident_fields(a.work))
    if a.title_id and not res.get("title_id"):
        res["title_id"] = a.title_id
    if not res.get("title"):
        res["title"] = a.source
    if a.prepare_s is not None:
        res["prepare_s"] = round(a.prepare_s)
    for f in a.field or []:
        k, v = _field(f)
        if k not in PROTECTED:
            res[k] = v
    if a.prepare_failed is not None:
        res.update(stage="none", blocker="prepare failed: " + a.prepare_failed[:200])
        return res
    if a.not_booted is not None:
        res.update(stage="prepares", best="prepares", blocker="not booted: " + a.not_booted[:200])
        return res
    log_text = ""
    if a.log:
        try:
            with open(a.log, encoding="utf-8", errors="replace") as f:
                log_text = f.read()
        except OSError:
            pass
    status = monotonic(read_jsonl(a.status)) if a.status else []
    presses = [int(p) for p in (a.presses or "").split(",") if p.strip().lstrip("-").isdigit()]
    disc = [d for d in read_jsonl(os.path.join(a.work, "discovered.jsonl")) if d.get("t") != "run"] \
        if a.work else []
    exit_info, override = parse_exit(a.exit, log_text)
    stage, blocker, summary = classify(status, presses, exit_info, a.run_s, a.sustain, log_text, disc)
    if override and (blocker is None or not blocker.startswith("launches another XBE")):
        blocker = override + ("; " + blocker if blocker and exit_info is None else "")
        if stage == "sustained" and exit_info is None:
            stage = "responsive"          # cut short: not a whole sustained window
    r = renderer_of(log_text)
    if r and "renderer" not in res:
        res["renderer"] = r
    res.update(stage=stage, blocker=blocker, best=stage,
               runs=[dict({"stage": stage, "blocker": blocker, "run_s": round(a.run_s)}, **summary,
                          screenshot=None)])
    return res


def add_arguments(g):
    g.add_argument("--out", help="append the line here (default: print only)")
    g.add_argument("--source", required=True, help="the title's name (the certificate's)")
    g.add_argument("--title-id", help="when ident.json has none")
    g.add_argument("--work", help="the title's Prepare work folder (ident.json, discovered.jsonl)")
    g.add_argument("--toolkit", help="default: tools/VERSION, else git")
    g.add_argument("--status", help="the run's RECOMP_STATUS file")
    g.add_argument("--log", help="the run's log (logcat of the game process)")
    g.add_argument("--presses", default="", help="press times in the status clock, ms, comma separated")
    g.add_argument("--exit", default="none",
                   help="none (the check ended it), exit:N, signal:N, crash[:N], java, anr, lowmem, window, lost, "
                        "away, nostart")
    g.add_argument("--run-s", type=float, default=0, help="how long the run lasted")
    g.add_argument("--sustain", type=float, default=60, help="the sustained window, seconds (default 60)")
    g.add_argument("--prepare-s", type=float, help="how long the Prepare took")
    g.add_argument("--prepare-failed", help="CODE: message; the title is « none »")
    g.add_argument("--not-booted", help="why the check did not boot it; the title is « prepares »")
    g.add_argument("--field", action="append", help="key=value added to the line (JSON values allowed)")


def grade_cmd(a):
    res = record(a)
    line = json.dumps(res, sort_keys=False)
    if a.out:
        d = os.path.dirname(os.path.abspath(a.out))
        os.makedirs(d, exist_ok=True)
        with open(a.out, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    print(line, flush=True)
    return 0


def device_label(r):
    """The Device column of the matrix: « AYN Thor, Android 13 » for a line from the app, "" for a desktop run."""
    d = r.get("device")
    if not d:
        return ""
    return "%s, Android %s" % (d, r["android"]) if r.get("android") else str(d)

