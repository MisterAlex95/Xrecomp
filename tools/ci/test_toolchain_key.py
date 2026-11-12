"""toolchain_key.sh (XBA-125): the CI rebuilds the on-device toolchain only when its key changes, so the key must move
with what the toolchain is built from and stay put otherwise. Runs with the rest of `pytest tools/` in the linux job."""
import os
import shutil
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SCRIPT = os.path.join(HERE, "toolchain_key.sh")

pytestmark = pytest.mark.skipif(shutil.which("sh") is None or shutil.which("sha256sum") is None,
                                reason="needs sh and sha256sum")


@pytest.fixture
def tree(tmp_path):
    """A copy of what the key reads: tools/android/toolchain/ and android.Dockerfile, plus an unrelated file."""
    shutil.copytree(os.path.join(ROOT, "tools", "android", "toolchain"), tmp_path / "tools" / "android" / "toolchain",
                    ignore=shutil.ignore_patterns("__pycache__"))
    (tmp_path / "tools" / "docker").mkdir(parents=True)
    shutil.copy(os.path.join(ROOT, "tools", "docker", "android.Dockerfile"), tmp_path / "tools" / "docker")
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "other.c").write_text("int x;\n")
    return tmp_path


def key(root):
    out = subprocess.run(["sh", SCRIPT, str(root)], check=True, capture_output=True, text=True).stdout.strip()
    assert len(out) == 16 and all(c in "0123456789abcdef" for c in out), out
    return out


def edit(path, old, new):
    text = path.read_text()
    assert old in text, (path, old)
    path.write_text(text.replace(old, new, 1))


def test_same_as_the_repository(tree):
    assert key(tree) == key(ROOT)


def test_unrelated_changes_keep_the_key(tree):
    k = key(tree)
    (tree / "src" / "other.c").write_text("int y;\n")
    df = tree / "tools" / "docker" / "android.Dockerfile"
    edit(df, "FROM android AS xrecomp-apk", "FROM android AS xrecomp-apk\n# a comment of the APK stage")
    edit(df, "ARG GRADLE_VERSION=", "ARG GRADLE_VERSION=0")
    assert key(tree) == k


@pytest.mark.parametrize("change", ["versions", "script", "new-file", "ndk", "stage"])
def test_toolchain_inputs_change_the_key(tree, change):
    k = key(tree)
    tc = tree / "tools" / "android" / "toolchain"
    df = tree / "tools" / "docker" / "android.Dockerfile"
    if change == "versions":
        edit(tc / "versions.env", "LLVM_VERSION=", "LLVM_VERSION=0")
    elif change == "script":
        (tc / "build_llvm.sh").write_text((tc / "build_llvm.sh").read_text() + "\n# changed\n")
    elif change == "new-file":
        (tc / "patch.diff").write_text("x\n")
    elif change == "ndk":
        edit(df, "ARG NDK_VERSION=", "ARG NDK_VERSION=0")
    else:
        edit(df, "FROM toolchain-base AS toolchain-llvm\nARG JOBS=6", "FROM toolchain-base AS toolchain-llvm\nARG JOBS=5")
    assert key(tree) != k


def test_not_a_repository(tmp_path):
    r = subprocess.run(["sh", SCRIPT, str(tmp_path)], capture_output=True, text=True)
    assert r.returncode != 0 and "not a repository root" in r.stderr
