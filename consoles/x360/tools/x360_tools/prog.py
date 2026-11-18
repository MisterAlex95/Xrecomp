"""A tiny assembler with labels for tests and synthetic titles (XBA-230 / XBA-270): words back to back from a base,
branch targets and data words resolved by label at link()."""


class Prog:
    """Leaf functions back to back in .text; branch targets by label."""

    def __init__(self, base):
        self.base = base
        self.words = []
        self.labels = {}
        self.fixups = []
        self.funcs = []

    @property
    def here(self):
        return self.base + 4 * len(self.words)

    def func(self, name):
        self.funcs.append([name, len(self.words), None])
        self.label(name)

    def end(self):
        self.funcs[-1][2] = len(self.words)

    def label(self, name):
        self.labels[name] = self.here

    def __call__(self, *ws):
        for w in ws:
            if isinstance(w, list):
                self.words.extend(w)
            else:
                self.words.append(w)

    def branch(self, enc, label):
        self.fixups.append((len(self.words), enc, label))
        self.words.append(0)

    def link(self):
        for k, enc, label in self.fixups:
            self.words[k] = enc(self.base + 4 * k, self.labels[label])
        return self.words
