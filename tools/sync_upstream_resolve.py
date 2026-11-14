"""
Resolves the merge conflicts of tools/sync_upstream.sh that come only from
text this fork renamed in upstream's files (XBA-109, XBA-138).

    python3 tools/sync_upstream_resolve.py [--repo DIR]

Run inside a merge that stopped on conflicts. This fork renamed some of
upstream's text: the include guards of eight headers (a title's name ->
XBOXRECOMP_<FILE>_H; in one of them only the closing `#endif` comment) and
the kernel thunk table's default and its comment in src/kernel/kernel.h. Each
upstream change next to one of those lines then conflicts, although neither
side disagrees about anything.

For each conflicted file the merge is redone with `git merge-file` on our
version, and on the merge base and upstream's version both translated to our
names: our include guard in place of theirs, our name in the last
`#endif /* ... */` comment, and our text in place of upstream's for each
region in REGIONS. Only what we renamed is translated;
the rest of upstream's change merges as it would have. A file that still
conflicts after that is a real conflict and is left as git left it.

Files in OURS were rewritten for this fork (the README, 2026-11-14): a
conflict there keeps our version, and prints where upstream's text goes.

Exit status: 0 when no conflict is left (resolved files are staged), 1 when
some remain (listed on stderr), 2 when there is no merge in progress.
"""

import argparse
import re
import subprocess
import sys

# Regions of upstream's text we replaced with ours, per path: (start, end)
# regexes over lines, matched in both versions; upstream's region becomes ours.
REGIONS = {
    "src/kernel/kernel.h": [
        # The thunk table's comment and default (XBA-109: no title's address as the default).
        (r"^ \* (Legacy define for backward compatibility|The initial value is 0)",
         r"^#define XBOX_KERNEL_THUNK_TABLE_BASE\b"),
        (r"^ \* If not called, ", r"^ \* If not called, "),
    ],
}

# Files this fork rewrote: a conflict keeps ours. The value says what to do
# with upstream's change by hand.
OURS = {
    "README.md": "upstream's README changed: refresh docs/upstream/README.md and CHANGELOG.md from "
                 "`git show upstream/main:README.md`",
}

_GUARD = re.compile(r"^#ifndef\s+(\w+)\s*\n#define\s+\1\b", re.M)
_ENDIF = re.compile(r"^#endif[ \t]*/\*[ \t]*(\w+)[ \t]*\*/", re.M)


def git(repo, *args, check=True, binary=False):
    r = subprocess.run(["git", "-C", repo, *args], capture_output=True, check=False)
    if check and r.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), r.stderr.decode(errors="replace").strip()))
    return r.stdout if binary else r.stdout.decode("utf-8", errors="surrogateescape")


def include_guard(text):
    m = _GUARD.search(text)
    return m.group(1) if m else None


def endif_name(text):
    """The name in the comment of the file's last `#endif /* NAME */`."""
    names = _ENDIF.findall(text)
    return names[-1] if names else None


def _region(lines, start, end):
    """(first, last) line indexes of the region, or None."""
    s = next((i for i, ln in enumerate(lines) if re.search(start, ln)), None)
    if s is None:
        return None
    e = next((i for i in range(s, len(lines)) if re.search(end, lines[i])), None)
    return (s, e) if e is not None else None


def translate(text, ours, path):
    """`text` (the base's or upstream's version of `path`) with what this fork
    renamed in it put back to our names, read from `ours`."""
    g_theirs, g_ours = include_guard(text), include_guard(ours)
    if g_theirs and g_ours and g_theirs != g_ours:
        text = re.sub(r"\b%s\b" % re.escape(g_theirs), g_ours, text)
    # The closing comment alone can differ (the guard itself was already ours).
    e_theirs, e_ours = endif_name(text), endif_name(ours)
    if e_theirs and e_ours and e_theirs != e_ours:
        text = re.sub(r"^(#endif[ \t]*/\*[ \t]*)%s\b" % re.escape(e_theirs), r"\g<1>" + e_ours, text, flags=re.M)
    regions = REGIONS.get(path, [])
    if regions:
        lines = text.splitlines(keepends=True)
        our_lines = ours.splitlines(keepends=True)
        for start, end in regions:
            mine, theirs = _region(our_lines, start, end), _region(lines, start, end)
            if mine and theirs:
                lines[theirs[0]:theirs[1] + 1] = our_lines[mine[0]:mine[1] + 1]
        text = "".join(lines)
    return text


def merge_file(repo, ours, base, theirs):
    """git merge-file on three texts: (clean, merged text)."""
    import os
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        paths = []
        for name, t in (("ours", ours), ("base", base), ("theirs", theirs)):
            p = os.path.join(d, name)
            with open(p, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(t)
            paths.append(p)
        r = subprocess.run(["git", "-C", repo, "merge-file", "-p", *paths], capture_output=True)
        return r.returncode == 0, r.stdout.decode("utf-8", errors="surrogateescape")


def resolve(repo):
    """Resolve what can be; returns the paths still in conflict."""
    left = []
    for path in filter(None, git(repo, "diff", "--name-only", "-z", "--diff-filter=U").split("\0")):
        stages = {}
        for line in git(repo, "ls-files", "-u", "--", path).splitlines():
            meta, _ = line.split("\t", 1)
            _, sha, stage = meta.split()
            stages[int(stage)] = sha
        if set(stages) != {1, 2, 3}:            # added or deleted on one side: not a rename
            left.append(path)
            continue
        if path in OURS:
            git(repo, "checkout", "--ours", "--", path)
            git(repo, "add", "--", path)
            print("  kept ours: %s (%s)" % (path, OURS[path]))
            continue
        base, ours, theirs = (git(repo, "cat-file", "blob", stages[i]) for i in (1, 2, 3))
        tb, tt = translate(base, ours, path), translate(theirs, ours, path)
        if (tb, tt) == (base, theirs):
            left.append(path)                   # nothing of ours to translate: a real conflict
            continue
        clean, merged = merge_file(repo, ours, tb, tt)
        if not clean:
            left.append(path)
            continue
        top = git(repo, "rev-parse", "--show-toplevel").strip()
        with open("%s/%s" % (top, path), "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
            f.write(merged)
        git(repo, "add", "--", path)
        print("  resolved %s (renamed text only)" % path)
    return left


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", default=".", help="the worktree with the merge in progress")
    a = ap.parse_args(argv)
    if subprocess.run(["git", "-C", a.repo, "rev-parse", "-q", "--verify", "MERGE_HEAD"],
                      capture_output=True).returncode != 0:
        print("no merge in progress in %s" % a.repo, file=sys.stderr)
        return 2
    left = resolve(a.repo)
    for p in left:
        print("  conflict: %s" % p, file=sys.stderr)
    return 1 if left else 0


if __name__ == "__main__":
    sys.exit(main())
