"""Code copied into RDRAM by PI DMA, found statically (XBA-169).

libultra games load further code segments with osPiStartDma (through a PI manager thread) or a wrapper of their own
(dma_read(dst, rom_start, rom_end), ...), libdragon with dma_read(dst, pi_address, len); the arguments are usually
link-time constants (segment symbols) built with LUI / ADDIU / ORI right before the call. Without symbols we do not
know which function is the DMA routine, so every call site is looked at: the constant arguments (a0..a3 and the
first stack arguments, sp+0x10..sp+0x1C, stored just before the call) that read as an RDRAM address (the
destination), a cartridge address or ROM offset (the source) and a length or a source end make a candidate segment:
ROM bytes [source, source + length) at that RDRAM address. A candidate is kept when its first instructions decode
as code. A wrong candidate costs only compile time (entries run only when their bytes are in memory); a segment
copied with computed arguments (tables of segments, compressed code) is found by the discovery loop instead.
"""

from .discover import Image, plausible_code
from .isa import A0, SP

MAX_SEG = 0x100000  # 1 MB per segment at most
MAX_SEGS = 64
STACK_ARGS = (0x10, 0x14, 0x18, 0x1C)


def _rom_offset(v, rom_size):
    if 0x10001000 <= v < 0x10000000 + rom_size:
        return v - 0x10000000
    if 0x1000 <= v < rom_size and v < 0x04000000:
        return v
    return None


def _rdram(v):
    if 0x80000400 <= v < 0x80800000:
        return v
    if 0xA0000400 <= v < 0xA0800000:
        return v - 0x20000000
    return None


def call_sites(img):
    """(jal address, {arg: constant}) for every JAL / JALR in img whose arguments are constants built just before it
    (straight-line code since the last label-like instruction: transfers reset the tracking)."""
    consts = {}
    stack = {}
    out = []
    a = img.base
    pend = None
    while a < img.end:
        i = img.ins(a)
        n = i.name
        if n == "lui":
            consts[i.rt] = (i.imm << 16) & 0xFFFFFFFF
        elif n in ("addiu", "daddiu") and i.rs in consts:
            consts[i.rt] = (consts[i.rs] + i.simm) & 0xFFFFFFFF
        elif n in ("addiu", "daddiu") and i.rs == 0:
            consts[i.rt] = i.simm & 0xFFFFFFFF
        elif n == "ori" and (i.rs in consts or i.rs == 0):
            consts[i.rt] = (consts.get(i.rs, 0) | i.imm) & 0xFFFFFFFF
        elif n in ("or", "addu", "daddu") and i.rt == 0 and i.rs in consts:
            consts[i.rd] = consts[i.rs]
        elif n == "sw" and i.rs == SP and i.simm in STACK_ARGS and (i.rt in consts or i.rt == 0):
            stack[i.simm] = consts.get(i.rt, 0)
        else:
            for d in i.writes():
                consts.pop(d, None)
        if pend is not None:  # the delay slot of a call ran: the call happens now
            args = {r: consts[r] for r in range(A0, A0 + 4) if r in consts}
            for off, v in stack.items():
                args["sp+0x%X" % off] = v
            if args:
                out.append((pend, args))
            pend = None
            consts = {}
            stack = {}
        elif i.is_transfer():
            if i.name in ("jal", "jalr"):
                pend = a
            else:
                consts = {}
                stack = {}
        a += 4
    return out


def find_segments(rom, images):
    """Candidate code segments copied by PI DMA (rom: rom.Rom; images: the boot images). Returns [Image]."""
    size = len(rom.data)
    seen = set()
    out = []
    known = [(im.base, im.end) for im in images]
    for img in images:
        if not img.executable:
            continue
        for _addr, args in call_sites(img):
            vals = list(args.values())
            dsts = [d for d in (_rdram(v) for v in vals) if d is not None]
            srcs = [s for s in (_rom_offset(v, size) for v in vals) if s is not None]
            if not dsts or not srcs:
                continue
            for d in dsts:
                for s in srcs:
                    lens = [v for v in vals if 0 < v <= MAX_SEG and v != d and _rom_offset(v, size) != s]
                    ends = [e - s for e in srcs if 0 < e - s <= MAX_SEG]
                    for n in sorted(set(lens + ends), reverse=True)[:1]:
                        n &= ~3
                        key = (d, s, n)
                        if n < 16 or key in seen or s + n > size:
                            continue
                        if any(b <= d and d + n <= e and bytes(rom.data[s:s + n]) ==
                               bytes(next(im for im in images if im.base == b).data[d - b:d - b + n])
                               for b, e in known):
                            continue  # the boot image already holds those bytes there
                        seg = Image(d, rom.data[s:s + n], "dma")
                        if not plausible_code(seg, d, 8):
                            continue
                        seen.add(key)
                        out.append(seg)
                        if len(out) >= MAX_SEGS:
                            return out
    return out

