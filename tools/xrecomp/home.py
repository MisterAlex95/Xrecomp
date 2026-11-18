"""
Where xrecomp keeps its files (docs/desktop/cli.md « Storage »): the platform's user data folder, the same tree as
the macOS launcher's (docs/macos/launcher.md « Storage »), so the app and the command line share a Mac's library.

    <root>/
      settings.json         library folders, work root, Python (the Mac app's file; unknown keys are kept)
      defaults/recomp.cfg   Default settings
      titles/<ID>/          ID: an Xbox title ID (12345678) or a console title (gba-AXVE-1A2B3C4D, ps1-..., n64-...)
        work/               prepare --work (or <work_root>/<ID>)
        recomp.cfg          the game's settings over Default settings (XBA-188's per-game format)
        save/               an Xbox title's RECOMP_SAVE_DIR
        logs/run-*.log      the last 10 runs
        state.json          source path, title, console, play time, last prepare
      packs/<console>-<KEY>/  GBA / PS1 / N64 title packs (prepare --pack-dir)
      save/<console>-<KEY>/   GBA / PS1 / N64 saves and memory cards (the runtimes' RECOMP_SAVE_DIR)

<root> is $XRECOMP_HOME, else:
  Linux and other Unix   $XDG_DATA_HOME/xrecomp (~/.local/share/xrecomp): where gba_play / ps1_play / n64_play
                         and `python -m tools.prepare game.gba` already keep their saves and packs
  macOS                  ~/Library/Application Support/xboxrecomp (the XRecomp app's)
  Windows                %APPDATA%\\xrecomp

Game sources are only read, where they are: nothing copies a disc or an ISO (an Xbox image is read in place, XBA-126).
"""

import datetime
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONSOLES = ("xbox", "gba", "ps1", "n64", "gc")
CONFIG = "recomp.cfg"


def default_root(env=None, platform=None):
    env = os.environ if env is None else env
    platform = platform or sys.platform
    home = env.get("XRECOMP_HOME")
    if home:
        return os.path.abspath(os.path.expanduser(home))
    user = env.get("HOME") or os.path.expanduser("~")
    if platform == "darwin":
        return os.path.join(user, "Library", "Application Support", "xboxrecomp")
    if platform.startswith("win"):
        base = env.get("APPDATA") or os.path.join(env.get("USERPROFILE") or user, "AppData", "Roaming")
        return os.path.join(base, "xrecomp")
    base = env.get("XDG_DATA_HOME") or os.path.join(user, ".local", "share")
    return os.path.join(base, "xrecomp")


def default_library(platform=None):
    """The library folders when settings.json names none: the Mac app's default on a Mac, none elsewhere."""
    return ["~/Documents/xbox"] if (platform or sys.platform) == "darwin" else []


def console_of(game_id):
    """« gba-AXVE-1A2B3C4D » -> « gba »; an Xbox title ID -> « xbox »."""
    head = game_id.split("-", 1)[0].lower()
    return head if head in CONSOLES[1:] and "-" in game_id else "xbox"


def write_atomic(path, data):
    """Through <path>.tmp and a rename: a reader sees the old file or the new one."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w" if isinstance(data, str) else "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def read_json(path):
    try:
        with open(path) as f:
            v = json.load(f)
        return v if isinstance(v, dict) else {}
    except (OSError, ValueError):
        return {}


def iso_now(t=None):
    """A date as the Mac app writes it (Swift's ISO 8601: whole seconds, Z)."""
    t = datetime.datetime.now(datetime.timezone.utc) if t is None else t
    return t.strftime("%Y-%m-%dT%H:%M:%SZ")


class Home:
    def __init__(self, root=None, env=None, platform=None):
        self.root = os.path.abspath(root or default_root(env, platform))
        self.platform = platform or sys.platform

    # ── files ──
    @property
    def settings_file(self):
        return os.path.join(self.root, "settings.json")

    @property
    def defaults_dir(self):
        return os.path.join(self.root, "defaults")

    @property
    def titles(self):
        return os.path.join(self.root, "titles")

    @property
    def packs(self):
        return os.path.join(self.root, "packs")

    @property
    def console_saves(self):
        return os.path.join(self.root, "save")

    def settings(self):
        return read_json(self.settings_file)

    def save_settings(self, s):
        write_atomic(self.settings_file, json.dumps(s, indent=2, sort_keys=True) + "\n")

    def library_folders(self):
        s = self.settings()
        folders = s.get("library_folders")
        if not isinstance(folders, list):
            folders = default_library(self.platform)
        out = []
        for f in folders:
            p = os.path.normpath(os.path.expanduser(str(f)))
            if p and p not in out:
                out.append(p)
        return out

    def work_root(self):
        w = self.settings().get("work_root")
        return os.path.abspath(os.path.expanduser(w)) if isinstance(w, str) and w else None

    def title(self, game_id):
        return TitlePaths(self, game_id)

    def known_ids(self):
        try:
            names = sorted(os.listdir(self.titles))
        except OSError:
            return []
        return [n for n in names if os.path.isdir(os.path.join(self.titles, n))]


class TitlePaths:
    def __init__(self, home, game_id):
        self.home = home
        self.id = game_id if "-" in game_id else game_id.upper()
        self.console = console_of(self.id)
        self.dir = os.path.join(home.titles, self.id)
        w = home.work_root()
        self.work = os.path.join(w, self.id) if w else os.path.join(self.dir, "work")
        self.cfg_dir = self.dir
        self.logs = os.path.join(self.dir, "logs")
        self.state_file = os.path.join(self.dir, "state.json")

    @property
    def save(self):
        """RECOMP_SAVE_DIR: the title's own folder for an Xbox title (the Mac app's), the shared console one
        otherwise (the runtimes add <console>-<KEY>/ themselves)."""
        return os.path.join(self.dir, "save") if self.console == "xbox" else self.home.console_saves

    @property
    def pack(self):
        return os.path.join(self.home.packs, self.id) if self.console != "xbox" else None

    @property
    def discovered(self):
        """What runs found that the lift missed: the next prepare reads it (self-healing, XBA-74 / XBA-158)."""
        return os.path.join(self.work, "discovered.jsonl" if self.console == "xbox" else "discovered.txt")

    def state(self):
        return read_json(self.state_file)

    def save_state(self, st):
        st.setdefault("play_seconds", 0)
        st.setdefault("runs", 0)
        write_atomic(self.state_file, json.dumps(st, indent=2, sort_keys=True) + "\n")

    def update_state(self, **kv):
        st = self.state()
        for k, v in kv.items():
            if v is None:
                st.pop(k, None)
            else:
                st[k] = v
        self.save_state(st)
        return st
