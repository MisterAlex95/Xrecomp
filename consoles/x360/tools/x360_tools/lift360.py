"""The Xenon lifter with what discovery found in a title (XBA-270): save / restore helpers inlined at their call
sites, and bctr jump tables as switches. See discover360.py for how both are recognised."""

from tools.ppc.isa import BCCTR
from tools.ppc.lift import hx

from .xenon import XenonLifter


class TitleLifter(XenonLifter):
    def __init__(self, image, decode, func, name, comments=True, helpers=None, tables=None, direct=None,
                 leaders=None):
        """direct (WP-16): None, every call leaves to the dispatcher; or (callable, owner): `callable` the entries of
        the pack's functions a `bl` may call straight from C, `owner` a mapping resume point -> the entry of the
        function the runtime's resume table gives it. leaders: this function's find_leaders() when a lifter of the
        same function computed them already (recomp.py's direct-call pass), taken as they are."""
        self._leaders = leaders
        self.helpers = helpers or {}
        self.tables = tables or {}
        self.inlined = 0
        self.direct = direct
        self.callees = set()  # entries this function calls directly (recomp.py declares them)
        self.direct_rets = []  # (return point, this function's entry) of each direct call, checked by recomp.py
        self.indirect = 0  # how many of them are bctrl (RC_CALL_IND)
        super().__init__(image, decode, func, name, comments=comments)

    # WP-16: `bl T` with T a function of the pack calls xe_fn_T from C (xenon_rt.h RC_CALL): when the callee returns
    # to the point after the call (PC == that point, no other exit, the slice and the budget not run out) the caller
    # goes on at its label there, as the dispatcher would have done by calling this function at that resume point.
    # Only where the dispatcher would indeed come back to this function's code there: the resume table maps the
    # return point to this function (an address several functions hold maps to the first; an entry to its own).
    # Otherwise, and for import thunks (not functions), bcl and calls into the function itself at the next instruction
    # (`bl $+4`), the call leaves to the dispatcher as before. `bctrl` looks its target up at run time
    # (direct_icall, RC_CALL_IND).
    def direct_call(self, i):
        if self.direct is None:
            return None
        callable_, owner = self.direct
        t, ret = i.target, i.addr + 4
        if t not in callable_ or t == ret or ret not in self.leaders or owner.get(ret) != self.f.entry:
            return None
        self.callees.add(t)
        self.direct_rets.append((ret, self.f.entry))
        return "c->lr = %s; RC_CALL(%s, %s, %s, L_%08X);" % (hx(ret), self.fn_name(t), hx(t), hx(ret), ret)

    def direct_icall(self, i):
        """`bctrl` (unconditional): RC_CALL_IND, the target looked up at run time as the dispatcher does; same
        condition on the return point."""
        if self.direct is None or not i.lk or (i.rd & 0x14) != 0x14:
            return None
        ret = i.addr + 4
        if ret not in self.leaders or self.direct[1].get(ret) != self.f.entry:
            return None
        self.direct_rets.append((ret, self.f.entry))
        self.indirect += 1
        return "{ uint32_t t_ = c->ctr & ~3u; c->lr = %s; RC_CALL_IND(t_, %s, L_%08X); }" % (hx(ret), hx(ret), ret)

    @staticmethod
    def fn_name(entry):
        return "xe_fn_%08X" % entry

    def entry_check(self):
        # with direct calls most entries into a function are at its entry: one compare before the switch
        if self.direct is None:
            return ""
        return "if (c->pc == %s) goto L_%08X;" % (hx(self.f.entry), self.f.entry)

    def find_leaders(self):
        if self._leaders is not None:
            return self._leaders
        lead = super().find_leaders()
        for a in self.f.addrs:
            if a in self.tables and self.decode(a).kind == BCCTR:
                lead |= {t for t in self.tables[a] if t in self.f.addrs}
        return lead

    def _helper_body(self, entry, with_return):
        h, _reg = self.helpers[entry]
        out = []
        a = entry
        while a <= h.end:
            i = self.decode(a)
            if a == h.end and not with_return:
                break
            if self.needs_interp(i):
                return None  # cannot happen with the shapes find_helpers accepts; kept safe
            out.append(self.insn(i))
            a += 4
        return " ".join(out)

    def op_b(self, i):
        t = i.target
        if t in self.helpers:
            body = self._helper_body(t, with_return=not i.lk)
            if body is not None:
                self.inlined += 1
                h = self.helpers[t][0]
                tag = "/* %s %08X inlined */ " % (h.kind, t) if self.comments else ""
                if i.lk:
                    return "%sc->lr = %s; %s" % (tag, hx(i.addr + 4), body)
                return tag + body
        return super().op_b(i)

    def terminates(self, i):
        if i.name == "b" and i.lk and i.target in self.helpers:
            return False
        return super().terminates(i)

    def op_bcctr(self, i):
        d = self.direct_icall(i)
        if d:
            return d
        targets = self.tables.get(i.addr)
        if not targets or (i.rd & 0x14) != 0x14 or i.lk:
            return super().op_bcctr(i)
        cases = []
        for t in sorted(set(targets)):
            cases.append("case %s: %s" % (hx(t), self.go(t)))
        return "{ uint32_t t_ = c->ctr & ~3u; switch (t_) { %s default: break; } RC_EXIT(t_); }" % " ".join(cases)
