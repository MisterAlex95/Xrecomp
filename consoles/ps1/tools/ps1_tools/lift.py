"""MIPS I to C (XBA-161): one C function per discovered function, in the shape of ps1_recomp_rt.h.

The function is cut into blocks, each entered from the entry switch at its first instruction (a resume point), with
one event check per segment (« blocks and event checks » below): lifted code stops where the interpreter would
(scheduler event, interrupt) and the interpreter runs the rest of that block. Delay slots run inline after their
branch (and are emitted on their own too only when a branch of the function targets them). Semantics are the
interpreter's (ps1_cpu.c), instruction for instruction:
- the fetch cost of every instruction (RC_I), LW-class loads' cycles, GTE commands' cycles, at the same point;
- load delay slots: the value is committed at once when the next instruction cannot tell (isa.conflicts), else the
  function stops with the load in flight (RC_EXIT_LD) and the interpreter runs that instruction;
- branch delay slots inline; a branch in a delay slot, exceptions (overflow, address / bus errors, SYSCALL, BREAK,
  reserved, coprocessor unusable) and COP0 go to the interpreter (RC_INTERP: nothing of the instruction done);
- calls, returns, indirect jumps and jumps out of the function return to the dispatcher (bounded C stack);
- GTE commands and transfers call the core (ps1_rc_gte_*), IO goes through the core's bus functions.
"""

import re

from .isa import conflicts

CPI_RAM, CPI_RAM_UNCACHED = 1, 5


def hx(v):
    return "0x%08Xu" % (v & 0xFFFFFFFF)


def R(r):
    return "0u" if r == 0 else "r%d" % r


class FnLifter:
    def __init__(self, func, name, resume_all=False):
        self.resume_all = resume_all  # every instruction a block of its own (debugging)
        self.labels = 0
        self.f = func
        self.img = func.image
        self.name = name
        self.fallbacks = 0
        self.out = []
        self.targets = set()  # branch / jump targets inside the function
        for a in func.addrs:
            t = self.img.ins(a).target
            if t is not None and func.start <= t < func.end:
                self.targets.add(t)
        self.io = io_accesses(func)  # accesses whose address is a known IO register (lui / addiu / ori constants)

    def inside(self, a):
        return self.f.start <= a < self.f.end

    def emit(self, s):
        self.out.append(s)

    # ---- pieces ----
    def go(self, t):
        """Continue at static address t."""
        return "goto L_%08X;" % t if self.inside(t) else "RC_EXIT(%s);" % hx(t)

    def commit(self, rt, val, nxt, static_next):
        """Code that lands a delayed load of val into rt before the instruction at nxt runs (static_next: nxt's
        instruction is known: it is in this function)."""
        if rt == 0:
            return ""
        if static_next and self.inside(nxt) and not conflicts(self.img.ins(nxt), rt):
            return "%s = %s;" % (R(rt), val)
        return "RC_EXIT_LD(%s, %du, %s);" % (hx(nxt), rt, val)

    def body(self, i, fault, hand=None):
        """C for instruction i (not a transfer). fault: the statement that hands it to the interpreter; hand: the same
        with the rest of its block (« memory accesses » below). Returns (code, delayed, memory) where delayed is the
        register of a delayed load (value in ldv) or None, and memory says whether it accesses memory."""
        n, rs, rt, rd = i.name, R(i.rs), R(i.rt), R(i.rd)
        simm = i.simm & 0xFFFFFFFF
        hand = hand or fault
        if i.addr not in self.io and (i.cls in ("load", "store") or n in ("lwc2", "swc2")) and n not in ("swl", "swr"):
            return self.ram_access(i, hand)

        def w(reg, expr):
            return "" if reg == 0 else "%s = %s;" % (R(reg), expr)

        addr = "%s + %s" % (rs, hx(simm)) if simm else rs
        c = i.cls
        if c == "interp":
            return fault, None, False
        if c == "alu":
            if (i.word >> 26) == 0:
                ex = {
                    "sll": "%s << %d" % (rt, i.sa), "srl": "%s >> %d" % (rt, i.sa),
                    "sra": "(uint32_t)((int32_t)%s >> %d)" % (rt, i.sa),
                    "sllv": "%s << (%s & 31)" % (rt, rs), "srlv": "%s >> (%s & 31)" % (rt, rs),
                    "srav": "(uint32_t)((int32_t)%s >> (%s & 31))" % (rt, rs),
                    "mfhi": "hi", "mflo": "lo",
                    "addu": "%s + %s" % (rs, rt), "subu": "%s - %s" % (rs, rt),
                    "and": "%s & %s" % (rs, rt), "or": "%s | %s" % (rs, rt), "xor": "%s ^ %s" % (rs, rt),
                    "nor": "~(%s | %s)" % (rs, rt),
                    "slt": "(uint32_t)((int32_t)%s < (int32_t)%s)" % (rs, rt),
                    "sltu": "(uint32_t)(%s < %s)" % (rs, rt),
                }[n]
                return w(i.rd, ex), None, False
            ex = {
                "addiu": "%s + %s" % (rs, hx(simm)),
                "slti": "(uint32_t)((int32_t)%s < %d)" % (rs, i.simm),
                "sltiu": "(uint32_t)(%s < %s)" % (rs, hx(simm)),
                "andi": "%s & 0x%Xu" % (rs, i.imm), "ori": "%s | 0x%Xu" % (rs, i.imm),
                "xori": "%s ^ 0x%Xu" % (rs, i.imm), "lui": hx(i.imm << 16),
            }[n]
            return w(i.rt, ex), None, False
        if c == "trap":
            if n == "addi":
                a, b, d, ovf = rs, hx(simm), i.rt, "~(ea ^ eb) & (ea ^ er)"
            else:
                a, b, d = rs, rt, i.rd
                ovf = "~(ea ^ eb) & (ea ^ er)" if n == "add" else "(ea ^ eb) & (ea ^ er)"
            op = "-" if n == "sub" else "+"
            return ("{ uint32_t ea = %s, eb = %s, er = ea %s eb; if ((%s) & 0x80000000u) { %s } %s }"
                    % (a, b, op, ovf, fault, w(d, "er"))), None, False
        if c == "muldiv":
            code = {
                "mult": "{ int64_t ep = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s; lo = (uint32_t)ep; "
                        "hi = (uint32_t)((uint64_t)ep >> 32); }" % (rs, rt),
                "multu": "{ uint64_t ep = (uint64_t)%s * (uint64_t)%s; lo = (uint32_t)ep; hi = (uint32_t)(ep >> 32); }"
                         % (rs, rt),
                "div": "rc_div(%s, %s, &hi, &lo);" % (rs, rt),
                "divu": "rc_divu(%s, %s, &hi, &lo);" % (rs, rt),
                "mthi": "hi = %s;" % rs, "mtlo": "lo = %s;" % rs,
            }[n]
            return code, None, False
        if c == "load":
            if n == "lw":
                return "RC_LW(%s, 3u, ldv, %s);" % (addr, fault), i.rt, True
            if n in ("lwl", "lwr"):
                return ("{ uint32_t ea = %s, ew = 0; RC_LW(ea & ~3u, 0u, ew, %s); ldv = rc_%s(%s, ew, ea); }"
                        % (addr, fault, n, rt), i.rt, True)
            t, al = {"lb": ("int8_t", 0), "lbu": ("uint8_t", 0), "lh": ("int16_t", 1), "lhu": ("uint16_t", 1)}[n]
            return "RC_LD(%s, %s, %du, ldv, %s);" % (t, addr, al, fault), i.rt, True
        if c == "store":
            if n in ("swl", "swr"):
                return "RC_SWLR(%s, %s, %d, %s);" % (addr, rt, 1 if n == "swr" else 0, fault), None, True
            t, al = {"sb": ("uint8_t", 0), "sh": ("uint16_t", 1), "sw": ("uint32_t", 3)}[n]
            return "RC_ST(%s, %s, %du, %s, %s);" % (t, addr, al, rt, fault), None, True
        if c == "cop2":
            cu = "if (!RC_CU2()) { %s } " % fault
            if n == "cop2":
                return cu + "now += (uint32_t)ps1_rc_gte_cmd(RC_M, 0x%07Xu);" % (i.word & 0x1FFFFFF), None, False
            if n == "mfc2":
                return cu + "ldv = ps1_rc_gte_read(RC_M, %d);" % i.rd, i.rt, False
            if n == "cfc2":
                return cu + "ldv = ps1_rc_gte_read(RC_M, %d);" % (32 + i.rd), i.rt, False
            if n == "mtc2":
                return cu + "ps1_rc_gte_write(RC_M, %d, %s);" % (i.rd, rt), None, False
            if n == "ctc2":
                return cu + "ps1_rc_gte_write(RC_M, %d, %s);" % (32 + i.rd, rt), None, False
            if n == "lwc2":
                return (cu + "{ uint32_t ew = 0; RC_LW(%s, 3u, ew, %s); ps1_rc_gte_write(RC_M, %d, ew); }"
                        % (addr, fault, i.rt)), None, True
            if n == "swc2":
                return (cu + "RC_ST(uint32_t, %s, 3u, ps1_rc_gte_read(RC_M, %d), %s);" % (addr, i.rt, fault),
                        None, True)
        raise ValueError("cannot lift %s" % i.text())

    def ram_access(self, i, hand):
        """A load or store whose address is not a known IO register: main RAM and the scratchpad inline, anything
        else (IO, a fault, a store into this function's own code) handed to the interpreter, nothing done."""
        n, rt = i.name, R(i.rt)
        simm = i.simm & 0xFFFFFFFF
        addr = "%s + %s" % (R(i.rs), hx(simm)) if simm else R(i.rs)
        if n == "lw":
            return "RC_LW_RAM(%s, 3u, ldv, %s);" % (addr, hand), i.rt, True
        if n in ("lwl", "lwr"):
            return ("{ uint32_t ea = %s, ew = 0; RC_LW_RAM(ea & ~3u, 0u, ew, %s); ldv = rc_%s(%s, ew, ea); }"
                    % (addr, hand, n, rt), i.rt, True)
        if n in ("lb", "lbu", "lh", "lhu"):
            t, al = {"lb": ("int8_t", 0), "lbu": ("uint8_t", 0), "lh": ("int16_t", 1), "lhu": ("uint16_t", 1)}[n]
            return "RC_LD_RAM(%s, %s, %du, ldv, %s);" % (t, addr, al, hand), i.rt, True
        if n in ("sb", "sh", "sw"):
            t, al = {"sb": ("uint8_t", 0), "sh": ("uint16_t", 1), "sw": ("uint32_t", 3)}[n]
            return "RC_ST_RAM(%s, %s, %du, %s, %s);" % (t, addr, al, rt, hand), None, True
        cu = "if (!RC_CU2()) { %s } " % hand
        if n == "lwc2":
            return (cu + "{ uint32_t ew = 0; RC_LW_RAM(%s, 3u, ew, %s); ps1_rc_gte_write(RC_M, %d, ew); }"
                    % (addr, hand, i.rt)), None, True
        return cu + "RC_ST_RAM(uint32_t, %s, 3u, ps1_rc_gte_read(RC_M, %d), %s);" % (addr, i.rt, hand), None, True

    # ---- instructions ----
    def plain(self, a, left=1):
        """Instruction a (not a transfer), left instructions before the end of its block."""
        i = self.img.ins(a)
        fault = "RC_INTERP(%s);" % hx(a)
        code, ld, _ = self.body(i, fault, "RC_HAND(%s, %d);" % (hx(a), left))
        if i.cls == "interp":
            self.fallbacks += 1
        self.emit("    %s\n" % code)
        if ld:
            # the delayed load lands before the next instruction (in this function, sequentially)
            nxt = a + 4
            self.emit("    %s\n" % self.commit(ld, "ldv", nxt, self.inside(nxt)))

    def transfer(self, a):
        """A branch / jump at a with its delay slot inline. Returns True when the slot was consumed."""
        i = self.img.ins(a)
        s = a + 4
        if not self.inside(s) or self.img.ins(s).is_transfer():
            self.emit("    RC_INTERP(%s); /* no delay slot here / branch in a delay slot */\n" % hx(a))
            self.fallbacks += 1
            return False
        n = i.name
        rs, rt = R(i.rs), R(i.rt)
        static = i.target is not None
        tgt = hx(i.target) if static else "jt"
        # condition (before the link: BLTZAL / BGEZAL read rs first), then the link
        if i.cls == "branch":
            cond = {"beq": "%s == %s" % (rs, rt), "bne": "%s != %s" % (rs, rt), "blez": "(int32_t)%s <= 0" % rs,
                    "bgtz": "(int32_t)%s > 0" % rs}.get(n)
            if cond is None:
                cond = "(int32_t)%s >= 0" % rs if n.startswith("bgez") else "(int32_t)%s < 0" % rs
            self.emit("    tk = %s;\n" % cond)
        else:
            self.emit("    tk = 1;\n")
            if i.cls == "jr":
                self.emit("    jt = %s;\n" % rs)
        if i.link:
            self.emit("    %s = %s;\n" % (R(i.link), hx(a + 8)))
        if static:
            p = i.target & 0x1FFFFFFF
            if p in (0xA0, 0xB0, 0x30000):
                self.emit("    if (tk) h->hook_at = %s;\n" % tgt)
        else:
            self.emit("    RC_HOOK(jt);\n")
        npc = "tk ? %s : %s" % (tgt, hx(s + 4)) if i.cls == "branch" else tgt
        # the delay slot: its event check is its segment's (function())
        si = self.img.ins(s)
        self.emit("    RC_I();\n")
        fault = "RC_INTERP_SLOT(%s, %s, tk);" % (hx(s), npc)
        code, ld, _ = self.body(si, fault)
        if si.cls == "interp":
            self.fallbacks += 1
        self.emit("    %s\n" % code)
        uncond = i.cls != "branch"

        def path(dest_static, dest_expr):
            parts = []
            if ld:
                if dest_static is not None:
                    parts.append(self.commit(ld, "ldv", dest_static, self.inside(dest_static)))
                else:
                    parts.append("RC_EXIT_LD(%s, %du, ldv);" % (dest_expr, ld))
            if dest_static is not None:
                parts.append(self.go(dest_static))
            else:
                parts.append("RC_EXIT(%s);" % dest_expr)
            return " ".join(p for p in parts if p)

        taken = path(i.target if static else None, tgt)
        if uncond:
            self.emit("    %s\n" % taken)
        else:
            self.emit("    if (tk) { %s }\n" % taken)
            self.emit("    %s\n" % path(s + 4, hx(s + 4)))
        return True

    # ---- one function ----
    def function(self, comments=True):
        """The C function: blocks of instructions, each entered at its first instruction (a resume point), with one
        event check per segment."""
        f, img = self.f, self.img
        cpi = CPI_RAM_UNCACHED if (f.start >> 29) == 5 else CPI_RAM
        # which instructions are emitted: all but delay slots (they run inline after their branch) unless targeted
        emitted = []
        slot = set()
        for a in f.addrs:
            i = img.ins(a)
            if a not in slot or a in self.targets:
                emitted.append(a)
            if i.is_transfer() and self.inside(a + 4) and not img.ins(a + 4).is_transfer():
                slot.add(a + 4)
        # what each runs in sequence, itself and its inline slot: [(cycles at most, dynamic)]
        units = {}
        for a in emitted:
            i = img.ins(a)
            units[a] = [self.unit(i, cpi)]
            if i.is_transfer() and self.inside(a + 4) and not img.ins(a + 4).is_transfer():
                units[a].append(self.unit(img.ins(a + 4), cpi))
        leaders = self.block_leaders(emitted, units)
        self.labels = len(leaders)
        ends, end = {}, f.end  # instruction -> the address after its block
        for a in reversed(emitted):
            ends[a] = end
            if a in leaders:
                end = a
        # each instruction's C
        codes = {}
        for a in emitted:
            i = img.ins(a)
            self.out = []
            if i.is_transfer():
                self.transfer(a)
            else:
                self.plain(a, (ends[a] - a) // 4)
            codes[a] = self.out
        # the guest registers it writes (locals written back at the end) and the ones it only reads
        text = "".join("".join(c) for c in codes.values())
        written = set(WRITE_RE.findall(text)) | set(ADDR_RE.findall(text))
        read = set(USE_RE.findall(text)) - written
        regs = sorted(written | read, key=lambda r: (r[0] != "r", int(r[1:]) if r[0] == "r" else r))
        decl = ["    uint32_t %s;\n" % ", ".join("%s = %s" % (r, reg_field(r)) for r in regs if r in written),
                "    const uint32_t %s;\n" % ", ".join("%s = %s" % (r, reg_field(r)) for r in regs if r in read)]
        out = ["RC_FN %s(ps1_machine *m_)\n{\n" % self.name,
               "    RC_ENTER(%d, 0x%06Xu, %du);\n" % (cpi, f.start & 0x1FFFFF, f.end - f.start)]
        out += [d for d, s in zip(decl, (written, read)) if s]
        out.append("    switch (h->pc) {\n")
        out += ["    case %s: goto L_%08X;\n" % (hx(a), a) for a in emitted if a in leaders]
        # the other instructions (inside blocks): how many to their block's end, from a table
        tab = [0] * len(f.addrs)
        for a in emitted:
            if a not in leaders:
                tab[(a - f.start) // 4] = (ends[a] - a) // 4
        while tab and not tab[-1]:
            tab.pop()
        if tab:
            rows = [", ".join(str(v) for v in tab[k:k + 40]) for k in range(0, len(tab), 40)]
            out.append("    default: {\n        static const uint8_t mid_[%d] = {%s};\n"
                       % (len(tab), (",\n" + " " * 12).join(rows)))
            out.append("        RC_MID_TABLE(%s, mid_);\n        RC_NOT_COVERED();\n    }\n    }\n" % hx(f.start))
        else:
            out.append("    default: RC_NOT_COVERED();\n    }\n")
        # the blocks, an event check at the start of each segment
        seg_start = True
        for k, a in enumerate(emitted):
            i = img.ins(a)
            if a in leaders:
                out.append("L_%08X:%s\n" % (a, " /* %s */" % i.text() if comments else ""))
                seg_start = True
            elif comments:
                out.append("    /* %08X: %s */\n" % (a, i.text()))
            if seg_start:
                out.append("    RC_SEG(%s, %d, %d);\n" % (hx(a), segment_cycles(emitted, k, units, leaders),
                                                          (ends[a] - a) // 4))
            out.append("    RC_I();\n")
            out += codes[a]
            seg_start = units[a][-1][1]
        out.append("    RC_EXIT(%s);\n" % hx(f.end))
        out.append("    RC_EPILOGUE();\n")
        if written:
            out.append("    %s\n" % " ".join("%s = %s;" % (reg_field(r), r) for r in regs if r in written))
        out.append("}\n")
        return "".join(out)

    def unit(self, i, cpi):
        """(cycles at most, dynamic) of instruction i in a segment (« blocks and event checks » below)."""
        if is_memory(i) and (i.addr in self.io or i.name in ("swl", "swr")) or i.name == "cop2":
            return cpi, True
        return cpi + (3 if i.name in LW_CLASS else 0), False

    def block_leaders(self, emitted, units):
        """The resume points: the function's start, jump targets, the instruction after a transfer (return points,
        fall-through) and after one always handed to the interpreter (COP0, SYSCALL, ...); then one every MAX_BLOCK
        instructions. (After a load-use hazard, RC_EXIT_LD, the interpreter runs the rest of the block: compilers
        never emit one for the R3000, and data taken for code is full of them.)"""
        img = self.img
        em = set(emitted)
        lead = {self.f.start} | (self.targets & em)
        for k, a in enumerate(emitted[:-1]):
            i = img.ins(a)
            if i.is_transfer() or i.cls == "interp":
                lead.add(emitted[k + 1])
            # the not-taken path of a transfer with its slot inline goes to a + 8, even when the slot is also
            # emitted on its own (a jump target): a + 8 needs its label then too
            if i.is_transfer() and self.inside(a + 4) and not img.ins(a + 4).is_transfer() and a + 8 in em:
                lead.add(a + 8)
        run = 0
        for a in emitted:
            if a in lead or self.resume_all or run >= MAX_BLOCK:
                lead.add(a)
                run = 0
            run += len(units[a])
        return lead


# ---- blocks and event checks ----------------------------------------------------------------------------------------
#
# The interpreter checks `cycles < next_event` before every instruction. Lifted code stops before the same instruction
# with one check per segment, rather than a label, a test and an exit per instruction (what made the C slow to
# compile). A block starts at a resume point (block_leaders) and runs to the next one. A segment starts at the block's
# start or after a dynamic instruction (an access to a known IO register, SWL / SWR, a GTE command: they go through
# the core, which may move the clock and the next event) and runs up to the next dynamic one or the block's end. Its
# instructions before the last cost static cycles at most (their fetch, cpi each, and 3 for an LW-class load from
# RAM), so `now + those < ne` at its start means no check inside would stop; a dynamic instruction's own cycles, and
# whatever the core did, are seen by the check after it. RC_SEG is also the exact check of its first instruction: due
# now, leave for the dispatcher; due inside the segment, the interpreter runs the rest of the block
# (PS1_RC_EXIT_INTERP, rc_hint = how many instructions). Entered inside a block (an interrupt returns there), the
# function answers the same from its mid_ table. An IO access that left an interrupt pending sets the local next
# event to 0, so the check after it leaves.
#
# ---- memory accesses ----
#
# A load or store whose address is not a known IO register (io_accesses: a base built by LUI / ADDIU / ORI in the
# function, plus the offset, in the IO area) is a RAM access: main RAM and the scratchpad inline, with static cycles,
# and anything else (an IO register after all, an address or bus error, a store into this function's own code) is
# handed to the interpreter with the rest of its block (RC_HAND: one exit per access, no call into the core). The
# interpreter does it exactly; the dispatcher's content check sees a store into code. Accesses to known IO registers
# go through the core inline (ps1_rc_lw / read / write): the hardware registers of a game's drivers stay native.

MAX_BLOCK = 12  # instructions per block at most (an event in a block makes the interpreter run the rest of it)
LW_CLASS = ("lw", "lwl", "lwr", "lwc2")  # loads with the interpreter's load32 cycles (3 from main RAM)

# The guest registers in the C of a function (locals r1-r31, hi, lo): assigned, address taken (rc_div), used.
WRITE_RE = re.compile(r"\b(r\d+|hi|lo) = (?!=)")
ADDR_RE = re.compile(r"&(hi|lo)\b")
USE_RE = re.compile(r"\b(r(?:[1-9]|[12]\d|3[01])|hi|lo)\b")


def reg_field(r):
    """The head field of a guest register local."""
    return "h->%s" % r if r in ("hi", "lo") else "h->r[%s]" % r[1:]


def is_memory(i):
    return i.cls in ("load", "store") or i.name in ("lwc2", "swc2")


def is_io(addr):
    """Is addr (virtual) outside main RAM and the scratchpad: an IO register, the expansion areas, the BIOS?"""
    p = addr & 0x1FFFFFFF
    return p >= 0x1F000000 and not (0x1F800000 <= p < 0x1F800400)


def io_accesses(func):
    """The memory accesses of func whose address is known to be IO: the base register's value comes from LUI (then
    ADDIU / ORI on it) earlier in the function, in address order, with no other write to it in between. A heuristic
    for speed only: an IO access it misses is handed to the interpreter, a RAM access it takes for IO goes through the
    core; both are exact."""
    img, known, io = func.image, {}, set()
    for a in func.addrs:
        i = img.ins(a)
        if is_memory(i) and i.rs in known and is_io(known[i.rs] + i.simm):
            io.add(a)
        n = i.name
        if n == "lui":
            known[i.rt] = i.imm << 16
            continue
        if n in ("addiu", "ori") and i.rs in known and i.rt == i.rs:
            known[i.rt] = (known[i.rs] + i.simm) & 0xFFFFFFFF if n == "addiu" else known[i.rs] | i.imm
            continue
        for r in (i.dest(), i.ldest(), i.link):
            known.pop(r, None)
    return io


def segment_cycles(emitted, k, units, leaders):
    """The static cycles, at most, of the segment starting at emitted[k] before its last instruction."""
    costs = []
    while k < len(emitted):
        for cyc, dyn in units[emitted[k]]:
            costs.append(cyc)
            if dyn:
                return sum(costs[:-1])
        k += 1
        if k < len(emitted) and emitted[k] in leaders:
            break
    return sum(costs[:-1])
