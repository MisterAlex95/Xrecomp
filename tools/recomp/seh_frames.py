"""Exception frames of a function: what a handler may call or jump to (XBA-112).

A function that registers an exception frame is entered, at run time, in more
places than its own entry: the handler calls its funclets (an __except filter,
a __finally block, a catch block, a destructor unwind action) and, to resume,
jumps into its body (an __except block, a catch continuation). None of those is
reached by the function's own control flow, so a decoder that keeps only what
it can reach from the entry drops them, and they get no label.

This module reads the frame the function registers and the compiler's tables
behind it, and returns those addresses. Two frame kinds, both as MSVC 7.x
(the XDK's compiler) emits them:

EH3, for __try/__except/__finally, either inline

    push -1 ; push <scope table> ; push <_except_handler3>
    mov eax, fs:[0] ; push eax ; mov fs:[0], esp

or through the CRT helper: push <locals size> ; push <scope table> ;
call __SEH_prolog. The scope table is an array of
{enclosing level, filter, handler}; a filter of 0 marks a __finally (the
handler is a funclet), otherwise the filter is a funclet and the handler is
the __except block, a resume target. The table has no length field: it is
one more than the highest try level the function stores at [ebp-4].

C++ EH, for try/catch and objects with destructors:

    push -1 ; push <__ehhandler$f> ; mov eax, fs:[0] ; push eax
    mov fs:[0], esp

where __ehhandler$f is `mov eax, <FuncInfo> ; jmp __CxxFrameHandler`.
FuncInfo (magic 0x1993052x) gives the unwind map (destructor funclets) and
the try-block map, whose handler arrays give the catch funclets. A catch
funclet returns its continuation address in eax (`mov eax, <label> ; ret`),
which the frame handler then jumps to: those are the resume targets.
"""
from dataclasses import dataclass, field

_EH_MAGICS = (0x19930520, 0x19930521, 0x19930522)
_MINUS_ONE = 0xFFFFFFFF
_MAX_SCOPE = 64
_MAX_STATES = 1024
_MAX_TRY = 256
_MAX_CATCH = 64


@dataclass
class SehFrame:
    kind: str                     # "eh3" or "cxx"
    anchor_after: int             # instruction after which the frame is live
    table: int                    # scope table or FuncInfo VA
    funclets: set = field(default_factory=set)   # called by the handler
    resume: list = field(default_factory=list)   # jumped to, in order
    outside: set = field(default_factory=set)    # targets past [start, end)

    @property
    def targets(self):
        return set(self.funclets) | set(self.resume)


def _imm(op):
    return None if op is None or op.type != "imm" else op.imm & 0xFFFFFFFF


def _is_fs0(op):
    return (op.type == "mem" and op.mem_seg == "fs" and not op.mem_base
            and not op.mem_index and (op.mem_disp or 0) == 0)


def _is_ebp_minus4(op):
    return (op.type == "mem" and op.mem_base == "ebp" and not op.mem_index
            and op.mem_disp == -4 and not op.mem_seg)


_UNKNOWN = object()
_REG32 = {"eax": "eax", "ax": "eax", "al": "eax", "ah": "eax",
          "ebx": "ebx", "bx": "ebx", "bl": "ebx", "bh": "ebx",
          "ecx": "ecx", "cx": "ecx", "cl": "ecx", "ch": "ecx",
          "edx": "edx", "dx": "edx", "dl": "edx", "dh": "edx",
          "esi": "esi", "si": "esi", "edi": "edi", "di": "edi",
          "ebp": "ebp", "bp": "ebp", "esp": "esp", "sp": "esp"}


def _try_level_stores(instructions):
    """The try levels the function stores at [ebp-4], in order; _UNKNOWN for
    a store whose value a linear read cannot tell.

    MSVC enters a level three ways: `mov dword ptr [ebp-4], imm`, `and dword
    ptr [ebp-4], 0` (level 0, three bytes shorter), and `mov [ebp-4], reg`
    with a register it zeroed or loaded before (`xor ebx, ebx`). Reading
    only the first left the table count at _MAX_SCOPE for a function that
    enters level 0 with `and`, and the scope table of the function linked
    after it -- its entries start at enclosing level -1 too -- was read as
    this one's: its handlers came out as "targets outside" this function.
    """
    regs = {}
    out = []
    for insn in instructions:
        m, ops = insn.mnemonic, insn.operands
        if len(ops) == 2 and _is_ebp_minus4(ops[0]):
            src = ops[1]
            if m == "mov":
                if src.type == "imm":
                    out.append(src.imm & 0xFFFFFFFF)
                elif src.type == "reg":
                    out.append(regs.get(src.reg, _UNKNOWN))
                else:
                    out.append(_UNKNOWN)
            elif m == "and" and src.type == "imm" and src.imm & 0xFFFFFFFF == 0:
                out.append(0)
            elif m == "or" and src.type == "imm" and src.imm & 0xFFFFFFFF == _MINUS_ONE:
                out.append(_MINUS_ONE)
            elif m in ("inc", "dec", "add", "sub", "xor", "and", "or"):
                out.append(_UNKNOWN)
            continue
        if len(ops) == 1 and m in ("inc", "dec") and _is_ebp_minus4(ops[0]):
            out.append(_UNKNOWN)
            continue
        # A register's value, read straight down (no CFG): only the three
        # shapes a compiler loads a try level with; any other write forgets.
        if m == "call":
            for r in ("eax", "ecx", "edx"):
                regs.pop(r, None)
            continue
        if not ops or ops[0].type != "reg":
            continue
        dst = ops[0].reg
        full = _REG32.get(dst)
        if full is None:
            continue
        if (m in ("xor", "sub") and len(ops) == 2 and ops[1].type == "reg"
                and ops[1].reg == dst and dst == full):
            regs[full] = 0
        elif (m == "mov" and len(ops) == 2 and ops[1].type == "imm"
              and dst == full):
            regs[full] = ops[1].imm & 0xFFFFFFFF
        elif m not in ("push", "cmp", "test") and not m.startswith("j"):
            regs.pop(full, None)
    return out


def _registration(instructions, seh_prologs):
    """(anchor index, pushed immediates nearest-last, via_prolog) or None."""
    for i, insn in enumerate(instructions):
        ops = insn.operands
        if (insn.mnemonic == "call" and insn.call_target in seh_prologs
                and i >= 1):
            pushes = [_imm(p.operands[0]) for p in instructions[max(0, i - 2):i]
                      if p.mnemonic == "push" and p.operands]
            return i, [p for p in pushes if p is not None], True
        if (insn.mnemonic == "mov" and len(ops) == 2 and _is_fs0(ops[0])
                and ops[1].type == "reg" and ops[1].reg == "esp"):
            pushes = []
            for p in instructions[max(0, i - 8):i]:
                if p.mnemonic == "push" and p.operands:
                    v = _imm(p.operands[0])
                    if v is not None:
                        pushes.append(v)
            if _MINUS_ONE in pushes:
                last = len(pushes) - 1 - pushes[::-1].index(_MINUS_ONE)
                pushes = pushes[last + 1:]
            return i, pushes, False
    return None


def analyze_seh_frame(instructions, start, end, read_u32, seh_prologs=(),
                      is_code=None):
    """The exception frame `instructions` (a linear decode of [start, end))
    registers, or None. read_u32(va) returns a guest dword or None when the
    address is not in the image; is_code(va) says whether va is code."""
    seh_prologs = frozenset(seh_prologs or ())
    found = _registration(instructions, seh_prologs)
    if not found:
        return None
    at, pushes, via_prolog = found
    if is_code is None:
        def is_code(va):
            return start <= va < end

    if via_prolog:
        if not pushes:
            return None
        frame = SehFrame("eh3", instructions[at].address, pushes[-1])
        _read_scope_table(frame, instructions, read_u32, is_code)
    else:
        if not pushes:
            return None
        handler = pushes[-1]
        funcinfo = _cxx_funcinfo(handler, read_u32)
        if funcinfo is not None:
            frame = SehFrame("cxx", instructions[at].address, funcinfo)
            _read_funcinfo(frame, instructions, read_u32, is_code, start, end)
        elif len(pushes) >= 2:
            frame = SehFrame("eh3", instructions[at].address, pushes[-2])
            _read_scope_table(frame, instructions, read_u32, is_code)
        else:
            return None

    frame.outside = {t for t in frame.targets if not (start < t < end)}
    frame.funclets -= frame.outside
    frame.resume = [t for t in frame.resume if t not in frame.outside]
    return frame


def _read_scope_table(frame, instructions, read_u32, is_code):
    stores = _try_level_stores(instructions)
    levels = [v for v in stores
              if v is not _UNKNOWN and v != _MINUS_ONE and v < _MAX_SCOPE]
    # A store the read cannot tell counts for nothing, as before it did.
    count = (max(levels) + 1) if levels else _MAX_SCOPE
    for i in range(count):
        base = frame.table + 12 * i
        enclosing, filt, handler = (read_u32(base), read_u32(base + 4),
                                    read_u32(base + 8))
        if None in (enclosing, filt, handler):
            break
        if enclosing != _MINUS_ONE and enclosing >= i:
            break
        if not is_code(handler) or (filt and not is_code(filt)):
            break
        if filt:
            frame.funclets.add(filt)
            if handler not in frame.resume:
                frame.resume.append(handler)
        else:
            frame.funclets.add(handler)


def _cxx_funcinfo(handler, read_u32):
    """FuncInfo VA behind an __ehhandler stub (mov eax, imm32; jmp rel)."""
    b0 = read_u32(handler)
    if b0 is None or (b0 & 0xFF) != 0xB8:
        return None
    b4 = read_u32(handler + 4)
    if b4 is None or ((b4 >> 8) & 0xFF) not in (0xE9, 0xEB):
        return None
    info = ((b0 >> 8) | ((b4 & 0xFF) << 24)) & 0xFFFFFFFF
    magic = read_u32(info)
    return info if magic in _EH_MAGICS else None


def _read_funcinfo(frame, instructions, read_u32, is_code, start, end):
    info = frame.table
    max_state = read_u32(info + 4) or 0
    unwind = read_u32(info + 8) or 0
    n_try = read_u32(info + 12) or 0
    tries = read_u32(info + 16) or 0
    if max_state <= _MAX_STATES and unwind:
        for s in range(max_state):
            action = read_u32(unwind + 8 * s + 4)
            if action and is_code(action):
                frame.funclets.add(action)
    catches = []
    if n_try <= _MAX_TRY and tries:
        for t in range(n_try):
            entry = tries + 20 * t
            n_catch = read_u32(entry + 12) or 0
            handlers = read_u32(entry + 16) or 0
            if n_catch > _MAX_CATCH or not handlers:
                continue
            for c in range(n_catch):
                addr = read_u32(handlers + 16 * c + 12)
                if addr and is_code(addr):
                    frame.funclets.add(addr)
                    catches.append(addr)
    # A catch funclet hands its continuation back in eax.
    by_addr = {insn.address: k for k, insn in enumerate(instructions)}
    for h in catches:
        k = by_addr.get(h)
        while k is not None and k < len(instructions):
            insn = instructions[k]
            ops = insn.operands
            if (insn.mnemonic == "mov" and len(ops) == 2
                    and ops[0].type == "reg" and ops[0].reg == "eax"):
                v = _imm(ops[1])
                if v is not None and start < v < end and v not in frame.resume:
                    frame.resume.append(v)
            if insn.is_ret or insn.mnemonic == "jmp":
                break
            k += 1
