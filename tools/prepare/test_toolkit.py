"""tools.prepare.toolkit: the toolkit tree's patterns and digest (XBA-127,
docs/android/prepare.md « When a pack is stale »)."""

import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

from tools.prepare import toolkit       # noqa: E402


def test_ant_patterns():
    m = toolkit.ant_match
    assert m("tools/**/*.py", "tools/a.py") and m("tools/**/*.py", "tools/x/y/a.py")
    assert not m("tools/**/*.py", "tools/a.pyc") and not m("tools/**/*.py", "src/a.py")
    assert m("tools/xbe_ident/*.json", "tools/xbe_ident/engines.json")
    assert not m("tools/xbe_ident/*.json", "tools/xbe_ident/sub/engines.json")
    assert m("**/test_*.py", "test_a.py") and m("**/test_*.py", "tools/prepare/test_a.py")
    assert m("**/tests/**", "tools/x/tests/a/b.py") and not m("**/tests/**", "tools/x/tests.py")
    assert m("tools/*/output/**", "tools/disasm/output/f.json")
    assert not m("tools/*/output/**", "tools/a/b/output/f.json")
    assert m("templates/new-game/**", "templates/new-game/src/main.c")
    assert m("src/**/*.h", "src/kernel/kernel.h") and not m("src/**/*.h", "src/kernel/kernel.c")
    assert m("**/.gitignore", "templates/new-game/.gitignore") and m("a/", "a/b/c")


def _tree(root, files):
    for rel, data in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


def test_selection_and_digest(tmp_path):
    _tree(tmp_path, {
        "tools/recomp/lifter.py": b"lift",
        "tools/recomp/test_lifter.py": b"test",
        "tools/recomp/__pycache__/lifter.cpython-313.pyc": b"pyc",
        "tools/disasm/output/functions.json": b"{}",
        "tools/android/theme_preview/x.py": b"ui",
        "tools/TOOLKIT_SHA256": b"not hashed",
        "src/kernel/kernel.h": b"h",
        "src/kernel/kernel.c": b"c",
        "templates/new-game/.gitignore": b"build/",
        "templates/new-game/src/main.c": b"main",
        "android/app/src/main/java/A.java": b"java",
        "docs/x.md": b"doc",
    })
    tree = toolkit.load_tree()
    assert toolkit.files(str(tmp_path), tree) == ["src/kernel/kernel.h", "templates/new-game/src/main.c",
                                                  "tools/recomp/lifter.py"]
    h = hashlib.sha256()
    for rel, data in (("src/kernel/kernel.h", b"h"), ("templates/new-game/src/main.c", b"main"),
                      ("tools/recomp/lifter.py", b"lift")):
        h.update(rel.encode() + b"\0" + str(len(data)).encode() + b"\0" + data)
    want = h.hexdigest()
    assert toolkit.compute(str(tmp_path), tree) == want

    # What the app's other parts change does not count; a lifter change does.
    (tmp_path / "android/app/src/main/java/A.java").write_bytes(b"java 2")
    (tmp_path / "src/kernel/kernel.c").write_bytes(b"c 2")
    (tmp_path / "tools/recomp/test_lifter.py").write_bytes(b"test 2")
    assert toolkit.compute(str(tmp_path), tree) == want
    (tmp_path / "tools/recomp/lifter.py").write_bytes(b"lift 2")
    assert toolkit.compute(str(tmp_path), tree) != want


def test_digest_reads_the_app_build_file(tmp_path, monkeypatch):
    """On the device the toolkit carries tools/TOOLKIT_SHA256 (android/app/build.gradle); a host computes it."""
    monkeypatch.setattr(toolkit, "_cache", {})
    _tree(tmp_path, {"tools/recomp/lifter.py": b"lift"})
    computed = toolkit.digest(str(tmp_path))
    assert len(computed) == 64
    monkeypatch.setattr(toolkit, "_cache", {})
    (tmp_path / "tools/TOOLKIT_SHA256").write_text("AB" * 32 + "\n")
    assert toolkit.digest(str(tmp_path)) == "ab" * 32
    monkeypatch.setattr(toolkit, "_cache", {})
    (tmp_path / "tools/TOOLKIT_SHA256").write_text("garbage\n")
    assert toolkit.digest(str(tmp_path)) == computed


def test_tree_file_is_what_gradle_reads():
    """android/app/build.gradle takes its patterns from toolkit_tree.json (one list, two readers)."""
    root = toolkit.ROOT
    gradle = open(os.path.join(root, "android", "app", "build.gradle")).read()
    assert "tools/prepare/toolkit_tree.json" in gradle and "exclude_everywhere" in gradle
    tree = json.load(open(toolkit.TREE_FILE))
    assert tree["trees"] and all(t["include"] for t in tree["trees"])
    # The real toolkit: the lifter is in, the app and the tests are not.
    files = toolkit.files(root, tree)
    assert "tools/recomp/lifter.py" in files and "tools/prepare/toolkit.py" in files
    assert not any(f.startswith(("android/", "docs/")) or "/test_" in f or f.endswith(".c") and f.startswith("src/")
                   for f in files)
