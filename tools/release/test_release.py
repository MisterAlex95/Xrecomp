"""Tests of tools/release/release.py on throwaway git repositories."""

import importlib.util
import subprocess
from pathlib import Path

import pytest

_spec = importlib.util.spec_from_file_location("xrecomp_release", Path(__file__).resolve().parent / "release.py")
release = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(release)

README = """# Releases

| Release | Notes |
|---|---|
| `xrecomp-v0.13.1` | [xrecomp-v0.13.1.md](xrecomp-v0.13.1.md) |

More text.
"""


def run(*args, cwd):
    return subprocess.run(args, cwd=cwd, check=True, capture_output=True, text=True).stdout


@pytest.fixture
def repo(tmp_path, monkeypatch):
    env = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid"}
    for k, v in env.items():
        monkeypatch.setenv(k, v)
    run("git", "init", "-q", "-b", "main", cwd=tmp_path)
    (tmp_path / "docs/releases").mkdir(parents=True)
    (tmp_path / "docs/releases/README.md").write_text(README)
    run("git", "add", ".", cwd=tmp_path)
    run("git", "commit", "-qm", "Docs: release notes for XRecomp 0.13.1", cwd=tmp_path)
    run("git", "tag", "-a", "xrecomp-v0.13.1", "-m", "XRecomp 0.13.1", cwd=tmp_path)
    monkeypatch.chdir(tmp_path)
    return tmp_path


def commit(repo, message):
    run("git", "commit", "-q", "--allow-empty", "-m", message, cwd=repo)
    return run("git", "rev-parse", "HEAD", cwd=repo).strip()


def test_nothing_since_the_tag(repo):
    assert release.next_release() is None


def test_no_trailer_is_a_patch(repo):
    commit(repo, "Kernel bridge: a fix")
    version, bump, _ = release.next_release()
    assert (version, bump) == ("0.13.2", "patch")


def test_largest_trailer_wins(repo):
    commit(repo, "Kernel bridge: a fix")
    commit(repo, "Recomp: a new pass\n\nLonger text.\n\nRelease: minor")
    commit(repo, "Docs: typo\n\nRelease: none")
    version, bump, _ = release.next_release()
    assert (version, bump) == ("0.14.0", "minor")


def test_only_none_means_no_release(repo):
    commit(repo, "Docs: typo\n\nRelease: none")
    assert release.next_release() is None


def test_major(repo):
    commit(repo, "App: 1.0\n\nRelease: major")
    assert release.next_release()[0] == "1.0.0"


def test_a_trailer_in_the_middle_of_the_body_is_not_one(repo):
    commit(repo, "Recomp: a pass\n\nRelease: minor\n\nThis paragraph comes after.")
    assert release.next_release()[1] == "patch"


def test_release_commits_are_ignored(repo):
    commit(repo, "Release: XRecomp 0.13.2")
    assert release.next_release() is None


def test_prepare_writes_notes_and_row(repo):
    sha = commit(repo, "Kernel bridge: timers start once (XBA-12)\n\nSee XBA-40.")
    commit(repo, "Recomp: a pass\n\nRelease: minor")
    commit(repo, "a subject without an area")
    commit(repo, "Docs: typo\n\nRelease: none")
    assert release.main(["prepare"]) == 0
    notes = (repo / "docs/releases/xrecomp-v0.14.0.md").read_text()
    assert notes.startswith("# XRecomp 0.14.0\n\n" + release.PLACEHOLDER)
    assert f"### Kernel bridge\n\n- timers start once (XBA-12) ({sha[:8]}, XBA-40)" in notes
    assert "### Recomp\n\n- a pass" in notes
    assert notes.index("### Recomp") < notes.index("### Other")
    assert "typo" not in notes
    readme = (repo / "docs/releases/README.md").read_text()
    assert "|---|---|\n| `xrecomp-v0.14.0` | [xrecomp-v0.14.0.md](xrecomp-v0.14.0.md) |\n| `xrecomp-v0.13.1`" in readme
    assert release.main(["prepare"]) == 1  # notes already there, tag missing: refuses
    assert (repo / "docs/releases/README.md").read_text() == readme


def test_prepare_keeps_the_hand_written_part_when_the_version_moves(repo, tmp_path_factory):
    commit(repo, "Kernel bridge: a fix")
    release.main(["prepare"])
    old = repo / "docs/releases/xrecomp-v0.13.2.md"
    text = old.read_text().replace(release.PLACEHOLDER, "Highlights by hand.\n\n## Known problems\n\nNone.")
    carry = tmp_path_factory.mktemp("carry") / "carried.md"
    carry.write_text(text)
    old.unlink()
    run("git", "checkout", "-q", "--", "docs/releases/README.md", cwd=repo)
    commit(repo, "Recomp: a pass\n\nRelease: minor")
    release.main(["prepare", "--carry", str(carry)])
    notes = (repo / "docs/releases/xrecomp-v0.14.0.md").read_text()
    assert notes.startswith("# XRecomp 0.14.0\n\nHighlights by hand.\n\n## Known problems\n\nNone.\n\n## Changes\n\n")
    assert notes.count("## Changes") == 1
    assert "- a pass" in notes and "- a fix" in notes


def test_pending(repo, capsys):
    base = commit(repo, "Kernel bridge: a fix")
    (repo / "docs/releases/xrecomp-v0.13.2.md").write_text("# XRecomp 0.13.2\n")
    run("git", "add", ".", cwd=repo)
    head = commit(repo, "Release: XRecomp 0.13.2")
    capsys.readouterr()
    release.main(["pending", base, head])
    assert capsys.readouterr().out == "xrecomp-v0.13.2\n"
    # A later push that does not add notes tags nothing.
    later = commit(repo, "Kernel bridge: another fix")
    release.main(["pending", head, later])
    assert capsys.readouterr().out == ""
    # Already tagged at HEAD (a re-run of the job): still the tag.
    run("git", "tag", "xrecomp-v0.13.2", head, cwd=repo)
    release.main(["pending", base, head])
    assert capsys.readouterr().out == "xrecomp-v0.13.2\n"
    release.main(["pending", head, later])
    assert capsys.readouterr().out == ""
