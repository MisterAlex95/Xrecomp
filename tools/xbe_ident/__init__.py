"""
What an XBE was built with: its XDK version and its engines (XBA-67).

One function, `identify(path)`, for tools.prepare (which picks the profile
layers from it, XBA-66) and for the library triage (XBA-63). It reads the
XBE only: no disassembly, so it runs in a second on the device too.

XDK version
    The XBE's library table: one entry per statically linked XDK library
    (XAPILIB, D3D8, DSOUND, XNET, ...), each with a build number. The XDK
    version is the XAPI library's build (the header points at its entry;
    else the entry named XAPILIB; else the most common build). Titles can
    mix builds (a QFE'd D3D8, an older XNET): `mixed` says so and
    `libraries` keeps every one.

Engines and middleware
    Marker strings from engines.json, matched as bytes over the whole XBE.
    An entry matches when `min` distinct markers occur; the markers found
    are returned as the evidence.

Code probes (XBA-105)
    What the title's linked code does, where a profile layer's choice
    depends on it rather than on a build number: byte shapes searched in the
    executable sections, counted. `d3d_latch_wait`: D3D's fence wait that
    spins on PGRAPH 0x400B10 (the 0x0310 latch) until its fence bits (mask
    0x7C) match the GPU semaphore -- `mov r32, [reg + 0x400B10]` (or the
    absolute 0xFD400B10) followed within 32 bytes by a `test`/`and` with
    0x7C. Under the segment scan nothing latches that register, so a title
    whose D3D waits there needs the DMA walk (profiles/xdk/d3d8ltcg).

Image facts (XBA-124)
    tools.xbe_parser.image_report: the key pair, the sections and their
    preload flags, the sections that share a virtual address, TLS, and what
    the parser had to work around.

Nothing from the XBE is stored: the result holds names, numbers and the
markers (our own strings) that matched.
"""

import collections
import json
import os

from tools.xbe_parser.xbe_parser import SECTION_EXECUTABLE, XBEParser, image_report

ENGINES_JSON = os.path.join(os.path.dirname(os.path.abspath(__file__)), "engines.json")

# Library version flags (XBE_LIBRARYVERSION): QFE in bits 0-12, approved in 13-14, debug in 15.
QFE_MASK = 0x1FFF
DEBUG_FLAG = 0x8000


def load_engines(path=ENGINES_JSON):
    with open(path, encoding="utf-8") as f:
        table = json.load(f)["engines"]
    for e in table:
        if not e.get("markers") or not 1 <= e.get("min", 1) <= len(e["markers"]):
            raise ValueError("engines.json: %s: min must be 1..len(markers)" % e.get("name"))
    return table


def _lib_at(xbe, va):
    """The library table entry at a header VA (kernel / XAPI pointers), or None."""
    h = xbe.header
    if not va or not h.library_versions_addr:
        return None
    i, r = divmod(va - h.library_versions_addr, 16)
    if r or not 0 <= i < len(xbe.libraries):
        return None
    return xbe.libraries[i]


def xdk_of(xbe):
    """{"build", "source", "mixed", "qfe", "debug", "libraries": {name: build}}; build 0 when the table is empty."""
    libs = xbe.libraries
    if not libs:
        return {"build": 0, "source": "none", "mixed": False, "qfe": 0, "debug": False, "libraries": {}}
    xapi, source = _lib_at(xbe, xbe.header.xapi_library_version_addr), "header"
    if xapi is None:
        xapi = next((l for l in libs if l.name.upper() == "XAPILIB"), None)
        source = "XAPILIB"
    if xapi is None:
        build = collections.Counter(l.build for l in libs).most_common(1)[0][0]
        xapi = next(l for l in libs if l.build == build)
        source = "most common"
    return {
        "build": xapi.build,
        "source": source,
        "mixed": len({l.build for l in libs}) > 1,
        "qfe": xapi.flags & QFE_MASK,
        "debug": bool(xapi.flags & DEBUG_FLAG),
        "libraries": {l.name: l.build for l in libs},
    }


def engines_in(data, table=None):
    """[{"name", "layer", "kind", "markers": [found...]}] for every entry of the table that matches data."""
    found = []
    for e in table if table is not None else load_engines():
        hits = [m for m in e["markers"] if data.find(m.encode("ascii")) >= 0]
        if len(hits) >= e.get("min", 1):
            found.append({"name": e["name"], "layer": e["layer"], "kind": e["kind"], "markers": hits})
    return found


_LATCH_DISP = b"\x10\x0B\x40\x00"        # disp32 0x00400B10: [register base + PGRAPH 0x400B10]
_LATCH_ABS = b"\x10\x0B\x40\xFD"         # absolute 0xFD400B10
_FENCE_MASK = b"\x7C\x00\x00\x00"


def _is_latch_load(data, i, absolute):
    """data[i:i+4] is the address of a `mov r32, [...]` that reads the latch."""
    if absolute:
        return (i >= 1 and data[i - 1] == 0xA1) or \
               (i >= 2 and data[i - 2] == 0x8B and (data[i - 1] & 0xC7) == 0x05)
    if i >= 2 and data[i - 2] == 0x8B and 0x80 <= data[i - 1] <= 0xBF and (data[i - 1] & 7) != 4:
        return True                                     # mod=10, no SIB
    return i >= 3 and data[i - 3] == 0x8B and (data[i - 2] & 0xC7) == 0x84     # mod=10 with a SIB byte


def _tests_0x7c(w):
    """A test/and of a register with 0x7C (the fence bits) in the window w."""
    for k in range(len(w) - 1):
        b0, b1 = w[k], w[k + 1]
        if b0 == 0xA8 and b1 == 0x7C:                                           # test al, 0x7C
            return True
        if k + 2 < len(w) and w[k + 2] == 0x7C and (
                (b0 == 0xF6 and 0xC0 <= b1 <= 0xC7) or                          # test r8, 0x7C
                (b0 in (0x80, 0x83) and 0xE0 <= b1 <= 0xE7)):                   # and r8/r32, 0x7C
            return True
        if b0 == 0xA9 and w[k + 1:k + 5] == _FENCE_MASK:                        # test eax, 0x7C
            return True
        if b0 == 0xF7 and 0xC0 <= b1 <= 0xC7 and w[k + 2:k + 6] == _FENCE_MASK:  # test r32, 0x7C
            return True
    return False


def probes_in(xbe):
    """{"d3d_latch_wait": sites} over the executable sections (see the module docstring)."""
    sites = 0
    for s in xbe.sections:
        if not s.flags & SECTION_EXECUTABLE or not s.raw_size:
            continue
        data = xbe.raw_data[s.raw_addr:s.raw_addr + s.raw_size]
        for pat, absolute in ((_LATCH_DISP, False), (_LATCH_ABS, True)):
            i = data.find(pat)
            while i >= 0:
                if _is_latch_load(data, i, absolute) and _tests_0x7c(data[i + 4:i + 36]):
                    sites += 1
                i = data.find(pat, i + 1)
    return {"d3d_latch_wait": sites}


def identify(path, table=None):
    """What the XBE at path was built with. Raises ValueError for a file that is not an XBE."""
    xbe = XBEParser(path).parse()
    c = xbe.certificate
    engines = engines_in(xbe.raw_data, table)
    xdk = xdk_of(xbe)
    ident = {
        "title_id": "%08X" % c.title_id,
        "title": c.title_name,
        "version": c.version,
        "xdk": xdk,
        "engines": engines,
        # The kernel ordinals it imports: an XDK layer whose library the table
        # does not name is picked by them (XBA-106, tools.prepare.profiles).
        "imports": sorted({imp.ordinal for imp in xbe.kernel_imports}),
        "kernel_imports": sorted({i.ordinal for i in xbe.kernel_imports}),
        "probes": probes_in(xbe),
        "image": image_report(xbe),
    }
    # The profile layers this XBE selects, lowest first (tools.prepare.profiles:
    # the xdk layers whose libraries it links, the engine layers that exist).
    from tools.prepare import profiles
    ident["layers"] = profiles.auto_layers(ident)
    return ident


def blockers(ident):
    """What stops the title from running as lifted, one line each (XBA-124):
    sections that share a virtual address and both hold code. The loader maps
    one at a time (XeLoadSection), but the lifter reads one section's bytes
    at an address and the dispatch table has one function there, so the other
    section's code is not in the C at all."""
    out = []
    for o in (ident.get("image") or {}).get("overlaps", []):
        if o["code"]:
            out.append("sections %s and %s both hold code at %s (+0x%X): only one of them is lifted "
                       "(overlapping code sections, XBA-124)" % (o["a"], o["b"], o["va"], o["size"]))
    return out


def summary(ident):
    """One line: 'XDK 5849 (mixed), RenderWare; middleware Bink'."""
    x = ident["xdk"]
    line = "XDK %d%s" % (x["build"], " (mixed)" if x["mixed"] else "") if x["build"] else "XDK unknown"
    eng = [e["name"] for e in ident["engines"] if e["kind"] == "engine"]
    mid = [e["name"] for e in ident["engines"] if e["kind"] != "engine"]
    line += ", " + (", ".join(eng) if eng else "no known engine")
    if mid:
        line += "; middleware " + ", ".join(mid)
    return line
