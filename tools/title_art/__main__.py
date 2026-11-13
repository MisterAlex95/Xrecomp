"""
A game's title image and the app icon of its own APK.

    python -m tools.title_art SOURCE [--png FILE] [--android-res DIR] [--scale F] [--json]

SOURCE is what tools.prepare takes: a disc image, a folder holding
default.xbe, or a default.xbe. Prints the title ID, the name and whether the
XBE has a title image; --png writes the image as decoded (often 128 x 128);
--android-res writes the adaptive icon resources a title's APK is built with
(android/app/build.gradle -PxbaIconRes=DIR, tools/android/title_apk.sh).

Everything written comes from your own game: keep it out of the repository.
Exit status 0, 1 when the source cannot be read, 2 when --png was asked and
the XBE has no title image.
"""

import argparse
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from tools.prepare import identify          # noqa: E402
from tools.title_art import art, icons      # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.title_art", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help=".iso/.xiso, a folder with default.xbe, or default.xbe")
    ap.add_argument("--png", metavar="FILE", help="write the title image as a PNG")
    ap.add_argument("--android-res", metavar="DIR", help="write the APK's adaptive icon resources to DIR")
    ap.add_argument("--scale", type=float, default=icons.DEFAULT_SCALE,
                    help="the image's size in the icon layer (default %.3f, the visible part)" % icons.DEFAULT_SCALE)
    ap.add_argument("--json", action="store_true", help="one JSON object on stdout")
    a = ap.parse_args(argv)
    try:
        _kind, _path, data = identify.read_source(a.source)
        cert = identify.parse_xbe(data).certificate
    except identify.IdentifyError as e:
        print("%s: %s" % (e.code, e.msg), file=sys.stderr)
        return 1
    tid = "%08X" % cert.title_id
    name = cert.title_name.strip() or tid
    img = art.title_image(data)
    out = {"title_id": tid, "name": name, "image": [img.w, img.h] if img else None}
    if a.png:
        if img is None:
            print("%s: no title image in this XBE" % a.source, file=sys.stderr)
            return 2
        icons.write_png(a.png, img)
        out["png"] = os.path.abspath(a.png)
    if a.android_res:
        try:
            out["icon"] = icons.android_res(a.android_res, img, tid, a.scale)
        except ValueError as e:
            print("--scale: %s" % e, file=sys.stderr)
            return 1
        out["android_res"] = os.path.abspath(a.android_res)
    if a.json:
        print(json.dumps(out))
    else:
        print("%s  %s  (%s)" % (tid, name, "title image %dx%d" % (img.w, img.h) if img else "no title image"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
