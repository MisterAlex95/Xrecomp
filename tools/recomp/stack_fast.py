"""Stack slots without volatile and without the g_cpu barrier (XBA-307).

Every guest access of the lifted code is volatile and goes through
recomp_guest_base(), whose asm makes every dirty guest register reach g_cpu
first, so that a host fault becomes an exact guest exception (XBA-138). A
stack slot needs neither: it is not MMIO, and in a function whose stack
addresses never leave it as data nothing but that function can reach it.
This module picks those functions and rewrites their stack accesses to the
STK forms of recomp_types.h (plain recomp_gm_u32 words, may_alias bytes and
halfwords):

  * `eligible(instructions)`: the function keeps its stack to itself. Esp (and
    ebp in a function whose prologue is `push ebp; mov ebp, esp`) appear only
    as the base or index of a memory operand, in push/pop/call/ret/leave, as
    the destination of add/sub/and, in the prologue and in `mov esp, ebp`.
    Anything that reads one of them as a value (lea of a stack slot, mov
    reg, esp, push esp, fs:[0] = esp, pushad) or loads esp from elsewhere (a
    stack switch, XBA-101) leaves the function as it was: a pointer into its
    frame would be dereferenced through typed MEM accesses, which the
    compiler may reorder against the STK words of the same bytes. A frame
    function only counts as one in a body that is one guest function
    (not coalesced or recovered) and that leaves [ebp] alone once its frame
    is torn down.
  * `rewrite(lines, ebp_frame)`: MEMn/SMEMn/MEMF/MEMD/XMM_MEM/XMM_STORE whose
    address is esp or frame ebp plus registers and constants, PUSH32/POP32
    on esp. Writes of the word-composed forms (float, double, signed 32 and
    64) become STKx_W(addr, value). A stack access left in a form that is not
    a word (a cast in the address, a compound assignment on a float), or esp
    / frame ebp handed to any helper not on an allow-list (an x87
    environment helper reads it through MEM16), gives the function up: it
    stays as it was.
  * `fence(lines)`: RECOMP_STK_FENCE() at the entry and before every return
    of every function, fast or not (before, not after, a tail call), so that
    an inlined callee's accesses stay on their side of the call
    (recomp_types.h).

XBA_RECOMP_STK=0 in the environment turns all of it off (the old text).
"""

import os
import re

STACK_REGS = ("esp", "sp", "spl")
FRAME_REGS = ("ebp", "bp", "bpl")

# Where a stack address may go once the rewrite is done: the STK forms, a
# MEM32 the rewrite could not read (MEM32 is STK32's type), the helpers made
# of MEM32 words or memcpy, and XBOX_PTR (atomics, which order everything;
# char pointers). Anything else handed esp or the frame's ebp is a helper the
# rewrite cannot see into (recomp_fnsave and the like access it through MEM16
# and MEM8): the function gives up.
_ALLOWED_CALLS = frozenset({
    "STK32", "STK16", "STK8", "STKS16", "STKS8", "STKS32", "STKS32_W", "STKS64", "STKS64_W",
    "STKF", "STKF_W", "STKD", "STKD_W", "STK_PUSH32", "STK_POP32", "STK_XMM", "STK_XMM_STORE",
    "MEM32", "XMM_LOAD_LOW", "XMM_LOAD_HIGH", "XMM_STORE_LOW", "XMM_STORE_HIGH",
    "MMX_MEM", "MMX_STORE", "XBOX_PTR",
})
_NOT_CALLS = frozenset({"if", "while", "for", "switch", "return", "sizeof"})
_COMMENT = re.compile(r"/\*.*?\*/")
_CALL_OPEN = re.compile(r"\b([A-Za-z_]\w*)\s*\(")


def enabled():
    return os.environ.get("XBA_RECOMP_STK", "1") not in ("0", "off", "no")


def has_frame_prologue(instructions):
    return (len(instructions) >= 2
            and instructions[0].mnemonic == "push"
            and instructions[0].op_str.strip() == "ebp"
            and instructions[1].mnemonic == "mov"
            and instructions[1].op_str.replace(" ", "") == "ebp,esp")


def _uses_reg_as_base(insn, regs):
    return any(op.type == "mem" and (op.mem_base in regs or op.mem_index in regs)
               for op in (getattr(insn, "operands", None) or ()))


def eligible(instructions, single_body=True):
    """(ok, ebp_frame, reason). single_body: the C function is one guest
    function entered at its start (not a coalesced or recovered body, where
    a part could use ebp as a plain register after another part's prologue).
    A frame function that is not, or that touches [ebp] after a `pop ebp` /
    `leave` before its next ret or jmp, is left as it was: its [ebp+N] could
    be anything, MMIO included, and as MEM accesses they would meet the STK
    words of its own frame."""
    ebp_frame = has_frame_prologue(instructions)
    if ebp_frame and not single_body:
        return False, ebp_frame, "frame in a coalesced or recovered body"
    if ebp_frame:
        torn = None
        for insn in instructions[2:]:
            if insn.mnemonic == "leave" or (insn.mnemonic == "pop" and insn.op_str.strip() == "ebp"):
                torn = insn
            elif torn is not None and _uses_reg_as_base(insn, FRAME_REGS):
                return False, ebp_frame, (f"[ebp] at 0x{insn.address:08X} after the frame's"
                                          f" teardown at 0x{torn.address:08X}")
            if insn.is_terminator:
                torn = None
    stackish = STACK_REGS + (FRAME_REGS if ebp_frame else ())
    for k, insn in enumerate(instructions):
        m = insn.mnemonic
        ops = list(getattr(insn, "operands", None) or ())
        if m in ("pushal", "pushad", "pusha", "popal", "popad", "popa"):
            return False, ebp_frame, f"{m} at 0x{insn.address:08X}"
        if m in ("enter", "leave") and not ebp_frame:
            return False, ebp_frame, f"{m} without a frame at 0x{insn.address:08X}"
        if m == "lea":
            mem = ops[1] if len(ops) > 1 else None
            if mem is not None and mem.type == "mem" and (
                    mem.mem_base in stackish or mem.mem_index in stackish):
                dst = ops[0]
                if not (dst.type == "reg" and dst.reg == "esp"):
                    return False, ebp_frame, f"lea of a stack slot at 0x{insn.address:08X}"
            elif ops and ops[0].type == "reg" and ops[0].reg in stackish:
                return False, ebp_frame, f"lea into {ops[0].reg} at 0x{insn.address:08X}"
            continue
        for i, op in enumerate(ops):
            if op.type != "reg" or op.reg not in stackish:
                continue
            reg = op.reg
            src = ops[1] if len(ops) > 1 else None
            ok = False
            if reg == "esp" and i == 0 and m in ("add", "sub", "and") and src is not None \
                    and (src.type == "imm" or (src.type == "reg" and src.reg not in stackish)):
                ok = True
            elif m in ("cmp", "test"):
                ok = True
            elif ebp_frame and k == 0 and m == "push" and reg == "ebp":
                ok = True
            elif ebp_frame and k == 1 and m == "mov":
                ok = True                       # mov ebp, esp
            elif ebp_frame and m == "pop" and reg == "ebp":
                ok = True
            elif ebp_frame and m == "mov" and len(ops) == 2 and ops[0].type == "reg" \
                    and ops[0].reg == "esp" and src.type == "reg" and src.reg == "ebp":
                ok = True                       # mov esp, ebp
            if not ok:
                return False, ebp_frame, f"{m} {insn.op_str} at 0x{insn.address:08X}"
    return True, ebp_frame, ""


# ── text rewrite ────────────────────────────────────────────────

_ACCESS = re.compile(
    r"\b(PUSH32|POP32|MEM32|MEM16|MEM8|SMEM32|SMEM16|SMEM8|SMEM64|MEMF|MEMD|XMM_MEM|XMM_STORE)\(")

# Lvalue-capable STK forms, and the word-composed ones that need a _W write.
_LVALUE = {"MEM32": "STK32", "MEM16": "STK16", "MEM8": "STK8",
           "SMEM16": "STKS16", "SMEM8": "STKS8"}
_COMPOSED = {"SMEM32": "STKS32", "SMEM64": "STKS64", "MEMF": "STKF", "MEMD": "STKD"}
_CALLS = {"XMM_MEM": "STK_XMM", "XMM_STORE": "STK_XMM_STORE",
          "PUSH32": "STK_PUSH32", "POP32": "STK_POP32"}
_NOT_WORD = ("MEM16", "MEM8", "SMEM16", "SMEM8", "SMEM32", "SMEM64", "MEMF", "MEMD")

_TOKEN = re.compile(r"\s*(?:(0[xX][0-9A-Fa-f]+[uU]?|\d+[uU]?)|([A-Za-z_]\w*)|(.))")
_GPRS = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"}


class GiveUp(Exception):
    pass


def classify_address(addr, ebp_frame):
    """'stack' (esp/frame ebp + registers and constants), 'other' (no stack
    register in it), or 'unknown' (a stack register in something else)."""
    stack = {"esp"} | ({"ebp"} if ebp_frame else set())
    toks = []
    pos = 0
    while pos < len(addr):
        mt = _TOKEN.match(addr, pos)
        if not mt or mt.end() == pos:
            break
        pos = mt.end()
        if mt.group(1):
            toks.append(("n", mt.group(1)))
        elif mt.group(2):
            toks.append(("id", mt.group(2)))
        elif mt.group(3) and not mt.group(3).isspace():
            toks.append(("op", mt.group(3)))
    names = [t for k, t in toks if k == "id"]
    if not any(n in stack for n in names):
        return "other"
    if any(n not in _GPRS for n in names):
        return "unknown"
    if any(k == "op" and t not in "+-*()" for k, t in toks):
        return "unknown"
    hits = [i for i, (k, t) in enumerate(toks) if k == "id" and t in stack]
    if len(hits) != 1:
        return "unknown"
    i = hits[0]
    prev = toks[i - 1][1] if i > 0 else None
    nxt = toks[i + 1][1] if i + 1 < len(toks) else None
    if prev in ("-", "*") or nxt == "*":
        return "unknown"
    # A subtraction anywhere before it at paren depth 0 of its own group
    # would negate it: `x - (esp + 4)`. Not something _fmt_mem writes.
    depth = 0
    for k, t in toks[:i]:
        if t == "(":
            depth += 1
        elif t == ")":
            depth -= 1
        elif t == "-" and depth >= 0:
            # `esp - 0x10` has its minus after esp; one before it is suspect.
            return "unknown"
    return "stack"


def _close(text, open_at):
    """Index of the parenthesis closing the one at open_at."""
    depth = 0
    for j in range(open_at, len(text)):
        c = text[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j
    raise GiveUp("unbalanced parentheses")


def _split_first_arg(args):
    depth = 0
    for j, c in enumerate(args):
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "," and depth == 0:
            return args[:j], args[j + 1:]
    return args, None


def _rhs_end(text, start):
    """End of an assignment's right-hand side starting at start: the `;`,
    `,` or unmatched `)` that ends it at depth 0."""
    depth = 0
    for j in range(start, len(text)):
        c = text[j]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return j
            depth -= 1
        elif c in ";," and depth == 0:
            return j
    raise GiveUp("assignment without an end")


_ASSIGN = re.compile(r"\s*=(?!=)")
_COMPOUND = re.compile(r"\s*(?:[-+*/%&|^]=|<<=|>>=|\+\+|--)")


def rewrite_text(text, ebp_frame):
    """The text with its stack accesses in STK forms; GiveUp when a stack
    access would be left in a typed form."""
    out = []
    pos = 0
    while True:
        m = _ACCESS.search(text, pos)
        if not m:
            out.append(text[pos:])
            break
        name = m.group(1)
        open_at = m.end() - 1
        close = _close(text, open_at)
        args = text[open_at + 1:close]
        first, rest = _split_first_arg(args)
        first_rw = rewrite_text(first, ebp_frame)
        rest_rw = rewrite_text(rest, ebp_frame) if rest is not None else None
        out.append(text[pos:m.start()])
        after = close + 1
        if name in ("PUSH32", "POP32"):
            kind = "stack" if first.strip() == "esp" else (
                "other" if classify_address(first, ebp_frame) == "other" else "unknown")
        else:
            kind = classify_address(first, ebp_frame)
        if kind == "unknown" and name in _NOT_WORD + ("PUSH32", "POP32"):
            raise GiveUp(f"{name}({first}) is a stack access in a typed form")
        if kind != "stack":
            out.append(f"{name}({first_rw}" + (f",{rest_rw}" if rest_rw is not None else "") + ")")
            pos = after
            continue
        before = "".join(out).rstrip()
        if before.endswith("&") and not before.endswith("&&") and \
                (len(before) == 1 or before[:-1].rstrip()[-1:] in "(,=;{}!~+-*/%<>?:|^&"):
            raise GiveUp(f"address of {name}({first})")
        if name in _CALLS:
            out.append(f"{_CALLS[name]}({first_rw}" + (f",{rest_rw}" if rest_rw is not None else "") + ")")
            pos = after
            continue
        if name in _LVALUE:
            out.append(f"{_LVALUE[name]}({first_rw})")
            pos = after
            continue
        # Word-composed: a read, or a plain assignment turned into a call.
        stk = _COMPOSED[name]
        if _COMPOUND.match(text, after) or re.search(r"(\+\+|--)\s*$", before):
            raise GiveUp(f"compound assignment to {name}({first})")
        ma = _ASSIGN.match(text, after)
        if ma:
            end = _rhs_end(text, ma.end())
            rhs = rewrite_text(text[ma.end():end], ebp_frame).strip()
            out.append(f"{stk}_W({first_rw}, {rhs})")
            pos = end
            continue
        out.append(f"{stk}({first_rw})")
        pos = after
    return "".join(out)


def check_stack_uses(text, ebp_frame):
    """GiveUp when esp (or the frame's ebp) is an argument of a call or a
    macro not in _ALLOWED_CALLS. Casts and groupings are looked through."""
    text = _COMMENT.sub(" ", text)
    regs = r"\b(?:esp|ebp)\b" if ebp_frame else r"\besp\b"
    if not re.search(regs, text):
        return
    chars = list(text)                  # the allowed calls blanked, arguments included
    for m in _CALL_OPEN.finditer(text):
        if m.group(1) in _ALLOWED_CALLS:
            for j in range(m.start(), _close(text, m.end() - 1) + 1):
                chars[j] = " "
    text = "".join(chars)
    for m in re.finditer(regs, text):
        depth = 0
        for j in range(m.start() - 1, -1, -1):
            c = text[j]
            if c == ")":
                depth += 1
            elif c == "(" and depth:
                depth -= 1
            elif c == "(":
                callee = re.search(r"([A-Za-z_]\w*)\s*$", text[:j])
                if callee and callee.group(1) not in _NOT_CALLS:
                    raise GiveUp(f"{m.group(0)} passed to {callee.group(1)}()")


def rewrite(lines, ebp_frame):
    """(lines, None) rewritten, or (lines unchanged, reason)."""
    new = []
    try:
        for line in lines:
            s = line.lstrip()
            if s.startswith(("#", "/*", "*")):
                new.append(line)
                continue
            if _ACCESS.search(line):
                line = rewrite_text(line, ebp_frame)
            check_stack_uses(line, ebp_frame)
            new.append(line)
    except GiveUp as e:
        return lines, str(e)
    return new, None


_RETURN = re.compile(r"(\b([A-Za-z_]\w*)\(([^;]*)\);\s*)?return;")


def fence(body):
    """RECOMP_STK_FENCE() before every `return;` of a function body (the
    lines after its opening brace). Before, not after, a tail call
    (`name(); return;`, `RECOMP_ITAIL...(...); return;`): a fence between
    the call and the return would turn the host's sibling call into a call
    that grows the host stack."""
    def one(m):
        callee, args = m.group(2), m.group(3)
        before = m.string[:m.start()].rstrip()
        lone = bool(before) and before[-1] not in ";{}:"    # `if (c) ...` / `else ...`
        if callee and ((not args and not callee.isupper())
                       or callee.startswith("RECOMP_ITAIL")):
            if not lone:
                return "RECOMP_STK_FENCE(); " + m.group(0)
            # `if (c) name(); return;`: the call is the if's; fence the return.
        if m.group(1):
            return m.group(1) + "RECOMP_STK_FENCE(); return;"
        if lone:
            return "{ RECOMP_STK_FENCE(); return; }"
        return "RECOMP_STK_FENCE(); return;"
    return [_RETURN.sub(one, line)
            if "return;" in line and not line.lstrip().startswith(("/*", "*", "#"))
            else line for line in body]


# For test fixtures that compile lifted functions against a few macros of
# their own instead of recomp_types.h: the STK forms as the MEM forms.
FIXTURE_FALLBACK = r"""
#ifndef RECOMP_STK_FENCE
#define RECOMP_STK_FENCE()  ((void)0)
#define STK32(a)            MEM32(a)
#define STK16(a)            MEM16(a)
#define STK8(a)             MEM8(a)
#define STKS16(a)           SMEM16(a)
#define STKS8(a)            SMEM8(a)
#define STKS32(a)           SMEM32(a)
#define STKS32_W(a, v)      (SMEM32(a) = (v))
#define STKS64(a)           SMEM64(a)
#define STKS64_W(a, v)      (SMEM64(a) = (v))
#define STKF(a)             MEMF(a)
#define STKF_W(a, v)        (MEMF(a) = (v))
#define STKD(a)             MEMD(a)
#define STKD_W(a, v)        (MEMD(a) = (v))
#define STK_PUSH32(sp, v)   PUSH32(sp, v)
#define STK_POP32(sp, d)    POP32(sp, d)
#define STK_XMM(a)          XMM_MEM(a)
#define STK_XMM_STORE(a, v) XMM_STORE((a), (v))
#endif
"""
