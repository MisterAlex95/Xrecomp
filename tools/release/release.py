#!/usr/bin/env python3
"""Release automation in the spirit of semantic-release / release-please (docs/releases/README.md).

Commits keep our `Area: sentence` subjects. A commit chooses how far the next release moves with a trailer in the
last paragraph of its message:

    Release: major | minor | patch | none

No trailer counts as `patch` (upstream's commits, which we merge as they are, included). The next version is the
last `xrecomp-v*` tag reachable from HEAD bumped by the largest trailer since it; `none` everywhere means no release.

Subcommands (all read the git repository in the current directory):

    next                 print `<version> <bump>` for the next release, or nothing when there is none
    prepare [--carry F]  write docs/releases/xrecomp-v<next>.md and its row in docs/releases/README.md; F is the
                         same notes file from the open release PR: what its author wrote above the generated
                         section is kept (even when the version moved), the generated section is rewritten
    pending BASE HEAD    print the tag to create when the commits BASE..HEAD added a notes file whose tag does not
                         exist yet (CI calls it on each push to main: merging the release PR tags the release)

Python 3 standard library only (the CI container is Alpine).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

TAG_PREFIX = "xrecomp-v"
BUMPS = ("none", "patch", "minor", "major")
START = "<!-- release:changes start (generated from the commits; rewritten on every push to main) -->"
END = "<!-- release:changes end -->"
PLACEHOLDER = "<!-- Highlights: what changed, what works, known problems. Written by hand, kept across updates. -->"
VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
NOTES_RE = re.compile(r"^docs/releases/" + re.escape(TAG_PREFIX) + r"(\d+\.\d+\.\d+)\.md$")
RELEASE_SUBJECT = "Release: XRecomp "
NOTES_DIR = Path("docs/releases")


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def parse_version(v: str) -> tuple[int, int, int]:
    m = VERSION_RE.match(v)
    if not m:
        raise ValueError(f"not a version: {v}")
    return int(m[1]), int(m[2]), int(m[3])


def last_tag() -> str | None:
    """The highest xrecomp-v<x.y.z> tag reachable from HEAD."""
    tags = git("tag", "--merged", "HEAD", "--list", TAG_PREFIX + "*").split()
    versions = [t for t in tags if VERSION_RE.match(t[len(TAG_PREFIX):])]
    if not versions:
        return None
    return max(versions, key=lambda t: parse_version(t[len(TAG_PREFIX):]))


def commits_since(tag: str | None) -> list[dict]:
    """Non-merge commits since tag, newest first: sha, subject, body, bump."""
    rng = f"{tag}..HEAD" if tag else "HEAD"
    sep, end = "\x1f", "\x1e"
    out = git("log", "--no-merges", f"--format=%H{sep}%s{sep}%b{sep}%(trailers:key=Release,valueonly,separator=%x2C){end}", rng)
    commits = []
    for rec in out.split(end):
        rec = rec.strip("\n")
        if not rec:
            continue
        sha, subject, body, trailer = rec.split(sep)
        if subject.startswith(RELEASE_SUBJECT):
            continue
        values = [v.strip().lower() for v in trailer.split(",") if v.strip()]
        values = [v for v in values if v in BUMPS]
        bump = max(values, key=BUMPS.index) if values else "patch"
        commits.append({"sha": sha, "subject": subject, "body": body, "bump": bump})
    return commits


def bump_version(version: str | None, bump: str) -> str:
    if version is None:
        return "0.1.0"
    major, minor, patch = parse_version(version)
    if bump == "major":
        return f"{major + 1}.0.0"
    if bump == "minor":
        return f"{major}.{minor + 1}.0"
    return f"{major}.{minor}.{patch + 1}"


def next_release() -> tuple[str, str, list[dict]] | None:
    tag = last_tag()
    commits = commits_since(tag)
    bump = max((c["bump"] for c in commits), key=BUMPS.index, default="none")
    if bump == "none":
        return None
    return bump_version(tag[len(TAG_PREFIX):] if tag else None, bump), bump, commits


def split_subject(subject: str) -> tuple[str, str]:
    area, colon, rest = subject.partition(": ")
    if colon and 0 < len(area) <= 40 and "\n" not in area:
        return area, rest
    return "Other", subject


def changes_section(commits: list[dict]) -> str:
    groups: dict[str, list[str]] = {}
    for c in reversed(commits):  # oldest first inside an area
        if c["bump"] == "none":
            continue
        area, text = split_subject(c["subject"])
        refs = sorted(set(re.findall(r"\bXBA-\d+\b", c["subject"] + "\n" + c["body"])) - set(re.findall(r"\bXBA-\d+\b", text)),
                      key=lambda r: int(r[4:]))
        line = f"- {text} ({c['sha'][:8]}"
        line += (", " + ", ".join(refs) + ")") if refs else ")"
        groups.setdefault(area, []).append(line)
    parts = []
    for area in sorted(groups, key=lambda a: (a == "Other", a.lower())):
        parts.append(f"### {area}\n\n" + "\n".join(groups[area]))
    return "\n\n".join(parts) if parts else "No user-facing change."


def render_notes(version: str, commits: list[dict], carried: str | None) -> str:
    head = f"# XRecomp {version}\n\n{PLACEHOLDER}"
    if carried and START in carried:
        kept = carried.split(START, 1)[0].rstrip()
        kept = re.sub(r"\n*## Changes$", "", kept).rstrip()
        kept = re.sub(r"^# XRecomp \S+", f"# XRecomp {version}", kept, count=1)
        if kept:
            head = kept
    return head + "\n\n## Changes\n\n" + START + "\n\n" + changes_section(commits) + "\n\n" + END + "\n"


def add_table_row(readme: str, tag: str) -> str:
    row = f"| `{tag}` | [{tag}.md]({tag}.md) |"
    if row in readme:
        return readme
    lines = readme.split("\n")
    for i, line in enumerate(lines):
        if line.startswith("| Release | Notes |") and i + 1 < len(lines) and lines[i + 1].startswith("|---"):
            lines.insert(i + 2, row)
            return "\n".join(lines)
    raise SystemExit("docs/releases/README.md: no « | Release | Notes | » table")


def cmd_next(_args) -> int:
    nxt = next_release()
    if nxt:
        print(nxt[0], nxt[1])
    return 0


def cmd_prepare(args) -> int:
    nxt = next_release()
    if not nxt:
        print("nothing to release", file=sys.stderr)
        return 0
    version, bump, commits = nxt
    tag = TAG_PREFIX + version
    notes = NOTES_DIR / f"{tag}.md"
    if notes.exists():
        print(f"{notes} is already on this branch but {tag} does not exist: tag it (release.yml) first",
              file=sys.stderr)
        return 1
    carried = Path(args.carry).read_text() if args.carry else None
    notes.write_text(render_notes(version, commits, carried))
    readme = NOTES_DIR / "README.md"
    readme.write_text(add_table_row(readme.read_text(), tag))
    print(version, bump)
    return 0


def cmd_pending(args) -> int:
    added = git("diff", "--name-only", "--diff-filter=AR", args.base, args.head).split()
    versions = [m[1] for f in added if (m := NOTES_RE.match(f))]
    if not versions:
        return 0
    version = max(versions, key=parse_version)
    tag = TAG_PREFIX + version
    existing = git("tag", "--list", tag).split()
    if existing:
        if git("rev-list", "-n1", tag).strip() != git("rev-parse", args.head).strip():
            print(f"{tag} already exists on another commit: not tagging again", file=sys.stderr)
            return 0
    print(tag)
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("next").set_defaults(fn=cmd_next)
    pr = sub.add_parser("prepare")
    pr.add_argument("--carry")
    pr.set_defaults(fn=cmd_prepare)
    pe = sub.add_parser("pending")
    pe.add_argument("base")
    pe.add_argument("head")
    pe.set_defaults(fn=cmd_pending)
    args = p.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
