"""
python -m tools.xbe_ident default.xbe [more.xbe ...] [--json out.json]

One XBE: its XDK version, engines and profile layers. Several: one line each,
for the library triage (XBA-63). --json writes the full result (a list when
several XBEs are given).
"""
import argparse
import json
import sys

from tools.xbe_ident import identify, summary


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m tools.xbe_ident", description=__doc__.strip().splitlines()[0])
    ap.add_argument("xbe", nargs="+")
    ap.add_argument("--json", help="write the result here")
    a = ap.parse_args(argv)
    out, rc = [], 0
    for p in a.xbe:
        try:
            r = identify(p)
        except Exception as e:   # a bad or truncated file must not stop a triage run
            print("%s: not readable as an XBE (%s)" % (p, e), file=sys.stderr)
            rc = 1
            continue
        r["path"] = p
        out.append(r)
        if len(a.xbe) == 1:
            print("%s (title ID %s)" % (r["title"], r["title_id"]))
            print("  %s" % summary(r))
            for name, build in r["xdk"]["libraries"].items():
                print("    %-8s %d" % (name, build))
            for e in r["engines"]:
                print("  %s %s: %s" % (e["kind"], e["name"], ", ".join(e["markers"])))
            print("  layers: %s" % (" ".join(r["layers"]) or "(none)"))
        else:
            print("%s  %-40s %s" % (r["title_id"], r["title"][:40], summary(r)))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(out[0] if len(a.xbe) == 1 and out else out, f, indent=1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
