"""MIPS I to C (XBA-161): one C function per discovered function, in the shape of ps1_recomp_rt.h.

Every instruction has a label and a case in the entry switch (the dispatcher resumes anywhere), except delay slots,
which run inline after their branch (and get a label of their own only when a branch of the function targets them).
Semantics are the interpreter's (ps1_cpu.c), instruction for instruction:
- fetch cost and the scheduler test before every instruction (RC_I / RC_I_SLOT);
- load delay slots: the value is committed at once when the next instruction cannot tell (isa.conflicts), else the
  function stops with the load in flight (RC_EXIT_LD) and the interpreter runs that instruction;
- branch delay slots inline; a branch in a delay slot, exceptions (overflow, address / bus errors, SYSCALL, BREAK,
  reserved, coprocessor unusable) and COP0 go to the interpreter (RC_INTERP: nothing of the instruction done);
- calls, returns, indirect jumps and jumps out of the function return to the dispatcher (bounded C stack);
- GTE commands and transfers call the core (ps1_rc_gte_*), IO goes through the core's bus functions.
"""

from .isa import conflicts

CPI_RAM, CPI_RAM_UNCACHED = 1, 5


def hx(v):
    return "0x%08Xu" % (v & 0xFFFFFFFF)


def R(r):
    return "0u" if r == 0 else "r%d" % r


class FnLifter:
    def __init__(self, func, name, resume_all=True):
        self.resume_all = resume_all  # a label at every instruction (every one a resume point)
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

    def needs_label(self, a):
        """Resume points: where gotos land (branch targets, the fall-through after a branch's slot), where the
        dispatcher comes back (return addresses, the instruction after one the lifted code stopped behind: memory and
        GTE accesses, interpreter hand-overs) and the function's start. Elsewhere (a scheduler event stopped the
        code in a straight run) the interpreter steps to the next resume point."""
        f = self.f
        if a == f.start or a in self.targets:
            return True
        i = self.img.ins(a)
        if i.cls in ("interp", "trap") or i.is_transfer():
            return True
        for back in (4, 8):
            p = a - back
            if not self.inside(p):
                continue
            pi = self.img.ins(p)
            if back == 8 and pi.is_transfer():
                return True
            if back == 4 and pi.cls in ("load", "store", "cop2", "interp", "trap"):
                return True
        return False

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

    def body(self, i, fault, slot_ctx=None):
        """C for instruction i (not a transfer). fault: the statement that hands it to the interpreter. Returns
        (code, delayed) where delayed = (rt, expr) for a delayed load (value in ldv) or None, and whether it may set
        chk."""
        n, rs, rt, rd = i.name, R(i.rs), R(i.rt), R(i.rd)
        simm = i.simm & 0xFFFFFFFF

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
                return ("{ uint32_t ea = %s; if (ea & 3) { %s } RC_LW(ea, ldv, %s); }" % (addr, fault, fault),
                        i.rt, True)
            if n in ("lwl", "lwr"):
                return ("{ uint32_t ea = %s, ew = 0; RC_LW(ea & ~3u, ew, %s); ldv = rc_%s(%s, ew, ea); }"
                        % (addr, fault, n, rt), i.rt, True)
            t = {"lb": "int8_t", "lbu": "uint8_t", "lh": "int16_t", "lhu": "uint16_t"}[n]
            al = "if (ea & 1) { %s } " % fault if n in ("lh", "lhu") else ""
            return "{ uint32_t ea = %s; %sRC_LD(%s, ea, ldv, %s); }" % (addr, al, t, fault), i.rt, True
        if c == "store":
            if n in ("swl", "swr"):
                return "RC_SWLR(%s, %s, %d, %s);" % (addr, rt, 1 if n == "swr" else 0, fault), None, True
            t, al = {"sb": ("uint8_t", 0), "sh": ("uint16_t", 1), "sw": ("uint32_t", 3)}[n]
            alc = "if (ea & %d) { %s } " % (al, fault) if al else ""
            return "{ uint32_t ea = %s; %sRC_ST(%s, ea, %s, %s); }" % (addr, alc, t, rt, fault), None, True
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
                return (cu + "{ uint32_t ea = %s, ew = 0; if (ea & 3) { %s } RC_LW(ea, ew, %s); "
                        "ps1_rc_gte_write(RC_M, %d, ew); }" % (addr, fault, fault, i.rt)), None, True
            if n == "swc2":
                return (cu + "{ uint32_t ea = %s; if (ea & 3) { %s } RC_ST(uint32_t, ea, ps1_rc_gte_read(RC_M, %d), "
                        "%s); }" % (addr, fault, i.rt, fault)), None, True
        raise ValueError("cannot lift %s" % i.text())

    # ---- instructions ----
    def plain(self, a, comments):
        i = self.img.ins(a)
        fault = "RC_INTERP(%s);" % hx(a)
        code, ld, may_chk = self.body(i, fault)
        if i.cls == "interp":
            self.fallbacks += 1
        self.emit("    %s\n" % code)
        if ld is not None:
            # the delayed load lands before the next instruction (in this function, sequentially)
            nxt = a + 4
            if ld == 0:
                pass
            else:
                self.emit("    %s\n" % self.commit(ld, "ldv", nxt, self.inside(nxt)))
        if may_chk:
            self.emit("    if (chk) RC_EXIT(%s);\n" % hx(a + 4))

    def transfer(self, a, comments):
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
        # the delay slot
        si = self.img.ins(s)
        self.emit("    RC_I_SLOT(%s, %s, tk);\n" % (hx(s), npc))
        fault = "RC_INTERP_SLOT(%s, %s, tk);" % (hx(s), npc)
        code, ld, may_chk = self.body(si, fault)
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
            if may_chk:
                parts.append("if (chk) RC_EXIT(%s);" % dest_expr)
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

    def function(self, comments=True):
        f = self.f
        cpi = CPI_RAM_UNCACHED if (f.start >> 29) == 5 else CPI_RAM
        self.emit("RC_FN %s(ps1_machine *m_)\n{\n" % self.name)
        self.emit("    RC_ENTER(%d, 0x%06Xu, %du);\n" % (cpi, f.start & 0x1FFFFF, f.end - f.start))
        # which instructions are emitted: all but delay slots (they run inline after their branch) unless targeted
        emitted = []
        slot = set()
        for a in f.addrs:
            i = self.img.ins(a)
            if a not in slot or a in self.targets:
                emitted.append(a)
            if i.is_transfer() and self.inside(a + 4) and not self.img.ins(a + 4).is_transfer():
                slot.add(a + 4)
        labels = [a for a in emitted if self.resume_all or self.needs_label(a)]
        self.emit("    switch (h->pc) {\n")
        for a in labels:
            self.emit("    case %s: goto L_%08X;\n" % (hx(a), a))
        self.emit("    default: RC_NOT_COVERED();\n    }\n")
        label_set = set(labels)
        self.labels = len(labels)
        for a in emitted:
            i = self.img.ins(a)
            if a in label_set:
                self.emit("L_%08X:%s\n" % (a, " /* %s */" % i.text() if comments else ""))
            elif comments:
                self.emit("    /* %08X: %s */\n" % (a, i.text()))
            self.emit("    RC_I(%s);\n" % hx(a))
            if i.is_transfer():
                self.transfer(a, comments)  # every path of a lifted transfer ends in a goto or an exit
            else:
                self.plain(a, comments)
        self.emit("    RC_EXIT(%s);\n" % hx(f.end))
        self.emit("    RC_EPILOGUE();\n}\n")
        return "".join(self.out)
