"""
A title pack (docs/android/prepare.md): <packs>/<TITLEID>/{libtitle.so, pack.json}.

Built in <packs>/<TITLEID>.partial, pack.json written last, then renamed into
place: a reader sees a whole pack or none. The loader checks `format`,
`shell_abi` and `lib_sha256` before it dlopens `lib`.
"""

import datetime
import hashlib
import json
import os
import re
import shutil

FORMAT = 1
ABI = "arm64-v8a"
LIB = "libtitle.so"
ENTRY = "recomp_title_main"
# The shell's ABI version: what the app shell exports, bumped together with it
# when what a title may link against changes. The app passes its own value
# (--shell-abi); this is the default.
SHELL_ABI = 1

# title.json "env_force": options the title forces at start-up, over recomp.cfg
# and the launcher's settings (docs/runtime/environment-variables.md « Forced
# by a title »). Same rule as src/platform/recomp_cfg.c recomp_cfg_force_key_ok /
# _value_ok, and at most as many as recomp_pack.h RECOMP_PACK_ENV_FORCE_MAX.
ENV_FORCE_KEY = re.compile(r"RECOMP_[A-Z0-9_]+\Z")
ENV_FORCE_KEY_MAX = 63
ENV_FORCE_VALUE_MAX = 255
ENV_FORCE_MAX = 16

FIELDS = ("format", "title_id", "title", "xbe_sha256", "abi", "shell_abi", "toolkit", "lib",
          "lib_sha256", "lib_size", "entry", "cc", "built_at")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def env_force_of(profile):
    """The profile layers' merged "env_force" as a {key: value} dict ({} when it has
    none), checked: keys RECOMP_[A-Z0-9_]+ (63 characters at most), values
    strings of printable ASCII (255 at most; a number is taken as its text),
    16 entries at most. ValueError says which entry is wrong."""
    env = (profile or {}).get("env_force") or {}
    if not isinstance(env, dict):
        raise ValueError("title.json env_force: an object of RECOMP_* names to strings expected")
    if len(env) > ENV_FORCE_MAX:
        raise ValueError("title.json env_force: %d entries, at most %d" % (len(env), ENV_FORCE_MAX))
    out = {}
    for k, v in env.items():
        if not ENV_FORCE_KEY.match(k) or len(k) > ENV_FORCE_KEY_MAX:
            raise ValueError("title.json env_force: %r is not a RECOMP_[A-Z0-9_]+ name" % k)
        if isinstance(v, bool) or not isinstance(v, (str, int)):
            raise ValueError("title.json env_force: %s must be a string" % k)
        v = str(v)
        if len(v) > ENV_FORCE_VALUE_MAX or any(not (0x20 <= ord(c) <= 0x7e) for c in v):
            raise ValueError("title.json env_force: %s=%r is not printable ASCII of at most %d characters"
                             % (k, v, ENV_FORCE_VALUE_MAX))
        out[k] = v
    return out


def make_meta(lib_path, title_id, title, xbe_sha256, toolkit, cc, shell_abi=SHELL_ABI, now=None,
              env_force=None, layers=None, toolkit_sha256=None, title_layer=None):
    """pack.json's content. env_force (env_force_of) goes in only when it is
    not empty: a pack without it forces nothing. layers: the profile layers
    the pack was built with (tools.prepare.profiles), lowest first; the
    loader does not read them, they say what went in. toolkit_sha256: the
    toolkit digest (tools/prepare/toolkit.py), which the app compares with its
    own to offer a Prepare again after an update (XBA-127). title_layer:
    "on" / "off" when the toolkit has optional enhancements for the title
    (profiles/titles/<id>) and they went in / were left out, which the app's
    « Enhancements » switch is compared with (WP6); absent when it has none."""
    now = now or datetime.datetime.now(datetime.timezone.utc)
    meta = {
        "format": FORMAT,
        "title_id": title_id.upper(),
        "title": title,
        "xbe_sha256": xbe_sha256,
        "abi": ABI,
        "shell_abi": int(shell_abi),
        "toolkit": toolkit,
        "lib": LIB,
        "lib_sha256": sha256_file(lib_path),
        "lib_size": os.path.getsize(lib_path),
        "entry": ENTRY,
        "cc": cc,
        "built_at": now.replace(microsecond=0).isoformat().replace("+00:00", "Z"),
    }
    if env_force:
        meta["env_force"] = dict(env_force)
    if layers:
        meta["layers"] = list(layers)
    if toolkit_sha256:
        meta["toolkit_sha256"] = toolkit_sha256
    if title_layer:
        meta["title_layer"] = title_layer
    return meta


# The lifter's call-site table (tools/recomp/translator.py, recomp_dispatch.c): resume entries when it is not empty.
_CALL_SITE_COUNT = re.compile(r"g_recomp_call_site_count\s*=\s*(\d+)u?\s*;")


def resume_entries(sources):
    """XBA-258: whether the generated code has resume entries (XBA-250), so a state saved in one run resumes in
    another (« Resume where I left off »): recomp_dispatch.c's call-site table holds at least one site. sources: the
    project's C files (cc.collect_sources). False when the file is not among them or says nothing."""
    for src in sources:
        if os.path.basename(src) != "recomp_dispatch.c":
            continue
        try:
            with open(src, errors="replace") as f:
                m = _CALL_SITE_COUNT.search(f.read())
        except OSError:
            return False
        return bool(m and int(m.group(1)) > 0)
    return False


def _fsync_dir(d):
    try:
        fd = os.open(d, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(fd)
    except OSError:
        pass
    finally:
        os.close(fd)


def write_json_atomic(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=1, sort_keys=False)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# Another console's pack folder (XBA-147): "<console>-<title key>", kept as it is.
CONSOLE_DIR = re.compile(r"(gba|ps1|n64|gc)-[A-Za-z0-9_-]{1,40}\Z")


def dir_name(tid):
    """The pack folder's name: an Xbox title ID in upper case, another console's "gba-<KEY>" as it is."""
    return tid if CONSOLE_DIR.match(tid) else tid.upper()


def partial_dir(packs, tid):
    return os.path.join(packs, dir_name(tid) + ".partial")


def final_dir(packs, tid):
    return os.path.join(packs, dir_name(tid))


def begin(packs, tid):
    """A fresh <TID>.partial (an old one is what a killed run left)."""
    recover(packs, tid)
    d = partial_dir(packs, tid)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return d


def recover(packs, tid):
    """Undo a publish cut between its two renames: <TID> missing, <TID>.old
    there (it was the published pack). Removes a stale .old otherwise."""
    final, old = final_dir(packs, tid), final_dir(packs, tid) + ".old"
    if os.path.isdir(old):
        if not os.path.exists(final):
            os.rename(old, final)
        else:
            shutil.rmtree(old, ignore_errors=True)


def publish(packs, tid, meta):
    """Write pack.json last into <TID>.partial, then make it <TID>.

    POSIX cannot rename a directory over a non-empty one, so an existing pack
    moves to <TID>.old first and is removed after; recover() repairs a kill
    between the two renames."""
    part = partial_dir(packs, tid)
    lib = os.path.join(part, meta["lib"])
    with open(lib, "rb") as f:
        os.fsync(f.fileno())
    write_json_atomic(os.path.join(part, "pack.json"), meta)
    _fsync_dir(part)
    final, old = final_dir(packs, tid), final_dir(packs, tid) + ".old"
    shutil.rmtree(old, ignore_errors=True)
    if os.path.exists(final):
        os.rename(final, old)
    os.rename(part, final)
    _fsync_dir(packs)
    shutil.rmtree(old, ignore_errors=True)
    return final


def read(pack_dir):
    """pack.json as a dict, or None (no pack, or not a whole one)."""
    try:
        with open(os.path.join(pack_dir, "pack.json")) as f:
            meta = json.load(f)
    except (OSError, ValueError):
        return None
    if not isinstance(meta, dict) or any(k not in meta for k in FIELDS):
        return None
    return meta


def check(pack_dir, shell_abi=SHELL_ABI, verify_hash=True):
    """None when the pack is loadable by this shell, else why not (the
    loader's checks: format, abi, shell_abi, lib size and sha256)."""
    meta = read(pack_dir)
    if meta is None:
        return "no pack.json"
    if meta["format"] != FORMAT:
        return "format %r, expected %d" % (meta["format"], FORMAT)
    if meta["abi"] != ABI:
        return "abi %r" % meta["abi"]
    if meta["shell_abi"] != shell_abi:
        return "shell_abi %r, this shell is %d" % (meta["shell_abi"], shell_abi)
    lib = os.path.join(pack_dir, meta["lib"])
    if not os.path.isfile(lib) or os.path.getsize(lib) != meta["lib_size"]:
        return "%s missing or of the wrong size" % meta["lib"]
    if verify_hash and sha256_file(lib) != meta["lib_sha256"]:
        return "%s sha256 mismatch" % meta["lib"]
    for path, name in (meta.get("images") or {}).items():   # XBA-82: the disc's other images
        if name == "":                                      # named, prepared on its first launch
            continue
        if "/" in name or name in (".", "..") or not os.path.isfile(os.path.join(pack_dir, name)):
            return "image %s: %s missing" % (path, name)
    return None
