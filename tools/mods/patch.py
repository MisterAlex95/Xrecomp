"""
The enabled mods of a title, for Prepare: their patches applied in load order to a copy of the game's binary, and
the options the pack forces for them (docs/technical/mods.md « Prepare »).

    p = patch.plan("xbox", title_id, original_bytes)      # reads the store; ModError says what is wrong
    p.data            the binary to analyse and lift: the original's bytes when no mod patches it
    p.sha256          its SHA-256 (the Prepare stamp's: another patch set re-lifts)
    p.mod_set         the mod-set hash ("" for no mod)
    p.env(xbe_path)   RECOMP_MOD_SET, the display hint's options, RECOMP_MOD_XBE (the patched copy) when patched

The original is only read: the patched bytes go to a copy in the work folder (the caller writes p.data there).
"""

import hashlib
import os

from tools.mods import bps, display, ips, store
from tools.mods.package import ModError, wrong_binary_msg

ENV_XBE = "RECOMP_MOD_XBE"
ENV_OFF = "XBA_NO_MODS"                 # Prepare as if no mod were on (also XBA_NO_TITLE_LAYER, the app's switch)


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def xbe_title_id(data):
    """The title ID (8 upper-case hex digits) in an XBE's certificate, or None when data is not an XBE."""
    if len(data) < 0x120 or data[:4] != b"XBEH":
        return None
    base = int.from_bytes(data[0x104:0x108], "little")
    cert = int.from_bytes(data[0x118:0x11C], "little") - base
    if cert < 0 or cert + 12 > len(data):
        return None
    return "%08X" % int.from_bytes(data[cert + 8:cert + 12], "little")


def apply_mods(data, mods, original_sha=None):
    """data with every enabled mod's patches applied in load order (each patch to the previous one's result).
    ModError naming the mod and the patch when one does not fit."""
    original_sha = original_sha or sha256(data)
    out = bytes(data)
    for m in mods:
        if not m.patches:
            continue
        if m.binary and m.binary["sha256"] != original_sha:
            raise ModError(wrong_binary_msg(m, original_sha), "WRONG_BINARY", m)
        for p in m.patches:
            rel = os.path.relpath(p, m.path)
            try:
                with open(p, "rb") as f:
                    raw = f.read()
            except OSError as e:
                raise ModError("mod %s: patch %s cannot be read (%s)" % (m.id, rel, e), "MISSING", m)
            try:
                out = bps.apply(out, raw) if m.patch_kind(p) == "bps" else ips.apply(out, raw)
            except bps.BpsError as e:
                if e.kind == "source":
                    before = [x.id for x in mods[:mods.index(m)] if x.patches]
                    why = ("it was made for the unmodified game, and %s changed it first: put this mod first or "
                           "turn the other off" % ", ".join(before)) if before else \
                          ("it was made for another version of the game%s" %
                           (" (%s)" % m.binary["name"] if m.binary and m.binary.get("name") else ""))
                    raise ModError("mod %s: patch %s does not fit: %s" % (m.id, rel, why), "WRONG_BINARY", m)
                raise ModError("mod %s: patch %s: %s" % (m.id, rel, e), "BAD_MOD", m)
            except ips.IpsError as e:
                raise ModError("mod %s: patch %s: %s" % (m.id, rel, e), "BAD_MOD", m)
    return out


class Plan(object):
    def __init__(self, console, title_id, mods, data, original_sha):
        self.console = console
        self.title_id = title_id
        self.mods = mods
        self.data = data
        self.original_sha256 = original_sha
        self.sha256 = sha256(data) if data is not None else original_sha
        self.patched = self.sha256 != original_sha
        self.mod_set = store.mod_set_hash(mods)

    def ids(self):
        return [m.id for m in self.mods]

    def env(self, patched_path=None):
        """The options the pack forces for these mods ({} for none): RECOMP_MOD_SET (save states), the display
        hint (tools.mods.display.env), and RECOMP_MOD_XBE=<patched_path> so the runtime loads the patched copy's
        data, not the disc's (Xbox, when a patch changed the binary)."""
        if not self.mods:
            return {}
        env = {store.ENV_MOD_SET: self.mod_set}
        env.update(display.env(self.mods))
        if self.patched and patched_path and self.console == "xbox":
            p = os.path.abspath(patched_path)
            if len(p) > 255 or any(not (0x20 <= ord(c) <= 0x7E) for c in p):
                raise ModError("the patched binary's path cannot be handed to the runtime (non-ASCII or over 255 "
                               "characters): %s" % p)
            env[ENV_XBE] = p
        return env

    def summary(self):
        if not self.mods:
            return "no mod on"
        return "mods on, in load order: %s%s" % (", ".join("%s %s" % (m.id, m.version) for m in self.mods),
                                                 " (binary patched)" if self.patched else "")


def switched_off(env=None):
    env = os.environ if env is None else env
    return env.get(ENV_OFF, "") not in ("", "0") or env.get("XBA_NO_TITLE_LAYER", "") not in ("", "0")


def plan(console, title_id, data, root=None, off=None):
    """The Plan for this game: the store's enabled mods (none when switched off: XBA_NO_MODS=1 or the app's
    Enhancements switch, XBA_NO_TITLE_LAYER=1), their patches applied to data (the original binary's bytes)."""
    original_sha = sha256(data)
    if off is None:
        off = switched_off()
    if off:
        return Plan(console, title_id, [], data, original_sha)
    root = root or store.default_root()
    mods = store.enabled_mods(root, console, title_id, original_sha)
    return Plan(console, title_id, mods, apply_mods(data, mods, original_sha), original_sha)


def enabled_count(console, title_id, root=None):
    """How many mods the store has on for this title, whatever the switch says (0 when unreadable)."""
    try:
        return len(store.read_enabled(root or store.default_root(), console, title_id))
    except ModError:
        return 0
