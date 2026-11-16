"""
The library: what a source is (console, title, key), where its files go, and how prepared it is, with the Android
app's chips (app-shell.md: « Prepared », « Prepared · out of date », « Not prepared »).

The console is told the way `python -m tools.prepare` tells it (an N64 ROM's first word, a GBA header or .gba, a PS1
disc or EXE, else an Xbox disc, folder or default.xbe), with the same modules, so the two never disagree.
"""

import hashlib
import os

from tools.xrecomp.home import console_of

PREPARED, STALE, ABSENT = "prepared", "stale", "absent"
MISSING, UNREADABLE = "missing", "unreadable"
LABELS = {PREPARED: "Prepared", STALE: "Prepared · out of date", ABSENT: "Not prepared",
          MISSING: "Source gone", UNREADABLE: "Not readable"}

SCAN_DEPTH = 3
XBOX_IMAGE_EXT = (".iso", ".xiso", ".cso", ".zso", ".cci")
ROM_EXT = (".gba", ".z64", ".v64", ".n64")
PS1_EXT = (".cue", ".chd", ".bin", ".img", ".exe")


class Entry:
    """One game: id (titles/<id>), console, title, source, and after state(): state, reasons, exe / pack."""

    def __init__(self, source, console=None, game_id=None, title=None, key=None, sha256=None, error=None):
        self.source, self.console, self.id, self.title = source, console, game_id, title
        self.key, self.sha256, self.error = key, sha256, error
        self.state, self.reasons, self.note = None, [], None
        self.exe = self.pack_lib = None
        self.env_force = {}
        self.title_layer = None

    @property
    def label(self):
        return LABELS.get(self.state, self.state or "")

    def as_dict(self):
        return {"id": self.id, "console": self.console, "title": self.title, "source": self.source,
                "state": self.state, "label": self.label, "reasons": self.reasons, "note": self.note,
                "exe": self.exe, "pack": self.pack_lib, "error": self.error}


# ── identify ────────────────────────────────────────────────────

def console_of_source(path):
    """"n64" | "gba" | "ps1" | "xbox", in tools.prepare's order."""
    from tools.prepare import gba, n64, ps1
    if n64.is_n64(path):
        return "n64"
    if gba.is_gba(path):
        return "gba"
    if ps1.is_ps1(path):
        return "ps1"
    return "xbox"


def identify(path):
    """An Entry for a source; Entry.error says why when it cannot be read."""
    src = os.path.abspath(os.path.expanduser(path))
    if not os.path.exists(src):
        return Entry(src, error="no such file or folder")
    try:
        console = console_of_source(src)
    except Exception as e:      # a reader's bug must not stop a scan
        return Entry(src, error="%s: %s" % (type(e).__name__, e))
    try:
        if console == "gba":
            from tools.prepare import gba
            with open(src, "rb") as f:
                rom = f.read(gba.ROM_MAX + 1)
            if not rom or len(rom) > gba.ROM_MAX:
                raise ValueError("%d bytes, a GBA ROM is 1 byte to 32 MB" % len(rom))
            key = gba.title_key(rom)
            title = gba.rom_title(rom, os.path.splitext(os.path.basename(src))[0])
            return Entry(src, "gba", "gba-" + key, title, key, hashlib.sha256(rom).hexdigest())
        if console == "ps1":
            from tools.prepare import ps1
            key, _boot, exe = ps1.read_source(src)
            base = os.path.basename(src)
            return Entry(src, "ps1", "ps1-" + key, os.path.splitext(base)[0] or base, key,
                         hashlib.sha256(exe).hexdigest())
        if console == "n64":
            from tools.prepare import n64
            key, title, _order, sha, _size = n64.read_source(src)
            return Entry(src, "n64", "n64-" + key, title, key, sha)
        from tools.prepare import identify as xid
        info = xid.identify(src)
        tid = info["title_id"]
        return Entry(src, "xbox", tid, info.get("profile_name") or info["name"], tid)
    except Exception as e:
        code = getattr(e, "code", None)
        msg = getattr(e, "msg", None) or str(e) or type(e).__name__
        return Entry(src, console, error=("%s: %s" % (code, msg)) if code else msg)


def candidates(folder, depth=SCAN_DEPTH):
    """The sources under folder, `depth` levels deep: Xbox folders (default.xbe, not descended into), disc images,
    ROMs, PS1 discs (a folder with a .cue: its .bin / .img tracks are the cue's, not games of their own)."""
    out = []

    def walk(d, level):
        try:
            names = sorted(os.listdir(d))
        except OSError:
            return
        low = {n.lower(): n for n in names}
        if "default.xbe" in low and level > 0:
            out.append(d)
            return
        has_cue = any(n.endswith(".cue") for n in low)
        for n in names:
            p = os.path.join(d, n)
            ln = n.lower()
            if os.path.isdir(p):
                if level < depth and not n.startswith("."):
                    walk(p, level + 1)
                continue
            ext = os.path.splitext(ln)[1]
            if ext in (".bin", ".img") and has_cue:
                continue
            if ext == ".exe" or (ln == "default.xbe" and level == 0):
                continue        # a bare EXE / XBE is given by hand, not scanned
            if ext in XBOX_IMAGE_EXT or ext in ROM_EXT or ext in PS1_EXT:
                out.append(p)
    walk(os.path.abspath(os.path.expanduser(folder)), 0)
    return out


# ── state ───────────────────────────────────────────────────────

def _lines(path):
    try:
        with open(path, errors="replace") as f:
            return sum(1 for l in f if l.strip() and not l.lstrip().startswith("#"))
    except OSError:
        return 0


def describe(reasons):
    """Why prepare again, in the Mac app's words (TitleState.describe)."""
    r = [x.lower() for x in reasons]
    if any("discover" in x for x in r):
        return "improvements found while playing: prepare again"
    if any("toolkit" in x for x in r):
        return "prepare again (toolkit updated)"
    if any("profile" in x for x in r):
        return "prepare again (enhancements or title profile changed)"
    if any("xbe" in x for x in r):
        return "prepare again (the game's files changed)"
    if "not_built" in r:
        return "the last build did not finish: prepare again"
    if "runtime" in r:
        return "prepare again to rebuild (runtime updated)"
    if "no_stamp" in r:
        return "prepare again (made outside xrecomp)"
    return "prepare again (%s)" % ", ".join(reasons) if reasons else "prepare again"


def with_title_layer(enabled, fn, *args):
    """fn(*args) with XBA_NO_TITLE_LAYER as a prepare of this title would have it."""
    old = os.environ.get("XBA_NO_TITLE_LAYER")
    if enabled:
        os.environ.pop("XBA_NO_TITLE_LAYER", None)
    else:
        os.environ["XBA_NO_TITLE_LAYER"] = "1"
    try:
        return fn(*args)
    finally:
        if old is None:
            os.environ.pop("XBA_NO_TITLE_LAYER", None)
        else:
            os.environ["XBA_NO_TITLE_LAYER"] = old


def state(entry, home):
    """Fills entry.state (prepared / stale / absent), reasons, note, exe or pack_lib. Reads only."""
    if entry.error:
        entry.state = MISSING if not os.path.exists(entry.source) else UNREADABLE
        return entry
    t = home.title(entry.id)
    if entry.console == "xbox":
        from tools.prepare import __main__ as prep
        enhancements = t.state().get("enhancements") is not False
        try:
            out = with_title_layer(enhancements, prep.check_state, entry.source, t.work)
        except Exception as e:
            entry.state, entry.error = UNREADABLE, "%s: %s" % (getattr(e, "code", type(e).__name__),
                                                               getattr(e, "msg", None) or e)
            return entry
        entry.title = out.get("title") or entry.title
        entry.exe = out.get("exe")
        entry.env_force = out.get("env_force") or {}
        entry.title_layer = out.get("title_layer")
        entry.reasons = list(out.get("reasons") or [])
        entry.state = {"ready": PREPARED, "stale": STALE}.get(out["state"], ABSENT)
        if entry.state == STALE:
            entry.note = describe(entry.reasons)
        elif entry.state == ABSENT:
            entry.note = "not built yet: prepare it to play" if entry.reasons else "prepare it to play"
        return entry
    from tools.prepare import gba, toolkit
    cm = _console_module(entry.console)
    pack_dir = t.pack
    why = cm.check(pack_dir, entry.key, entry.sha256, gba.host_abi(), verify_hash=False)
    if why is not None:
        entry.state = ABSENT
        entry.note = "plays in the interpreter" + ("" if why == "no pack.json" else " (pack: %s)" % why)
        return entry
    meta = cm.read(pack_dir)
    entry.pack_lib = os.path.join(pack_dir, meta["lib"])
    if meta.get("toolkit_sha256") != toolkit.digest():
        entry.state, entry.reasons = STALE, ["toolkit"]
        entry.note = "prepared by an older toolkit; it still plays"
    else:
        entry.state = PREPARED
        entry.note = "runs natively"
    found = _lines(t.discovered)
    new = found - int(meta.get("discovered") or 0)
    if new > 0:
        entry.reasons.append("discovered")
        entry.note += " · %d new address%s found · prepare again" % (new, "" if new == 1 else "es")
    return entry


def _console_module(console):
    from tools.prepare import gba, n64, ps1
    return {"gba": gba, "ps1": ps1, "n64": n64}[console]


# ── the library ─────────────────────────────────────────────────

def library(home, folders=(), with_state=True):
    """Every game: the titles xrecomp knows (titles/<ID>/state.json's source) and the sources found in the library
    folders and `folders`. One entry per id: a known title's recorded source first."""
    by_id, order = {}, []

    def add(e):
        key = e.id or ("?" + e.source)
        if key in by_id:
            old = by_id[key]
            if old.error and not e.error:       # the recorded source is gone, a scanned copy is there
                by_id[key] = e
            return
        by_id[key] = e
        order.append(key)

    seen_sources = set()
    for gid in home.known_ids():
        t = home.title(gid)
        st = t.state()
        src = st.get("source_path")
        if not src:
            continue
        e = identify(src) if os.path.exists(src) else Entry(src, console_of(gid), gid, st.get("title"),
                                                            error="no such file or folder")
        if e.id is None:
            e.id, e.console, e.title = gid, console_of(gid), e.title or st.get("title")
        add(e)
        seen_sources.add(os.path.abspath(src))
    for folder in list(home.library_folders()) + list(folders):
        for src in candidates(folder):
            if os.path.abspath(src) in seen_sources:
                continue
            seen_sources.add(os.path.abspath(src))
            add(identify(src))
    entries = [by_id[k] for k in order]
    if with_state:
        for e in entries:
            state(e, home)
    return entries


class Ambiguous(Exception):
    pass


def resolve(home, ref, folders=()):
    """The Entry a GAME argument names: a source path, an id (titles/<ID>, any case), or a unique part of a title.
    LookupError when nothing matches, Ambiguous when several do."""
    if os.path.exists(os.path.expanduser(ref)):
        e = identify(ref)
        if e.error:
            raise LookupError("%s: %s" % (ref, e.error))
        return e
    entries = library(home, folders, with_state=False)
    low = ref.lower()
    exact = [e for e in entries if e.id and e.id.lower() == low]
    if exact:
        return exact[0]
    part = [e for e in entries if e.id and (low in (e.title or "").lower() or e.id.lower().startswith(low))]
    if len(part) == 1:
        return part[0]
    if part:
        raise Ambiguous("%r names several games: %s" % (ref, ", ".join("%s (%s)" % (e.id, e.title) for e in part)))
    raise LookupError("no game %r in the library (xrecomp list; a source path works too)" % ref)
