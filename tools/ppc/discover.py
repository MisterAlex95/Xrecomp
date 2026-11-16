"""Function discovery for PowerPC code (XBA-225): recursive descent from entry points, console-agnostic.

The image is any object with `in_code(addr) -> bool` and `u32(addr) -> int` (big-endian word); the decoder is a
`tools.ppc.isa.Decoder` (or a console layer's subclass). Entry points are the console layer's seeds (the program
entry, addresses the interpreter reported, pointers into code found in data, prologue patterns), plus every `bl`
target found on the way, to a fixpoint. A function is what its entry reaches through branches that are not calls:
`b` to another function's entry is a tail call (it leaves the function), `bclr` / `bcctr` that always branch and
`rfi` end a path, conditional ones fall through. Functions may overlap (shared tails are lifted in each); the table
gives each address to one of them (`owners`).

Nothing here is GameCube-specific: the Xbox 360 layer reuses it with its own decoder and image.
"""

from .isa import BCCTR, BCLR, BCOND, BRANCH, INVALID, RFI


class Function:
    __slots__ = ("entry", "addrs", "calls")

    def __init__(self, entry):
        self.entry = entry
        self.addrs = set()
        self.calls = set()

    def __repr__(self):
        return "<fn %08X: %d insns>" % (self.entry, len(self.addrs))


class Discovery:
    def __init__(self, image, decoder, seeds=()):
        self.image = image
        self.decoder = decoder
        self.seeds = {a for a in seeds if image.in_code(a) and a % 4 == 0}
        self._cache = {}

    def insn(self, a):
        i = self._cache.get(a)
        if i is None:
            i = self.decoder.decode(self.image.u32(a), a)
            self._cache[a] = i
        return i

    def explore(self, entry, entries):
        f = Function(entry)
        stack = [entry]
        while stack:
            a = stack.pop()
            while self.image.in_code(a) and a not in f.addrs:
                i = self.insn(a)
                f.addrs.add(a)
                k = i.kind
                if k == INVALID:
                    break  # lifted as a hand-off to the interpreter, path ends
                if k == BRANCH:
                    t = i.target
                    if i.lk:
                        if self.image.in_code(t):
                            f.calls.add(t)
                        a += 4
                        continue
                    if t != entry and t in entries:
                        break  # tail call
                    if self.image.in_code(t):
                        stack.append(t)
                    break
                if k == BCOND:
                    t = i.target
                    if i.lk:
                        if self.image.in_code(t):
                            f.calls.add(t)
                    elif self.image.in_code(t) and (t == entry or t not in entries):
                        stack.append(t)
                    if (i.rd & 0x14) == 0x14:
                        break  # branch always
                    a += 4
                    continue
                if k in (BCLR, BCCTR):
                    if (i.rd & 0x14) == 0x14 and not i.lk:
                        break
                    a += 4
                    continue
                if k == RFI:
                    break
                a += 4
        return f

    def run(self):
        """[Function] sorted by entry."""
        entries = set(self.seeds)
        while True:
            funcs = {e: self.explore(e, entries) for e in sorted(entries)}
            more = set()
            for f in funcs.values():
                more |= f.calls
            more -= entries
            if not more:
                break
            entries |= more
        self.functions = [funcs[e] for e in sorted(funcs)]
        return self.functions

    def stats(self):
        fs = self.functions
        covered = set()
        for f in fs:
            covered |= f.addrs
        return {"functions": len(fs), "instructions": len(covered),
                "lifted_instructions": sum(len(f.addrs) for f in fs)}


def owners(functions):
    """{addr: Function}: each address to the function whose entry it is, else to the first one holding it."""
    own = {}
    for f in functions:
        own[f.entry] = f
    for f in functions:
        for a in f.addrs:
            own.setdefault(a, f)
    return own
