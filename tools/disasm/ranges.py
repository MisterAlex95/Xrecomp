"""Membership in a set of half-open address ranges, by bisection.

`any(lo <= a < hi for lo, hi in ranges)` costs one step per range. An XBE whose
code is split in hundreds of sections (SpikeOut: 392, one per overlay segment)
made that scan most of the disassembler's time: the passes ask it for every
word of every section. Same answer, O(log n): the ranges are sorted and merged
(overlapping or touching ones become one) and looked up with bisect.
"""

import bisect


def merged(ranges):
    """The ranges sorted and merged: [(lo, hi)] disjoint, not touching, empty ones dropped."""
    out = []
    for lo, hi in sorted((lo, hi) for lo, hi in ranges if lo < hi):
        if out and lo <= out[-1][1]:
            if hi > out[-1][1]:
                out[-1] = (out[-1][0], hi)
        else:
            out.append((lo, hi))
    return out


def contains_fn(ranges):
    """A function addr -> bool: True when lo <= addr < hi for one of the ranges."""
    m = merged(ranges)
    if len(m) == 1:
        lo, hi = m[0]
        return lambda a: lo <= a < hi
    if not m:
        return lambda a: False
    starts = [lo for lo, _ in m]
    ends = [hi for _, hi in m]
    br = bisect.bisect_right

    def contains(a):
        i = br(starts, a) - 1
        return i >= 0 and a < ends[i]
    return contains
