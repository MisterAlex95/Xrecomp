"""
The toolkit digest (XBA-127, docs/android/prepare.md « When a pack is stale »):
what says whether a title pack was lifted by the toolkit this app carries.

The toolkit tree is what the Android app zips into assets/prepare/toolkit.zip
(tools/prepare/toolkit_tree.json, read by android/app/build.gradle too): the
pipeline's Python, the profiles, the title template and the headers the
generated C includes. Everything that ends up in a pack's bytes and is not the
player's XBE. The digest is sha256 over those files in path order, each as
path NUL size NUL bytes. Java, the shell's .c files (libmain.so: a pack links
against them, shell_abi guards that contract), the docs and the tests are not
in it, so an app update that changes only those keeps every pack current.

The app build writes the digest into the zip (tools/TOOLKIT_SHA256) and next
to it (assets/prepare/toolkit.zip.id); here it is read from that file when it
is there (the device), else computed from the tree (a host checkout).
"""

import fnmatch
import hashlib
import json
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TREE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "toolkit_tree.json")
DIGEST_FILE = os.path.join("tools", "TOOLKIT_SHA256")


def ant_match(pattern, path):
    """Ant/Gradle pattern matching on '/'-separated relative paths: '**' is
    any number of directories (none included), '*' and '?' stay inside one
    name; a pattern ending in '/' means '/**'."""
    if pattern.endswith("/"):
        pattern += "**"
    pat, parts = pattern.split("/"), path.split("/")

    def go(i, j):
        if i == len(pat):
            return j == len(parts)
        if pat[i] == "**":
            return any(go(i + 1, k) for k in range(j, len(parts) + 1))
        return j < len(parts) and fnmatch.fnmatchcase(parts[j], pat[i]) and go(i + 1, j + 1)

    return go(0, 0)


def load_tree(path=TREE_FILE):
    with open(path) as f:
        return json.load(f)


def selected(rel, tree):
    """True when the relative path rel belongs to the toolkit tree."""
    if any(ant_match(p, rel) for p in tree.get("exclude_everywhere", [])):
        return False
    for t in tree["trees"]:
        if any(ant_match(p, rel) for p in t["include"]) and not any(ant_match(p, rel) for p in t["exclude"]):
            return True
    return False


def files(root=ROOT, tree=None):
    """The toolkit tree's files under root, as sorted relative paths."""
    tree = tree or load_tree()
    # Only the top-level folders the patterns start with (not a checkout's build/).
    tops = sorted({p.split("/")[0] for t in tree["trees"] for p in t["include"]})
    out = []
    for top in tops:
        for d, dirs, names in os.walk(os.path.join(root, top)):
            rd = os.path.relpath(d, root).replace(os.sep, "/") + "/"
            dirs[:] = [n for n in dirs if n not in (".git", "__pycache__")]   # never selected
            for n in names:
                rel = rd + n
                if rel != DIGEST_FILE.replace(os.sep, "/") and selected(rel, tree):
                    out.append(rel)
    return sorted(out)


def compute(root=ROOT, tree=None):
    """sha256 over the toolkit tree: for each file in path order,
    path NUL size NUL bytes (android/app/build.gradle does the same)."""
    h = hashlib.sha256()
    for rel in files(root, tree):
        with open(os.path.join(root, rel), "rb") as f:
            data = f.read()
        h.update(rel.encode("utf-8") + b"\0" + str(len(data)).encode() + b"\0")
        h.update(data)
    return h.hexdigest()


_cache = {}


def digest(root=ROOT):
    """The toolkit digest: tools/TOOLKIT_SHA256 when the app build wrote it,
    else computed from the tree (once per process)."""
    if root in _cache:
        return _cache[root]
    v = ""
    try:
        with open(os.path.join(root, DIGEST_FILE)) as f:
            v = f.read().strip().lower()
    except OSError:
        pass
    if len(v) != 64 or any(c not in "0123456789abcdef" for c in v):
        v = compute(root)
    _cache[root] = v
    return v
