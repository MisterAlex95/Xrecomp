#!/usr/bin/env python3
"""Fails a CI run on any unexpected failure, and on any expected failure that no longer happens (XBA-53).

    check_expected.py ctest   --expected tools/ci/arm64-expected.txt --report c-tests.md --junit DIR
    check_expected.py android --expected tools/ci/android-unbuilt.txt --report build-report.md

ctest mode reads what the arm64 image produces (tools/docker/linux-arm64.Dockerfile):
  - DIR/<project>.xml, ctest's JUnit file per project (tools/run_c_tests.sh with XBR_JUNIT_DIR=DIR), and
    DIR/root.xml for the root build's ctest. A test fails when its status is "fail", or "notrun" for any reason
    other than a SKIP_* property (a missing executable is a failure, as ctest counts it).
  - the Markdown report of run_c_tests.sh, for the projects that did not configure or build (they have no JUnit
    file) and to cross-check the counts.
  Entries in the expected file: "<project>/<test>" for a test allowed to fail ("root/<test>" for the root build),
  "<project>" alone for a project allowed to fail to configure or build.

android mode reads build-report.md of the device-tests stage (tools/docker/android.Dockerfile): one row per test
project, "yes" or "no: <why>". Entries: the projects that are not built for Android.

Either way an entry must still fail: a listed test that passes, skips or is gone makes the list stale, and the run
fails until the line is removed. Lines are "<entry>  # reason"; blank lines and comments are ignored.
"flaky <entry>  # reason" marks an entry that fails only some of the time: it may fail or pass, and is never
stale (it must still exist in the results). Use it for a known intermittent failure that has its own ticket.
Exit status: 0 when the results match the list exactly, 1 otherwise, 2 on unreadable input.
"""
import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET


def read_expected(path):
    """{entry: line number} and the set of entries marked flaky."""
    entries, flaky = {}, set()
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            text = line.split("#", 1)[0].strip()
            if not text:
                continue
            is_flaky = text.startswith("flaky ")
            if is_flaky:
                text = text[len("flaky "):].strip()
            if text in entries:
                raise ValueError(f"{path}:{n}: {text} listed twice")
            entries[text] = n
            if is_flaky:
                flaky.add(text)
    return entries, flaky


def read_table(path):
    """Rows of a Markdown table as lists of cells (header and separator rows dropped)."""
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line.startswith("|"):
                continue
            cells = [c.strip() for c in line.strip("|").split("|")]
            if cells[0] in ("Project", "") or set(cells[0]) <= set("-:"):
                continue
            rows.append(cells)
    return rows


def junit_results(path):
    """{test name: 'pass' | 'skip' | 'fail'} from one ctest JUnit file."""
    out = {}
    root = ET.parse(path).getroot()
    for tc in root.iter("testcase"):
        name = tc.get("name")
        status = tc.get("status", "")
        skipped = tc.find("skipped")
        if status == "fail" or tc.find("failure") is not None or tc.find("error") is not None:
            out[name] = "fail"
        elif status in ("notrun", "disabled") or skipped is not None:
            why = skipped.get("message", "") if skipped is not None else ""
            # SKIP_RETURN_CODE / SKIP_REGULAR_EXPRESSION are real skips; "Unable to find executable" is not.
            out[name] = "skip" if status == "disabled" or why.startswith("SKIP_") else "fail"
        else:
            out[name] = "pass"
    return out


def ctest_failures(report, junit_dir):
    """Returns (failing entries, all entries that ran or were attempted, problems with the inputs)."""
    failing, seen, problems = {}, set(), []
    for cells in read_table(report):
        if len(cells) < 6:
            problems.append(f"{report}: malformed row {cells}")
            continue
        proj, conf, build, _passed, _skipped, failed = cells[:6]
        if conf == "windows":
            continue
        seen.add(proj)
        if conf == "FAIL" or build == "FAIL":
            failing[proj] = "configure failed" if conf == "FAIL" else "build failed"
            continue
        xml = os.path.join(junit_dir, proj + ".xml")
        if not os.path.isfile(xml):
            # a project that registers no test (d3d8_smoke) writes no file only if ctest found nothing to run
            if failed not in ("0", "-"):
                problems.append(f"{proj}: the report says {failed} failed but there is no {xml}")
            continue
        results = junit_results(xml)
        nfail = 0
        for test, res in results.items():
            seen.add(f"{proj}/{test}")
            if res == "fail":
                failing[f"{proj}/{test}"] = "failed"
                nfail += 1
        if failed.isdigit() and int(failed) > nfail:
            problems.append(f"{proj}: the report counts {failed} failures, the JUnit file {nfail}")
    root_xml = os.path.join(junit_dir, "root.xml")
    if os.path.isfile(root_xml):
        for test, res in junit_results(root_xml).items():
            seen.add(f"root/{test}")
            if res == "fail":
                failing[f"root/{test}"] = "failed"
    if not seen:
        problems.append(f"{report}: no project at all")
    return failing, seen, problems


def android_failures(report):
    failing, seen, problems = {}, set(), []
    for cells in read_table(report):
        if len(cells) < 2:
            problems.append(f"{report}: malformed row {cells}")
            continue
        proj, built = cells[0], cells[1]
        seen.add(proj)
        if built != "yes":
            failing[proj] = built
    if not seen:
        problems.append(f"{report}: no project at all")
    return failing, seen, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("mode", choices=("ctest", "android"))
    ap.add_argument("--expected", required=True)
    ap.add_argument("--report", required=True)
    ap.add_argument("--junit", help="ctest mode: directory of the JUnit files")
    a = ap.parse_args()
    try:
        expected, flaky = read_expected(a.expected)
        if a.mode == "ctest":
            if not a.junit:
                ap.error("ctest mode needs --junit")
            failing, seen, problems = ctest_failures(a.report, a.junit)
        else:
            failing, seen, problems = android_failures(a.report)
    except (OSError, ValueError, ET.ParseError) as e:
        print(f"check_expected: {e}", file=sys.stderr)
        return 2

    unexpected = sorted(set(failing) - set(expected))
    # A flaky entry that passed this time is fine, as long as it ran at all.
    stale = sorted(e for e in set(expected) - set(failing)
                   if not (e in flaky and e in seen))
    known = sorted(set(expected) & set(failing))
    now = "builds now" if a.mode == "android" else "no longer fails"
    for e in known:
        print(f"expected  {e}: {failing[e]}{' (flaky)' if e in flaky else ''}")
    for e in sorted(flaky & (set(seen) - set(failing))):
        print(f"flaky     {e}: passed this time")
    for e in unexpected:
        print(f"UNEXPECTED {e}: {failing[e]}")
    for e in stale:
        state = now if e in seen else "not found in the results"
        print(f"STALE     {e}: {state}; remove it from {a.expected} (line {expected[e]})")
    for p in problems:
        print(f"INPUT     {p}")
    ok = not (unexpected or stale or problems)
    print(f"{'OK' if ok else 'FAILED'}: {len(known)} expected, {len(unexpected)} unexpected, {len(stale)} stale"
          f"{', input problems' if problems else ''} ({a.expected})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
