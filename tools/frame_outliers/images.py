"""One-frame glitches in a sequence of frame dumps (RECOMP_FB_DUMP BMPs).

Two detectors, both local in time so camera motion and scene cuts do not
count:

- flash: frames i..i+k-1 (k <= max_run) differ from the frame before and the
  frame after much more (`ratio` times) than those two differ from each
  other. A frame that is wrong for one or two frames and then right again.
- black: the number of near-black pixels jumps above the median of the
  neighbouring frames by more than `black_excess` (a fraction of the area),
  the medians before and after agreeing (a spike, not a fade or a cut).
  The variant that found the Fable texture glitch: a surface sampling a
  wrong (black) texture for a frame.

numpy is used when it is importable (the launcher's and the CI venvs have
it); without it a pure-Python path gives the same numbers, slowly (fine for
tests and short windows).
"""

import os
import re
import struct
from collections import deque

try:
    import numpy as _np
except ImportError:  # pragma: no cover - exercised where numpy is missing
    _np = None


class Frame:
    """A decoded frame: width, height, rgb (top-down rows, 3 bytes a pixel)."""

    __slots__ = ("width", "height", "rgb", "arr")

    def __init__(self, width, height, rgb):
        self.width, self.height, self.rgb = width, height, rgb
        self.arr = None
        if _np is not None:
            self.arr = _np.frombuffer(rgb, dtype=_np.uint8).reshape(height, width, 3).astype(_np.int16)


def read_bmp(path, use_numpy=True):
    """Uncompressed 24- or 32-bit BMP (bottom-up or top-down) -> Frame."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM" or len(data) < 54:
        raise ValueError("%s: not a BMP" % path)
    off = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp, comp = struct.unpack_from("<HI", data, 28)
    if bpp not in (24, 32) or comp not in (0, 3):
        raise ValueError("%s: %d-bit BMP, compression %d not supported" % (path, bpp, comp))
    top_down = height < 0
    height = abs(height)
    step = bpp // 8
    stride = (width * step + 3) & ~3
    if len(data) < off + stride * height:
        raise ValueError("%s: truncated" % path)
    if use_numpy and _np is not None:
        a = _np.frombuffer(data, dtype=_np.uint8, count=stride * height, offset=off)
        a = a.reshape(height, stride)[:, :width * step].reshape(height, width, step)
        if not top_down:
            a = a[::-1]
        rgb = _np.ascontiguousarray(a[:, :, 2::-1]).tobytes()
        return Frame(width, height, rgb)
    out = bytearray(width * height * 3)
    for y in range(height):
        src = off + (y if top_down else height - 1 - y) * stride
        row = data[src:src + width * step]
        o = y * width * 3
        out[o + 0:o + width * 3:3] = row[2::step]
        out[o + 1:o + width * 3:3] = row[1::step]
        out[o + 2:o + width * 3:3] = row[0::step]
    f = Frame.__new__(Frame)
    f.width, f.height, f.rgb, f.arr = width, height, bytes(out), None
    return f


def _crop_box(frame, crop):
    if not crop:
        return 0, 0, frame.width, frame.height
    x0, y0, x1, y1 = crop
    return max(0, x0), max(0, y0), min(frame.width, x1), min(frame.height, y1)


def black_pixels(frame, crop=None, level=2):
    """Pixels inside crop (x0, y0, x1, y1) whose channels are all <= level."""
    x0, y0, x1, y1 = _crop_box(frame, crop)
    if frame.arr is not None:
        a = frame.arr[y0:y1, x0:x1]
        return int((a.max(axis=2) <= level).sum())
    n, w, rgb = 0, frame.width, frame.rgb
    for y in range(y0, y1):
        row = rgb[(y * w + x0) * 3:(y * w + x1) * 3]
        for i in range(0, len(row), 3):
            if row[i] <= level and row[i + 1] <= level and row[i + 2] <= level:
                n += 1
    return n


def changed_pixels(a, b, crop=None, threshold=24):
    """Pixels inside crop where some channel differs by more than threshold."""
    if (a.width, a.height) != (b.width, b.height):
        x0, y0, x1, y1 = _crop_box(a, crop)
        return (x1 - x0) * (y1 - y0)            # a size change: everything changed
    x0, y0, x1, y1 = _crop_box(a, crop)
    if a.arr is not None and b.arr is not None:
        d = _np.abs(a.arr[y0:y1, x0:x1] - b.arr[y0:y1, x0:x1]).max(axis=2)
        return int((d > threshold).sum())
    n, w = 0, a.width
    for y in range(y0, y1):
        s, e = (y * w + x0) * 3, (y * w + x1) * 3
        ra, rb = a.rgb[s:e], b.rgb[s:e]
        for i in range(0, e - s, 3):
            if (abs(ra[i] - rb[i]) > threshold or abs(ra[i + 1] - rb[i + 1]) > threshold
                    or abs(ra[i + 2] - rb[i + 2]) > threshold):
                n += 1
    return n


_NUM = re.compile(r"(\d+)\.bmp$", re.IGNORECASE)


def list_frames(directory, prefix=None):
    """[(number, path)] of the BMPs in directory named <prefix><number>.bmp,
    by number. Without a prefix: the most common one, leaving out the
    <prefix>S<n>.bmp scaled copies RECOMP_GPU_SCALE_DUMP writes."""
    stems = {}
    for name in os.listdir(directory):
        m = _NUM.search(name)
        if m:
            stems.setdefault(name[:m.start()], []).append((int(m.group(1)), os.path.join(directory, name)))
    if prefix is None or prefix == "":
        cands = [s for s in stems if not (s.endswith("S") and s[:-1] in stems)]
        if not cands:
            return []
        prefix = max(cands, key=lambda s: (len(stems[s]), -len(s)))
    return sorted(stems.get(prefix, []))


def _median(v):
    s = sorted(v)
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2.0


def analyse_frames(frames, crop=None, threshold=24, ratio=3.0, floor=0.001,
                   max_run=2, black_excess=0.01, black_window=6, black_level=2,
                   steady=0.1, detectors=("flash", "black"), use_numpy=True):
    """frames: [(number, path or Frame)] in order. Returns a dict:
    count, outliers (sorted frame numbers), flash [(numbers, before, after,
    d_in, d_skip)], black [(number, black, median)], frames, area.

    Neighbours are the previous / next entries of the list; a gap in the
    numbering (a dump window that stopped and restarted) is a boundary.
    With both detectors a black spike also needs a steady scene around it:
    the frames just before and just after it (around one or two frames)
    differ by less than `steady` of the area (a transition where every frame
    is another picture is no glitch to count).
    """
    def load(item):
        return item if isinstance(item, Frame) else read_bmp(item, use_numpy)

    flash, blacks, numbers = [], [], []
    area = None
    keep = max(max_run, 2) + 2
    win = deque()                       # (number, Frame), at most keep
    around = {}                         # number -> least change across it (frames i-1 .. i+1 or i+2)
    # memo of pairwise changed counts keyed by (number_a, number_b)
    memo = {}

    def diff(i, j):
        key = (win[i][0], win[j][0])
        if key not in memo:
            memo[key] = changed_pixels(win[i][1], win[j][1], crop, threshold)
        return memo[key]

    flagged = set()

    def check_runs():
        # win[-1] is the newest frame: test runs ending just before it.
        for k in range(1, max_run + 1):
            if len(win) < k + 2:
                break
            b, a = len(win) - k - 2, len(win) - 1          # before, after
            run = [win[b + 1 + t][0] for t in range(k)]
            if any(n in flagged for n in run):
                continue
            nums = [win[t][0] for t in range(b, a + 1)]
            if any(nums[t + 1] != nums[t] + 1 for t in range(len(nums) - 1)):
                continue
            skip = diff(b, a)
            limit = ratio * max(skip, floor * area)
            d_in, d_out = diff(b, b + 1), diff(a - 1, a)
            if d_in > limit and d_out > limit:
                if k > 1 and any(diff(b + 1 + t, b + 2 + t) > limit for t in range(k - 1)):
                    continue        # it keeps changing inside: not one wrong picture
                flagged.update(run)
                flash.append((run, nums[0], nums[-1], min(d_in, d_out), skip))

    bcounts = []
    for number, item in frames:
        fr = load(item)
        if area is None:
            x0, y0, x1, y1 = _crop_box(fr, crop)
            area = (x1 - x0) * (y1 - y0)
        numbers.append(number)
        if "black" in detectors:
            bcounts.append(black_pixels(fr, crop, black_level))
        if "flash" in detectors:
            win.append((number, fr))
            while len(win) > keep:
                old = win.popleft()
                for key in [k for k in memo if old[0] in k]:
                    del memo[key]
            check_runs()
            for k in (1, 2):                # change across one or two frames
                if len(win) >= k + 2:
                    nums = [win[t][0] for t in range(len(win) - k - 2, len(win))]
                    if all(nums[t + 1] == nums[t] + 1 for t in range(len(nums) - 1)):
                        d = diff(len(win) - k - 2, len(win) - 1)
                        for n in nums[1:-1]:
                            around[n] = min(around.get(n, d), d)

    if "black" in detectors and bcounts:
        limit = black_excess * area
        for i, v in enumerate(bcounts):
            # neighbours on each side, on the same side of a numbering gap only
            left = [bcounts[j] for j in range(max(0, i - black_window), i)
                    if numbers[i] - numbers[j] == i - j]
            right = [bcounts[j] for j in range(i + 1, min(len(bcounts), i + black_window + 1))
                     if numbers[j] - numbers[i] == j - i]
            if not left or not right:
                continue
            ml, mr = _median(left), _median(right)
            # a spike, not a step: the scene is the same on both sides
            if around and around.get(numbers[i], area) >= steady * area:
                continue
            if abs(ml - mr) < limit and v > max(ml, mr) + limit:
                blacks.append((numbers[i], v, _median(left + right)))

    outliers = sorted(set(n for r in flash for n in r[0]) | set(b[0] for b in blacks))
    return {"frames": len(numbers), "area": area or 0, "flash": flash, "black": blacks,
            "outliers": outliers, "count": len(outliers),
            "first": numbers[0] if numbers else None, "last": numbers[-1] if numbers else None}


def runs_of(numbers):
    """[3, 4, 9] -> [(3, 4), (9, 9)]."""
    out = []
    for n in numbers:
        if out and n == out[-1][1] + 1:
            out[-1] = (out[-1][0], n)
        else:
            out.append((n, n))
    return out


def format_report(res, limit=40):
    lines = []
    lines.append("frame dumps: %d frames (%s..%s), %d outlier frames"
                 % (res["frames"], res["first"], res["last"], res["count"]))
    lines.append("  flash (wrong for 1-2 frames, then right again): %d events"
                 % len(res["flash"]))
    for run, before, after, d_in, skip in res["flash"][:limit]:
        lines.append("    frames %s: %d px differ from frames %d and %d, which differ by %d px"
                     % ("-".join(str(n) for n in (run if len(run) == 1 else (run[0], run[-1]))),
                        d_in, before, after, skip))
    if len(res["flash"]) > limit:
        lines.append("    ... %d more" % (len(res["flash"]) - limit))
    lines.append("  black spikes (near-black pixels above the neighbours' median): %d frames"
                 % len(res["black"]))
    for n, v, med in res["black"][:limit]:
        lines.append("    frame %d: %d black px, neighbours' median %d (+%d)" % (n, v, med, v - med))
    if len(res["black"]) > limit:
        lines.append("    ... %d more" % (len(res["black"]) - limit))
    if res["outliers"]:
        lines.append("  outlier frames: " + ", ".join(
            str(a) if a == b else "%d-%d" % (a, b) for a, b in runs_of(res["outliers"])))
    return "\n".join(lines)
