"""check_expected.py on synthetic reports (XBA-53). Runs with the rest of `pytest tools/` in the linux job."""
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
CHECKER = os.path.join(HERE, "check_expected.py")

HEADER = "| Project | Configure | Build | Passed | Skipped | Failed |\n|---|---|---|---|---|---|\n"
BASE_ROWS = {
    "adpcm_decode": "ok | ok | 1 | 0 | 0",
    "apu_vp": "windows | - | - | - | -",
    "d3d8_smoke": "ok | ok | 0 | 0 | 0",
    "mmio_decode_a64": "ok | ok | 0 | 0 | 1",
    "mmx_helpers": "ok | ok | 0 | 1 | 0",
}
PASS = ("run", "")
FAIL = ("fail", '<failure message="Failed"/>')
SKIP = ("notrun", '<skipped message="SKIP_RETURN_CODE=77"/>')
NOEXE = ("notrun", '<skipped message="Unable to find executable"/>')
BASE_JUNIT = {
    "adpcm_decode": {"adpcm_decode": PASS},
    "mmio_decode_a64": {"mmio_decode_a64": FAIL},
    "mmx_helpers": {"mmx_helpers": SKIP},
    "root": {"mmx_helpers": SKIP, "xmm_helpers": SKIP},
}
EXPECTED = "# comment\n\nmmio_decode_a64/mmio_decode_a64   # qemu-user\n"


def write_ctest(tmp, rows=None, junit=None, expected=EXPECTED):
    rows = dict(BASE_ROWS, **(rows or {}))
    junit = dict(BASE_JUNIT, **(junit or {}))
    (tmp / "report.md").write_text(HEADER + "".join(f"| {p} | {r} |\n" for p, r in rows.items()))
    jdir = tmp / "junit"
    jdir.mkdir()
    for proj, tests in junit.items():
        if tests is None:
            continue
        cases = "".join(f'<testcase name="{t}" classname="{t}" time="0" status="{s}">{body}</testcase>\n'
                        for t, (s, body) in tests.items())
        (jdir / f"{proj}.xml").write_text(f'<?xml version="1.0" encoding="UTF-8"?>\n<testsuite name="x">\n{cases}</testsuite>\n')
    (tmp / "expected.txt").write_text(expected)
    return ["ctest", "--expected", str(tmp / "expected.txt"), "--report", str(tmp / "report.md"), "--junit", str(jdir)]


def run(args):
    r = subprocess.run([sys.executable, CHECKER] + args, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def test_all_expected_passes(tmp_path):
    rc, out = run(write_ctest(tmp_path))
    assert rc == 0, out
    assert "expected  mmio_decode_a64/mmio_decode_a64" in out


@pytest.mark.parametrize("junit,rows,needle", [
    ({"adpcm_decode": {"adpcm_decode": FAIL}}, {"adpcm_decode": "ok | ok | 0 | 0 | 1"}, "UNEXPECTED adpcm_decode/adpcm_decode"),
    ({"adpcm_decode": {"adpcm_decode": NOEXE}}, None, "UNEXPECTED adpcm_decode/adpcm_decode"),
    ({"adpcm_decode": None}, {"adpcm_decode": "ok | FAIL | 0 | 0 | 0"}, "UNEXPECTED adpcm_decode: build failed"),
    (None, {"adpcm_decode": "FAIL | - | 0 | 0 | 0"}, "UNEXPECTED adpcm_decode: configure failed"),
    ({"root": {"xmm_helpers": FAIL}}, None, "UNEXPECTED root/xmm_helpers"),
    ({"adpcm_decode": None}, {"adpcm_decode": "ok | ok | 0 | 0 | 1"}, "INPUT     adpcm_decode"),
])
def test_unexpected_failure_fails(tmp_path, junit, rows, needle):
    rc, out = run(write_ctest(tmp_path, rows=rows, junit=junit))
    assert rc == 1, out
    assert needle in out


@pytest.mark.parametrize("result", [PASS, SKIP])
def test_listed_test_that_no_longer_fails_is_stale(tmp_path, result):
    rc, out = run(write_ctest(tmp_path, junit={"mmio_decode_a64": {"mmio_decode_a64": result}},
                              rows={"mmio_decode_a64": "ok | ok | 1 | 0 | 0"}))
    assert rc == 1, out
    assert "STALE     mmio_decode_a64/mmio_decode_a64: no longer fails" in out


def test_listed_test_that_is_gone_is_stale(tmp_path):
    rc, out = run(write_ctest(tmp_path, expected=EXPECTED + "renamed/test\n"))
    assert rc == 1, out
    assert "STALE     renamed/test: not found" in out


def test_missing_report_is_an_input_error(tmp_path):
    args = write_ctest(tmp_path)
    args[args.index("--report") + 1] = str(tmp_path / "nope.md")
    rc, _ = run(args)
    assert rc == 2


ANDROID = ("| Project | Built for Android |\n|---|---|\n| adpcm_decode | yes |\n"
           "| apu_vp | no: configure ( Windows threads ) |\n")


@pytest.mark.parametrize("report,expected,rc,needle", [
    (ANDROID, "apu_vp  # Windows\n", 0, "OK: 1 expected"),
    (ANDROID.replace("| adpcm_decode | yes |", "| adpcm_decode | no: build (x.c:1: error: boom) |"),
     "apu_vp\n", 1, "UNEXPECTED adpcm_decode: no: build"),
    (ANDROID.replace("| apu_vp | no: configure ( Windows threads ) |", "| apu_vp | yes |"),
     "apu_vp\n", 1, "STALE     apu_vp: builds now"),
])
def test_android_build_report(tmp_path, report, expected, rc, needle):
    (tmp_path / "build-report.md").write_text(report)
    (tmp_path / "expected.txt").write_text(expected)
    got, out = run(["android", "--expected", str(tmp_path / "expected.txt"),
                    "--report", str(tmp_path / "build-report.md")])
    assert got == rc, out
    assert needle in out
