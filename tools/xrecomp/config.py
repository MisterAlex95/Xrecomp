"""
recomp.cfg as the launchers write it: the Android app's RecompConfig / GameSettings (XBA-41, XBA-188), the Mac app's
RecompConfig.swift / GameSettings.swift, the runtime's reader (src/platform/recomp_cfg.c).

One KEY=VALUE per line, blanks and # comments skipped, keys and values trimmed, the first line for a key wins.
Writing keeps every other line: a key's first line is rewritten in place (its later duplicates removed), a new key
is appended under the launchers' header.

A game's file layers over Default settings (defaults/recomp.cfg):
  - a key the game's file does not set follows Default settings;
  - KEY=VALUE is the game's own value;
  - KEY= (empty) is the runtime's default for this game even when Default settings set another: the variable is
    unset when the game starts.
"""

import os
import re

from tools.xrecomp.home import CONFIG, write_atomic

# The same header as the Mac app's (RecompConfig.header), so neither adds a second one.
HEADER = "# Written by the launcher's Settings screen; lines you add yourself are kept."
KEY_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
# Never taken from a settings file: the launcher sets them (as the Mac's ChildEnvironment.game).
PROTECTED = ("PATH", "RECOMP_SAVE_DIR")


def key_of(line):
    s = line.strip()
    if not s or s.startswith("#") or "=" not in s:
        return None
    return s.split("=", 1)[0].strip()


class RecompConfig:
    def __init__(self, lines=None, path=None, existed=False):
        self.lines = list(lines or [])
        self.path = path
        self.existed = existed

    @classmethod
    def load(cls, directory):
        path = os.path.join(directory, CONFIG)
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            return cls(path=path)
        text = text.replace("\r\n", "\n")
        if text.endswith("\n"):
            text = text[:-1]
        return cls(text.split("\n") if text else [], path, True)

    def get(self, key):
        """The value the runtime sees (the first line), "" for KEY=, None when the file does not set it."""
        for line in self.lines:
            if key_of(line) == key:
                return line.strip().split("=", 1)[1].strip()
        return None

    def pairs(self):
        """Every key the file sets with its value (first line wins), in file order."""
        seen, out = set(), []
        for line in self.lines:
            k = key_of(line)
            if k and k not in seen:
                seen.add(k)
                out.append((k, self.get(k)))
        return out

    def set(self, key, value):
        """KEY=value in place of the key's first line; None removes every line of the key."""
        placed, out = False, []
        for line in self.lines:
            if key_of(line) != key:
                out.append(line)
            elif value is not None and not placed:
                out.append("%s=%s" % (key, value))
                placed = True
        if value is not None and not placed:
            if HEADER not in out:
                if out and out[-1].strip():
                    out.append("")
                out.append(HEADER)
            out.append("%s=%s" % (key, value))
        self.lines = out

    def clear(self):
        """Every KEY line removed; comments and blank lines stay."""
        self.lines = [l for l in self.lines if key_of(l) is None]

    def save(self):
        """Nothing is created when there was no file and there would be only the header (or nothing)."""
        only_header = all(not l.strip() or l == HEADER for l in self.lines)
        if not self.existed and only_header:
            return False
        write_atomic(self.path, "".join(l + "\n" for l in self.lines))
        self.existed = True
        return True


def parse_assignment(text):
    """« KEY=VALUE » -> (KEY, VALUE); ValueError when it is not one."""
    if "=" not in text:
        raise ValueError("%r: expected KEY=VALUE" % text)
    k, v = text.split("=", 1)
    k, v = k.strip(), v.strip()
    if not KEY_RE.match(k):
        raise ValueError("%r: not a variable name" % k)
    if k in PROTECTED:
        raise ValueError("%s is set by xrecomp itself, not by a settings file" % k)
    if "\n" in v:
        raise ValueError("%s: one line only" % k)
    return k, v


def effective(game, defaults, key):
    """(value or None for the runtime default, where: "game" | "defaults" | "runtime")."""
    v = game.get(key) if game is not None else None
    if v is not None:
        return (v or None), ("game" if v else "runtime")
    v = defaults.get(key)
    if v:
        return v, "defaults"
    return None, "runtime"


def game_env(inherited, defaults, game, save_dir):
    """A game's environment (the Mac's ChildEnvironment.game): the inherited one, then Default settings, then the
    game's lines, then RECOMP_SAVE_DIR. An empty value unsets the variable; PATH and RECOMP_SAVE_DIR are not taken
    from the files."""
    e = dict(inherited)
    for cfg in (defaults, game):
        if cfg is None:
            continue
        for k, v in cfg.pairs():
            if k in PROTECTED:
                continue
            if v:
                e[k] = v
            else:
                e.pop(k, None)
    if save_dir:
        e["RECOMP_SAVE_DIR"] = save_dir
    return e
