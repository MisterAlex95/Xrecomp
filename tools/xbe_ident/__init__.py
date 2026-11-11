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

Nothing from the XBE is stored: the result holds names, numbers and the
markers (our own strings) that matched.
"""

import collections
import json
import os

from tools.xbe_parser.xbe_parser import XBEParser

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


def identify(path, table=None):
    """What the XBE at path was built with. Raises ValueError for a file that is not an XBE."""
    xbe = XBEParser(path).parse()
    c = xbe.certificate
    engines = engines_in(xbe.raw_data, table)
    xdk = xdk_of(xbe)
    ident = {
        "title_id": "%08X" % c.title_id,
        "title": c.title_name,
        "xdk": xdk,
        "engines": engines,
    }
    # The profile layers this XBE selects, lowest first (tools.prepare.profiles:
    # the xdk layers whose libraries it links, the engine layers that exist).
    from tools.prepare import profiles
    ident["layers"] = profiles.auto_layers(ident)
    return ident


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
