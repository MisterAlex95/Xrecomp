"""
mod.json "display": how the runtime should present what a mod makes the game draw (docs/technical/mods.md
« display »).

    "display": {
      "frames": "anamorphic-16:9",
      "wide_writes": [ {"va": "0x00010F88", "f32": [16.0, 9.0]} ]
    }

"frames": "anamorphic-16:9" says the mod makes the game draw its 3D scenes squeezed for a 16:9 screen (into the
console's 4:3 frame): the presenter stretches those frames to 16:9 and keeps 2D screens (menus, movies) at 4:3, as
for a title whose frames are wide (src/video/aspect_policy.h). It honours RECOMP_ASPECT: 4:3 never stretches, 16:9
always does, auto does on Android (a 16:9 handheld) and not on a desktop (the window keeps the console's shape).

"wide_writes" are guest data writes the runtime makes only when it does present wide, once the binary is loaded
(and again after a title reboot): a mod whose anamorphic switch is data (a display size the game reads) draws 4:3
when they are not made, so the picture is right with either setting. Each is {"va": guest address, and one of
"f32": [floats], "u32": [words], "hex": "bytes"}; little-endian. Xbox only for now.

Prepare hands both to the runtime as options the pack forces (env()): RECOMP_MOD_FRAMES and RECOMP_MOD_WIDE_WRITES.
"""

import re
import struct

FRAMES = ("anamorphic-16:9",)
WRITES_MAX = 8
WRITE_BYTES_MAX = 64
VA_RE = re.compile(r"0x[0-9A-Fa-f]{1,8}\Z")

ENV_FRAMES = "RECOMP_MOD_FRAMES"
ENV_WIDE_WRITES = "RECOMP_MOD_WIDE_WRITES"


def _err(msg):
    from tools.mods.package import ModError
    return ModError(msg)


def write_bytes(w):
    """The bytes one checked wide_writes entry stores."""
    if "f32" in w:
        return b"".join(struct.pack("<f", float(x)) for x in w["f32"])
    if "u32" in w:
        return b"".join(struct.pack("<I", int(x)) for x in w["u32"])
    return bytes.fromhex(w["hex"])


def validate(d, label):
    if not isinstance(d, dict) or set(d) - {"frames", "wide_writes"}:
        raise _err("%s: display: {\"frames\": ..., \"wide_writes\": [...]} expected" % label)
    if d.get("frames") not in FRAMES:
        raise _err("%s: display.frames %r: one of %s" % (label, d.get("frames"), ", ".join(FRAMES)))
    out = {"frames": d["frames"]}
    ws = d.get("wide_writes", [])
    if not isinstance(ws, list) or len(ws) > WRITES_MAX:
        raise _err("%s: display.wide_writes: a list of at most %d writes" % (label, WRITES_MAX))
    total = 0
    clean = []
    for i, w in enumerate(ws):
        where = "%s: display.wide_writes[%d]" % (label, i)
        if not isinstance(w, dict) or "va" not in w or len(w) != 2:
            raise _err("%s: {\"va\": \"0x...\", and one of \"f32\", \"u32\", \"hex\"} expected" % where)
        if not isinstance(w["va"], str) or not VA_RE.match(w["va"]):
            raise _err("%s: va %r: a guest address, \"0x\" and hexadecimal digits" % (where, w["va"]))
        kind = [k for k in w if k != "va"][0]
        v = w[kind]
        if kind in ("f32", "u32"):
            if not isinstance(v, list) or not v or any(isinstance(x, bool) for x in v):
                raise _err("%s: %s: a list of numbers" % (where, kind))
            if kind == "f32" and not all(isinstance(x, (int, float)) for x in v):
                raise _err("%s: f32: numbers" % where)
            if kind == "u32" and not all(isinstance(x, int) and 0 <= x <= 0xFFFFFFFF for x in v):
                raise _err("%s: u32: whole numbers from 0 to 0xFFFFFFFF" % where)
        elif kind == "hex":
            if not isinstance(v, str) or not v or len(v) % 2 or not re.match(r"[0-9A-Fa-f]+\Z", v):
                raise _err("%s: hex: an even number of hexadecimal digits" % where)
        else:
            raise _err("%s: unknown field \"%s\" (f32, u32 or hex)" % (where, kind))
        va = int(w["va"], 16)
        n = len(write_bytes(w))
        if va + n > 0x100000000:
            raise _err("%s: runs past the end of the address space" % where)
        total += n
        clean.append({"va": "0x%08X" % va, kind: v})
    if total > WRITE_BYTES_MAX:
        raise _err("%s: display.wide_writes: %d bytes, at most %d" % (label, total, WRITE_BYTES_MAX))
    if clean:
        out["wide_writes"] = clean
    return out


def env(mods):
    """The options the pack forces for the enabled mods' display hints, in load order ({} when none has one):
    RECOMP_MOD_FRAMES=<frames> and RECOMP_MOD_WIDE_WRITES="<va hex>=<bytes hex>;...", the runtime's format
    (src/kernel/xbox_mods.c)."""
    frames, writes = None, []
    for m in mods:
        d = m.display
        if not d:
            continue
        frames = d["frames"]
        for w in d.get("wide_writes", []):
            writes.append("%08X=%s" % (int(w["va"], 16), write_bytes(w).hex().upper()))
    out = {}
    if frames:
        out[ENV_FRAMES] = frames
    if writes:
        out[ENV_WIDE_WRITES] = ";".join(writes)
        if len(out[ENV_WIDE_WRITES]) > 255:
            raise _err("the enabled mods' display.wide_writes are too many together (%d characters, at most 255)"
                       % len(out[ENV_WIDE_WRITES]))
    return out
