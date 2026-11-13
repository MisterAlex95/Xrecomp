"""tools.compat: see __init__.py."""

import argparse
import json
import os
import re
import signal
import subprocess
import sys
import time

from tools.compat import STAGES, device

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
# "[SEH] unhandled exception ..." (XBA-112) is a guest raise nothing handled,
# after which the title goes on: not the crash, so not its detail.
CRASH_RE = re.compile(r"(SIGSEGV|SIGBUS|SIGILL|SIGABRT|(?<!\[SEH\] )unhandled exception|access violation|"
                      r"\[FAULT\][^\n]*|fault at [^\n]*)", re.I)
# A lifted div/idiv's #DE that no guest handler took: the runtime reports it
# and bugchecks (0x1E), kernel_hal.c recomp_divide_error.
DE_RE = re.compile(r"\[#DE\] integer divide error at (0x[0-9A-Fa-f]+): ([a-z ]+) \(status 0x[0-9A-Fa-f]+\)"
                   r"[^\n]*-- ([^\n]*?), stopping")
# A guest exception nothing handled (xbox_seh.c): the raise returns.
SEH_UNHANDLED_RE = re.compile(r"\[SEH\] unhandled exception (0x[0-9A-Fa-f]{8}) at (0x[0-9A-Fa-f]{8})")
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
    # A guest stack switch the runtime could not carry (XBA-101) explains a
    # hang, a crash or an exit alike: the side that should have run never did.
    switch = next((d for d in discovered if d.get("t") == "stack_switch"), None)
    if blocker and switch and not launch:
        blocker += "; stack switch at %s to %s: %s (XBA-101)" % (switch.get("va"), switch.get("to"),
                                                                   switch.get("why"))
    elif blocker and (blocker == "no flip" or blocker.startswith(("hang", "no status"))):
        u = UNIMPL_RE.search(log_text or "")
        if u:
            blocker += "; first untranslated instruction reached: %s at %s" % (u.group(1), u.group(2))
        else:
            addressed = [d for d in discovered if d.get("t") != "stub"]   # stubs: below
            if addressed:
                d = addressed[-1]
                blocker += "; last discovery: %s %s" % (d.get("t"), d.get("va"))
    # The guest's exceptions (XBA-110 x XBA-112), with the same words on the
    # device (tools/compat/device.py grades through this function).
    de = DE_RE.search(log_text or "")
    seh = SEH_UNHANDLED_RE.search(log_text or "")
    if blocker and de and not launch:
        blocker += "; divide error at %s: %s (%s)" % (de.group(1), de.group(2), de.group(3))
    elif blocker and seh and not launch and not blocker.startswith("crash"):
        blocker += "; unhandled guest exception %s at %s" % (seh.group(1), seh.group(2))
    if blocker is None and stage != "sustained":
        blocker = {"boots": "nothing lit on screen", "renders": "no response to the pad",
                   "responsive": "not sustained"}.get(stage)
    # A stub kernel route the title called (XBA-115): named with whatever else
    # stopped the run, short of a crash or a launch, which have their own cause.
    stubs = stub_calls(discovered)
    if stubs and blocker and not blocker.startswith(("crash", "launches")):
        blocker += "; calls stub " + ", ".join(stubs)
    # Code the title wrote or made at run time (XBA-118): the lifted C cannot
    # run it, whatever the stage, so it is named even on a sustained run.
    smc = [d for d in discovered or () if d.get("t") == "smc"]
    summary["smc"] = len(smc)
    if smc and not launch:
        named = "code written or run at run time x%d: %s (XBA-118)" % (len(smc), ", ".join(smc_sites(smc)))
        blocker = blocker + "; " + named if blocker else named
    return stage, blocker, summary


def smc_sites(smc, limit=3):
    """["exec_outside_image 0x00400000 (heap, from 0x00011234)", "code_write 0x00012345 (.text)", ...]: the
    discovery file's smc entries in the order they came, at most `limit` (then "+N more")."""
    out = []
    for d in smc:
        if d.get("kind") == "code_write":
            out.append("code_write %s (%s)" % (d.get("at") or d.get("va"), d.get("section", "?")))
        else:
            out.append("%s %s (%s, from %s)" % (d.get("kind", "smc"), d.get("va"), d.get("where", "?"),
                                                d.get("ret", "?")))
    if len(out) > limit:
        out = out[:limit] + ["+%d more" % (len(out) - limit)]
    return out


def stub_calls(discovered, limit=4):
    """["ordinal 136 (KeRemoveQueue)", ...] from the discovery file's stub
    entries, in the order they were first called, at most `limit` (then "+N")."""
    out = []
    for d in discovered or ():
        if d.get("t") != "stub" or "ord" not in d:
            continue
        s = "ordinal %s" % d["ord"] + (" (%s)" % d["name"] if d.get("name") else "")
        if s not in out:
            out.append(s)
    if len(out) > limit:
        out = out[:limit] + ["+%d more" % (len(out) - limit)]
    return out


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


def keep_last_shot(shots_dir, dest):
    """The runtime's RECOMP_FB_DUMP writes one BMP per report into shots_dir;
    keep the last as dest (the screen when the run ended) and drop the rest.
    Returns dest, or None when the run dumped nothing."""
    try:
        files = sorted(f for f in os.listdir(shots_dir) if f.lower().endswith(".bmp"))
    except OSError:
        return None
    if not files:
        return None
    os.replace(os.path.join(shots_dir, files[-1]), dest)
    for f in files[:-1]:
        os.remove(os.path.join(shots_dir, f))
    try:
        os.rmdir(shots_dir)
    except OSError:
        pass
    return dest


def boot(exe, work, seconds, run_index=0):
    """Run exe headless for `seconds`; returns (status, presses_ms, exit_info, run_s, log_text)."""
    status_path = os.path.join(work, "status-%d.jsonl" % run_index)
    live = os.path.join(work, "pad-live-%d.txt" % run_index)
    log = os.path.join(work, "run-%d.log" % run_index)
    for p in (status_path, live):
        if os.path.exists(p):
            os.remove(p)
    open(live, "w").close()
    # A screenshot per report (RECOMP_FB_DUMP); the last one is kept. It stays
    # in the work folder, on this machine: it is the game's picture.
    shots = os.path.join(work, "shots-%d" % run_index)
    os.makedirs(shots, exist_ok=True)
    env = dict(os.environ, RECOMP_HEADLESS="1", RECOMP_STATUS=status_path, RECOMP_STATUS_MS="1000",
               RECOMP_PAD_LIVE=live, RECOMP_USB=os.environ.get("RECOMP_USB", "1"),
               RECOMP_FB_DUMP=os.path.join(shots, "f"))
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
    keep_last_shot(shots, os.path.join(work, "shot-%d.bmp" % run_index))
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
        shot = os.path.join(work, "shot-%d.bmp" % i)
        runs.append({"stage": stage, "blocker": blocker, "run_s": round(run_s), **summary,
                     "screenshot": shot if os.path.isfile(shot) else None})
    # The title's stage: the lowest over the runs (a race that loses 1 run in 4 is not "sustained").
    worst = min(runs, key=lambda r: STAGES.index(r["stage"]))
    best = max(runs, key=lambda r: STAGES.index(r["stage"]))
    res.update(stage=worst["stage"], blocker=worst["blocker"], best=best["stage"], runs=runs,
               smc=max(r.get("smc", 0) for r in runs))
    if a.repeat > 1:
        res["failure_rate"] = round(sum(r["stage"] != best["stage"] for r in runs) / len(runs), 2)
    return res


def report(results):
    """A Markdown table, one row per title (the last result for each)."""
    last = {}
    for r in results:
        last[r.get("title_id") or r.get("source")] = r
    on_device = any(r.get("device") for r in results)
    rows = ["| Title | Stage | Best of runs | Blocker | XDK | Toolkit |" + (" Device |" if on_device else ""),
            "|---|---|---|---|---|---|" + ("---|" if on_device else "")]
    for k, r in sorted(last.items(), key=lambda kv: -STAGES.index(kv[1].get("stage", "none"))):
        rows.append("| %s | %s | %s | %s | %s | %s |" % (
            r.get("source", k), r.get("stage"), r.get("best", r.get("stage")),
            (r.get("blocker") or "").replace("|", "/"), r.get("xdk") or "", r.get("toolkit", ""))
            + (" %s |" % device.device_label(r).replace("|", "/") if on_device else ""))
    return "\n".join(rows)


def _key(r):
    return r.get("title_id") or r.get("source")


def history(results):
    """{title: [results in file order]}."""
    by = {}
    for r in results:
        by.setdefault(_key(r), []).append(r)
    return by


def regressions(results):
    """Titles whose last run reached a lower stage than the run before it
    (XBA-73): [{"title", "source", "from", "to", "blocker", "toolkit_from", "toolkit_to"}]."""
    out = []
    for k, runs in history(results).items():
        if len(runs) < 2:
            continue
        prev, last = runs[-2], runs[-1]
        a, b = STAGES.index(prev.get("stage", "none")), STAGES.index(last.get("stage", "none"))
        if b < a:
            out.append({"title": k, "source": last.get("source", k), "from": prev.get("stage"),
                        "to": last.get("stage"), "blocker": last.get("blocker"),
                        "toolkit_from": prev.get("toolkit"), "toolkit_to": last.get("toolkit")})
    return out


def matrix(results):
    """The « Compatibility » page (XBA-73): a summary of how many titles are
    at each stage, one row per title (its last run, the change since the run
    before), and the regressions."""
    by = history(results)
    rows, counts = [], {s: 0 for s in STAGES}
    on_device = any(r.get("device") for r in results)    # lines from the app (XBA-130): a Device column
    for k, runs in sorted(by.items(), key=lambda kv: (-STAGES.index(kv[1][-1].get("stage", "none")),
                                                      str(kv[1][-1].get("source", kv[0])).lower())):
        last = runs[-1]
        stage = last.get("stage", "none")
        counts[stage] += 1
        change = ""
        if len(runs) > 1:
            d = STAGES.index(stage) - STAGES.index(runs[-2].get("stage", "none"))
            change = "up from %s" % runs[-2].get("stage") if d > 0 else \
                     "**down from %s**" % runs[-2].get("stage") if d < 0 else "same"
        rows.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
            str(last.get("source", k)).replace("|", "/"), last.get("title_id") or "", stage, change,
            (last.get("blocker") or "").replace("|", "/"), last.get("xdk") or "",
            ", ".join(l for l in (last.get("layers") or []) if l.startswith("engines/")) or "",
            "%s %s" % (last.get("toolkit", ""), (last.get("time") or "")[:10]))
            + (" %s |" % device.device_label(last).replace("|", "/") if on_device else ""))
    total = sum(counts.values()) or 1
    smc = sum(1 for runs in by.values() if runs[-1].get("smc"))
    out = ["**%d titles.** " % sum(counts.values()) + "; ".join(
               "%s: %d (%d %%)" % (s, counts[s], counts[s] * 100 // total) for s in reversed(STAGES) if counts[s])
           + ("; code written or run at run time (XBA-118): %d" % smc if smc else ""),
           "",
           "| Title | Title ID | Stage | Since the run before | Blocker | XDK | Engine | Toolkit, date |"
           + (" Device |" if on_device else ""),
           "|---|---|---|---|---|---|---|---|" + ("---|" if on_device else "")] + rows
    reg = regressions(results)
    out += ["", "**Regressions** (a Bug each): " + ("none" if not reg else "; ".join(
        "%s %s -> %s (%s -> %s)" % (r["source"], r["from"], r["to"], r["toolkit_from"], r["toolkit_to"])
        for r in reg))]
    return "\n".join(out)


def plane_sync(results, project="XBA", page="Compatibility", dry_run=False, run=subprocess.run):
    """Write the matrix to the Plane page and open a Bug per regression not
    already open (the plane CLI, as on the agents' host). Returns the bug
    titles it created (or would create)."""
    import tempfile
    md = "# Compatibility\n\nWritten by `python -m tools.compat plane` from the compat runner's results " \
         "(docs/technical/compat-runner.md).\n\n" + matrix(results) + "\n"
    created = []
    existing = ""
    if not dry_run:
        with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False) as f:
            f.write(md)
        run(["plane", "page", "write", "-p", project, "-t", page, "-f", f.name], check=False)
        os.remove(f.name)
        existing = run(["plane", "list", "-p", project], capture_output=True, text=True, check=False).stdout or ""
    for r in regressions(results):
        title = "Regression: %s %s -> %s (toolkit %s)" % (r["source"], r["from"], r["to"], r["toolkit_to"])
        if title in existing:
            continue
        created.append(title)
        if not dry_run:
            run(["plane", "create", "-p", project, "-T", "Bug", "-t", title, "--parent", "XBA-60", "-m",
                 "Found by the compat runner (XBA-73). Was %s with toolkit %s, now %s with toolkit %s. Blocker: %s. "
                 "Results: the run's compat-results.jsonl; bisect between the two toolkit commits."
                 % (r["from"], r["toolkit_from"], r["to"], r["toolkit_to"], r["blocker"] or "none")],
                check=False)
    return created


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
    p.add_argument("--matrix", action="store_true", help="the « Compatibility » page instead (summary, changes, regressions)")
    q = sub.add_parser("plane", help="write the matrix to the Plane page, a Bug per regression (agents' host)")
    q.add_argument("results")
    q.add_argument("--project", default="XBA")
    q.add_argument("--dry-run", action="store_true")
    g = sub.add_parser("grade", help="grade a run made elsewhere (the app's library check), append its line")
    device.add_arguments(g)
    a = ap.parse_args(argv)
    if a.cmd == "grade":
        return device.grade_cmd(a)
    if a.cmd == "report":
        res = read_jsonl(a.results)
        print(matrix(res) if a.matrix else report(res))
        return 0
    if a.cmd == "plane":
        for t in plane_sync(read_jsonl(a.results), a.project, dry_run=a.dry_run):
            print(("would open: " if a.dry_run else "opened: ") + t)
        return 0
    for src in a.sources:
        res = run_one(src, a)
        with open(a.out, "a") as f:
            f.write(json.dumps(res) + "\n")
        print("%-40s %-10s %s" % (res["source"], res["stage"], res.get("blocker") or ""), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
