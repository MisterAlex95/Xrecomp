"""xrecomp (tools/xrecomp, docs/desktop/cli.md): the user folders per platform, recomp.cfg and the per-game layer,
the library's states for an Xbox title (the synthetic conformance XBE) and GBA / PS1 / N64 games (homebrew-shaped
synthetic ROMs, a GBA pack built end to end with the fake lifter), and the play command lines. Every test runs in a
temporary home ($XRECOMP_HOME)."""

import io
import json
import os
import shutil
import stat
import sys
from contextlib import redirect_stdout

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, ROOT)

from tools.prepare import gba, toolkit                         # noqa: E402
from tools.prepare.test_gba_prepare import make_rom as gba_rom  # noqa: E402
from tools.prepare.test_n64_prepare import make_rom as n64_rom  # noqa: E402
from tools.prepare.test_ps1_prepare import make_exe as ps1_exe  # noqa: E402
from tools.prepare.test_gc_prepare import make_disc as gc_disc  # noqa: E402
from tools.xrecomp import __main__ as cli                      # noqa: E402
from tools.xrecomp import config, library                      # noqa: E402
from tools.xrecomp.home import Home, default_root              # noqa: E402

TEST_XBE = os.path.join(ROOT, "tools", "conformance", "test.xbe")
FAKE_DIR = os.path.join(ROOT, "tools", "prepare", "tests")
CC = shutil.which(os.environ.get("CC", "cc"))


@pytest.fixture
def home(tmp_path, monkeypatch):
    monkeypatch.setenv("XRECOMP_HOME", str(tmp_path / "home"))
    for k in ("XBA_NO_TITLE_LAYER", "XRECOMP_GBA_PLAY", "XRECOMP_PS1_PLAY", "XRECOMP_N64_PLAY", "XRECOMP_GC_PLAY",
              "RECOMP_GPU",
              "RECOMP_RUMBLE", "RECOMP_ASPECT"):
        monkeypatch.delenv(k, raising=False)
    return Home()


def run(*argv):
    out = io.StringIO()
    with redirect_stdout(out):
        rc = cli.main(list(argv))
    return rc, out.getvalue()


def xbox_folder(tmp_path):
    d = tmp_path / "lib" / "Synth"
    d.mkdir(parents=True)
    shutil.copy(TEST_XBE, d / "default.xbe")
    return d


# ── paths ──

def test_default_root_per_platform():
    assert default_root({"HOME": "/h"}, "linux") == "/h/.local/share/xrecomp"
    assert default_root({"HOME": "/h", "XDG_DATA_HOME": "/x"}, "linux") == "/x/xrecomp"
    assert default_root({"HOME": "/h"}, "darwin") == "/h/Library/Application Support/xboxrecomp"
    assert default_root({"HOME": "/h", "APPDATA": "C:/AD"}, "win32") == os.path.join("C:/AD", "xrecomp")
    assert default_root({"HOME": "/h", "XRECOMP_HOME": "/elsewhere"}, "darwin") == "/elsewhere"


def test_title_paths(home, tmp_path):
    x = home.title("1234567a")
    assert x.id == "1234567A" and x.console == "xbox"
    assert x.work == os.path.join(home.root, "titles", "1234567A", "work")
    assert x.save == os.path.join(home.root, "titles", "1234567A", "save")     # the Mac app's per-title save
    assert x.discovered.endswith(os.path.join("work", "discovered.jsonl")) and x.pack is None
    g = home.title("gba-AXVE-1A2B3C4D")
    assert g.console == "gba" and g.save == os.path.join(home.root, "save")    # the runtimes add gba-<KEY>/
    assert g.pack == os.path.join(home.root, "packs", "gba-AXVE-1A2B3C4D")
    assert g.discovered.endswith(os.path.join("work", "discovered.txt"))
    home.save_settings({"work_root": str(tmp_path / "big"), "toolkit_path": "/kept"})
    assert home.title("12345678").work == str(tmp_path / "big" / "12345678")
    assert home.settings()["toolkit_path"] == "/kept"


# ── recomp.cfg ──

def test_recomp_cfg_round_trip(tmp_path):
    (tmp_path / "recomp.cfg").write_text("# mine\r\nRECOMP_GPU = 1\nRECOMP_X=a\nRECOMP_GPU=2\n\nFOO=bar\n")
    c = config.RecompConfig.load(str(tmp_path))
    assert c.get("RECOMP_GPU") == "1" and c.get("NOPE") is None
    assert c.pairs() == [("RECOMP_GPU", "1"), ("RECOMP_X", "a"), ("FOO", "bar")]
    c.set("FOO", None)
    c.set("RECOMP_GPU", "0")            # in place, the duplicate gone
    c.set("RECOMP_NEW", "")             # a game's runtime default
    c.save()
    assert (tmp_path / "recomp.cfg").read_text() == (
        "# mine\nRECOMP_GPU=0\nRECOMP_X=a\n\n%s\nRECOMP_NEW=\n" % config.HEADER)
    c.set("RECOMP_NEW2", "1")           # one header only
    assert c.lines.count(config.HEADER) == 1
    empty = config.RecompConfig.load(str(tmp_path / "none"))
    empty.set("A", "1")
    empty.set("A", None)
    assert empty.save() is False and not (tmp_path / "none").exists()


def test_parse_assignment():
    assert config.parse_assignment(" RECOMP_GPU = 1 ") == ("RECOMP_GPU", "1")
    assert config.parse_assignment("RECOMP_GPU=") == ("RECOMP_GPU", "")
    for bad in ("RECOMP_GPU", "1X=2", "RECOMP_SAVE_DIR=/x", "PATH=/bin"):
        with pytest.raises(ValueError):
            config.parse_assignment(bad)


def test_game_env_layers():
    d = config.RecompConfig(["RECOMP_GPU=1", "RECOMP_RUMBLE=0", "RECOMP_SAVE_DIR=/no", "PATH=/no"])
    g = config.RecompConfig(["RECOMP_GPU=", "RECOMP_ASPECT=16:9", "RECOMP_ASPECT=4:3"])
    env = config.game_env({"PATH": "/bin", "RECOMP_GPU": "1", "HOME": "/h"}, d, g, "/saves")
    assert env == {"PATH": "/bin", "HOME": "/h", "RECOMP_RUMBLE": "0", "RECOMP_ASPECT": "16:9",
                   "RECOMP_SAVE_DIR": "/saves"}
    assert config.effective(g, d, "RECOMP_GPU") == (None, "runtime")
    assert config.effective(g, d, "RECOMP_RUMBLE") == ("0", "defaults")
    assert config.effective(g, d, "RECOMP_ASPECT") == ("16:9", "game")
    assert config.effective(g, d, "RECOMP_NONE") == (None, "runtime")


def test_settings_command_round_trip(home, tmp_path):
    src = xbox_folder(tmp_path)
    rc, out = run("settings", "RECOMP_GPU=1", "RECOMP_RUMBLE=0")
    assert rc == 0 and "RECOMP_GPU=1" in out
    assert open(os.path.join(home.defaults_dir, "recomp.cfg")).read() == (
        "%s\nRECOMP_GPU=1\nRECOMP_RUMBLE=0\n" % config.HEADER)
    assert home.settings()["global_defaults"] == {"RECOMP_GPU": "1", "RECOMP_RUMBLE": "0"}  # the Mac's copy
    rc, out = run("settings", "RECOMP_RUMBLE=")            # on Default settings: back to the runtime default
    assert "RECOMP_RUMBLE" not in open(os.path.join(home.defaults_dir, "recomp.cfg")).read()

    rc, out = run("settings", str(src), "RECOMP_GPU=", "RECOMP_ASPECT=16:9")
    assert rc == 0
    cfg = os.path.join(home.root, "titles", "12345678", "recomp.cfg")
    assert open(cfg).read() == "%s\nRECOMP_GPU=\nRECOMP_ASPECT=16:9\n" % config.HEADER
    assert "RECOMP_GPU=  (runtime default, this game)" in out and "RECOMP_ASPECT=16:9  (this game)" in out
    # The game is now known: by its ID, any case, or a part of its title.
    rc, out = run("settings", "12345678", "--unset", "RECOMP_GPU")
    assert "RECOMP_GPU=1  (as in Default settings)" in out
    with open(cfg, "a") as f:
        f.write("# my note\n")
    rc, out = run("settings", "synth", "--reset")
    assert rc == 0 and open(cfg).read() == "%s\n# my note\n" % config.HEADER
    assert run("settings", "nosuchgame", "A=1")[0] == 1
    assert run("settings", "RECOMP_SAVE_DIR=/x")[0] == 2


# ── the library ──

def test_scan_candidates(tmp_path):
    lib = tmp_path / "lib"
    (lib / "Xbox Game").mkdir(parents=True)
    (lib / "Xbox Game" / "DEFAULT.XBE").write_bytes(b"x")
    (lib / "Xbox Game" / "media.iso").write_bytes(b"x")     # inside a disc folder: not a game of its own
    (lib / "ps1").mkdir()
    for n in ("Game.cue", "Game (Track 1).bin", "Game (Track 2).bin"):
        (lib / "ps1" / n).write_bytes(b"x")
    (lib / "a" / "b" / "c").mkdir(parents=True)
    (lib / "a" / "b" / "c" / "deep.gba").write_bytes(b"x")
    (lib / "a" / "b" / "c" / "d").mkdir()
    (lib / "a" / "b" / "c" / "d" / "too-deep.gba").write_bytes(b"x")
    (lib / "rom.z64").write_bytes(b"x")
    (lib / "notes.txt").write_bytes(b"x")
    got = sorted(os.path.relpath(p, lib) for p in library.candidates(str(lib)))
    assert got == ["Xbox Game", os.path.join("a", "b", "c", "deep.gba"), os.path.join("ps1", "Game.cue"), "rom.z64"]


def test_identify_each_console(tmp_path):
    (tmp_path / "g.gba").write_bytes(gba_rom())
    (tmp_path / "m.z64").write_bytes(n64_rom())
    (tmp_path / "boot.exe").write_bytes(ps1_exe())
    e = library.identify(str(tmp_path / "g.gba"))
    assert (e.console, e.id, e.title) == ("gba", "gba-" + gba.title_key(gba_rom()), "PREPARE TEST")
    e = library.identify(str(tmp_path / "m.z64"))
    assert (e.console, e.id, e.title) == ("n64", "n64-NSME-635A2BFF", "XBA PREPARE TEST")
    e = library.identify(str(tmp_path / "boot.exe"))
    assert e.console == "ps1" and e.id.startswith("ps1-EXE-") and e.sha256
    e = library.identify(TEST_XBE)
    assert (e.console, e.id, e.title) == ("xbox", "12345678", "SYNTH")
    assert library.identify(str(tmp_path / "gone.iso")).error
    (tmp_path / "junk.iso").write_bytes(b"\0" * 4096)
    assert library.identify(str(tmp_path / "junk.iso")).error


def test_identify_gamecube(tmp_path):
    """XBA-274: a GameCube disc (an .iso read by content, before the PS1's) and a homebrew DOL: their keys and the
    main DOL's SHA-256, the identity a GameCube pack.json records."""
    import hashlib
    import zlib
    disc, _g, dol, _raw, _lo = gc_disc(tmp_path)
    e = library.identify(str(disc))
    assert (e.console, e.id, e.title, e.key) == ("gc", "gc-GXRP01-1", "XRecomp disc test", "GXRP01-1")
    assert e.sha256 == hashlib.sha256(dol).hexdigest() and not e.error
    (tmp_path / "prog.dol").write_bytes(dol)
    e = library.identify(str(tmp_path / "prog.dol"))
    assert (e.console, e.id, e.title) == ("gc", "gc-DOL-%08X" % (zlib.crc32(dol) & 0xFFFFFFFF), "prog")
    assert sorted(os.path.basename(p) for p in library.candidates(str(tmp_path))) == ["Game.iso", "prog.dol"]
    from tools.xrecomp.home import console_of
    assert console_of("gc-GXRP01-1") == "gc"


def test_gamecube_list_and_play(home, tmp_path, monkeypatch):
    """XBA-274: a GameCube game in the library plays through gc_play (in the interpreter until prepared), its saves
    in the consoles' save folder (RECOMP_SAVE_DIR: gc_play's memory cards and states)."""
    disc, _g, _dol, _raw, _lo = gc_disc(tmp_path)
    player = _fake_player(tmp_path, monkeypatch, "gc")
    rc, out = run("list", str(tmp_path), "--json")
    [e] = json.loads(out)
    assert (e["id"], e["console"], e["state"], e["note"]) == ("gc-GXRP01-1", "gc", "absent",
                                                              "plays in the interpreter")
    rc, out = run("play", str(disc), "--print")
    assert rc == 0
    lines = out.splitlines()
    assert "export RECOMP_SAVE_DIR=" + home.console_saves in lines
    assert lines[-1] == "%s %s" % (player, disc)
    assert cli.console_unsupported("gc", "win32") and "GameCube" in cli.console_unsupported("gc", "win32")


def test_list_states_xbox(home, tmp_path):
    src = xbox_folder(tmp_path)
    rc, out = run("list", str(src.parent), "--json")
    [e] = json.loads(out)
    assert (e["id"], e["console"], e["state"], e["label"]) == ("12345678", "xbox", "absent", "Not prepared")
    # A build there (made outside xrecomp: no stamp): playable, out of date.
    t = home.title("12345678")
    proj = os.path.join(t.work, "project")
    os.makedirs(os.path.join(proj, "build"))
    with open(os.path.join(proj, "CMakeLists.txt"), "w") as f:
        f.write("project(synth C)\n")
    exe = os.path.join(proj, "build", "synth")
    with open(exe, "w") as f:
        f.write("#!/bin/sh\necho played \"$RECOMP_ASPECT\"\n")
    os.chmod(exe, 0o755)
    rc, out = run("list", str(src.parent), "--json")
    [e] = json.loads(out)
    assert e["state"] == "stale" and e["label"] == "Prepared · out of date" and e["reasons"] == ["no_stamp"]
    assert e["exe"] == exe
    rc, out = run("list", str(src.parent))
    assert "12345678  xbox  Prepared · out of date  SYNTH  prepare again (made outside xrecomp)" in out


def test_play_command_line_xbox(home, tmp_path):
    src = xbox_folder(tmp_path)
    t = home.title("12345678")
    proj = os.path.join(t.work, "project")
    os.makedirs(os.path.join(proj, "build"))
    with open(os.path.join(proj, "CMakeLists.txt"), "w") as f:
        f.write("project(synth C)\n")
    exe = os.path.join(proj, "build", "synth")
    with open(exe, "w") as f:
        f.write("#!/bin/sh\necho \"played $RECOMP_ASPECT gpu=${RECOMP_GPU-unset}\"\n")
    os.chmod(exe, 0o755)
    run("settings", "RECOMP_GPU=1")
    run("settings", str(src), "RECOMP_ASPECT=16:9", "RECOMP_GPU=")
    rc, out = run("play", "12345678", "--print")
    assert rc == 0
    lines = out.splitlines()
    assert lines[0] == "cd " + os.path.dirname(exe)
    assert "export RECOMP_ASPECT=16:9" in lines and "export RECOMP_SAVE_DIR=" + t.save in lines
    assert not any("RECOMP_GPU" in l for l in lines)       # the game's KEY= beats Default settings
    assert lines[-1] == exe
    if os.name == "posix":
        rc, out = run("play", "synth")
        assert rc == 0
        st = t.state()
        assert st["runs"] == 1 and st["source_path"] == str(src) and st["last_played"].endswith("Z")
        log = open(os.path.join(t.logs, os.listdir(t.logs)[0])).read()
        assert "played 16:9 gpu=unset" in log and "# RECOMP_ASPECT=16:9" in log
        assert os.path.isdir(t.save)


def test_play_not_prepared_says_so(home, tmp_path):
    src = xbox_folder(tmp_path)
    rc, _ = run("play", str(src), "--print")
    assert rc == 1


def _fake_player(tmp_path, monkeypatch, console):
    p = tmp_path / ("%s_play" % console)
    p.write_text("#!/bin/sh\necho \"$@\"\n")
    p.chmod(p.stat().st_mode | stat.S_IEXEC)
    monkeypatch.setenv("XRECOMP_%s_PLAY" % console.upper(), str(p))
    return str(p)


def test_console_not_prepared_plays_interpreted(home, tmp_path, monkeypatch):
    rom = tmp_path / "m.z64"
    rom.write_bytes(n64_rom())
    player = _fake_player(tmp_path, monkeypatch, "n64")
    rc, out = run("list", str(tmp_path), "--json")
    [e] = json.loads(out)
    assert (e["id"], e["state"], e["note"]) == ("n64-NSME-635A2BFF", "absent", "plays in the interpreter")
    run("settings", "RECOMP_N64_PAK=rumble")
    rc, out = run("play", str(rom), "--print")
    assert rc == 0
    lines = out.splitlines()
    assert lines[0] == "cd " + ROOT
    assert "export RECOMP_N64_PAK=rumble" in lines and "export RECOMP_SAVE_DIR=" + home.console_saves in lines
    assert lines[-1] == "%s %s" % (player, rom)            # no pack: no --pack, no --missed


def test_find_player_in_a_build_folder(tmp_path):
    exe = tmp_path / "build-x" / "consoles" / "ps1" / "runtime" / "ps1_play"
    exe.parent.mkdir(parents=True)
    exe.write_text("")
    exe.chmod(0o755)
    env = {"PATH": str(tmp_path / "nothing")}
    assert cli.find_player("ps1", env, str(tmp_path)) == str(exe)
    assert cli.find_player("gba", env, str(tmp_path)) is None
    assert cli.find_player("gba", dict(env, XRECOMP_GBA_PLAY="/x/gba_play"), str(tmp_path)) == "/x/gba_play"


def test_find_player_in_a_release_archive(tmp_path, monkeypatch):
    """A release archive's players sit with its prebuilt runtime (runtime/<host>/bin, XBA-87); a build folder of
    the toolkit wins over them."""
    from tools.prepare import runtime
    monkeypatch.delenv("XBA_HOST", raising=False)
    exe = tmp_path / "runtime" / runtime.host_id() / "bin" / "n64_play"
    exe.parent.mkdir(parents=True)
    exe.write_text("")
    exe.chmod(0o755)
    env = {"PATH": str(tmp_path / "nothing")}
    assert cli.find_player("n64", env, str(tmp_path)) == str(exe)
    assert cli.find_player("ps1", env, str(tmp_path)) is None
    built = tmp_path / "build" / "consoles" / "n64" / "runtime" / "n64_play"
    built.parent.mkdir(parents=True)
    built.write_text("")
    built.chmod(0o755)
    assert cli.find_player("n64", env, str(tmp_path)) == str(built)


@pytest.mark.skipif(not CC, reason="no host C compiler")
def test_gba_prepare_list_play(home, tmp_path, monkeypatch):
    monkeypatch.setenv("XBA_GBA_RECOMP", "fake_gba_recomp")
    monkeypatch.setenv("XBA_GBA_TOOLS_DIR", FAKE_DIR)
    monkeypatch.setenv("XBA_PYTHON", sys.executable)
    rom = tmp_path / "lib" / "Game.gba"
    rom.parent.mkdir()
    rom.write_bytes(gba_rom())
    key = "gba-" + gba.title_key(gba_rom())
    rc, out = run("prepare", str(rom), "--print")
    assert rc == 0 and "--pack-dir %s" % home.packs in out and not os.path.exists(home.titles)
    rc, out = run("prepare", str(rom))
    assert rc == 0, out
    assert "[5/5] pack" in out and "Prepared: %s" % key in out
    t = home.title(key)
    st = t.state()
    assert st["source_path"] == str(rom) and st["console"] == "gba" and st["last_prepare"]["ok"] is True
    assert os.path.isfile(os.path.join(home.packs, key, "pack.json"))
    assert os.path.isfile(os.path.join(t.work, "stamp.json"))
    assert not os.path.exists(os.path.join(home.root, "work"))   # everything under titles/ and packs/

    # Known now: listed without any library folder.
    rc, out = run("list", "--json")
    [e] = json.loads(out)
    assert (e["id"], e["state"], e["label"], e["note"]) == (key, "prepared", "Prepared", "runs natively")

    player = _fake_player(tmp_path, monkeypatch, "gba")
    rc, out = run("play", key, "--print")
    lib = os.path.join(home.packs, key, "libtitle.so")
    assert out.splitlines()[-1] == "%s %s --pack %s --missed %s" % (player, rom, lib, t.discovered)

    # A run found code the pack did not cover: still prepared, prepare again offered.
    with open(t.discovered, "w") as f:
        f.write("0x08000101\n0x080001F0\n")
    [e] = json.loads(run("list", "--json")[1])
    assert e["state"] == "prepared" and e["note"] == "runs natively · 2 new addresses found · prepare again"
    rc, out = run("prepare", key)       # by its ID too: the lift takes them
    assert rc == 0
    assert json.loads(open(os.path.join(home.packs, key, "pack.json")).read())["discovered"] == 2
    assert json.loads(run("list", "--json")[1])[0]["note"] == "runs natively"

    # Another toolkit made the pack: out of date, still playable with its pack.
    pj = os.path.join(home.packs, key, "pack.json")
    meta = json.loads(open(pj).read())
    meta["toolkit_sha256"] = "0" * 64
    open(pj, "w").write(json.dumps(meta))
    [e] = json.loads(run("list", "--json")[1])
    assert e["state"] == "stale" and e["label"] == "Prepared · out of date" and toolkit.digest() != "0" * 64
    assert "--pack" in run("play", key, "--print")[1]

    # The source moved away: the title stays, its state says so.
    os.remove(rom)
    [e] = json.loads(run("list", "--json")[1])
    assert e["state"] == "missing" and e["label"] == "Source gone"


def test_consoles_refused_on_windows(home, tmp_path, monkeypatch):
    """XBA-89: Windows prepares and plays Xbox games only; the console runtimes are POSIX-only."""
    assert cli.console_unsupported("xbox", "win32") is None
    assert cli.console_unsupported("gba", "linux") is None
    why = cli.console_unsupported("ps1", "win32")
    assert why.startswith("PS1 games are not supported on Windows") and "POSIX-only" in why
    rom = tmp_path / "m.z64"
    rom.write_bytes(n64_rom())
    _fake_player(tmp_path, monkeypatch, "n64")
    monkeypatch.setattr(cli.sys, "platform", "win32")
    rc, out = run("play", str(rom), "--print")
    assert rc == 1
    rc, out = run("prepare", str(rom), "--print")
    assert rc == 1
