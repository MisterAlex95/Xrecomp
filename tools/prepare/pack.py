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
import shutil

FORMAT = 1
ABI = "arm64-v8a"
LIB = "libtitle.so"
ENTRY = "recomp_title_main"
# The shell's ABI version: what the app shell exports, bumped together with it
# when what a title may link against changes. The app passes its own value
# (--shell-abi); this is the default.
SHELL_ABI = 1

FIELDS = ("format", "title_id", "title", "xbe_sha256", "abi", "shell_abi", "toolkit", "lib",
          "lib_sha256", "lib_size", "entry", "cc", "built_at")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def make_meta(lib_path, title_id, title, xbe_sha256, toolkit, cc, shell_abi=SHELL_ABI, now=None):
    now = now or datetime.datetime.now(datetime.timezone.utc)
    return {
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


def partial_dir(packs, tid):
    return os.path.join(packs, tid.upper() + ".partial")


def final_dir(packs, tid):
    return os.path.join(packs, tid.upper())


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
    return None
