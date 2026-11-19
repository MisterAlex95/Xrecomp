"""
Find the XDK's XMV player API in an XBE, by structure (XBA-199).

Titles play their movies with the XDK's XMV library, linked statically into
the XBE: its code moves from title to title, and no symbol names it. The
runtime carries a decoder of its own (docs/runtime/xmv.md) behind a hook for
the library's

    HRESULT WINAPI XMVDecoder_GetNextFrame(XMVDecoder *pDecoder,
                                           D3DSurface *pSurface,
                                           XMVRESULT *pResult,
                                           <timestamp> *pTimeStamp);

(stdcall, four stack arguments, `ret 0x10`), the call a title polls while a
movie plays. This module finds that function, and the decoder's
XMVDecoder_CloseDecoder (one argument, `ret 4`) and
XMVDecoder_CreateDecoderForFile (flags, file name, &decoder: `ret 0xC`) next
to it, so tools.prepare can bind the hook with no title layer
(tools.recomp --xmv-bind).

What it reads, all from the XBE (nothing is stored but names, addresses and
the evidence):

- The library table: the XDK build, and an XMV library entry if the title has
  one (names starting with "XMV").
- The sections: the XDK's libraries put their code in sections of their own
  (D3D, DSOUND, XPP...); the XMV library's is named XMV. A candidate inside it
  is the library's; with such a section, one outside it is not.
- The strings: the names of .xmv files (ANSI or UTF-16) and the functions that
  use them: the title's movie player.
- The call graph, from one linear sweep of the code: each function whose every
  `ret` is `ret 0x10` and that something calls is a candidate, scored by its
  call sites (outside the XMV section: the public API is called by the title):
    loop      the site sits in a loop (a branch after it goes back above it),
              or its function is called from one (the title's per-frame
              update): the movie loop polls GetNextFrame;
    result    a cmp with 1 (XMV_NEWFRAME) or 2 (XMV_ENDOFFILE) right after
              the call: the loop reads *pResult;
    locals    the address of a local passed (lea of a stack slot): &result,
              &timestamp;
    create    a `ret 0xC` function called before it in the same function
              (CreateDecoderForFile), and close: a `ret 4` one after it
              (CloseDecoder);
    movie     the function (or one it calls, or one that calls it) uses the
              name of an .xmv file.
  A recorded signature (profiles/xdk/<build>/xmv.json, below) that matches
  the candidate counts most. Many call sites (more than 8) count against.
- The confidence is the best candidate's score, lowered when a second one
  comes close (ambiguous). `high` (>= 0.7, unique) is what Prepare binds.
- Played by the library (XBA-309): when the XMV section is known and the best
  candidate is called only from inside it, the title never calls
  GetNextFrame: it hands its movies to the library's own XMVDecoder_Play,
  which polls GetNextFrame and paces the pictures on a clock that (from its
  disassembly) the library's GetNextFrame starts. Bound there in a retail
  title, the runtime's hook decoded every frame but Play presented 157 of
  1765 (presumably that clock never started), so such a candidate is never
  bound, whatever its score or signature (`library_only`).

The timestamp (XBA-316): how wide *pTimeStamp is, and its unit, depends on
the XDK build. XDK 5849's library stores one DWORD, the frame's time in
milliseconds (read from its code: one load of the 4th argument, one 32-bit
store, the value summed from the stream's ms frame deltas). The report's
`timestamp` says what the runtime should write, from, in this order: the
store read in this XBE's own GetNextFrame (`body`: an ebp frame, the 4th
argument loaded into a register, the stores through it; conservative: 4 or
8 bytes, else not read), the builds where it was read (TIME_BY_BUILD), and
otherwise the default, one DWORD in ms (`default`, assumed: it never writes
past a DWORD). An 8-byte store is taken as 100 ns units (REFERENCE_TIME),
a presumption: no build seen so far has one. A body that disagrees with its
build's entry gets the DWORD (the safe side) and says so.

Signatures. `--record get_next_frame=0x...` writes a masked signature of
the confirmed function (tools/fusion/signatures.py's masking: opcodes and
structure offsets kept, absolute addresses and branch targets wildcarded,
the first 32 bytes and the function's size) to
profiles/xdk/<build>/xmv.json. Every later XBE whose candidate matches it is
found with high confidence. Only the masked pattern of XDK library code is
written, as the signature tooling already does; no title code.

    python -m tools.xmv.find_api default.xbe            # the report
    python -m tools.xmv.find_api default.xbe --json     # the same, as JSON
    python -m tools.xmv.find_api default.xbe --record get_next_frame=0x0012ABC0 [close_decoder=0x...]
"""

import argparse
import bisect
import glob
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from tools.xbe_parser.xbe_parser import SECTION_EXECUTABLE, XBEParser  # noqa: E402

PROFILES_XDK = os.path.join(ROOT, "profiles", "xdk")
HIGH = 0.7              # what Prepare binds
AMBIGUOUS_GAP = 0.15    # a second candidate this close lowers the confidence
MAX_SITES = 8           # GetNextFrame is called from a few places, not dozens
CFG_LIMIT = 4000        # instructions walked per function

API = ("get_next_frame", "close_decoder", "create_decoder")
NAMES = {"get_next_frame": "XMVDecoder_GetNextFrame", "close_decoder": "XMVDecoder_CloseDecoder",
         "create_decoder": "XMVDecoder_CreateDecoderForFile"}
RET_BYTES = {"get_next_frame": 0x10, "close_decoder": 4, "create_decoder": 0xC}

WEIGHTS = {
    "loop": 0.25, "loop_up": 0.15, "result": 0.15, "locals": 0.05, "create": 0.1, "close": 0.05,
    "close_xmv": 0.1, "movie": 0.2, "movie_near": 0.12, "section": 0.25, "outside_section": -0.25,
    "library": 0.05, "signature": 0.5, "signature_other_build": 0.3, "many_sites": -0.3,
}

# XBA-316: *pTimeStamp per XDK build, where someone read the store in the library's GetNextFrame.
# Only builds actually read go here; every other build gets TIME_DEFAULT (assumed, reported as such).
TIME_LAYOUTS = ("ms32", "100ns64")      # the runtime's XMV_TIME_MS32, XMV_TIME_100NS64
TIME_BY_BUILD = {
    5849: "ms32",       # XBA-309/316: one retail XBE (XMV library 5849): `mov [eax], edi`, the time in ms
}
TIME_DEFAULT = "ms32"

_HEX = re.compile(r"0x[0-9a-f]+|\b\d+\b")
_STACK = re.compile(r"\[(ebp|esp)\b")


def _cs():
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    return Cs(CS_ARCH_X86, CS_MODE_32)


class Image:
    """The XBE's sections, a VA reader and the code's call graph."""

    def __init__(self, path):
        self.xbe = XBEParser(path).parse()
        self.raw = self.xbe.raw_data
        self.secs = []          # (lo, hi, name, executable, raw bytes)
        for s in self.xbe.sections:
            data = self.raw[s.raw_addr:s.raw_addr + min(s.raw_size, s.virtual_size or s.raw_size)]
            self.secs.append((s.virtual_addr, s.virtual_addr + len(data), s.name,
                              bool(s.flags & SECTION_EXECUTABLE), data))
        self.md = _cs()
        self._one = {}
        self._cfg = {}
        self.sites = {}         # callee -> [call site]
        self.calls = []         # (site, callee), sorted by site
        self._sweep()
        starts = set(self.sites)
        if self.is_code(self.xbe.header.entry_point):
            starts.add(self.xbe.header.entry_point)
        self.starts = sorted(starts)

    # ── memory ──────────────────────────────────────────────────────────
    def section_of(self, va):
        for s in self.secs:
            if s[0] <= va < s[1]:
                return s
        return None

    def is_code(self, va):
        s = self.section_of(va)
        return bool(s and s[3])

    def read(self, va, n):
        s = self.section_of(va)
        if not s:
            return b""
        return s[4][va - s[0]:va - s[0] + n]

    def insn(self, va):
        """(size, mnemonic, op_str) of the instruction at va, or None."""
        r = self._one.get(va)
        if r is None and va not in self._one:
            r = next(((sz, m, op) for _, sz, m, op in self.md.disasm_lite(self.read(va, 15), va, 1)), None)
            self._one[va] = r
        return r

    def decode(self, lo, hi):
        """[(va, size, mnemonic, op_str)] linearly over [lo, hi); undecodable bytes skipped."""
        out, va = [], lo
        while va < hi:
            code = self.read(va, hi - va)
            if not code:
                break
            last = va
            for a, sz, m, op in self.md.disasm_lite(code, va):
                out.append((a, sz, m, op))
                last = a + sz
            va = last if last > va else va + 1      # capstone stops at an undecodable byte
        return out

    # ── call graph ──────────────────────────────────────────────────────
    def _sweep(self):
        for lo, hi, _, exe, data in self.secs:
            if not exe:
                continue
            for a, _, m, op in self.decode(lo, hi):
                if m == "call" and op.startswith("0x"):
                    t = int(op, 16)
                    if self.is_code(t):
                        self.sites.setdefault(t, []).append(a)
                        self.calls.append((a, t))
        self.calls.sort()
        self._call_sites = [c[0] for c in self.calls]

    def function_of(self, va):
        """The nearest call target at or below va (a function start), or None."""
        i = bisect.bisect_right(self.starts, va) - 1
        return self.starts[i] if i >= 0 else None

    def function_end(self, f):
        i = bisect.bisect_right(self.starts, f)
        end = self.starts[i] if i < len(self.starts) else f + 0x4000
        s = self.section_of(f)
        return min(end, f + 0x4000, s[1] if s else end)

    def calls_in(self, lo, hi):
        i = bisect.bisect_left(self._call_sites, lo)
        out = []
        while i < len(self.calls) and self.calls[i][0] < hi:
            out.append(self.calls[i])
            i += 1
        return out

    def cfg(self, f):
        """{"rets": {bytes popped}, "size": bytes, "insns": n} of the function at f (branches followed)."""
        if f in self._cfg:
            return self._cfg[f]
        rets, seen, work, end, n = set(), set(), [f], f, 0
        sec = self.section_of(f)
        while work and n < CFG_LIMIT:
            va = work.pop()
            while va not in seen and n < CFG_LIMIT:
                ins = self.insn(va)
                if not ins:
                    break
                seen.add(va)
                n += 1
                sz, m, op = ins
                end = max(end, va + sz)
                if m == "ret":
                    rets.add(int(op, 0) if op else 0)
                    break
                if m.startswith("j"):
                    t = int(op, 16) if op.startswith("0x") else None
                    # A jump into another function (a tail call) is not this body.
                    inside = t is not None and sec and sec[0] <= t < sec[1] and \
                        (t not in self.sites or t == f) and abs(t - f) < 0x10000
                    if m == "jmp":
                        if inside:
                            va = t
                            continue
                        break
                    if inside:
                        work.append(t)
                if m in ("int3", "hlt", "ud2"):
                    break
                va += sz
        r = {"rets": rets, "size": end - f, "insns": n}
        self._cfg[f] = r
        return r


def _imm(op):
    """The last operand's immediate, or None."""
    last = op.rsplit(",", 1)[-1].strip()
    try:
        return int(last, 0)
    except ValueError:
        return None


def movie_strings(img):
    """[(va, text)] of the strings naming .xmv files (ANSI or UTF-16)."""
    out = []
    pat = re.compile(rb"\.xmv(?=\x00)", re.I)
    wpat = re.compile(rb"\.\x00[xX]\x00[mM]\x00[vV]\x00(?=\x00\x00)")
    for lo, _, _, _, data in img.secs:
        for m in pat.finditer(data):
            i = m.start()
            while i > 0 and m.end() - i < 260 and 0x20 <= data[i - 1] < 0x7F:
                i -= 1
            out.append((lo + i, data[i:m.end()].decode("ascii", "replace")))
        for m in wpat.finditer(data):
            i = m.start()
            while i > 1 and m.end() - i < 520 and data[i - 1] == 0 and 0x20 <= data[i - 2] < 0x7F:
                i -= 2
            out.append((lo + i, data[i:m.end()].decode("utf-16-le", "replace")))
    return sorted(set(out))


def movie_functions(img, strings):
    """The functions whose code holds the address of a movie string."""
    funcs = set()
    for va, _ in strings:
        needle = va.to_bytes(4, "little")
        for lo, _, _, exe, data in img.secs:
            if not exe:
                continue
            i = data.find(needle)
            while i >= 0:
                f = img.function_of(lo + i)
                if f is not None:
                    funcs.add(f)
                i = data.find(needle, i + 1)
    return funcs


def xmv_library(xbe):
    for lib in xbe.libraries:
        if lib.name.upper().startswith("XMV"):
            return {"name": lib.name, "build": lib.build}
    return None


def xmv_section(img):
    for lo, hi, name, exe, _ in img.secs:
        if exe and name.upper().startswith("XMV"):
            return {"name": name, "lo": lo, "hi": hi}
    return None


def xdk_build(xbe):
    from tools.xbe_ident import xdk_of
    return xdk_of(xbe)["build"]


# ── signatures ──────────────────────────────────────────────────────────────
def load_signatures(root=PROFILES_XDK):
    """[(build, api name, {"size", "pat", "mask"})] from profiles/xdk/*/xmv.json."""
    out = []
    for path in sorted(glob.glob(os.path.join(root, "*", "xmv.json"))):
        try:
            with open(path, encoding="utf-8") as f:
                db = json.load(f)
        except (OSError, ValueError):
            continue
        for name, sigs in (db.get("functions") or {}).items():
            for s in sigs:
                out.append((int(db.get("build", 0)), name, s))
    return out


def signature_of(img, va):
    """{"size", "pat", "mask"}: the masked signature of the function at va (tools.fusion.signatures)."""
    from tools.fusion.signatures import PAT_LEN, masked_pattern
    lo = min(s[0] for s in img.secs)
    hi = max(s[1] for s in img.secs)
    size = img.cfg(va)["size"]
    pat, mask = masked_pattern(img.read(va, min(size, PAT_LEN)), va, lo, hi)
    return {"size": size, "pat": pat.hex(), "mask": mask.hex()}


def _sig_match(a, b):
    if a["size"] != b["size"]:
        return False
    from tools.fusion.signatures import _pat_matches
    return _pat_matches(bytes.fromhex(a["pat"]), bytes.fromhex(a["mask"]),
                        bytes.fromhex(b["pat"]), bytes.fromhex(b["mask"]))


# ── scoring ─────────────────────────────────────────────────────────────────
def _site_evidence(img, site, sec, movie_funcs, loop_sites):
    """{feature: weight} for one call site of a candidate, plus the create / close callees seen."""
    ev, found = {}, {}
    f = img.function_of(site - 1)
    if f is None or site - f > 0x4000:
        f = max(site - 0x400, img.section_of(site)[0])
    fend = max(img.function_end(f), site + 5)
    insns = img.decode(f, min(fend, site + 0x800))
    idx = next((i for i, x in enumerate(insns) if x[0] == site), None)
    if idx is None:                     # not on an instruction boundary from f: decode from the site
        insns = img.decode(site, min(fend, site + 0x800))
        idx = 0
    before, after = insns[:idx], insns[idx + 1:]
    # loop: a branch after the call back to it or above it, within the function
    if any(m.startswith("j") and op.startswith("0x") and f <= int(op, 16) <= site for _, _, m, op in after):
        ev["loop"] = WEIGHTS["loop"]
    elif any(s in loop_sites for s in img.sites.get(f, ())):
        ev["loop_up"] = WEIGHTS["loop_up"]
    # result: cmp ..., 1 / 2 right after
    for _, _, m, op in after[:8]:
        if m == "cmp" and _imm(op) in (1, 2):
            ev["result"] = WEIGHTS["result"]
            break
    # locals: lea r, [ebp/esp ...] among the instructions before
    if any(m == "lea" and _STACK.search(op) for _, _, m, op in before[-12:]):
        ev["locals"] = WEIGHTS["locals"]

    def in_sec(t):
        return bool(sec and sec["lo"] <= t < sec["hi"])

    for a, sz, m, op in before:
        if m == "call" and op.startswith("0x"):
            t = int(op, 16)
            if img.is_code(t) and img.cfg(t)["rets"] == {0xC} and (not sec or in_sec(t)):
                found["create_decoder"] = t     # the last one before the poll
    if "create_decoder" in found:
        ev["create"] = WEIGHTS["create"]
    for a, sz, m, op in after:
        if m == "call" and op.startswith("0x"):
            t = int(op, 16)
            if t != img_target(img, site) and img.is_code(t) and img.cfg(t)["rets"] == {4} \
                    and (not sec or in_sec(t)):
                found["close_decoder"] = t
                ev["close_xmv" if sec else "close"] = WEIGHTS["close_xmv" if sec else "close"]
                break
    # movie: this function, or a neighbour in the call graph, uses an .xmv name
    if f in movie_funcs:
        ev["movie"] = WEIGHTS["movie"]
    else:
        callees = {t for _, t in img.calls_in(f, fend)}
        callers = {img.function_of(s) for s in img.sites.get(f, ())}
        if (callees | callers) & movie_funcs:
            ev["movie_near"] = WEIGHTS["movie_near"]
    return ev, found


def img_target(img, site):
    ins = img.insn(site)
    return int(ins[2], 16) if ins and ins[1] == "call" and ins[2].startswith("0x") else None


class _LoopSites:
    """Call sites inside a loop of their function (a later branch back above them), asked lazily: the
    'called from a loop' rule (a title's per-frame update calling GetNextFrame once per frame)."""

    def __init__(self, img):
        self.img, self.memo = img, {}

    def __contains__(self, site):
        if site not in self.memo:
            img = self.img
            f = img.function_of(site - 1) or site
            hi = min(img.function_end(f), site + 0x800)
            self.memo[site] = any(m.startswith("j") and op.startswith("0x") and f <= int(op, 16) <= site
                                  for _, _, m, op in img.decode(site, hi))
        return self.memo[site]


def find(path, signatures=None):
    """The report: {"xdk", "library", "section", "strings", "candidates", "best", "confidence", "high"}."""
    img = Image(path)
    xbe = img.xbe
    sec = xmv_section(img)
    lib = xmv_library(xbe)
    build = (lib or {}).get("build") or xdk_build(xbe)
    strings = movie_strings(img)
    mfuncs = movie_functions(img, strings)
    sigs = load_signatures() if signatures is None else signatures
    loops = _LoopSites(img)

    # Candidates: called functions with a `ret 0x10` byte pattern in their body, then checked by walking them.
    rets = []
    for lo, _, _, exe, data in img.secs:
        if exe:
            i = data.find(b"\xC2\x10\x00")
            while i >= 0:
                rets.append(lo + i)
                i = data.find(b"\xC2\x10\x00", i + 1)
    rets.sort()
    cands = []
    for t in sorted(img.sites):
        j = bisect.bisect_left(rets, t)
        if j == len(rets) or rets[j] >= img.function_end(t):
            continue
        if img.cfg(t)["rets"] != {0x10}:
            continue
        sites = img.sites[t]
        public = [s for s in sites if not (sec and sec["lo"] <= s < sec["hi"])]
        ev, found, best_site = {}, {}, None
        for s in public or sites:
            e, fo = _site_evidence(img, s, sec, mfuncs, loops)
            if best_site is None or sum(e.values()) > sum(ev.values()):
                ev, found, best_site = e, fo, s
        if sec:
            ev["section" if sec["lo"] <= t < sec["hi"] else "outside_section"] = \
                WEIGHTS["section" if sec["lo"] <= t < sec["hi"] else "outside_section"]
        if lib:
            ev["library"] = WEIGHTS["library"]
        if len(sites) > MAX_SITES:
            ev["many_sites"] = WEIGHTS["many_sites"]
        if sigs:
            mine = signature_of(img, t)
            for b, name, s in sigs:
                if name == "get_next_frame" and _sig_match(mine, s):
                    k = "signature" if b == build else "signature_other_build"
                    if k == "signature" or "signature" not in ev:
                        ev.pop("signature_other_build", None)
                        ev[k] = WEIGHTS[k]
        score = max(0.0, min(1.0, sum(ev.values())))
        cands.append({"va": t, "score": round(score, 3), "evidence": ev, "sites": len(sites),
                      "site": best_site, "related": found, "public_sites": len(public)})
    cands.sort(key=lambda c: (-c["score"], c["va"]))
    best = {k: None for k in API}
    conf = 0.0
    library_only = False
    if cands and cands[0]["score"] > 0:
        top = cands[0]
        library_only = bool(sec) and top["public_sites"] == 0
        conf = top["score"]
        if len(cands) > 1 and cands[1]["score"] >= top["score"] - AMBIGUOUS_GAP:
            conf = max(0.0, conf - 0.3)
        best["get_next_frame"] = top["va"]
        best.update({k: v for k, v in top["related"].items()})
    timestamp = timestamp_layout(img, best["get_next_frame"], build)
    return {
        "path": os.path.abspath(path),
        "xdk": build,
        "library": lib,
        "section": sec,
        "strings": [{"va": va, "text": tx} for va, tx in strings],
        "movie_functions": sorted(mfuncs),
        "candidates": cands[:8],
        "best": best,
        "confidence": round(conf, 3),
        "high": conf >= HIGH and not library_only,
        "library_only": library_only,
        "timestamp": timestamp,
    }


# ── the timestamp's layout (XBA-316) ─────────────────────────────────────
_REG32 = ("eax", "ebx", "ecx", "edx", "esi", "edi")
_SIZES = {"byte": 1, "word": 2, "dword": 4, "qword": 8, "xmmword": 16}
FOLLOW = 32             # instructions followed after the 4th argument is loaded
_NOT_STORES = ("cmp", "test", "push")


def timestamp_store(img, f):
    """How many bytes the GetNextFrame at f stores through its 4th argument: 4, 8, or None when that cannot
    be read safely. Only the plain shape: an ebp frame (push ebp; mov ebp, esp), `mov reg, [ebp + 0x14]`,
    then stores `[reg]` / `[reg + n]` before reg changes, a call or a jmp/ret. Every load is followed and the
    widths joined; anything else (an esp-only frame, a pointer handed to a helper, an indexed store) is None."""
    ins = img.decode(f, img.function_end(f))
    if len(ins) < 2 or ins[0][2:] != ("push", "ebp") or ins[1][2:] != ("mov", "ebp, esp"):
        return None
    reach = 0
    for i, (_, _, m, op) in enumerate(ins):
        dst, _, src = (x.strip() for x in op.partition(","))
        if m != "mov" or dst not in _REG32 or src != "dword ptr [ebp + 0x14]":
            continue
        mem = re.compile(r"(byte|word|dword|qword|xmmword) ptr \[%s(?: \+ (0x[0-9a-f]+|\d+))?\]$" % dst)
        for _, _, m2, op2 in ins[i + 1:i + 1 + FOLLOW]:
            if m2 in ("call", "ret", "jmp", "leave"):
                break
            d2 = op2.partition(",")[0].strip()
            if m2 in _NOT_STORES:
                continue
            hit = mem.match(d2)
            if hit:
                reach = max(reach, int(hit.group(2) or "0", 0) + _SIZES[hit.group(1)])
            elif "ptr [" in d2 and re.search(r"\b%s\b" % dst, d2):
                return None                     # a store through it of a shape not read here
            elif d2 == dst:
                break                           # the register now holds something else
    return reach if reach in (4, 8) else None


def timestamp_layout(img, f, build):
    """{"layout", "source", "store_bytes", "build_entry"}: what the runtime writes at *pTimeStamp."""
    width = timestamp_store(img, f) if f else None
    body = {4: "ms32", 8: "100ns64"}.get(width)
    known = TIME_BY_BUILD.get(build)
    if body and known and body != known:
        layout, source = TIME_DEFAULT, "conflict"
    elif body:
        layout, source = body, "body"
    elif known:
        layout, source = known, "build"
    else:
        layout, source = TIME_DEFAULT, "default"
    return {"layout": layout, "source": source, "store_bytes": width, "build_entry": known}


def timestamp_text(ts, build=None):
    """How the report and the prepare log say it."""
    what = {"ms32": "one DWORD in ms", "100ns64": "64-bit in 100 ns units"}[ts["layout"]]
    why = {
        "body": "the %d-byte store read in this XBE's GetNextFrame%s" % (
            ts["store_bytes"] or 0, ", 100 ns presumed" if ts["layout"] == "100ns64" else ""),
        "build": "as read in XDK %s's library" % build,
        "default": "assumed: no store read here, XDK %s not checked" % (build or "unknown"),
        "conflict": "the %s-byte store read here disagrees with XDK %s's (%s): the DWORD, which never writes "
                    "past the field" % (ts["store_bytes"], build, ts["build_entry"]),
    }[ts["source"]]
    return "%s (%s)" % (what, why)


# ── what Prepare binds ──────────────────────────────────────────────────────
def bind_plan(report):
    """The tools.recomp --xmv-bind file's content, or None: the functions to bind when the confidence is high."""
    if not report.get("high") or not report["best"].get("get_next_frame"):
        return None
    plan = {"confidence": report["confidence"], "xdk": report["xdk"]}
    ts = report.get("timestamp") or {"layout": TIME_DEFAULT, "source": "default"}
    plan["timestamp"], plan["timestamp_source"] = ts["layout"], ts["source"]
    for k in ("get_next_frame", "close_decoder"):
        if report["best"].get(k):
            plan[k] = "0x%08X" % report["best"][k]
    return plan


def prepare_line(report, plan, mode):
    """One line for the prepare log."""
    best = report["best"].get("get_next_frame")
    if mode == "native":
        return "XMV: RECOMP_XMV=native, the title's own movie decoder is kept (nothing bound)"
    if plan:
        extra = " and %s at %s" % (NAMES["close_decoder"], plan["close_decoder"]) if plan.get("close_decoder") else ""
        ts = report.get("timestamp")
        return ("XMV: %s at %s%s (confidence %.2f): bound to the runtime's decoder "
                "(RECOMP_XMV=native at run time keeps the title's own)%s"
                % (NAMES["get_next_frame"], plan["get_next_frame"], extra, plan["confidence"],
                   "; frame times written as " + timestamp_text(ts, report["xdk"]) if ts else ""))
    if best and report.get("library_only"):
        return ("XMV: %s at 0x%08X is called only by the XMV library itself: the title plays its movies with "
                "the library's XMVDecoder_Play, which needs the library's own GetNextFrame; not bound, the title's "
                "own decoder plays them" % (NAMES["get_next_frame"], best))
    if best:
        return ("XMV: best guess for %s 0x%08X, confidence %.2f: too low, not bound "
                "(python -m tools.xmv.find_api default.xbe says why)" % (NAMES["get_next_frame"], best,
                                                                         report["confidence"]))
    if report.get("section") or report.get("library") or report.get("strings"):
        return "XMV: the title has movies but no %s was found: not bound" % NAMES["get_next_frame"]
    return None


def prepare_bind(xbe, out_dir, log=print):
    """For tools.prepare: find the API, write <out_dir>/xmv_api.json (the report) and, when it is bound,
    <out_dir>/xmv_bind.json; returns the bind file's path or None. RECOMP_XMV=native in the prepare's
    environment binds nothing. Never raises: a failure is a line in the log."""
    mode = (os.environ.get("RECOMP_XMV") or "auto").strip().lower()
    os.makedirs(out_dir, exist_ok=True)
    bind = os.path.join(out_dir, "xmv_bind.json")
    if os.path.exists(bind):
        os.remove(bind)
    try:
        report = find(xbe)
    except ImportError as e:
        log("XMV: not searched (%s)" % e)
        return None
    except Exception as e:              # never let the search stop a prepare
        log("XMV: search failed: %s" % e)
        return None
    with open(os.path.join(out_dir, "xmv_api.json"), "w") as f:
        json.dump(_jsonable(report), f, indent=1)
    plan = None if mode == "native" else bind_plan(report)
    line = prepare_line(report, plan, mode)
    if line:
        log(line)
    if not plan:
        return None
    with open(bind, "w") as f:
        json.dump(plan, f, indent=1)
    return bind


def bind_key(path):
    """What the lift depends on: the bind file's content and the generated binding's version, or "" with
    none."""
    if not path or not os.path.exists(path):
        return ""
    from tools.recomp.xmv_bind import VERSION
    with open(path, encoding="utf-8") as f:
        return json.dumps(dict(json.load(f), binding=VERSION), sort_keys=True)


# ── CLI ─────────────────────────────────────────────────────────────────────
def _jsonable(report):
    r = dict(report)
    hexa = lambda v: "0x%08X" % v if isinstance(v, int) else v      # noqa: E731
    r["strings"] = [{"va": hexa(s["va"]), "text": s["text"]} for s in report["strings"]]
    r["movie_functions"] = [hexa(v) for v in report["movie_functions"]]
    r["best"] = {k: hexa(v) for k, v in report["best"].items()}
    r["candidates"] = [dict(c, va=hexa(c["va"]), site=hexa(c["site"]),
                            related={k: hexa(v) for k, v in c["related"].items()}) for c in report["candidates"]]
    if report["section"]:
        r["section"] = dict(report["section"], lo=hexa(report["section"]["lo"]), hi=hexa(report["section"]["hi"]))
    return r


def format_report(report):
    out = []
    lib, sec = report["library"], report["section"]
    out.append("XDK build %s; XMV library entry: %s; XMV section: %s" % (
        report["xdk"] or "unknown", "%s %d" % (lib["name"], lib["build"]) if lib else "none",
        "%s 0x%08X-0x%08X" % (sec["name"], sec["lo"], sec["hi"]) if sec else "none"))
    out.append("movie names: %d%s" % (len(report["strings"]), "".join(
        "\n  0x%08X %s" % (s["va"], s["text"]) for s in report["strings"][:12])))
    out.append("functions using them: %s" % (" ".join("0x%08X" % v for v in report["movie_functions"][:12])
                                              or "none"))
    out.append("%s candidates (every ret is ret 0x10), best first:" % NAMES["get_next_frame"])
    for c in report["candidates"]:
        rel = ", ".join("%s 0x%08X" % (NAMES[k].split("_", 1)[1], v) for k, v in sorted(c["related"].items()))
        out.append("  0x%08X  score %.2f  %d call site(s)  %s%s" % (
            c["va"], c["score"], c["sites"], " ".join("%s%+.2f" % kv for kv in sorted(c["evidence"].items())),
            "; " + rel if rel else ""))
    if not report["candidates"]:
        out.append("  none")
    best = report["best"]
    out.append("best: " + ", ".join("%s %s" % (NAMES[k], "0x%08X" % best[k] if best[k] else "-") for k in API))
    if best["get_next_frame"] and report.get("timestamp"):
        out.append("timestamp: " + timestamp_text(report["timestamp"], report["xdk"]))
    out.append("confidence %.2f: %s" % (report["confidence"], "high, tools.prepare binds it" if report["high"]
                                        else "called only by the XMV library (XMVDecoder_Play): never bound"
                                        if report.get("library_only")
                                        else "not bound by tools.prepare (needs %.2f)" % HIGH))
    return "\n".join(out)


def record(path, confirmed, build=None, root=PROFILES_XDK):
    """Write the masked signatures of the confirmed functions ({api name: va}) to profiles/xdk/<build>/xmv.json."""
    img = Image(path)
    lib = xmv_library(img.xbe)
    build = build or (lib or {}).get("build") or xdk_build(img.xbe)
    if not build:
        raise SystemExit("record: the XBE names no XDK build; give --build")
    d = os.path.join(root, str(build))
    os.makedirs(d, exist_ok=True)
    out = os.path.join(d, "xmv.json")
    db = {"build": build, "functions": {}}
    if os.path.exists(out):
        with open(out, encoding="utf-8") as f:
            db = json.load(f)
    db["_what"] = ("Masked signatures of the XDK XMV library's API (tools.xmv.find_api --record): the first 32 "
                   "bytes with addresses and branch targets wildcarded, and the function's size. XDK library "
                   "code only, no title code.")
    added = []
    for name, va in confirmed.items():
        if name not in API:
            raise SystemExit("record: %s is not one of %s" % (name, ", ".join(API)))
        want = RET_BYTES[name]
        got = img.cfg(va)["rets"]
        if got != {want}:
            raise SystemExit("record: 0x%08X returns with %s, not ret 0x%X: not %s"
                             % (va, sorted(got) or "nothing", want, NAMES[name]))
        sig = signature_of(img, va)
        lst = db["functions"].setdefault(name, [])
        if sig not in lst:
            lst.append(sig)
            added.append(name)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(db, f, indent=1, sort_keys=True)
        f.write("\n")
    return out, added


def main(argv=None):
    ap = argparse.ArgumentParser(prog="tools.xmv.find_api", description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xbe", help="default.xbe (or any XBE)")
    ap.add_argument("--json", action="store_true", help="the report as JSON")
    ap.add_argument("--record", nargs="+", metavar="NAME=VA",
                    help="write the masked signature of confirmed functions (get_next_frame, close_decoder, "
                         "create_decoder) to profiles/xdk/<build>/xmv.json")
    ap.add_argument("--build", type=int, help="with --record: the XDK build to file it under")
    a = ap.parse_args(argv)
    if a.record:
        conf = {}
        for item in a.record:
            k, _, v = item.partition("=")
            conf[k.strip()] = int(v, 16)
        out, added = record(a.xbe, conf, a.build)
        print("recorded %s in %s" % (", ".join(added) or "nothing new", os.path.relpath(out, ROOT)))
        return 0
    report = find(a.xbe)
    if a.json:
        print(json.dumps(_jsonable(report), indent=1))
    else:
        print(format_report(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
