"""The Xenon lifter with what discovery found in a title (XBA-270): save / restore helpers inlined at their call
sites, and bctr jump tables as switches. See discover360.py for how both are recognised."""

from tools.ppc.isa import BCCTR
from tools.ppc.lift import hx

from .xenon import XenonLifter


class TitleLifter(XenonLifter):
    def __init__(self, image, decode, func, name, comments=True, helpers=None, tables=None):
        self.helpers = helpers or {}
        self.tables = tables or {}
        self.inlined = 0
        super().__init__(image, decode, func, name, comments=comments)

    def find_leaders(self):
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
        targets = self.tables.get(i.addr)
        if not targets or (i.rd & 0x14) != 0x14 or i.lk:
            return super().op_bcctr(i)
        cases = []
        for t in sorted(set(targets)):
            cases.append("case %s: %s" % (hx(t), self.go(t)))
        return "{ uint32_t t_ = c->ctr & ~3u; switch (t_) { %s default: break; } RC_EXIT(t_); }" % " ".join(cases)
