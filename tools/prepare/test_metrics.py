"""XBA-128: what a Prepare costs, measured as it runs, and the device's say on
the compiles allowed at once (tools/prepare/metrics.py, compile.compile_all)."""

import json
import os
import stat
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.prepare import compile as cc      # noqa: E402
from tools.prepare import metrics            # noqa: E402

# A stand-in compiler: --version, else holds ~8 MB for a moment and writes -o. It keeps a count of the
# compiles running at once in COUNT (a line per start "+", per end "-").
FAKE_CC = r'''#!/usr/bin/env python3
import os, sys, time
if "--version" in sys.argv:
    print("clang version 19.0.1 (fake)")
    sys.exit(0)
count = os.environ["FAKE_CC_COUNT"]
with open(count, "a") as f:
    f.write("+\n")
blob = bytearray(8 << 20)
for i in range(0, len(blob), 4096):
    blob[i] = 1
time.sleep(0.3)
out = sys.argv[sys.argv.index("-o") + 1]
with open(out, "wb") as f:
    f.write(b"obj")
with open(count, "a") as f:
    f.write("-\n")
'''


def _fake_cc(tmp_path):
    p = tmp_path / "fakecc"
    p.write_text(FAKE_CC)
    p.chmod(p.stat().st_mode | stat.S_IEXEC)
    return str(p)


def _max_running(count_file):
    n = top = 0
    for line in open(count_file).read().split():
        n += 1 if line == "+" else -1
        top = max(top, n)
    return top


def _sources(tmp_path, n):
    d = tmp_path / "src"
    d.mkdir()
    out = []
    for i in range(n):
        p = d / ("f%d.c" % i)
        p.write_text("int f%d(void) { return %d; }\n" % (i, i) * (i + 1))
        out.append(str(p))
    return out


def test_jobs_now_reads_the_last_whole_line(tmp_path):
    log = tmp_path / "device.jsonl"
    assert metrics.jobs_now(None, 4) == 4 and metrics.jobs_now(str(log), 4) == 4      # no log: --jobs
    log.write_text('{"t": 1, "thermal": 1, "jobs": 4}\n{"t": 2, "thermal": 3, "jobs": 1}\n{"t": 3, "jobs": 0')
    assert metrics.jobs_now(str(log), 4) == 1          # the torn last line waits
    with open(log, "a") as f:
        f.write("}\n")
    assert metrics.jobs_now(str(log), 4) == 0
    log.write_text('{"t": 4, "jobs": 9}\n')
    assert metrics.jobs_now(str(log), 3) == 3          # never above --jobs
    log.write_text('{"t": 4, "thermal": 2}\n')
    assert metrics.jobs_now(str(log), 3) == 3          # no "jobs": --jobs


def test_thermal_summary(tmp_path):
    log = tmp_path / "device.jsonl"
    t0 = 1000
    rows = [{"t": t0 - 50, "thermal": 0, "jobs": 4}] + \
           [{"t": t0 + 10 * i, "thermal": [1, 2, 3, 3, 2, 1][i], "jobs": [4, 4, 1, 1, 4, 4][i],
             "temp_c": 30 + i} for i in range(6)]
    log.write_text("".join(json.dumps(r) + "\n" for r in rows))
    s = metrics.thermal_summary(str(log), t0)
    assert s["samples"] == 6 and s["max"] == 3 and s["severe_s"] == 20 and s["jobs_min"] == 1
    assert s["temp_c_max"] == 35 and s["curve"][0] == [0, 1]
    assert metrics.thermal_summary(str(tmp_path / "none"), t0) is None


def test_compile_all_measures_each_job_and_obeys_the_device(tmp_path, monkeypatch):
    count = tmp_path / "count"
    monkeypatch.setenv("FAKE_CC_COUNT", str(count))
    tc = cc.Toolchain(_fake_cc(tmp_path), str(tmp_path))
    srcs = _sources(tmp_path, 6)
    # The device allows one compile at a time although --jobs is 4 (a severe thermal status).
    stats = {}
    objs, n = cc.compile_all(srcs, ["-O2"], tc, str(tmp_path / "obj"), 4, limit=lambda: 1, stats=stats)
    assert n == 6 and all(os.path.isfile(o) for o in objs)
    assert _max_running(count) == 1
    assert stats["compiled"] == 6 and len(stats["peak_kb"]) == 6
    if hasattr(os, "wait4"):
        assert min(stats["peak_kb"]) > 8 * 1024        # each compile's own peak: the 8 MB it held
    # Without a limit, --jobs: several at once.
    count.write_text("")
    (tmp_path / "b").mkdir()
    cc.compile_all(_sources(tmp_path / "b", 6), ["-O2"], tc, str(tmp_path / "obj2"), 4)
    assert _max_running(count) > 1


def test_compile_all_holds_at_zero_then_goes_on(tmp_path, monkeypatch):
    count = tmp_path / "count"
    monkeypatch.setenv("FAKE_CC_COUNT", str(count))
    tc = cc.Toolchain(_fake_cc(tmp_path), str(tmp_path))
    allowed = [0]
    threading.Timer(1.5, lambda: allowed.__setitem__(0, 2)).start()
    stats = {}
    t0 = time.time()
    objs, n = cc.compile_all(_sources(tmp_path, 3), ["-O1"], tc, str(tmp_path / "obj"), 4,
                             limit=lambda: allowed[0], stats=stats)
    assert n == 3 and time.time() - t0 >= 1.5
    assert stats["held_s"] >= 1.0 and _max_running(count) <= 2


def test_a_cancel_during_a_hold_ends_the_compile(tmp_path, monkeypatch):
    monkeypatch.setenv("FAKE_CC_COUNT", str(tmp_path / "count"))
    tc = cc.Toolchain(_fake_cc(tmp_path), str(tmp_path))
    tc.version_text()
    threading.Timer(0.5, tc.procs.cancel).start()
    try:
        cc.compile_all(_sources(tmp_path, 2), ["-O1"], tc, str(tmp_path / "obj"), 2, limit=lambda: 0)
        raise AssertionError("not cancelled")
    except cc.Cancelled:
        pass


def test_summary(tmp_path):
    steps = metrics.Steps()
    steps.times = {"identify": 1.0, "analyse": 20.5, "lift": 30.0, "compile": 200.0, "pack": 3.0}
    steps.skipped = ["parse", "disasm"]
    src = tmp_path / "a.c"
    src.write_bytes(b"x" * 2_000_000)
    obj = tmp_path / "a.o"
    obj.write_bytes(b"o" * 1_000_000)
    part = tmp_path / "pack"
    part.mkdir()
    (part / "libtitle.so").write_bytes(b"l" * 3_000_000)
    log = tmp_path / "device.jsonl"
    log.write_text(json.dumps({"t": time.time(), "thermal": 3, "jobs": 1, "temp_c": 40.5}) + "\n")
    s = metrics.summary(steps, {"peak_kb": [100 * 1024, 300 * 1024, 600 * 1024], "compiled": 3, "held_s": 12.4},
                        [str(src)], [str(obj)], 4, "2", str(part), str(log), tools_peak_kb=900 * 1024)
    assert s["steps_s"]["compile"] == 200.0 and s["skipped"] == ["parse", "disasm"]
    assert (s["c_mb"], s["obj_mb"], s["pack_mb"]) == (2.0, 1.0, 3.0)
    assert s["peak_rss_mb"] == {"max": 600, "p90": 600, "median": 300} and s["tools_peak_mb"] == 900
    assert s["held_s"] == 12 and s["thermal"]["max"] == 3 and s["thermal"]["temp_c_max"] == 40.5
    assert s["jobs"] == 4 and s["opt"] == "2" and s["compiled"] == 3


def test_pack_json_carries_what_the_prepare_cost(tmp_path, monkeypatch):
    from tools.prepare.test_android_prepare import _make_pack
    meta, links = _make_pack(tmp_path, monkeypatch, {})
    p = meta["prepare"]
    assert len(links) == 1 and set(p) >= {"total_s", "steps_s", "skipped", "jobs", "opt", "files", "compiled",
                                         "c_mb", "obj_mb", "pack_mb", "cpus"}
    assert "thermal" not in p           # no device log on a host
