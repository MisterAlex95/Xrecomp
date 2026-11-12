"""python -m tools.triage SOURCE... [--json triage.json] [--markdown triage.md]: see __init__.py."""
import argparse
import json
import sys

from tools.triage import markdown, triage_disc


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m tools.triage", description=__doc__)
    ap.add_argument("sources", nargs="+", help="disc images, extracted disc folders or default.xbe files")
    ap.add_argument("--json", help="write the records here")
    ap.add_argument("--markdown", help="write the table here (else it is printed)")
    a = ap.parse_args(argv)
    records = []
    for s in a.sources:
        r = triage_disc(s)
        records.append(r)
        print("%-40s %s" % (r["source"][:40], r.get("error") or r.get("summary", "")), file=sys.stderr, flush=True)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(records, f, indent=1)
    md = markdown(records)
    if a.markdown:
        with open(a.markdown, "w", encoding="utf-8") as f:
            f.write(md + "\n")
    else:
        print(md)
    return 0 if all(not r.get("error") for r in records) else 1


if __name__ == "__main__":
    sys.exit(main())
