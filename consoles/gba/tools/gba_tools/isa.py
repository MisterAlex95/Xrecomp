"""ARMv4T decoding shared by function discovery and the lifter (XBA-150, XBA-151).

The classification mirrors the core's interpreter (consoles/gba/runtime/gba_cpu.c, arm_execute / thumb_execute)
bit for bit, so the lifted code runs exactly the semantics the interpreter gives each encoding, quirks included.
Capstone (when installed) only supplies the disassembly text written as comments in the generated C.
"""


def sext(v: int, bits: int) -> int:
    m = 1 << (bits - 1)
    return (v & (m - 1)) - (v & m)


def ror(v: int, n: int) -> int:
    n &= 31
    v &= 0xFFFFFFFF
    return ((v >> n) | (v << (32 - n))) & 0xFFFFFFFF if n else v


def arm_class(op: int) -> str:
    """The interpreter's dispatch for an ARM opcode (condition aside)."""
    k = (op >> 25) & 7
    if k == 0:
        if (op & 0x0FC000F0) == 0x00000090:
            return "mul"
        if (op & 0x0F8000F0) == 0x00800090:
            return "mull"
        if (op & 0x0FB00FF0) == 0x01000090:
            return "swp"
        if (op & 0x0FFFFFF0) == 0x012FFF10:
            return "bx"
        if (op & 0x90) == 0x90 and (op & 0x60):
            return "half"
        if (op & 0x01900000) == 0x01000000:
            if (op & 0x0FBF0FFF) == 0x010F0000:
                return "mrs"
            if (op & 0x0DB0F000) == 0x0120F000:
                return "msr"
            return "undef"
        return "dp"
    if k == 1:
        if (op & 0x01900000) == 0x01000000:
            return "msr" if (op & 0x0FB0F000) == 0x0320F000 else "undef"
        return "dp"
    if k == 2:
        return "single"
    if k == 3:
        return "undef" if op & 0x10 else "single"
    if k == 4:
        return "block"
    if k == 5:
        return "branch"
    if k == 6:
        return "undef"
    return "swi" if op & (1 << 24) else "undef"


def thumb_class(op: int) -> str:
    """The interpreter's dispatch for a Thumb opcode."""
    k = op >> 11
    if k <= 2:
        return "shift"
    if k == 3:
        return "addsub"
    if k <= 7:
        return "imm8"
    if k == 8:
        return "hireg" if op & 0x400 else "alu"
    if k == 9:
        return "ldrpc"
    if k in (0xA, 0xB):
        return "regoff"
    if k <= 0xF:
        return "immoff"
    if k <= 0x11:
        return "half"
    if k <= 0x13:
        return "sprel"
    if k == 0x14:
        return "addpc"
    if k == 0x15:
        return "addsp"
    if k in (0x16, 0x17):
        if (op & 0xF00) == 0:
            return "spadj"
        if (op & 0x600) == 0x400:
            return "pushpop"
        return "undef"
    if k in (0x18, 0x19):
        return "ldmstm"
    if k in (0x1A, 0x1B):
        c = (op >> 8) & 15
        if c == 15:
            return "swi"
        if c == 14:
            return "undef"
        return "bcond"
    if k == 0x1C:
        return "b"
    if k == 0x1E:
        return "blhi"
    if k == 0x1F:
        return "bllo"
    return "undef"


def arm_branch_target(op: int, addr: int) -> int:
    return (addr + 8 + sext(op & 0xFFFFFF, 24) * 4) & 0xFFFFFFFF


def thumb_bcond_target(op: int, addr: int) -> int:
    return (addr + 4 + sext(op & 0xFF, 8) * 2) & 0xFFFFFFFF


def thumb_b_target(op: int, addr: int) -> int:
    return (addr + 4 + sext(op & 0x7FF, 11) * 2) & 0xFFFFFFFF


def thumb_bl_target(hi: int, lo: int, addr_hi: int) -> int:
    return (addr_hi + 4 + (sext(hi & 0x7FF, 11) << 12) + (lo & 0x7FF) * 2) & 0xFFFFFFFF


_cs = {}


def disasm(op: int, addr: int, thumb: bool) -> str:
    """Capstone's text for one instruction, '' when Capstone is not installed or refuses it."""
    try:
        import capstone  # noqa: PLC0415  (optional dependency)
    except ImportError:
        return ""
    key = bool(thumb)
    if key not in _cs:
        _cs[key] = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
    raw = op.to_bytes(2 if thumb else 4, "little")
    for i in _cs[key].disasm(raw, addr):
        return ("%s %s" % (i.mnemonic, i.op_str)).strip()
    return ""
