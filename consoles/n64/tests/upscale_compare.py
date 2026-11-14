"""upscale_compare.py ONE.ppm UP.ppm (XBA-185, run_upscale.sh): the upscaled picture UP, sampled at the centre of each
pixel of the 1x picture ONE, must match it but at edges (at most 1 % of the pixels off by more than 48 summed over
R, G, B), and must not be blank. Binary PPMs as n64_run --ppm writes them."""
import sys


def ppm(path):
    data = open(path, 'rb').read()
    magic, size, depth, pixels = data.split(b'\n', 3)
    if magic != b'P6' or depth != b'255':
        raise SystemExit(f"FAIL: {path} is not a binary PPM")
    w, h = map(int, size.split())
    return w, h, pixels


def main():
    w1, h1, one = ppm(sys.argv[1])
    w2, h2, up = ppm(sys.argv[2])
    if w2 % w1 or h2 % h1:
        raise SystemExit(f"FAIL: {w2} x {h2} is not a multiple of {w1} x {h1}")
    sx, sy = w2 // w1, h2 // h1
    off = 0
    lit = 0
    for y in range(h1):
        for x in range(w1):
            i = 3 * (y * w1 + x)
            j = 3 * ((y * sy + sy // 2) * w2 + x * sx + sx // 2)
            a, b = one[i:i + 3], up[j:j + 3]
            off += sum(abs(a[k] - b[k]) for k in range(3)) > 48
            lit += any(b)
    n = w1 * h1
    print(f"upscale_compare: {w1} x {h1} vs {w2} x {h2}: {off} of {n} pixels off, {lit} lit")
    if lit < n // 20:
        raise SystemExit("FAIL: the upscaled picture is (almost) blank")
    if off > n // 100:
        raise SystemExit("FAIL: the upscaled picture differs from the 1x one")


main()
