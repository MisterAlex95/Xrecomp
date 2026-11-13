"""
What one game source is, without preparing it: for a launcher's library scan.

    python -m tools.prepare.identify SOURCE [--json] [--xbe-out FILE]

SOURCE is what tools.prepare takes: a disc image (.iso / .xiso), a folder
holding default.xbe, or a default.xbe. It reads the XBE header and
certificate and prints the title ID, the name and the source's size. For an
image only default.xbe is read (never the rest of the disc); --xbe-out FILE
writes that default.xbe to FILE (for any source), so the caller can decode the
title image from it. Nothing else is written. The --json output is the
contract with the macOS launcher (docs/macos/launcher.md « Prepare interface »).

Exit status 0 when the source was identified, 1 when not; with --json the
failure is {"error": {"code": ..., "msg": ...}} on stdout.
"""

import argparse
import errno
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

PROFILES = os.path.join(ROOT, "profiles")
IMAGE_EXT = (".iso", ".xiso")
# The section holding the title's image (an XPR0 texture): what the launchers decode as art.
TITLE_IMAGE_SECTION = "$$XTIMAGE"


class IdentifyError(Exception):
    """code: NO_XBE (no default.xbe), BAD_IMAGE (not a readable XDVDFS image,
    e.g. one still being copied), BAD_XBE (default.xbe does not parse)."""

    def __init__(self, code, msg):
        Exception.__init__(self, msg)
        self.code, self.msg = code, msg


def source_kind(src):
    """"folder", "image" or "xbe", or IdentifyError."""
    if os.path.isdir(src):
        return "folder"
    low = src.lower()
    if low.endswith(IMAGE_EXT) and os.path.isfile(src):
        return "image"
    if low.endswith(".xbe") and os.path.isfile(src):
        return "xbe"
    if not os.path.exists(src):
        raise IdentifyError("NO_XBE", "%s: no such file or folder" % src)
    raise IdentifyError("NO_XBE", "%s: expected a .iso/.xiso, a folder with default.xbe, or default.xbe" % src)


def read_source(src):
    """(kind, path of default.xbe or None for an image, its bytes)."""
    src = os.path.abspath(src)
    kind = source_kind(src)
    if kind == "folder":
        xbe = os.path.join(src, "default.xbe")
        if not os.path.isfile(xbe):
            raise IdentifyError("NO_XBE", "%s: no default.xbe in this folder" % src)
    elif kind == "xbe":
        xbe = src
    else:
        from tools.xiso.xdvdfs import Xiso, XisoError
        try:
            with Xiso(src) as iso:
                e = iso.find("default.xbe")
                if e is None:
                    raise IdentifyError("NO_XBE", "%s: no default.xbe in the image" % src)
                data = iso.read(e)
        except XisoError as e:
            raise IdentifyError("BAD_IMAGE", str(e))
        except OSError as e:
            raise IdentifyError("BAD_IMAGE", "%s: %s" % (src, e))
        if len(data) != e.size:
            raise IdentifyError("BAD_IMAGE", "%s: the image ends inside default.xbe (still being copied?)" % src)
        return kind, None, data
    try:
        with open(xbe, "rb") as f:
            return kind, xbe, f.read()
    except OSError as e:
        raise IdentifyError("NO_XBE", "%s: %s" % (xbe, e))


def parse_xbe(data):
    """tools.xbe_parser's XBEFile for XBE bytes, or IdentifyError BAD_XBE."""
    from tools.xbe_parser.xbe_parser import XBEParser
    p = XBEParser.__new__(XBEParser)
    p.filepath, p.data = "default.xbe", data
    try:
        return p.parse()
    except Exception as e:          # ValueError (magic), struct.error / IndexError (truncated)
        raise IdentifyError("BAD_XBE", "default.xbe does not parse: %s" % e)


def profile_name(tid):
    """The title profile's "name" (profiles/titles/<tid>/title.json), or None."""
    try:
        from tools.prepare import profiles
        prof = profiles.resolve(tid, None, PROFILES)
    except Exception:
        return None
    return (prof or {}).get("name") or None


def write_atomic(path, data):
    path = os.path.abspath(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def identify(source, xbe_out=None):
    """The --json answer as a dict; IdentifyError when the source is not usable."""
    src = os.path.abspath(source)
    kind, xbe_path, data = read_source(src)
    x = parse_xbe(data)
    c = x.certificate
    tid = "%08X" % c.title_id
    if xbe_out:
        try:
            write_atomic(xbe_out, data)
        except OSError as e:
            raise IdentifyError("NO_SPACE" if e.errno == errno.ENOSPC else "BAD_ARGS",
                                "--xbe-out %s: %s" % (xbe_out, e))
        xbe_path = os.path.abspath(xbe_out)
    name = c.title_name.strip() or tid
    return {
        "source": src,
        "kind": kind,
        "size": os.path.getsize(src) if kind != "folder" else len(data),
        "title_id": tid,
        "name": name,
        "profile_name": profile_name(tid),
        "version": c.version,
        "region": c.game_region,
        "disc_number": c.disc_number,
        "xbe": xbe_path,
        "xbe_size": len(data),
        "has_title_image": any(s.name == TITLE_IMAGE_SECTION for s in x.sections),
    }


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.prepare.identify", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help=".iso/.xiso, a folder with default.xbe, or default.xbe")
    ap.add_argument("--json", action="store_true", help="one JSON object on stdout")
    ap.add_argument("--xbe-out", metavar="FILE", help="write the source's default.xbe to FILE")
    a = ap.parse_args(argv)
    try:
        out = identify(a.source, a.xbe_out)
    except IdentifyError as e:
        if a.json:
            print(json.dumps({"error": {"code": e.code, "msg": e.msg}}))
        else:
            print("%s: %s" % (e.code, e.msg), file=sys.stderr)
        return 1
    if a.json:
        print(json.dumps(out))
    else:
        print("%s  %s  (%s, %d bytes)" % (out["title_id"], out["profile_name"] or out["name"],
                                         out["kind"], out["size"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
