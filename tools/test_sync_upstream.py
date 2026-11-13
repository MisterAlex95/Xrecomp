"""tools/sync_upstream.sh and tools/sync_upstream_resolve.py on a fixture: two
local repositories built in a temp dir, an "upstream" and a fork of it that
renamed an include guard and the thunk table's text (XBA-109, XBA-138). No
network: the fork's `upstream` remote is the local fixture."""

import os
import shutil
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from tools.sync_upstream_resolve import include_guard, translate   # noqa: E402

TOOLS = os.path.dirname(os.path.abspath(__file__))

pytestmark = pytest.mark.skipif(not shutil.which("git") or not shutil.which("bash"), reason="needs git and bash")

ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
           GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")

DSOUND_BASE = """\
/* DirectSound for the guest. */
#ifndef OLDTITLE_DSOUND_XBOX_H
#define OLDTITLE_DSOUND_XBOX_H

int dsound_init(void);
int dsound_play(int voice);

#endif /* OLDTITLE_DSOUND_XBOX_H */
"""

# The guard is already neutral upstream; only the closing comment was renamed.
VIDEO_BASE = """\
#ifndef XBOXRECOMP_VIDEO_PLAYER_H
#define XBOXRECOMP_VIDEO_PLAYER_H

int video_play(const char *path);

#endif /* OLDTITLE_VIDEO_PLAYER_H */
"""

KERNEL_BASE = """\
/* kernel */
/*
 * The kernel thunk table.
 *
 * Legacy define for backward compatibility (a title's address).
 * New code should use xbox_kernel_set_thunk_address() instead.
 */
#define XBOX_KERNEL_THUNK_TABLE_BASE  0x00ABC000  /* default; overridden at runtime */
#define XBOX_KERNEL_THUNK_COUNT 378

/*
 * Set the kernel thunk table address for the current game.
 * If not called, the default (a legacy title's 0x00ABC000) is used.
 */
void xbox_kernel_set_thunk_address(unsigned va, unsigned count);
"""


def git(repo, *args, check=True):
    return subprocess.run(["git", "-C", str(repo), *args], env=ENV, capture_output=True, text=True, check=check)


def commit(repo, files, msg):
    for path, text in files.items():
        p = repo / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        git(repo, "add", path)
    git(repo, "commit", "-q", "-m", msg)


def fork_ours():
    dsound = DSOUND_BASE.replace("OLDTITLE_DSOUND_XBOX_H", "XBOXRECOMP_DSOUND_XBOX_H")
    dsound = dsound.replace("int dsound_init(void);", "int dsound_init(void);   /* ours: a comment */")
    kernel = KERNEL_BASE.replace(
        " * Legacy define for backward compatibility (a title's address).\n"
        " * New code should use xbox_kernel_set_thunk_address() instead.\n",
        " * The initial value is 0, not set: xbox_MemoryLayoutInit() sets the\n"
        " * title's own address and xbox_kernel_bridge_init() refuses to patch\n"
        " * without one.\n")
    kernel = kernel.replace("0x00ABC000  /* default; overridden at runtime */", "0x00000000  /* not set; set at runtime */")
    kernel = kernel.replace(" * If not called, the default (a legacy title's 0x00ABC000) is used.",
                            " * If not called, xbox_kernel_bridge_init() finds no table and patches nothing.")
    video = VIDEO_BASE.replace("OLDTITLE_VIDEO_PLAYER_H", "XBOXRECOMP_VIDEO_PLAYER_H")
    return {"src/audio/dsound_xbox.h": dsound, "src/kernel/kernel.h": kernel, "src/video/video_player.h": video}


def upstream_next():
    """Upstream's next change: lines next to every renamed one."""
    dsound = DSOUND_BASE.replace("int dsound_play(int voice);\n\n#endif",
                                 "int dsound_play(int voice);\nint dsound_stop(int voice);\n#endif")
    dsound = dsound.replace("/* DirectSound for the guest. */", "/* DirectSound for the guest (v2). */")
    kernel = KERNEL_BASE.replace("#define XBOX_KERNEL_THUNK_COUNT 378", "#define XBOX_KERNEL_THUNK_COUNT 379")
    kernel = kernel.replace(" * The kernel thunk table.\n *\n", " * The kernel thunk table (patched at boot).\n *\n")
    kernel = kernel.replace(" * If not called, the default (a legacy title's 0x00ABC000) is used.\n */",
                            " * If not called, the default (a legacy title's 0x00ABC000) is used.\n"
                            " * Call it once.\n */")
    video = VIDEO_BASE.replace("int video_play(const char *path);\n\n#endif",
                               "int video_play(const char *path);\nint video_stop(void);\n#endif")
    return {"src/audio/dsound_xbox.h": dsound, "src/kernel/kernel.h": kernel, "src/video/video_player.h": video}


@pytest.fixture
def repos(tmp_path):
    up, fork = tmp_path / "upstream", tmp_path / "fork"
    up.mkdir()
    git(up, "init", "-q", "-b", "main")
    commit(up, {"src/audio/dsound_xbox.h": DSOUND_BASE, "src/kernel/kernel.h": KERNEL_BASE,
                "src/video/video_player.h": VIDEO_BASE, "README": "upstream\n"}, "upstream base")
    subprocess.run(["git", "clone", "-q", "-o", "upstream", str(up), str(fork)], env=ENV, check=True)
    commit(fork, fork_ours(), "fork: neutral include guard, no title's thunk address")
    commit(fork, {"README": "upstream\nand the fork\n"}, "fork: readme")
    commit(up, upstream_next(), "upstream: next to the renamed lines")
    return up, fork


def sync(fork):
    return subprocess.run(["bash", os.path.join(TOOLS, "sync_upstream.sh")], cwd=str(fork), env=ENV,
                          capture_output=True, text=True)


def test_include_guard_and_translate():
    assert include_guard(DSOUND_BASE) == "OLDTITLE_DSOUND_XBOX_H"
    ours = fork_ours()
    t = translate(DSOUND_BASE, ours["src/audio/dsound_xbox.h"], "src/audio/dsound_xbox.h")
    assert "OLDTITLE" not in t and t.count("XBOXRECOMP_DSOUND_XBOX_H") == 3
    assert "/* ours: a comment */" not in t          # only the renamed text is ours
    k = translate(KERNEL_BASE, ours["src/kernel/kernel.h"], "src/kernel/kernel.h")
    assert k == ours["src/kernel/kernel.h"]
    # Nothing of ours in a file without a guard or a region: unchanged.
    assert translate("int x;\n", "int x;\n", "src/other.c") == "int x;\n"


def test_a_plain_merge_conflicts_on_the_renamed_lines(repos):
    """The control: without the resolver both files conflict."""
    _, fork = repos
    git(fork, "fetch", "-q", "upstream", "main")
    r = git(fork, "merge", "--no-ff", "-q", "-m", "m", "upstream/main", check=False)
    assert r.returncode != 0
    left = sorted(git(fork, "diff", "--name-only", "--diff-filter=U").stdout.split())
    assert left == ["src/audio/dsound_xbox.h", "src/kernel/kernel.h", "src/video/video_player.h"]
    git(fork, "merge", "--abort")


def test_the_sync_resolves_renamed_text_and_merges(repos):
    _, fork = repos
    r = sync(fork)
    assert r.returncode == 0, r.stdout + r.stderr
    for f in ("src/audio/dsound_xbox.h", "src/kernel/kernel.h", "src/video/video_player.h"):
        assert "resolved " + f in r.stdout, r.stdout
    log = git(fork, "log", "-1", "--format=%P %s").stdout
    assert len(log.split()[:2]) == 2 and "Merge upstream" in log
    ours = fork_ours()
    dsound = (fork / "src/audio/dsound_xbox.h").read_text()
    assert dsound == ours["src/audio/dsound_xbox.h"].replace(
        "int dsound_play(int voice);\n\n#endif", "int dsound_play(int voice);\nint dsound_stop(int voice);\n#endif"
    ).replace("/* DirectSound for the guest. */", "/* DirectSound for the guest (v2). */")
    kernel = (fork / "src/kernel/kernel.h").read_text()
    assert "<<<<<<<" not in kernel and "0x00ABC000" not in kernel
    assert "XBOX_KERNEL_THUNK_COUNT 379" in kernel and "(patched at boot)" in kernel and " * Call it once." in kernel
    assert "refuses to patch" in kernel and "finds no table and patches nothing" in kernel
    assert (fork / "src/video/video_player.h").read_text() == VIDEO_BASE.replace(
        "int video_play(const char *path);\n\n#endif /* OLDTITLE_VIDEO_PLAYER_H */",
        "int video_play(const char *path);\nint video_stop(void);\n#endif /* XBOXRECOMP_VIDEO_PLAYER_H */")
    assert (fork / "README").read_text() == "upstream\nand the fork\n"
    assert git(fork, "status", "--porcelain").stdout == ""
    assert sync(fork).returncode == 0                # and again: up to date


def test_a_real_conflict_still_stops_the_sync(repos):
    up, fork = repos
    text = (up / "src/audio/dsound_xbox.h").read_text()
    commit(up, {"src/audio/dsound_xbox.h": text.replace("int dsound_init(void);", "long dsound_init(void);")},
           "upstream: the same line as ours")
    head = git(fork, "rev-parse", "HEAD").stdout
    r = sync(fork)
    assert r.returncode == 1
    assert "src/audio/dsound_xbox.h" in r.stderr and "conflict: src/audio/dsound_xbox.h" in r.stderr
    assert git(fork, "rev-parse", "HEAD").stdout == head          # merge aborted, main unchanged
    assert git(fork, "status", "--porcelain").stdout == ""


def test_the_resolver_says_when_no_merge_is_in_progress(repos):
    _, fork = repos
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "sync_upstream_resolve.py"), "--repo", str(fork)],
                       env=ENV, capture_output=True, text=True)
    assert r.returncode == 2
