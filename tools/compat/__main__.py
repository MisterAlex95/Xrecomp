"""tools.compat: see __init__.py."""

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time

from tools.compat import STAGES

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# The universal input: Start a few times, then A, every PRESS_EVERY seconds.
PRESS_FIRST = 8.0
PRESS_EVERY = 2.0
RESPONSE_WINDOW_MS = 5000
STALL_S = 30


# ── Reading a run ───────────────────────────────────────────────

def read_jsonl(path):
    out = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                try:
                    out.append(json.loads(line))
                except ValueError:
                    continue            # a torn last line
    except OSError:
        pass
    return out


LAUNCH_RE = re.compile(r"\[KERNEL\] launch of '([^']*)' is not this title")
CRASH_RE = re.compile(r"(SIGSEGV|SIGBUS|SIGILL|SIGABRT|unhandled exception|access violation|"
                      r"\[FAULT\][^\n]*|fault at [^\n]*)", re.I)
UNIMPL_RE = re.compile(r"\[UNIMPL\] untranslated instruction REACHED: `([^`]*)` at (0x[0-9A-Fa-f]+)")


def classify(status, presses_ms, exit_info, run_s, sustain_s, log_text="", discovered=()):
    """(stage, blocker, summary) from what a run left.

    status: the status file's lines; presses_ms: when the runner pressed,
    in the status clock; exit_info: None when the runner stopped it, else
    ("exit", code) or ("signal", n)."""
    last = status[-1] if status else {}
    summary = {k: last.get(k, 0) for k in ("flips", "frames", "audio_ms", "pad_reads", "threads", "discovered")}
    summary["lit"] = int(any(s.get("lit") for s in status))
    stage = "prepares"
    if status and any(s.get("flips") or s.get("threads") or s.get("pad_reads") or s.get("audio_ms")
                      for s in status):
        stage = "boots"
        if any(s.get("lit") and s.get("flips") for s in status):
            stage = "renders"
            if summary["pad_reads"] and _responds(status, presses_ms):
                stage = "responsive"
                if exit_info is None and _sustained(status, sustain_s):
                    stage = "sustained"

    blocker = None
    m = CRASH_RE.search(log_text or "")
    launch = LAUNCH_RE.search(log_text or "")
    if launch:
        blocker = "launches another XBE: %s (multi-XBE discs, XBA-82)" % launch.group(1)
    elif exit_info and exit_info[0] == "signal":
        blocker = "crash (signal %d)%s" % (exit_info[1], ": " + m.group(1)[:120] if m else "")
    elif exit_info:
        blocker = "exited with code %d after %.0f s" % (exit_info[1], run_s)
    elif not status:
        blocker = "no status line: the runtime did not start"
    elif not summary["flips"]:
        blocker = "no flip"
    else:
        stall = _stalled_for(status)
        if stall >= STALL_S:
            blocker = "hang: no flip for the last %d s" % stall
    if blocker and (blocker == "no flip" or blocker.startswith(("hang", "no status"))):
        u = UNIMPL_RE.search(log_text or "")
        if u:
            blocker += "; first untranslated instruction reached: %s at %s" % (u.group(1), u.group(2))
        elif discovered:
            d = discovered[-1]
            blocker += "; last discovery: %s %s" % (d.get("t"), d.get("va"))
    if blocker is None and stage != "sustained":
        blocker = {"boots": "nothing lit on screen", "renders": "no response to the pad",
                   "responsive": "not sustained"}.get(stage)
    return stage, blocker, summary


def _responds(status, presses_ms):
    """The picture changed within RESPONSE_WINDOW_MS after at least one press."""
    for p in presses_ms:
        before = [s.get("frames", 0) for s in status if s.get("ms", 0) <= p]
        after = [s.get("frames", 0) for s in status if p < s.get("ms", 0) <= p + RESPONSE_WINDOW_MS]
        if before and after and max(after) > before[-1]:
            return True
    return False


def _sustained(status, sustain_s):
    end = status[-1].get("ms", 0)
    if end < sustain_s * 1000:
        return False
    window = [s for s in status if s.get("ms", 0) >= end - sustain_s * 1000]
    if len(window) < 2 or window[-1].get("frames", 0) <= window[0].get("frames", 0):
        return False
    t = window[0].get("ms", 0)
    while t + 10000 <= end:
        seg = [s.get("flips", 0) for s in window if t <= s.get("ms", 0) <= t + 10000]
        if len(seg) < 2 or seg[-1] <= seg[0]:
            return False
        t += 10000
    return True


def _stalled_for(status):
    """Seconds since the flip count last moved."""
    last = status[-1]
    for s in reversed(status):
        if s.get("flips", 0) != last.get("flips", 0):
            return int((last.get("ms", 0) - s.get("ms", 0)) / 1000)
    return int(last.get("ms", 0) / 1000)


# ── Running ─────────────────────────────────────────────────────

def project_exe(work):
    """The executable tools.prepare built in <work>/project/build."""
    try:
        with open(os.path.join(work, "project", "CMakeLists.txt")) as f:
            m = re.search(r"^project\((\S+)", f.read(), re.M)
    except OSError:
        return None
    if not m:
        return None
    for name in (m.group(1), m.group(1) + ".exe"):
        p = os.path.join(work, "project", "build", name)
        if os.path.isfile(p):
            return p
    return None


def _lower_priority():
    try:
        os.nice(10)
    except OSError:
        pass


def prepare(source, work, log):
    t0 = time.time()
    with open(log, "w") as f:
        rc = subprocess.call([sys.executable, "-m", "tools.prepare", source, "--work", work],
                             cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, preexec_fn=_lower_priority)
    tail = ""
    if rc:
        with open(log, errors="replace") as f:
            lines = [l.strip() for l in f if l.strip()]
        tail = next((l for l in reversed(lines) if not l.startswith("---")), "")[:200]
    return rc, time.time() - t0, tail


def boot(exe, work, seconds, run_index=0):
    """Run exe headless for `seconds`; returns (status, presses_ms, exit_info, run_s, log_text)."""
    status_path = os.path.join(work, "status-%d.jsonl" % run_index)
    live = os.path.join(work, "pad-live-%d.txt" % run_index)
    log = os.path.join(work, "run-%d.log" % run_index)
    for p in (status_path, live):
        if os.path.exists(p):
            os.remove(p)
    open(live, "w").close()
    env = dict(os.environ, RECOMP_HEADLESS="1", RECOMP_STATUS=status_path, RECOMP_STATUS_MS="1000",
               RECOMP_PAD_LIVE=live, RECOMP_USB=os.environ.get("RECOMP_USB", "1"))
    t0 = time.time()
    with open(log, "w") as out:
        proc = subprocess.Popen([exe], cwd=os.path.dirname(exe), env=env, stdout=out,
                                stderr=subprocess.STDOUT, start_new_session=True,
                                preexec_fn=_lower_priority)
    presses = []
    next_press, n = PRESS_FIRST, 0
    exit_info = None
    while True:
        rc = proc.poll()
        el = time.time() - t0
        if rc is not None:
            exit_info = ("signal", -rc) if rc < 0 else ("exit", rc)
            break
        if el >= seconds:
            break
        if el >= next_press:
            with open(live, "a") as f:
                f.write(("start" if n < 3 else "a") + "\n")
            presses.append(int(el * 1000))
            n += 1
            next_press += PRESS_EVERY
        time.sleep(0.2)
    run_s = time.time() - t0
    if exit_info is None:
        for sig, wait in ((signal.SIGTERM, 5), (signal.SIGKILL, 5)):
            try:
                os.kill(proc.pid, sig)          # by PID, never by name
            except ProcessLookupError:
                break
            try:
                proc.wait(wait)
                break
            except subprocess.TimeoutExpired:
                continue
    with open(log, errors="replace") as f:
        log_text = f.read()
    return read_jsonl(status_path), presses, exit_info, run_s, log_text


def toolkit_commit():
    try:
        return subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def run_one(source, a):
    name = os.path.basename(os.path.normpath(source))
    work = os.path.join(os.path.abspath(a.work_root), re.sub(r"[^A-Za-z0-9._-]", "_", name))
    os.makedirs(work, exist_ok=True)
    res = {"source": name, "toolkit": toolkit_commit(), "time": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    if not a.no_prepare:
        rc, secs, tail = prepare(source, work, os.path.join(work, "compat-prepare.log"))
        res["prepare_s"] = round(secs)
        if rc:
            res.update(stage="none", blocker="prepare failed: " + tail)
            return res
    exe = project_exe(work)
    if not exe:
        res.update(stage="none", blocker="no executable in %s/project/build" % work)
        return res
    try:
        with open(os.path.join(work, "ident.json")) as f:
            ident = json.load(f)
        res["title_id"] = ident.get("title_id")
        res["xdk"] = ident.get("xdk", {}).get("build")
        res["layers"] = ident.get("layers")
    except (OSError, ValueError):
        pass
    runs = []
    for i in range(a.repeat):
        status, presses, exit_info, run_s, log_text = boot(exe, work, a.seconds, i)
        disc = [d for d in read_jsonl(os.path.join(work, "discovered.jsonl")) if d.get("t") != "run"]
        stage, blocker, summary = classify(status, presses, exit_info, run_s, a.sustain, log_text, disc)
        runs.append({"stage": stage, "blocker": blocker, "run_s": round(run_s), **summary})
    # The title's stage: the lowest over the runs (a race that loses 1 run in 4 is not "sustained").
    worst = min(runs, key=lambda r: STAGES.index(r["stage"]))
    best = max(runs, key=lambda r: STAGES.index(r["stage"]))
    res.update(stage=worst["stage"], blocker=worst["blocker"], best=best["stage"], runs=runs)
    if a.repeat > 1:
        res["failure_rate"] = round(sum(r["stage"] != best["stage"] for r in runs) / len(runs), 2)
    return res


def report(results):
    """A Markdown table, one row per title (the last result for each)."""
    last = {}
    for r in results:
        last[r.get("title_id") or r.get("source")] = r
    rows = ["| Title | Stage | Best of runs | Blocker | XDK | Toolkit |", "|---|---|---|---|---|---|"]
    for k, r in sorted(last.items(), key=lambda kv: -STAGES.index(kv[1].get("stage", "none"))):
        rows.append("| %s | %s | %s | %s | %s | %s |" % (
            r.get("source", k), r.get("stage"), r.get("best", r.get("stage")),
            (r.get("blocker") or "").replace("|", "/"), r.get("xdk") or "", r.get("toolkit", "")))
    return "\n".join(rows)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.compat", description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="prepare and boot titles, append results")
    r.add_argument("sources", nargs="+", help="disc images, game folders or default.xbe files")
    r.add_argument("--work-root", default="compat-work", help="one work folder per title under it")
    r.add_argument("--seconds", type=float, default=180, help="how long each boot runs (default 180)")
    r.add_argument("--sustain", type=float, default=60, help="the sustained window, seconds (default 60)")
    r.add_argument("--repeat", type=int, default=1, help="boots per title: the stage is the worst run")
    r.add_argument("--no-prepare", action="store_true", help="reuse the work folder's build")
    r.add_argument("--out", default="compat-results.jsonl", help="results, appended (default compat-results.jsonl)")
    p = sub.add_parser("report", help="Markdown table from a results file")
    p.add_argument("results")
    a = ap.parse_args(argv)
    if a.cmd == "report":
        print(report(read_jsonl(a.results)))
        return 0
    for src in a.sources:
        res = run_one(src, a)
        with open(a.out, "a") as f:
            f.write(json.dumps(res) + "\n")
        print("%-40s %-10s %s" % (res["source"], res["stage"], res.get("blocker") or ""), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
