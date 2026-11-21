"""
What a Prepare cost, measured as it runs (XBA-128), and the device's say on
how many compiles may run at once.

The device log (`--device-log PATH`, written by the app, docs/android/prepare.md
« The device log »): JSON lines the app appends every few seconds and on each
thermal change,

    {"t": 1731456000, "thermal": 3, "jobs": 1, "temp_c": 41.5}

  t        seconds since the epoch
  thermal  PowerManager's thermal status (0 none ... 6 shutdown)
  jobs     compiles allowed at once from now on; 0 holds (nothing new starts,
           the running ones finish); absent: --jobs. Never above --jobs.
  temp_c   the battery's temperature, when the app has it

The compile driver reads the last whole line before each compile starts
(jobs_now); the pack step summarises the lines of this run (thermal_summary)
into pack.json's "prepare" object, with the time of each step, the compile
jobs' peak RSS (each clang's own, from wait4), the sizes and the jobs.
"""

import json
import os
import time

# How long a held compile (jobs 0) waits before it reads the log again.
HOLD_POLL_S = 1.0


def _lines(path, tail=None):
    """The whole JSON objects of the log (the last `tail` bytes only, if given)."""
    try:
        with open(path, "rb") as f:
            if tail:
                f.seek(0, os.SEEK_END)
                f.seek(max(0, f.tell() - tail))
            data = f.read()
    except (OSError, TypeError):
        return []
    end = data.rfind(b"\n")
    out = []
    for line in data[:end + 1].decode("utf-8", "replace").splitlines():
        try:
            o = json.loads(line)
        except ValueError:
            continue
        if isinstance(o, dict):
            out.append(o)
    return out


def jobs_now(path, default):
    """Compiles allowed now: the log's last "jobs", within 0..default; default without a log."""
    if not path:
        return default
    for o in reversed(_lines(path, tail=4096)):
        j = o.get("jobs")
        if isinstance(j, int) and not isinstance(j, bool):
            return max(0, min(default, j))
    return default


def thermal_summary(path, since):
    """What the log says about this run (lines from `since`, epoch seconds), or None without any."""
    rows = [o for o in _lines(path) if isinstance(o.get("t"), (int, float)) and o["t"] >= since - 1]
    if not rows:
        return None
    th = [o["thermal"] for o in rows if isinstance(o.get("thermal"), int)]
    temps = [o["temp_c"] for o in rows if isinstance(o.get("temp_c"), (int, float))]
    jobs = [o["jobs"] for o in rows if isinstance(o.get("jobs"), int)]
    severe_s = 0.0
    for a, b in zip(rows, rows[1:] + [None]):
        if isinstance(a.get("thermal"), int) and a["thermal"] >= 3:
            severe_s += ((b["t"] if b else time.time()) - a["t"])
    out = {"samples": len(rows), "max": max(th) if th else None, "severe_s": round(severe_s),
           "jobs_min": min(jobs) if jobs else None}
    if temps:
        out["temp_c_max"] = round(max(temps), 1)
    # A few samples to see the curve: every Nth, at most 24, as [seconds into the run, status].
    step = max(1, len(rows) // 24)
    out["curve"] = [[round(o["t"] - since), o.get("thermal")] for o in rows[::step]][:24]
    return out


def percentile(values, p):
    if not values:
        return None
    v = sorted(values)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


class Steps:
    """The wall time of each step of this run."""

    def __init__(self):
        self.t0 = time.time()
        self.times = {}
        self.skipped = []           # steps found done (stamps): a resumed or a repeated Prepare

    def run(self, name, fn):
        t = time.time()
        try:
            return fn()
        finally:
            self.times[name] = round(time.time() - t, 1)

    def total(self):
        return round(time.time() - self.t0, 1)


def dir_bytes(paths):
    n = 0
    for p in paths:
        try:
            n += os.path.getsize(p)
        except OSError:
            pass
    return n


def summary(steps, compile_stats, sources, objects, jobs, opt, pack_dir, device_log, tools_peak_kb=0):
    """pack.json's "prepare" object (docs/android/prepare.md « pack.json »)."""
    peaks = [k for k in compile_stats.get("peak_kb", []) if k]
    mb = lambda kb: int(round(kb / 1024.0)) if kb else None
    lib = 0
    try:
        for fn in os.listdir(pack_dir):
            if fn.endswith(".so"):
                lib += os.path.getsize(os.path.join(pack_dir, fn))
    except OSError:
        pass
    out = {"total_s": steps.total(), "steps_s": dict(steps.times), "skipped": list(steps.skipped),
           "jobs": jobs, "opt": str(opt), "files": len(sources), "compiled": compile_stats.get("compiled", 0),
           "c_mb": round(dir_bytes(sources) / 1e6, 1), "obj_mb": round(dir_bytes(objects) / 1e6, 1),
           "pack_mb": round(lib / 1e6, 1), "cpus": os.cpu_count()}
    if peaks:
        out["peak_rss_mb"] = {"max": mb(max(peaks)), "p90": mb(percentile(peaks, 90)),
                              "median": mb(percentile(peaks, 50))}
    if tools_peak_kb:
        out["tools_peak_mb"] = mb(tools_peak_kb)
    if compile_stats.get("early"):
        out["early"] = compile_stats["early"]       # files compiled while the lifter ran (compile.py EarlyCompile)
    if compile_stats.get("lift_peak_kb"):
        out["lift_peak_mb"] = mb(compile_stats["lift_peak_kb"])
    if compile_stats.get("held_s"):
        out["held_s"] = round(compile_stats["held_s"])
    th = thermal_summary(device_log, steps.t0) if device_log else None
    if th:
        out["thermal"] = th
    return out


def children_peak_kb():
    """The largest resident set of any child waited for so far (Linux: kB), 0 when unknown."""
    try:
        import resource
        return int(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
    except (ImportError, OSError, ValueError):
        return 0
