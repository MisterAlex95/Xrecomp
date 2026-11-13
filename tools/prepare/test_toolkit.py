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


# What a pack's bytes depend on besides the XBE (docs/android/prepare.md « When a pack is stale »): the lifter and
# its modules, the generated code's runtime header and the kernel headers it names, the profiles (compiled into
# libtitle.so) and the kernel stub table's generator. The runtime's .c files are the shell's (libmain.so), not the
# pack's. A file here that the tree stopped selecting would leave packs lifted by an older lifter marked current.
PACK_INPUTS = (
    "tools/recomp/lifter.py", "tools/recomp/translator.py", "tools/recomp/seh_frames.py",   # XBA-112, XBA-101
    "tools/recomp/__main__.py", "tools/kernel_audit/stubs.py",                              # XBA-115
    "templates/runtime/recomp_types.h", "src/kernel/xbox_seh.h", "src/kernel/recomp_stack_switch.h",
    "src/kernel/recomp_discovery.h", "src/kernel/kernel_stubs.h",
    "profiles/titles/4d5300d1/overrides.c", "profiles/titles/4d5300d1/title.json",
    # Wave 3: images as sources (XBA-126), the XDK defaults (XBA-106), SMC (XBA-118), renderer coverage (XBA-120).
    "tools/xiso/source.py", "tools/xiso/xdvdfs.py", "tools/prepare/android.py", "tools/prepare/profiles.py",
    "templates/new-game/src/main.c", "profiles/xdk/d3d8/layer.json", "src/kernel/recomp_smc.h",
    "src/kernel/kernel_disc.h", "src/kernel/recomp_xdk_defaults.h", "src/kernel/nv2a_coverage.h",
    # Wave 4: the D3D layout table and the layer pick by code probes (XBA-105), the XBE reader (XBA-124).
    "profiles/xdk/d3d8/d3d8.c", "profiles/xdk/d3d8ltcg/layer.json", "tools/xbe_ident/__init__.py",
    "tools/xbe_parser/xbe_parser.py", "src/kernel/xbox_xbe_sections.h",
    # Wave 5: the analysis (XBA-137's static discovery) decides which functions are lifted.
    "tools/disasm/discovery.py", "tools/disasm/functions.py", "tools/disasm/engine.py", "tools/disasm/disasm.py",
    "tools/prepare/discovered.py",
)


def test_the_tree_covers_what_a_pack_is_made_of():
    sel = set(toolkit.files())
    missing = [p for p in PACK_INPUTS if os.path.exists(os.path.join(toolkit.ROOT, p)) and p not in sel]
    assert not missing, missing
    assert not [p for p in PACK_INPUTS if not os.path.exists(os.path.join(toolkit.ROOT, p))], "renamed? update the list"
    assert "tools/recomp/test_seh_frames.py" not in sel and "src/kernel/xbox_seh.c" not in sel


def test_a_lifter_change_changes_the_digest(tmp_path):
    tree = toolkit.load_tree()
    for rel in ("tools/recomp/seh_frames.py", "tools/disasm/discovery.py", "templates/runtime/recomp_types.h",
                "src/kernel/xbox_seh.c"):
        dst = tmp_path / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text("one\n")
    before = toolkit.compute(str(tmp_path), tree)
    (tmp_path / "tools/recomp/seh_frames.py").write_text("two\n")
    after = toolkit.compute(str(tmp_path), tree)
    (tmp_path / "tools/disasm/discovery.py").write_text("two\n")      # an analysis change too (XBA-137)
    assert toolkit.compute(str(tmp_path), tree) != after
    after = toolkit.compute(str(tmp_path), tree)
    (tmp_path / "src/kernel/xbox_seh.c").write_text("two\n")              # the shell's: no new Prepare
    assert before != after and toolkit.compute(str(tmp_path), tree) == after
