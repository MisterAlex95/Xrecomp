"""--split-bytes chunks written while the translation goes on (ChunkStream), so that tools.prepare compiles them beside
the lift: what the lift leaves is byte for byte what it left before, and the end rewrites no chunk the stream
forecast right (only the last one, and what the funclet entries placed after every function change)."""

import os
import random

from tools.recomp import translator as tr
from tools.recomp.lifter import Lifter
from tools.recomp.translator import BatchTranslator, ChunkStream

BASE = 0x00011000


class _FakeTranslator:
    """Functions of varied sizes calling later ones and a few addresses no function starts at (stubbed), some with
    exception funclets: one a new entry, one an address another function owns (its copy goes to the parent)."""

    def __init__(self, n, seed):
        self.owned_function_starts = set()
        self.lifter = Lifter()
        self.lifter.referenced_calls = {}
        self.rnd = random.Random(seed)
        self.addrs = [BASE + 0x40 * i for i in range(n)]
        self._seh_frame = None

    def translate_function(self, addr, func_info):
        name = func_info["name"]
        if addr % 0x1000 == 0x0C0:
            return None                                     # a failed translation: a stub
        body = []
        for _ in range(self.rnd.randrange(1, 6)):
            t = self.rnd.choice(self.addrs) if self.rnd.random() < 0.8 else self.rnd.choice(self.addrs) + 2
            self.lifter.referenced_calls[t] = "sub_%08X" % t
            body.append("    RECOMP_ABI_CALL(0x%08Xu, sub_%08X);" % (t, t))
        body.append("    /* %s */" % ("x" * self.rnd.choice((50, 400, 1500, 4000))))
        return "void %s(void)\n{\n%s\n}\n" % (name, "\n".join(body))

    def seh_funclet_entries(self, addr, name, code, frame):
        i = (addr - BASE) // 0x40
        if i % 7 == 3:                                      # a funclet no function owns: appended at the end
            fa = addr + 0x10
        elif i % 11 == 5:                                   # one at the next function's start: the copy stays here
            fa = addr + 0x40
        else:
            return []
        fname = "sub_%08X" % fa
        copy = "void %s__seh(uint32_t _seh_target)\n{\n    /* copy of %s */\n}\n" % (name, name)
        return [(fa, fname, copy + "void %s__seh(uint32_t _seh_target);\nvoid %s(void) {\n    %s__seh(0x%08Xu);\n}\n"
                 % (name, fname, name, fa))]

    def _stub_ret_bytes(self, addr):
        return 0

    def _stub_epilogue(self, addr):
        return None


def _lift(out, n=600, seed=3, chunk_bytes=6000):
    batch = BatchTranslator.__new__(BatchTranslator)
    batch.translator = _FakeTranslator(n, seed)
    funcs = [(a, {"name": "sub_%08X" % a}) for a in batch.translator.addrs]
    return batch.translate_batch_split(funcs, str(out), chunk_bytes=chunk_bytes)


def _tree(d):
    out = {}
    for fn in sorted(os.listdir(d)):
        with open(os.path.join(d, fn), "rb") as f:
            out[fn] = f.read()
    return out


def test_the_stream_changes_nothing_the_lift_writes(tmp_path, monkeypatch):
    _lift(tmp_path / "stream")
    monkeypatch.setattr(ChunkStream, "add", lambda self, *a: None)
    monkeypatch.setattr(ChunkStream, "_types", lambda self: None)
    _lift(tmp_path / "plain")
    a, b = _tree(tmp_path / "stream"), _tree(tmp_path / "plain")
    assert a == b and len([fn for fn in a if fn.startswith("recomp_0")]) > 20


def test_the_end_rewrites_no_chunk_forecast_right(tmp_path, monkeypatch):
    writes = []
    real = tr.write_if_changed

    def spy(path, text):
        wrote = real(path, text)
        writes.append((os.path.basename(path), wrote))
        return wrote
    monkeypatch.setattr(tr, "write_if_changed", spy)
    stats = _lift(tmp_path)
    chunks = sorted(fn for fn in os.listdir(tmp_path) if fn.startswith("recomp_0"))
    streamed = {fn for fn, wrote in writes[:writes.index(("recomp_funcs.h", True))] if fn.startswith("recomp_0")}
    assert len(streamed) >= len(chunks) - 4         # the last ones: the open chunk, the funclets placed at the end
    again = [fn for fn in streamed if sum(1 for f, w in writes if f == fn and w) > 1]
    assert again == [], again                               # every forecast right
    assert stats["num_chunks"] == len(chunks)


def test_a_wrong_forecast_is_written_again(tmp_path, monkeypatch):
    """A forecast that misses (here: every streamed chunk spoilt) is put right by the end."""
    real = ChunkStream._write

    def spoil(self):
        self.cur = [(a, n, c + "/* wrong */") for a, n, c in self.cur]
        real(self)
    monkeypatch.setattr(ChunkStream, "_write", spoil)
    _lift(tmp_path / "spoilt")
    monkeypatch.setattr(ChunkStream, "_write", real)
    _lift(tmp_path / "right")
    assert _tree(tmp_path / "spoilt") == _tree(tmp_path / "right")
