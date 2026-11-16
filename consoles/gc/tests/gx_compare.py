#!/usr/bin/env python3
"""Compares a GX test frame with its reference PNG, with a tolerance (XBA-262).

    gx_compare.py GOT EXPECTED [--update] [--tol N] [--max-bad FRACTION]

A pixel is bad when one of its channels differs by more than --tol (default 12: llvmpipe builds and GPUs round
interpolation and filtering a little differently); the frames match when at most --max-bad of the pixels are bad
(default 0.003) and the mean absolute difference stays under 2. Exit 0 on a match, 1 with the numbers otherwise, 2 on
a bad file. --update writes GOT to EXPECTED (deflate-compressed). Standard library only: the reader is
consoles/nds/tests/png_compare.py's.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "nds", "tests"))
import png_compare  # noqa: E402


def main(argv):
    args = [a for a in argv[1:]]
    tol, max_bad, update = 12, 0.003, False
    pos = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--update":
            update = True
        elif a == "--tol":
            i += 1
            tol = int(args[i])
        elif a == "--max-bad":
            i += 1
            max_bad = float(args[i])
        else:
            pos.append(a)
        i += 1
    if len(pos) != 2:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    got_path, exp_path = pos
    try:
        gw, gh, got = png_compare.read_png(got_path)
    except (OSError, ValueError) as e:
        print("gx_compare: %s" % e)
        return 2
    if update:
        png_compare.write_png(exp_path, gw, gh, got)
        print("gx_compare: %s updated from %s" % (exp_path, got_path))
        return 0
    try:
        ew, eh, exp = png_compare.read_png(exp_path)
    except (OSError, ValueError) as e:
        print("gx_compare: %s" % e)
        return 2
    if (gw, gh) != (ew, eh):
        print("gx_compare: size %dx%d, expected %dx%d" % (gw, gh, ew, eh))
        return 1
    bad, total_diff, worst = 0, 0, 0
    box = None
    for y in range(gh):
        rg, re = got[y], exp[y]
        for x in range(gw):
            d = 0
            for c in range(3):
                v = abs(rg[x][c] - re[x][c])
                total_diff += v
                if v > d:
                    d = v
            if d > worst:
                worst = d
            if d > tol:
                bad += 1
                box = (min(box[0], x), min(box[1], y), max(box[2], x), max(box[3], y)) if box else (x, y, x, y)
    n = gw * gh
    mean = total_diff / (3.0 * n)
    ok = bad <= max_bad * n and mean < 2.0
    print("gx_compare: %s: %d of %d pixels off by more than %d (%.3f%%), mean difference %.3f, worst %d%s" % (
        "match" if ok else "MISMATCH", bad, n, tol, 100.0 * bad / n, mean, worst,
        ", in %d,%d..%d,%d" % box if box else ""))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
