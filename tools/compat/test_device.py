"""tools.compat grade: the app's library check (XBA-130) grades its runs with
the desktop rules and writes the desktop schema; report and plane read it."""

import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.compat import device                                      # noqa: E402
from tools.compat.__main__ import classify, main, matrix, plane_sync, report    # noqa: E402
from tools.compat.test_compat import line, playing                   # noqa: E402

# The keys every line has, desktop or device: report, matrix and plane read these.
SCHEMA = {"source", "toolkit", "time", "stage", "blocker"}
RUN_KEYS = {"stage", "blocker", "run_s", "flips", "frames", "audio_ms", "pad_reads", "threads", "discovered", "lit",
            "screenshot"}


def _files(tmp_path, status, log="", ident=None, discovered=()):
    work = tmp_path / "work"
    work.mkdir(exist_ok=True)
    if ident is not None:
        (work / "ident.json").write_text(json.dumps(ident))
    if discovered:
        (work / "discovered.jsonl").write_text("".join(json.dumps(d) + "\n" for d in discovered))
    st = tmp_path / "status.jsonl"
    st.write_text("".join(json.dumps(s) + "\n" for s in status))
    lg = tmp_path / "run.log"
    lg.write_text(log)
    return work, st, lg


IDENT = {"title_id": "58424101", "title": "Smoke", "layers": ["xdk/d3d8", "engines/renderware"],
         "xdk": {"build": 5849, "mixed": False, "libraries": {"XAPILIB": 5849, "D3D8": 5849}},
         "engines": [{"name": "RenderWare", "kind": "engine", "markers": ["our marker"]}]}


def _grade(tmp_path, *args):
    out = tmp_path / "compat-results.jsonl"
    rc = main(["grade", "--out", str(out), "--source", "Smoke", "--toolkit", "abc123"] + list(args))
    assert rc == 0
    return [json.loads(l) for l in out.read_text().splitlines()]


def test_grade_uses_the_desktop_rules_and_schema(tmp_path):
    st = playing(180)
    work, sp, lg = _files(tmp_path, st, "  [GPUGL] Adreno (TM) 740 | OpenGL ES 3.2 | GLSL 3.20\n", IDENT)
    lines = _grade(tmp_path, "--work", str(work), "--status", str(sp), "--log", str(lg), "--presses",
                   "8000,10000", "--run-s", "180", "--prepare-s", "612.4",
                   "--field", "device=AYN Thor", "--field", "android=13", "--field", "peak_mem_mb=2310",
                   "--field", "app_build=abc123 (7)")
    r = lines[-1]
    stage, blocker, summary = classify(st, [8000, 10000], None, 180, 60, "", [])
    assert (r["stage"], r["blocker"]) == (stage, blocker) == ("sustained", None)
    assert SCHEMA <= set(r) and set(r["runs"][0]) == RUN_KEYS
    assert r["title_id"] == "58424101" and r["title"] == "Smoke" and r["source"] == "Smoke"
    assert r["xdk"] == 5849 and r["xdk_libraries"] == {"XAPILIB": 5849, "D3D8": 5849}
    assert r["engines"] == ["RenderWare"] and r["layers"] == IDENT["layers"]
    assert r["device"] == "AYN Thor" and r["android"] == 13 and r["peak_mem_mb"] == 2310
    assert r["renderer"] == "GL: Adreno (TM) 740" and r["prepare_s"] == 612 and r["host"] == "android"
    assert r["toolkit"] == "abc123" and r["runs"][0]["screenshot"] is None
    # Nothing from the XBE beyond names and numbers: the engine markers stay out.
    assert "our marker" not in json.dumps(r)


def test_grade_crash_unimpl_and_discovery(tmp_path):
    st = [line(t, threads=2) for t in range(1000, 20001, 1000)]
    log = "F libc: Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0 in tid 123\n"
    work, sp, lg = _files(tmp_path, st, log, IDENT)
    r = _grade(tmp_path, "--work", str(work), "--status", str(sp), "--log", str(lg), "--exit", "crash",
               "--run-s", "20")[-1]
    assert r["stage"] == "boots" and r["blocker"].startswith("crash (signal 11): SIGSEGV")
    r = _grade(tmp_path, "--work", str(work), "--status", str(sp), "--log", str(lg), "--exit", "exit:3",
               "--run-s", "20")[-1]
    assert r["blocker"] == "exited with code 3 after 20 s"
    work, sp, lg = _files(tmp_path, st, "", IDENT, [{"t": "run"}, {"t": "icall", "va": "0x00045678"}])
    r = _grade(tmp_path, "--work", str(work), "--status", str(sp), "--log", str(lg), "--run-s", "180")[-1]
    assert r["blocker"] == "no flip; last discovery: icall 0x00045678"


def test_grade_android_exits(tmp_path):
    st = playing(40)
    work, sp, lg = _files(tmp_path, st, "", IDENT)
    r = _grade(tmp_path, "--work", str(work), "--status", str(sp), "--exit", "lowmem", "--run-s", "40")[-1]
    assert r["blocker"] == "ended by the system: low memory"
    r = _grade(tmp_path, "--work", str(work), "--status", str(sp), "--exit", "anr", "--run-s", "40")[-1]
    assert r["blocker"] == "not responding (ANR)"
    r = _grade(tmp_path, "--work", str(work), "--status", str(sp), "--exit", "crash", "--run-s", "40")[-1]
    assert r["blocker"] == "native crash"
    # The check died during the boot (app killed, device rebooted): what the run left is graded, and said.
    r = _grade(tmp_path, "--work", str(work), "--status", str(sp), "--exit", "lost", "--run-s", "40",
               "--presses", "8000")[-1]
    assert r["stage"] == "responsive" and r["blocker"].startswith("the check was interrupted")


def test_prepare_failed_and_not_booted(tmp_path):
    work, _, _ = _files(tmp_path, [], "", IDENT)
    r = _grade(tmp_path, "--work", str(work), "--prepare-failed", "COMPILE_FAILED: gen_12.c: error")[-1]
    assert (r["stage"], r["blocker"]) == ("none", "prepare failed: COMPILE_FAILED: gen_12.c: error")
    assert r["title_id"] == "58424101"
    r = _grade(tmp_path, "--title-id", "58424102", "--not-booted", "the game's files are not in the app")[-1]
    assert (r["stage"], r["title_id"]) == ("prepares", "58424102")
    assert r["blocker"] == "not booted: the game's files are not in the app"


def test_field_cannot_replace_the_grade(tmp_path):
    work, sp, _ = _files(tmp_path, playing(30), "", IDENT)
    r = _grade(tmp_path, "--work", str(work), "--status", str(sp), "--run-s", "30", "--presses", "8000",
               "--field", "stage=sustained",
               "--field", "title_id=00000000")[-1]
    assert r["stage"] == "responsive" and r["title_id"] == "58424101"


def test_a_quick_reboot_keeps_one_clock():
    st = [line(1000, flips=10), line(2000, flips=20), line(1000, flips=5), line(2000, flips=9)]
    assert [s["ms"] for s in device.monotonic(st)] == [1000, 2000, 3000, 4000]
    assert device.monotonic(playing(10)) == playing(10)


def test_report_and_matrix_read_a_device_file(tmp_path):
    desktop = {"source": "Smoke", "title_id": "58424101", "stage": "boots", "blocker": "no flip", "xdk": 5849,
               "toolkit": "aaa", "time": "2026-11-10T00:00:00Z"}
    work, sp, lg = _files(tmp_path, playing(180), "", IDENT)
    dev = _grade(tmp_path, "--work", str(work), "--status", str(sp), "--presses", "8000", "--run-s", "180",
                 "--field", "device=AYN Thor", "--field", "android=13", "--field", "unknown_future_key=1")[-1]
    res = [desktop, dev]
    m = matrix(res)
    assert "| Toolkit, date | Device |" in m
    row = next(l for l in m.splitlines() if l.startswith("| Smoke"))
    assert "| sustained | up from boots |" in row and row.endswith("| AYN Thor, Android 13 |")
    assert row.count("|") == 10
    t = report(res)
    assert t.splitlines()[0].endswith("Device |") and "AYN Thor, Android 13" in t
    # A desktop-only file keeps its table as before.
    assert "Device" not in matrix([desktop]) and "Device" not in report([desktop])
    calls = []
    assert plane_sync(res, dry_run=True, run=lambda *a, **k: calls.append(a)) == []


def test_the_cli_reads_the_file_the_device_wrote(tmp_path):
    work, sp, lg = _files(tmp_path, playing(60), "", IDENT)
    _grade(tmp_path, "--work", str(work), "--status", str(sp), "--presses", "8000", "--run-s", "60",
           "--field", "device=AYN Thor")
    out = tmp_path / "compat-results.jsonl"
    for args in (["report", str(out)], ["report", str(out), "--matrix"], ["plane", str(out), "--dry-run"]):
        r = subprocess.run([sys.executable, "-m", "tools.compat"] + args, cwd=ROOT, capture_output=True,
                           text=True, timeout=60)
        assert r.returncode == 0, r.stderr
    r = subprocess.run([sys.executable, "-m", "tools.compat", "report", str(out), "--matrix"], cwd=ROOT,
                       capture_output=True, text=True, timeout=60)
    assert "AYN Thor" in r.stdout and "| Smoke |" in r.stdout


def test_the_renderer_is_what_really_drew():
    """XBA-130 x XBA-119: the GPU by default, and each way it falls back."""
    from tools.compat.device import renderer_of
    gl = "  [GPUGL] GPU back end by default on this platform (RECOMP_GPU=0 for the software renderer)\n" \
         "  [GPUGL] GPU back end selected (RECOMP_GPU=1); the context is created on the executor thread\n"
    line = "  [GPUGL] Adreno (TM) 740 | OpenGL ES 3.2 | GLSL 3.20\n"
    assert renderer_of(gl + line) == "GL: Adreno (TM) 740"
    assert renderer_of(gl + line + "  [GPUGL] the base vertex shader does not compile: falling back to the CPU "
                       "rasteriser\n") == "CPU rasteriser (GL Adreno (TM) 740: shader failed)"
    assert renderer_of(gl + line + "  [GPUGL] context lost at frame 812: dropping every GL object; the CPU "
                       "rasteriser takes over\n") == "CPU rasteriser (GL Adreno (TM) 740: gave up after GL resets)"
    assert renderer_of(gl + "  [GPUGL] no GL context: falling back to the CPU rasteriser\n") \
        == "CPU rasteriser (no GL context)"
    assert renderer_of("  [GPUGL] CPU rasteriser selected (RECOMP_GPU=0)\n") == "CPU rasteriser (RECOMP_GPU=0)"
    assert renderer_of(gl) == "GPU selected (no frame drawn)"
    assert renderer_of("no GPU lines at all\n") is None


def test_the_device_names_the_new_blockers_as_the_desktop_does(tmp_path):
    """wave1/1b's blocker kinds through `grade`, word for word classify()'s:
    a stub route, a stack switch the runtime could not carry, a refused
    cross-stack SEH resume (recorded as a stack_switch), an unhandled guest
    exception and a divide error no handler took."""
    st = [line(t) for t in range(1000, 20001, 1000)]
    disc = [{"t": "stub", "ord": 136, "name": "KeRemoveQueue", "ret": "0x00012345"},
            {"t": "stack_switch", "va": "0x000100B1", "to": "0x00010078",
             "why": "an exception handler resumes into a frame on another host stack (SEH, XBA-112)"}]
    seh = "  [SEH] unhandled exception 0xC0000005 at 0x00045678 (record 0x0017FE00); execution continues after the raise\n"
    de = ("[#DE] integer divide error at 0x00012340: divide by zero (status 0xC0000094); eax=00000007 edx=00000000 "
          "divisor=00000000 esp=0017FF00 -- no guest handler took it, stopping\n[EXIT] KeBugCheckEx 0x0000001E\n")
    for log, exit_arg, exit_info, discovered in ((seh, "none", None, disc), (de, "exit:30", ("exit", 30), ()),
                                                  (seh, "none", None, ())):
        sub = tmp_path / ("c%d" % len(list(tmp_path.iterdir())))
        sub.mkdir()
        work, sp, lg = _files(sub, st, log, IDENT, discovered)
        r = _grade(sub, "--work", str(work), "--status", str(sp), "--log", str(lg), "--exit", exit_arg,
                   "--run-s", "20")[-1]
        want = classify(st, [], exit_info, 20, 60, log, list(discovered))[1]
        assert r["blocker"] == want, (r["blocker"], want)
        if discovered:
            assert "stack switch at 0x000100B1" in want and "calls stub ordinal 136 (KeRemoveQueue)" in want, want
        elif log is de:
            assert "divide error at 0x00012340: divide by zero (no guest handler took it)" in want, want
        else:
            assert want.endswith("unhandled guest exception 0xC0000005 at 0x00045678"), want
