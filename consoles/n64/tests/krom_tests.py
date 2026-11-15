"""krom_tests.py N64_RUN DIR [options] (XBA-202): krom's (PeterLemon/N64) test ROMs against their reference screenshots.

DIR is what tools/consoles/build_n64_krom_tests.sh wrote: DIR/list.txt names each test <group>/.../<name>, with
DIR/roms/<test>.z64 and the console's screenshot DIR/roms/<test>.png. Each ROM runs under N64_RUN --frames F --ppm
(the frame shown after F frames) and is compared with its screenshot: the screenshot is brought to the frame's size
when it is an integer multiple of it or the frame of it (nearest pixel), and a pixel differs when R, G and B differ by
more than --tol summed; a test passes when at most --max-off of its pixels (a fraction) differ. The screenshots are
captures of the console's analog output with the VI's filters, so the defaults leave a little room; krom's PASS /
FAIL labels are a few hundred pixels each, far above it.

  --frames F        frames before the capture (default 90)
  --tol T           summed RGB difference allowed per pixel (default 72)
  --max-off X       fraction of pixels allowed to differ (default 0.002)
  --min-pass N      fail unless at least N tests pass (else exit 0 even if some fail)
  --jobs J          ROMs run at once (default 4)
  --only REGEX      tests whose name matches
  --table           a pass / total table per group (the first two path levels)
  --keep DIR        keep each frame as DIR/<test>.ppm
  -v                one line per test

Exit 77 (skipped) when DIR/list.txt is missing. Standard library only (PNG decoded with zlib)."""
import concurrent.futures
import functools
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib


def png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f"{path}: not a PNG")
    pos = 8
    idat = b''
    plte = b''
    w = h = depth = ctype = interlace = 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            plte = body
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
    if (depth != 8 and not (ctype == 3 and depth in (1, 2, 4))) or interlace or ctype not in (0, 2, 3, 4, 6):
        raise ValueError(f"{path}: unsupported PNG (depth {depth}, type {ctype}, interlace {interlace})")
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * bpp if depth == 8 else (w * depth + 7) // 8
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        prev = line
        o = y * w * 3
        if ctype == 2:
            out[o:o + w * 3] = line
        elif ctype == 6:
            out[o:o + w * 3] = bytes(b for i in range(w) for b in line[4 * i:4 * i + 3])
        elif ctype == 3:
            if depth < 8:
                per = 8 // depth
                line = [(line[i // per] >> (8 - depth * (i % per + 1))) & ((1 << depth) - 1) for i in range(w)]
            out[o:o + w * 3] = b''.join(plte[3 * v:3 * v + 3] for v in line)
        else:
            out[o:o + w * 3] = bytes(line[i * bpp] for i in range(w) for _ in range(3))
    return w, h, bytes(out)


def ppm(path):
    data = open(path, 'rb').read()
    magic, size, depth, pixels = data.split(b'\n', 3)
    if magic != b'P6' or depth != b'255':
        raise ValueError(f"{path}: not a binary PPM")
    w, h = map(int, size.split())
    return w, h, pixels


def resample(w, h, px, nw, nh):
    """Nearest pixel, for integer ratios only (else None)."""
    if (w, h) == (nw, nh):
        return px
    if not ((w % nw == 0 or nw % w == 0) and (h % nh == 0 or nh % h == 0)):
        return None
    out = bytearray(nw * nh * 3)
    for y in range(nh):
        sy = (y * h) // nh if nh >= h else y * (h // nh)
        row = sy * w * 3
        for x in range(nw):
            sx = (x * w) // nw if nw >= w else x * (w // nw)
            out[3 * (y * nw + x):3 * (y * nw + x) + 3] = px[row + 3 * sx:row + 3 * sx + 3]
    return bytes(out)


def row_off(fp, fw, fh, fy, rp, rrow, xs, rw, tol):
    """Pixels of one screenshot line that match none of frame lines fy - 1, fy, fy + 1."""
    rows = [fy * fw * 3] + [r * fw * 3 for r in (fy - 1, fy + 1) if 0 <= r < fh]
    off = 0
    for x in range(rw):
        j = rrow + 3 * x
        r0, g0, b0 = rp[j], rp[j + 1], rp[j + 2]
        for row in rows:
            i = row + xs[x]
            if abs(fp[i] - r0) + abs(fp[i + 1] - g0) + abs(fp[i + 2] - b0) <= tol:
                break
        else:
            off += 1
    return off


def compare(frame, ref, tol, slack=8):
    """Fraction of the screenshot's pixels off, or a reason string. The frame is sampled at each screenshot pixel
    centre (nearest; the horizontal ratio is the widths'), its lines either as they are or stretched from the VI's
    237 / 474 active lines (V_VIDEO 37..511, krom's NTSC modes) to the screenshot's 240 / 480 as some captures are;
    the better of the two counts. Captures also gained or lost lines part way down (RSPTest/CP2/VCL) and their
    rounding of the stretch is not known, so each screenshot line is compared with the frame's line at its place
    and, when that one differs, the lines up to `slack` - 1 above and below (the best), each pixel matching on its
    line or the next one up or down; so at most `slack` lines of shift. krom's results are
    coloured (PASS green, FAIL red) and a wrong value changes whole glyph lines, which no line shift hides."""
    fw, fh, fp = frame
    rw, rh, rp = ref
    if fw * fh == 0:
        return "no picture"
    xs = [3 * ((2 * x + 1) * fw // (2 * rw)) for x in range(rw)]
    best = None
    for num, den in ((1, 1), (237, 240)):
        src_h = rh * fw * num // (rw * den)
        if src_h > fh:
            continue
        off = 0
        for y in range(rh):
            fy = (2 * y + 1) * src_h // (2 * rh)  # pixel centres
            rrow = y * rw * 3
            o = row_off(fp, fw, fh, fy, rp, rrow, xs, rw, tol)
            for d in range(1, slack):
                if o == 0:
                    break
                for r in (fy - d, fy + d):
                    if 0 <= r < fh:
                        o = min(o, row_off(fp, fw, fh, r, rp, rrow, xs, rw, tol))
            off += o
        if best is None or off < best:
            best = off
    if best is None:
        return f"size {fw}x{fh} vs {rw}x{rh}"
    return best


# Tests that draw a PASS / FAIL label per result (one wrong label is about 170 pixels): a pixel count. The others
# (pictures: RDP, video, demos) allow a fraction of their area for edge and filter differences.
LABELLED = ('CPUTest', 'RSPTest')


def labels(w, h, px, rows):
    """Pixels of krom's FAIL (red) and PASS (green) label boxes in the first `rows` lines."""
    red = green = 0
    for i in range(0, 3 * w * min(h, rows), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if r > 192 and g < 64 and b < 64:
            red += 1
        elif g > 96 and r < 64 and b < 64:
            green += 1
    return red, green


def verdicts_ok(frame, ref):
    """The ROM's own verdicts are at least the console's: no more red label pixels (FAIL boxes, also the test names
    and headers, the same in both) and no fewer green ones (PASS boxes) than the screenshot. For labelled tests whose
    screenshot is shifted by more lines than the comparison allows, or older than the source (RSPTest/CP2/RESERVED:
    every result PASS on both, the layout of one block differs)."""
    fw, fh, fp = frame
    rw, rh, rp = ref
    if fw != rw:
        return False
    fr, fg = labels(fw, fh, fp, rh)
    rr, rg = labels(rw, rh, rp, rh)
    return rg > 0 and fr <= rr + 16 and fg >= rg - 64  # a label box is about 170 pixels


def run_one(args, root, test, keep=None):
    rom = os.path.join(root, 'roms', test + '.z64')
    ref = os.path.join(root, 'roms', test + '.png')
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(keep, test + '.ppm') if keep else os.path.join(tmp, 'f.ppm')
        if keep:
            os.makedirs(os.path.dirname(out), exist_ok=True)
        try:
            subprocess.run([args['run'], rom, '--frames', str(args['frames']), '--fb' if args['source'] == 'fb' else '--ppm', out, '--quiet'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
        except subprocess.TimeoutExpired:
            return test, "timeout", 0, False
        if not os.path.exists(out):
            return test, "no frame", 0, False
        try:
            r = png(ref)
            f = ppm(out)
            v = test.split('/')[0] in LABELLED and verdicts_ok(f, r)
            return test, compare(f, r, args['tol']), r[0] * r[1], v
        except ValueError as e:
            return test, str(e), 0, False


def main():
    argv = sys.argv[1:]
    if len(argv) < 2:
        raise SystemExit(__doc__)
    run, root = argv[0], argv[1]
    opts = {'frames': 90, 'tol': 72, 'max-off': 64, 'max-off-gfx': 0.002, 'min-pass': None, 'jobs': 4, 'only': None, 'keep': None, 'source': 'fb'}
    table = verbose = False
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == '--table':
            table = True
        elif a == '-v':
            verbose = True
        elif a.startswith('--') and a[2:] in opts and i + 1 < len(argv):
            opts[a[2:]] = argv[i + 1]
            i += 1
        else:
            raise SystemExit(f"unknown option {a}")
        i += 1
    lst = os.path.join(root, 'list.txt')
    if not os.path.exists(lst):
        print(f"skipped: {lst} missing (tools/consoles/build_n64_krom_tests.sh)")
        sys.exit(77)
    tests = [t.strip() for t in open(lst) if t.strip()]
    if opts['only']:
        tests = [t for t in tests if re.search(opts['only'], t)]
    args = {'run': run, 'frames': int(opts['frames']), 'tol': int(opts['tol']), 'source': opts['source']}
    max_off = int(opts['max-off'])
    max_gfx = float(opts['max-off-gfx'])
    results = {}
    areas = {}
    verdict = {}
    with concurrent.futures.ProcessPoolExecutor(int(opts['jobs'])) as ex:
        for test, r, area, v in ex.map(functools.partial(run_one, args, root, keep=opts['keep']), tests):
            results[test] = r
            areas[test] = area
            verdict[test] = v
    groups = {}
    passed = 0
    for test in tests:
        r = results[test]
        ok = isinstance(r, int) and r <= (max_off if test.split('/')[0] in LABELLED else max_gfx * areas[test])
        by_verdict = not ok and verdict[test]
        ok = ok or by_verdict
        passed += ok
        g = '/'.join(test.split('/')[:2]) if test.count('/') >= 2 else test.split('/')[0]
        gp, gt = groups.get(g, (0, 0))
        groups[g] = (gp + ok, gt + 1)
        if verbose or not ok:
            what = f"{r} pixels off" if isinstance(r, int) else r
            if by_verdict:
                what += ", verdicts match"
            print(f"{'PASS' if ok else 'FAIL'} {test}: {what}")
    if table:
        print(f"{'group':<28} pass/total")
        for g in sorted(groups):
            print(f"{g:<28} {groups[g][0]}/{groups[g][1]}")
    print(f"krom tests: {passed} of {len(tests)} pass")
    if opts['min-pass'] is not None:
        if passed < int(opts['min-pass']):
            print(f"FAIL: fewer than {opts['min-pass']} pass")
            sys.exit(1)
    elif passed < len(tests):
        sys.exit(1)


if __name__ == '__main__':
    main()
