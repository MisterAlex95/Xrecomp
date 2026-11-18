"""Structured exception code the call graph does not reach (XBA-282): the __except blocks, filters and __finally
handlers that only a scope table names.

A function whose .pdata entry has the exception flag (bit 31 of the info word, IMAGE_CE_RUNTIME_FUNCTION_ENTRY) has
its handler and handler data in the two words before it. When the handler is the kernel's __C_specific_handler (an
import thunk) or a function, and the data is in the image, the data is a scope table {Count, {Begin, End, Handler,
JumpTarget}...}: every filter / __finally (Handler, when it is code) and every __except block (JumpTarget) is a seed
for discovery, so the runtime's exception dispatch (consoles/x360/runtime/x360_seh.c) lands on lifted code. A title
without such records gets no new seed: its output is unchanged."""

import struct

from tools.xex.xex import IMPORT_THUNK

EXCEPTION_FLAG = 0x80000000
MAX_SCOPES = 256


def pdata_entries(img):
    """(start, info) of every .pdata entry."""
    out = []
    for s in img.sections:
        if s.name != ".pdata":
            continue
        raw = img.read(s.va, s.size) or b""
        for o in range(0, len(raw) - 7, 8):
            start, info = struct.unpack_from(">II", raw, o)
            if start == 0:
                break
            out.append((start, info))
    return out


def seh_seeds(img):
    """The code addresses the scope tables name (a set; empty for a title without exception records)."""
    if img.image is None:
        return set()
    entries = pdata_entries(img)
    starts = {s for s, _ in entries}
    thunks = {r.addr for r in img.imports if r.kind == IMPORT_THUNK}
    lo, hi = img.base, img.base + len(img.image)

    def u32(a):
        if a < lo or a + 4 > hi:
            return None
        return img.u32(a)

    seeds = set()
    for start, info in entries:
        if not info & EXCEPTION_FLAG or start < lo + 8:
            continue
        h, d = u32(start - 8), u32(start - 4)
        if h is None or d is None or not (h in thunks or h in starts) or not lo <= d < hi:
            continue
        n = u32(d)
        if n is None or n > MAX_SCOPES:
            continue
        for k in range(n):
            rec = [u32(d + 4 + 16 * k + 4 * i) for i in range(4)]
            if None in rec:
                break
            b, e, handler, jump = rec
            if not (start <= b <= e):
                continue
            if handler > 1 and img.in_code(handler) and not handler & 3:
                seeds.add(handler)
            if jump and img.in_code(jump) and not jump & 3:
                seeds.add(jump)
    return seeds
