"""Per-batch records (RECOMP_GPU_BATCHLOG) and batch signature tables
(RECOMP_GPU_SIGDUMP with RECOMP_GPU_SIGDUMP_KEEP=1): frames that draw the same
batches as their neighbours but some of them with other textures or state.

A batch is identified within a frame by what it draws -- target, depth
target, vertex buffer, index count and bytes, first element, vertex program --
plus its occurrence number among identical ones; what it looks like is its
stage textures (address, format, size) and its state signature (colour
target addresses in both replaced by their rank, see canonical_frames). A batch whose
look in frame i differs from frames i-1 and i+1 while those two agree (or, for
two frames, i and i+1 agree with each other and differ from i-1 and i+2,
which agree) flickers. Frames with such batches are reported, with the
texture substitutions seen, aggregated.

The record layout is the BLG_* enum of src/gpu/nv2a_gl.c.
"""

import re
import struct
from collections import Counter, OrderedDict

MAGIC = 0x31474C42                      # "BLG1"
FIELDS = ("magic", "size", "frame", "index", "sig", "target", "target_wh", "zeta",
          "flags", "count", "vcount", "first", "vb", "stride", "streams",
          "prog", "prog_hi", "idx", "idx_hi", "vtx", "vtx_hi", "const", "const_hi",
          "tex0", "tex1", "tex2", "tex3", "fmt0", "fmt1", "fmt2", "fmt3", "cmask", "look")
NWORDS = len(FIELDS)


def pack_record(**kw):
    """One record as nv2a_gl.c writes it (tests and tools)."""
    w = [0] * NWORDS
    w[0], w[1] = MAGIC, NWORDS
    for k, v in kw.items():
        w[FIELDS.index(k)] = v & 0xFFFFFFFF
    return struct.pack("<%dI" % NWORDS, *w)


def read_records(path):
    """Yield one dict per record. Stops at a truncated tail record."""
    with open(path, "rb") as f:
        data = f.read()
    pos = 0
    while pos + 8 <= len(data):
        magic, size = struct.unpack_from("<II", data, pos)
        if magic != MAGIC or size < 2:
            raise ValueError("%s: bad record at byte %d (magic %08X)" % (path, pos, magic))
        if pos + size * 4 > len(data):
            break
        w = struct.unpack_from("<%dI" % size, data, pos)
        rec = dict(zip(FIELDS, w[:NWORDS]))
        for k in FIELDS[len(w):]:
            rec[k] = 0
        yield rec
        pos += size * 4


def tex_desc(addr, fmt):
    if not addr:
        return "-"
    return "%08X(%ux%u f%02X)" % (addr, ((fmt >> 8) & 0xFFF) + 1, ((fmt >> 20) & 0xFFF) + 1, fmt & 0xFF)


def group_frames(records):
    """Yield (frame, [records]) for runs of records of one frame, in order."""
    cur, recs = None, []
    for r in records:
        if r["frame"] != cur:
            if recs:
                yield cur, recs
            cur, recs = r["frame"], []
        recs.append(r)
    if recs:
        yield cur, recs


def _targets(recs):
    """Colour targets of a frame, in the order they are first drawn to."""
    order = []
    for r in recs:
        t = r["target"]
        if t and t not in order:
            order.append(t)
    return order


def canonical_frames(records):
    """Yield (frame, {batch key: [(look, record)]}) per frame; batches
    drawing the same thing (a quad drawn for many billboards) share a key.

    Addresses of colour targets -- as a batch's target or as a texture -- are
    replaced by their rank in the frame's target order (or, for a texture that
    is a target of the previous or next frame only, by its rank there): with
    double buffering every other frame draws into, and samples, the other
    buffer, and that is no change of look. The look uses the state signature
    without the target address (record word `look`)."""
    prev_order, cur = [], None
    it = group_frames(records)

    def canon(addr, order, near):
        if not addr:
            return 0
        if addr in order:
            return 0xF0000000 | order.index(addr)
        for k, o in enumerate(near):
            if addr in o:
                return 0xF1000000 | k << 16 | o.index(addr)
        return addr

    def emit(frame, recs, order, near):
        out = OrderedDict()
        for r in recs:
            what = (canon(r["target"], order, ()), r["zeta"], r["flags"], r["count"], r["first"],
                    r["vb"], r["stride"], r["prog"], r["prog_hi"], r["idx"], r["idx_hi"])
            look = (r["look"], r["cmask"]) + tuple(canon(r["tex%d" % i], order, near) for i in range(4)) \
                + tuple(r["fmt%d" % i] for i in range(4))
            out.setdefault(what, []).append((look, r))
        return out

    for frame, recs in it:
        order = _targets(recs)
        if cur is not None:
            f0, r0, o0 = cur
            nxt = order if frame == f0 + 1 else []
            yield f0, emit(f0, r0, o0, (prev_order, nxt))
            prev_order = o0 if frame == f0 + 1 else []
        cur = (frame, recs, order)
    if cur is not None:
        f0, r0, o0 = cur
        yield f0, emit(f0, r0, o0, (prev_order, []))


def analyse_records(records, with_constants=False, min_shared=0.95):
    """Streams the records (a window of four frames in memory). Batches of
    one key are compared as a multiset of looks, so a change in the order the
    title draws them is no change."""
    events, struct_out = [], []
    win = []                            # [(frame, {key: [(look, rec)]})], at most 4
    flagged = {}                        # frame -> keys already reported (window only)
    nframes, nb_min, nb_max, first, last = 0, None, 0, None, None

    def look(entry):
        lk, r = entry
        return lk + ((r["const"], r["const_hi"]) if with_constants else ())

    def mlook(entries):
        return tuple(sorted(look(e) for e in entries))

    def pick(good, bad):
        """The records that differ: one of `good` whose look `bad` lacks, and
        the other way round."""
        lg, lb = set(look(e) for e in good), set(look(e) for e in bad)
        g = next((e for e in good if look(e) not in lb), good[0])
        b = next((e for e in bad if look(e) not in lg), bad[0])
        return g[1], b[1]

    def shape(F):
        return {k: len(v) for k, v in F.items()}

    def consecutive(fr):
        return all(fr[i + 1] == fr[i] + 1 for i in range(len(fr) - 1))

    for frame, batches in canonical_frames(records):
        nframes += 1
        n = sum(len(v) for v in batches.values())
        nb_min = n if nb_min is None else min(nb_min, n)
        nb_max = max(nb_max, n)
        first = frame if first is None else first
        last = frame
        win.append((frame, batches))
        if len(win) > 4:
            flagged.pop(win.pop(0)[0], None)
        if len(win) < 3:
            continue
        (a, A), (b, B), (c, C) = win[-3:]
        if not consecutive([a, b, c]):
            continue
        sa, sb, sc = shape(A), shape(B), shape(C)
        if sa == sc and sb != sa:
            missing = sum(max(0, v - sb.get(k, 0)) for k, v in sa.items())
            extra = sum(max(0, v - sa.get(k, 0)) for k, v in sb.items())
            struct_out.append((b, missing, extra))
        shared = [k for k in sa if sb.get(k) == sa[k] == sc.get(k)]
        if len(shared) >= min_shared * max(len(sa), len(sb), len(sc), 1):
            fb = flagged.setdefault(b, set())
            for k in shared:
                la, lb = mlook(A[k]), mlook(B[k])
                if la == mlook(C[k]) != lb:
                    events.append(((b,), (k, la, lb)) + pick(A[k], B[k]))
                    fb.add(k)
        # two frames wrong: a | b c | d with a == d, b == c
        if len(win) == 4:
            (a, A), (b, B), (c, C), (d, D) = win
            if not consecutive([a, b, c, d]):
                continue
            sa, sb, sc, sd = shape(A), shape(B), shape(C), shape(D)
            shared = [k for k in sa if sb.get(k) == sa[k] == sc.get(k) == sd.get(k)]
            if len(shared) < min_shared * max(len(sa), len(sb), len(sc), len(sd), 1):
                continue
            fb, fc = flagged.setdefault(b, set()), flagged.setdefault(c, set())
            for k in shared:
                if k in fb or k in fc:
                    continue
                la, lb, lc, ld = mlook(A[k]), mlook(B[k]), mlook(C[k]), mlook(D[k])
                if la == ld and lb == lc and lb != la:
                    events.append(((b, c), (k, la, lb)) + pick(A[k], B[k]))
                    fb.add(k)
                    fc.add(k)

    events, periodic = split_periodic(events, lambda e: e[1], lambda e: e[0][0])
    struct_out, s_periodic = split_periodic(struct_out, lambda e: e[1:], lambda e: e[0])
    out_frames = sorted(set(f for e in events for f in e[0]))
    subst = Counter()
    by_batch = Counter()
    for fr, _, good, bad in events:
        for d in _changes(good, bad, with_constants):
            subst[d] += len(fr)
        by_batch[_batch_desc(good)] += len(fr)
    per = Counter()
    for group, period, items in periodic:
        _, _, good, bad = items[0]
        per[(period, _batch_desc(good), "; ".join(_changes(good, bad, with_constants)))] += len(items)
    for group, period, items in s_periodic:
        per[(period, "batch set", "%d batches missing, %d extra" % group)] += len(items)
    per = [(k[0], n, k[1], k[2]) for k, n in per.most_common()]
    return {"frames": nframes, "batches_min": nb_min or 0, "batches_max": nb_max,
            "first": first, "last": last,
            "events": events, "outliers": out_frames, "count": len(out_frames),
            "substitutions": subst.most_common(), "batches": by_batch.most_common(),
            "structure": struct_out, "periodic": per}


def _batch_desc(r):
    return "target %08X vb %08X %u indices" % (r["target"] or r["zeta"] | 1, r["vb"], r["count"])


def _changes(good, bad, with_constants):
    diffs = []
    for st in range(4):
        g, b = (good["tex%d" % st], good["fmt%d" % st]), (bad["tex%d" % st], bad["fmt%d" % st])
        if g != b:
            diffs.append("t%d %s instead of %s" % (st, tex_desc(*b), tex_desc(*g)))
    if good["cmask"] != bad["cmask"]:
        diffs.append("colour mask %08X instead of %08X" % (bad["cmask"], good["cmask"]))
    if not diffs and good["look"] != bad["look"]:
        diffs.append("state (signature %08X instead of %08X)" % (bad["sig"], good["sig"]))
    if not diffs:
        diffs.append("vertex constants" if with_constants else "swapped target textures")
    return diffs


def split_periodic(items, group_of, frame_of, min_repeats=4, min_period=3, share=0.8):
    """Split events into (irregular, periodic). Events of one group (the same
    batch changing the same way) that recur at one fixed interval of at least
    min_period frames, min_repeats times or more, are the title's own rhythm
    (something re-rendered every N frames), not a glitch: periodic is
    [(group, period, events)]."""
    groups = OrderedDict()
    for it in items:
        groups.setdefault(group_of(it), []).append(it)
    keep, periodic = [], []
    for g, its in groups.items():
        fr = sorted(frame_of(it) for it in its)
        gaps = Counter(fr[i + 1] - fr[i] for i in range(len(fr) - 1))
        if len(fr) >= min_repeats:
            gap, n = gaps.most_common(1)[0]
            if gap >= min_period and n >= share * (len(fr) - 1):
                periodic.append((g, gap, its))
                continue
        keep.extend(its)
    keep.sort(key=frame_of)
    return keep, periodic


def format_records_report(res, limit=12, label="batch records"):
    from .images import runs_of
    lines = ["%s: %d frames (%s..%s), %d-%d batches a frame"
             % (label, res["frames"], res["first"], res["last"], res["batches_min"], res["batches_max"]),
             "  frames with a batch whose textures/state flicker (same batches as both"
             " neighbours, other look): %d" % res["count"]]
    if res["outliers"]:
        lines.append("    " + ", ".join(str(a) if a == b else "%d-%d" % (a, b)
                                         for a, b in runs_of(res["outliers"])))
    if res["substitutions"]:
        lines.append("  what changes (batch-frames):")
        for d, n in res["substitutions"][:limit]:
            lines.append("    %5d  %s" % (n, d))
    if res["batches"]:
        lines.append("  which batches (batch-frames):")
        for d, n in res["batches"][:limit]:
            lines.append("    %5d  %s" % (n, d))
    lines.append("  frames whose batch set differs from both neighbours (which agree): %d"
                 % len(res["structure"]))
    for f, missing, extra in res["structure"][:limit]:
        lines.append("    frame %d: %d batches missing, %d extra" % (f, missing, extra))
    if res["periodic"]:
        lines.append("  periodic changes, not counted (the title's own rhythm):")
        for period, n, what, how in res["periodic"][:limit]:
            lines.append("    every %d frames (%dx): %s: %s" % (period, n, what, how))
    return "\n".join(lines)


# ---- [GPUSIG] tables ------------------------------------------------------

_SIGHEAD = re.compile(r"\[GPUSIG\] frame (\d+):")
_SIGLINE = re.compile(r"\[GPUSIG\] ([0-9A-Fa-f]{8}) n (\d+) tris (\d+) : (.*)")


def read_sig_tables(lines):
    """{frame: {sig: (batches, tris, description)}} from a run log."""
    tables, cur = OrderedDict(), None
    for line in lines:
        m = _SIGHEAD.search(line)
        if m:
            cur = tables.setdefault(int(m.group(1)), {})
            continue
        m = _SIGLINE.search(line)
        if m and cur is not None:
            cur[m.group(1).upper()] = (int(m.group(2)), int(m.group(3)), m.group(4).strip())
    return tables


_RT = re.compile(r"\brt ([0-9A-Fa-f]{8})\b")
_AT = re.compile(r"@[0-9A-Fa-f]{8}")


def canonical_table(table):
    """{signature: (n, tris, desc)} -> {class: (n, tris, desc, [signatures])}.
    A signature includes the target address, so with double buffering every
    other frame has other signatures; the class is the description with the
    target address replaced by its rank in the frame and the (first batch's)
    texture addresses left out, signatures of one class summed."""
    order, out = [], OrderedDict()
    for sig, (n, tris, desc) in table.items():
        m = _RT.search(desc)
        if m:
            a = m.group(1).upper()
            if a not in order:
                order.append(a)
            key = desc[:m.start()] + "rt #%d" % order.index(a) + desc[m.end():]
        else:
            key = desc
        key = _AT.sub("", key)
        if key in out:
            n0, t0, d0, sigs = out[key]
            out[key] = (n0 + n, t0 + tris, d0, sigs + [sig])
        else:
            out[key] = (n, tris, desc, [sig])
    return out


def analyse_sig_tables(tables):
    """Frames whose table (signature class -> batches, triangles) differs from
    both neighbours while the neighbours' tables are equal."""
    nums = sorted(tables)
    canon = {f: canonical_table(tables[f]) for f in nums}
    out = []
    for i in range(1, len(nums) - 1):
        a, b, c = nums[i - 1], nums[i], nums[i + 1]
        if b != a + 1 or c != b + 1:
            continue
        A = {k: v[:2] for k, v in canon[a].items()}
        B = {k: v[:2] for k, v in canon[b].items()}
        C = {k: v[:2] for k, v in canon[c].items()}
        if A == C and B != A:
            diff, keys = [], []
            for k in sorted(set(A) | set(B)):
                if A.get(k) != B.get(k):
                    e = canon[b].get(k) or canon[a].get(k)
                    diff.append(("/".join(e[3]), A.get(k), B.get(k), e[2]))
                    keys.append(k)
            out.append((b, diff, tuple(keys)))
    out, periodic = split_periodic(out, lambda o: o[2], lambda o: o[0])
    return {"frames": len(nums), "first": nums[0] if nums else None,
            "last": nums[-1] if nums else None, "outliers": [o[0] for o in out],
            "count": len(out), "details": [o[:2] for o in out],
            "periodic": [(period, len(its), its[0][1]) for _, period, its in periodic]}


def format_sig_report(res, limit=10):
    from .images import runs_of
    lines = ["signature tables: %d frames (%s..%s), %d frames differ from both neighbours"
             " (which agree)" % (res["frames"], res["first"], res["last"], res["count"])]
    if res["outliers"]:
        lines.append("    " + ", ".join(str(a) if a == b else "%d-%d" % (a, b)
                                         for a, b in runs_of(res["outliers"])))
    for f, diff in res["details"][:limit]:
        lines.append("    frame %d:" % f)
        for k, was, now, desc in diff[:6]:
            lines.append("      %s %s -> %s  %s" % (k, "n %d tris %d" % was if was else "absent",
                                                   "n %d tris %d" % now if now else "absent",
                                                   desc[:120]))
    if res["periodic"]:
        lines.append("  periodic changes, not counted (the title's own rhythm):")
        for period, n, diff in res["periodic"][:limit]:
            lines.append("    every %d frames (%dx): %s" % (period, n, "; ".join(
                "%s %s" % (d[0], d[3][:60]) for d in diff[:2])))
    return "\n".join(lines)
