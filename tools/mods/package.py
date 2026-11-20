"""
mod.json: the schema of a mod package, loading and validation (docs/technical/mods.md « mod.json »).

A mod is a folder named after its id, holding mod.json and the files it names:

    <id>/
      mod.json
      patches/...     BPS or IPS patches of the game's binary (XBE, ROM...), applied before Prepare in load order

load(folder, console=..., title_id=..., binary_sha256=...) reads and checks one; a ModError says what is wrong in
words a player can act on (which mod, which field, what was expected).
"""

import hashlib
import json
import os
import re

FORMAT = 1                      # mod.json "format": bumped when an older XRecomp could not read a newer mod
MOD_JSON = "mod.json"
MOD_JSON_MAX = 64 * 1024

CONSOLES = ("xbox", "x360", "gba", "ps1", "n64", "gc")
ID_RE = re.compile(r"[a-z0-9][a-z0-9._-]{0,63}\Z")
VERSION_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9.+_-]{0,31}\Z")
HEX8_RE = re.compile(r"[0-9A-Fa-f]{8}\Z")
GAME_KEY_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")
SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
PATCH_KINDS = {".bps": "bps", ".ips": "ips"}
TEXT_MAX = 200

KEYS = {"format", "id", "name", "version", "console", "title_id", "binary", "patches", "display", "description",
        "authors", "url"}


class ModError(ValueError):
    """A mod that cannot be used, and why. code: a stable word for callers (BAD_MOD, WRONG_TITLE, WRONG_BINARY,
    NEWER_FORMAT, UNSAFE_PATH, MISSING)."""

    def __init__(self, msg, code="BAD_MOD", mod=None):
        ValueError.__init__(self, msg)
        self.code = code
        self.mod = mod


class Mod(object):
    """One checked mod: its folder, its mod.json fields, and the sha256 of its mod.json bytes."""

    def __init__(self, path, data, json_sha256):
        self.path = path
        self.data = data
        self.json_sha256 = json_sha256
        self.id = data["id"]
        self.name = data["name"]
        self.version = data["version"]
        self.console = data["console"]
        self.title_id = data["title_id"]
        self.binary = data.get("binary")
        self.patches = [os.path.join(path, p) for p in data.get("patches", [])]
        self.display = data.get("display")

    def __repr__(self):
        return "Mod(%s %s)" % (self.id, self.version)

    def patch_kind(self, p):
        return PATCH_KINDS[os.path.splitext(p)[1].lower()]


def _text(v, field, label, n=TEXT_MAX):
    if not isinstance(v, str) or not v.strip() or len(v) > n or any(ord(c) < 0x20 for c in v):
        raise ModError("%s: %s must be a line of text of at most %d characters" % (label, field, n))
    return v


def norm_title_id(console, tid):
    """The title key as the store and the runtime spell it: an Xbox / Xbox 360 title ID in 8 upper-case hex digits;
    another console's game key as given (letters, digits, '.', '_', '-')."""
    if console in ("xbox", "x360"):
        if not isinstance(tid, str) or not HEX8_RE.match(tid):
            raise ModError("title_id %r: an %s title ID is 8 hexadecimal digits (58420001)" % (tid, console))
        return tid.upper()
    if not isinstance(tid, str) or not GAME_KEY_RE.match(tid):
        raise ModError("title_id %r: letters, digits, '.', '_' or '-', at most 64" % (tid,))
    return tid


def safe_path(mod_dir, rel, label="path"):
    """The absolute path of `rel` inside mod_dir, or ModError(UNSAFE_PATH): only relative paths with '/' separators,
    no '..' part, nothing that resolves (through a symbolic link) outside the mod's folder."""
    if not isinstance(rel, str) or not rel or len(rel) > 255:
        raise ModError("%s: a relative path inside the mod expected" % label, "UNSAFE_PATH")
    if "\\" in rel or rel.startswith("/") or re.match(r"[A-Za-z]:", rel) or "\0" in rel:
        raise ModError("%s %r: only relative paths with '/' inside the mod's folder" % (label, rel), "UNSAFE_PATH")
    parts = rel.split("/")
    if any(p in ("..", "") for p in parts):
        raise ModError("%s %r: '..' and empty parts are not allowed (the path must stay inside the mod)"
                       % (label, rel), "UNSAFE_PATH")
    root = os.path.realpath(mod_dir)
    full = os.path.realpath(os.path.join(root, *parts))
    if full != root and not full.startswith(root + os.sep):
        raise ModError("%s %r leaves the mod's folder (a symbolic link to %s)" % (label, rel, full), "UNSAFE_PATH")
    return os.path.join(mod_dir, *parts)


def check_tree(mod_dir, label):
    """Every symbolic link inside the mod's folder must stay inside it (ModError UNSAFE_PATH otherwise): the folder
    is copied, overlaid and hashed as a whole by later steps."""
    root = os.path.realpath(mod_dir)
    for top, dirs, files in os.walk(mod_dir):
        for n in dirs + files:
            p = os.path.join(top, n)
            if os.path.islink(p):
                t = os.path.realpath(p)
                if t != root and not t.startswith(root + os.sep):
                    raise ModError("%s: %s is a symbolic link out of the mod's folder (to %s)"
                                   % (label, os.path.relpath(p, mod_dir), t), "UNSAFE_PATH")


def validate(data, mod_dir, label):
    """Checks a parsed mod.json (dict) against the schema; returns it normalised. ModError on the first problem."""
    if not isinstance(data, dict):
        raise ModError("%s: mod.json must hold one JSON object" % label)
    fmt = data.get("format")
    if isinstance(fmt, bool) or not isinstance(fmt, int) or fmt < 1:
        raise ModError("%s: \"format\" must be a whole number (this XRecomp reads format %d)" % (label, FORMAT))
    if fmt > FORMAT:
        raise ModError("%s: made for a newer XRecomp (mod format %d, this one reads %d): update XRecomp"
                       % (label, fmt, FORMAT), "NEWER_FORMAT")
    for k in data:
        if k not in KEYS and not k.startswith("_"):
            raise ModError("%s: unknown field \"%s\" (fields: %s)" % (label, k, ", ".join(sorted(KEYS))))
    for k in ("id", "name", "version", "console", "title_id"):
        if k not in data:
            raise ModError("%s: \"%s\" is missing" % (label, k))
    out = dict(data)
    if not isinstance(data["id"], str) or not ID_RE.match(data["id"]):
        raise ModError("%s: id %r: lower-case letters, digits, '.', '_' or '-', at most 64" % (label, data["id"]))
    folder = os.path.basename(os.path.normpath(mod_dir))
    if folder != data["id"]:
        raise ModError("%s: id \"%s\" but the folder is \"%s\" (a mod's folder is named after its id)"
                       % (label, data["id"], folder))
    _text(data["name"], "name", label)
    if not isinstance(data["version"], str) or not VERSION_RE.match(data["version"]):
        raise ModError("%s: version %r: letters, digits, '.', '+', '_' or '-' (1.0.0)" % (label, data["version"]))
    if data["console"] not in CONSOLES:
        raise ModError("%s: console %r: one of %s" % (label, data["console"], ", ".join(CONSOLES)))
    try:
        out["title_id"] = norm_title_id(data["console"], data["title_id"])
    except ModError as e:
        raise ModError("%s: %s" % (label, e))
    for k in ("description", "url"):
        if k in data:
            _text(data[k], k, label, 2000 if k == "description" else TEXT_MAX)
    if "authors" in data:
        a = data["authors"]
        if not isinstance(a, list) or len(a) > 32:
            raise ModError("%s: authors: a list of names" % label)
        for x in a:
            _text(x, "authors", label)
    if "binary" in data:
        b = data["binary"]
        if not isinstance(b, dict) or set(b) - {"sha256", "name"} or "sha256" not in b:
            raise ModError("%s: binary: {\"sha256\": <the game binary's SHA-256>, \"name\": <its version>} expected"
                           % label)
        if not isinstance(b["sha256"], str) or not SHA256_RE.match(b["sha256"].lower()):
            raise ModError("%s: binary.sha256: 64 hexadecimal digits" % label)
        out["binary"] = dict(b, sha256=b["sha256"].lower())
        if "name" in b:
            _text(b["name"], "binary.name", label)
    if "patches" in data:
        ps = data["patches"]
        if not isinstance(ps, list) or not ps or len(ps) > 16:
            raise ModError("%s: patches: a list of 1 to 16 patch files" % label)
        if "binary" not in data:
            raise ModError("%s: patches need \"binary\": the SHA-256 of the game binary they were made for" % label)
        seen = set()
        for p in ps:
            full = safe_path(mod_dir, p, "%s: patch" % label)
            ext = os.path.splitext(p)[1].lower()
            if ext not in PATCH_KINDS:
                raise ModError("%s: patch %r: a .bps or .ips file (xdelta is not supported yet)" % (label, p))
            if not os.path.isfile(full):
                raise ModError("%s: patch %r is not in the mod" % (label, p), "MISSING")
            if p in seen:
                raise ModError("%s: patch %r named twice" % (label, p))
            seen.add(p)
    if "display" in data:
        from tools.mods import display
        out["display"] = display.validate(data["display"], label)
    return out


def load(mod_dir, console=None, title_id=None, binary_sha256=None):
    """The checked Mod in mod_dir. With console / title_id: it must be for that game (WRONG_TITLE); with
    binary_sha256 (the game binary's SHA-256, hex): a mod that names a binary must name this one (WRONG_BINARY, the
    message naming the version the mod expects)."""
    label = "mod %s" % os.path.basename(os.path.normpath(mod_dir))
    if not os.path.isdir(mod_dir):
        raise ModError("%s: no such folder" % label, "MISSING")
    check_tree(mod_dir, label)
    jp = safe_path(mod_dir, MOD_JSON, label)
    try:
        with open(jp, "rb") as f:
            raw = f.read(MOD_JSON_MAX + 1)
    except OSError:
        raise ModError("%s: no mod.json" % label, "MISSING")
    if len(raw) > MOD_JSON_MAX:
        raise ModError("%s: mod.json is larger than %d KB" % (label, MOD_JSON_MAX // 1024))
    try:
        data = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as e:
        raise ModError("%s: mod.json is not valid JSON (%s)" % (label, e))
    data = validate(data, mod_dir, label)
    m = Mod(mod_dir, data, hashlib.sha256(raw).hexdigest())
    if console is not None and m.console != console:
        raise ModError("%s is a %s mod, this game is a %s game" % (label, m.console, console), "WRONG_TITLE", m)
    if title_id is not None:
        want = norm_title_id(console or m.console, title_id)
        if m.title_id != want:
            raise ModError("%s (%s) is made for title %s, this game is %s" % (label, m.name, m.title_id, want),
                           "WRONG_TITLE", m)
    if binary_sha256 is not None and m.binary and m.binary["sha256"] != binary_sha256.lower():
        raise ModError(wrong_binary_msg(m, binary_sha256), "WRONG_BINARY", m)
    return m


def wrong_binary_msg(m, have):
    name = m.binary.get("name")
    return ("mod %s (%s) is made for another version of this game: %s(SHA-256 %s...), yours is %s..."
            % (m.id, m.name, ("%s " % name) if name else "", m.binary["sha256"][:12], have.lower()[:12]))
