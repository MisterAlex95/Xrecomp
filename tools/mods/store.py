"""
The per-title mod store, the enabled list in load order, and the mod-set hash (docs/technical/mods.md « Store »).

    <mods root>/<console>/<title id>/
      enabled.json        {"format": 1, "enabled": ["first", "second"]}: the mods turned on, in load order
      <mod id>/mod.json   one folder per imported mod (tools.mods.package)

Load order: the first is applied first, a later mod applies on top of it (its patch sees the earlier ones' result;
where two change the same thing, the later wins). The mods root is --mods / $XBA_MODS_DIR, else <user data>/mods
(XRECOMP_HOME, or the platform's folder: the same root as xrecomp's, tools/xrecomp/home.py).

The mod-set hash names exactly which mods are on, at which version and in which order: a save state made under
another mod set is refused (src/kernel/xbox_state.c, the identity's mod set), and the Prepare stamp holds it.
"""

import hashlib
import json
import os
import sys

from tools.mods import package
from tools.mods.package import ModError

ENABLED = "enabled.json"
ENABLED_FORMAT = 1
MODSET_TAG = b"xrecomp-modset-1\n"
ENV_DIR = "XBA_MODS_DIR"
ENV_MOD_SET = "RECOMP_MOD_SET"


def default_root(env=None, platform=None):
    """$XBA_MODS_DIR, else <XRECOMP_HOME or the platform's user data folder>/mods."""
    env = os.environ if env is None else env
    platform = platform or sys.platform
    if env.get(ENV_DIR):
        return os.path.abspath(os.path.expanduser(env[ENV_DIR]))
    home = env.get("XRECOMP_HOME")
    if home:
        return os.path.join(os.path.abspath(os.path.expanduser(home)), "mods")
    user = env.get("HOME") or os.path.expanduser("~")
    if platform == "darwin":
        base = os.path.join(user, "Library", "Application Support", "xboxrecomp")
    elif platform.startswith("win"):
        base = os.path.join(env.get("APPDATA") or os.path.join(user, "AppData", "Roaming"), "xrecomp")
    else:
        base = os.path.join(env.get("XDG_DATA_HOME") or os.path.join(user, ".local", "share"), "xrecomp")
    return os.path.join(base, "mods")


def title_dir(root, console, title_id):
    if console not in package.CONSOLES:
        raise ModError("console %r: one of %s" % (console, ", ".join(package.CONSOLES)))
    return os.path.join(root, console, package.norm_title_id(console, title_id))


def installed(root, console, title_id):
    """The ids of the mod folders of this title (sorted), checked or not."""
    d = title_dir(root, console, title_id)
    try:
        names = sorted(os.listdir(d))
    except OSError:
        return []
    return [n for n in names if os.path.isfile(os.path.join(d, n, package.MOD_JSON))]


def read_enabled(root, console, title_id):
    """The enabled ids in load order ([] without enabled.json). ModError when the file is damaged."""
    p = os.path.join(title_dir(root, console, title_id), ENABLED)
    try:
        with open(p, encoding="utf-8") as f:
            data = json.load(f)
    except FileNotFoundError:
        return []
    except (OSError, ValueError) as e:
        raise ModError("%s: cannot be read (%s)" % (p, e))
    if not isinstance(data, dict) or not isinstance(data.get("enabled"), list):
        raise ModError("%s: {\"format\": 1, \"enabled\": [mod ids in load order]} expected" % p)
    if isinstance(data.get("format"), int) and data["format"] > ENABLED_FORMAT:
        raise ModError("%s: written by a newer XRecomp (format %s)" % (p, data["format"]), "NEWER_FORMAT")
    ids = data["enabled"]
    for i in ids:
        if not isinstance(i, str) or not package.ID_RE.match(i):
            raise ModError("%s: %r is not a mod id" % (p, i))
    if len(set(ids)) != len(ids):
        raise ModError("%s: a mod is listed twice" % p)
    return list(ids)


def write_enabled(root, console, title_id, ids):
    for i in ids:
        if not isinstance(i, str) or not package.ID_RE.match(i):
            raise ModError("%r is not a mod id" % (i,))
    if len(set(ids)) != len(ids):
        raise ModError("a mod is listed twice")
    d = title_dir(root, console, title_id)
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, ENABLED)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"format": ENABLED_FORMAT, "enabled": list(ids)}, f, indent=1)
        f.write("\n")
    os.replace(tmp, p)


def enable(root, console, title_id, mod_id, position=None):
    """Turns mod_id on (last in the load order, or at `position`); a mod already on moves there. The mod must be
    installed and valid for this title."""
    package.load(os.path.join(title_dir(root, console, title_id), mod_id), console, title_id)
    ids = [i for i in read_enabled(root, console, title_id) if i != mod_id]
    ids.insert(len(ids) if position is None else max(0, min(position, len(ids))), mod_id)
    write_enabled(root, console, title_id, ids)
    return ids


def disable(root, console, title_id, mod_id):
    ids = [i for i in read_enabled(root, console, title_id) if i != mod_id]
    write_enabled(root, console, title_id, ids)
    return ids


def enabled_mods(root, console, title_id, binary_sha256=None):
    """The enabled mods of this title, checked, in load order. A mod that is on but missing or not valid for this
    game (title, binary version) is a ModError: the player asked for it, so it is said, not skipped."""
    d = title_dir(root, console, title_id)
    out = []
    for i in read_enabled(root, console, title_id):
        p = os.path.join(d, i)
        if not os.path.isdir(p):
            raise ModError("mod %s is turned on but not installed (%s)" % (i, p), "MISSING")
        out.append(package.load(p, console, title_id, binary_sha256))
    return out


def mod_set_hash(mods):
    """The mod-set hash of mods in this load order: SHA-256 (hex) over each mod's id, version and mod.json digest;
    "" for no mod. Stable: the same mods, versions and order give the same hash on every host."""
    if not mods:
        return ""
    h = hashlib.sha256(MODSET_TAG)
    for m in mods:
        h.update(("%s\t%s\t%s\n" % (m.id, m.version, m.json_sha256)).encode("utf-8"))
    return h.hexdigest()


def state_mod_set(hexhash):
    """The 32-bit mod set a save state's identity holds (xbox_state.c: the first 8 hex digits of RECOMP_MOD_SET,
    big-endian; 0 is no mod, so a hash that starts with 8 zeros counts as 1)."""
    if not hexhash:
        return 0
    v = int(hexhash[:8], 16)
    return v or 1
