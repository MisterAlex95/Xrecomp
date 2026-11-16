"""Synthetic programs for the complete Gekko layer (XBA-260), each run interpreted and recompiled under gc_run, both
compared with values worked out here: address translation (BATs, page table, DSI / ISI), the locked cache (dcbz_l,
DMA), the time base and decrementer (cycle estimates, the decrementer exception), the fres / frsqrte estimate
tables, FPSCR rounding modes / non-IEEE mode / exception bits, and the exceptions raised from lifted code (program:
trap, privileged, illegal; FP unavailable; external; system call).

Exception handlers sit at the vectors (a text section at 0x80000300, run at the real-mode vector addresses, so they
are interpreted in both runs) and append (vector, SRR0, SRR1, DSISR or DEC, DAR or TBL) to a log in memory, then fix
SRR0 / SRR1 so the program goes on. No toolchain needed. GC_TEST_CC / GC_TEST_EMULATOR (test_gc_tools) run them for
another host, e.g. arm64 under qemu."""

import math
import os
import shutil
import struct
import subprocess
import sys

import pytest

from . import CONSOLE  # noqa: F401
from tools.ppc import asm

from .dol import build
from .gekko import cycles
from .test_gc_tools import CC, CFLAGS, EMULATOR, HERE, RUNTIME, _build_gc_run

TEXT = 0x80003100
DATA = 0x80010000
OUT = 0x80011000
LOG = 0x80012000
LOGP = 0x80012FF0      # the log pointer (a physical address: handlers run in real mode)
MSR = 0x3032           # FP ME IR DR RI, as gc_create leaves it
ANY = None             # a log field not checked


class Asm:
    """Instruction words at base, with labels and forward references."""

    def __init__(self, base):
        self.base, self.words, self.labels, self.fix = base, [], {}, []

    def at(self):
        return self.base + 4 * len(self.words)

    def __call__(self, *words):
        self.words.extend(words)
        return self

    def label(self, name):
        self.labels[name] = self.at()

    def ref(self, fn, name):
        """A word fn(at, target) once label name is known (branches)."""
        self.fix.append((len(self.words), fn, name))
        self.words.append(0)

    def li32(self, r, v):
        self(asm.lis(r, (v >> 16) & 0xFFFF), asm.ori(r, r, v & 0xFFFF))

    def li32_label(self, r, name):
        self.fix.append((len(self.words), lambda at, t: asm.lis(r, (t >> 16) & 0xFFFF), name))
        self.fix.append((len(self.words) + 1, lambda at, t: asm.ori(r, r, t & 0xFFFF), name))
        self.words += [0, 0]

    def pad_to(self, addr):
        while self.at() < addr:
            self.words.append(0)

    def resolve(self):
        for k, fn, name in self.fix:
            self.words[k] = fn(self.base + 4 * k, self.labels[name])
        self.fix = []
        return self.words

    def data(self):
        return struct.pack(">%dI" % len(self.words), *self.resolve())

    def idle(self):
        name = "idle_%d" % len(self.words)
        self.label(name)
        self.ref(asm.b, name)


def out(a, r, k):
    a(asm.stw(r, 4 * k, 31))


HANDLED = (0x300, 0x400, 0x500, 0x700, 0x800, 0x900, 0xC00)


def handlers():
    """The vectors' code: log (vector, SRR0, SRR1, DSISR / DEC, DAR / TBL), fix up, rfi. Returns the Asm."""
    a = Asm(0x80000300)
    for v in HANDLED:
        a.pad_to(0x80000000 + v)
        a.label(v)
        a(asm.mtspr(272, 3), asm.mtspr(273, 4), asm.mtspr(274, 5),
          asm.lis(3, (LOGP & 0x3FFFFFFF) >> 16), asm.lwz(5, LOGP & 0xFFFF, 3),
          asm.li(4, v), asm.stw(4, 0, 5), asm.mfspr(4, 26), asm.stw(4, 4, 5), asm.mfspr(4, 27), asm.stw(4, 8, 5))
        if v == 0x900:
            a(asm.mfdec(4), asm.stw(4, 12, 5), asm.mftb(4), asm.stw(4, 16, 5))
        else:
            a(asm.mfspr(4, 18), asm.stw(4, 12, 5), asm.mfspr(4, 19), asm.stw(4, 16, 5))
        a(asm.addi(5, 5, 20), asm.stw(5, LOGP & 0xFFFF, 3))
        if v in (0x300, 0x700, 0x900):                  # skip the instruction (the idle loop for 0x900)
            a(asm.mfspr(4, 26), asm.addi(4, 4, 4), asm.mtspr(26, 4))
        if v == 0x700:                                  # back to supervisor mode
            a(asm.mfspr(4, 27), asm.rlwinm(4, 4, 0, 18, 16), asm.mtspr(27, 4))
        if v == 0x400:                                  # return to the caller (bctrl)
            a(asm.mflr(4), asm.mtspr(26, 4))
        if v == 0x800:                                  # FP on, retry
            a(asm.mfspr(4, 27), asm.ori(4, 4, 0x2000), asm.mtspr(27, 4))
        if v == 0x500:                                  # acknowledge EXI channel 0's transfer interrupt
            a(asm.lis(4, 0x0C00), asm.ori(4, 4, 0x6800), asm.li(3, 8), asm.stw(3, 0, 4))
        a(asm.mfspr(5, 274), asm.mfspr(4, 273), asm.mfspr(3, 272), asm.rfi())
    return a


def make_dol(main, datas=()):
    vec = handlers()
    datas = list(datas) + [(LOGP, struct.pack(">I", LOG & 0x3FFFFFFF))]
    return build([(TEXT, main.data()), (vec.base, vec.data())], datas), vec


def parse_peeks(stdout):
    groups = []
    for line in stdout.splitlines():
        if line.startswith("peek "):
            groups.append([int(x, 16) for x in line[5:].split()])
        elif groups and line and all(ch in "0123456789ABCDEF " for ch in line):
            groups[-1] += [int(x, 16) for x in line.split()]
    return groups


@pytest.fixture(scope="module")
def gc_run(tmp_path_factory):
    if not shutil.which(CC) or not sys.platform.startswith("linux"):
        pytest.skip("no C compiler / not Linux")
    return _build_gc_run(tmp_path_factory.mktemp("gc_run"))


def run_both(gc_run, tmp_path, dol, peeks):
    """[peek groups] of the interpreted run, checked equal to the recompiled run's; and the recompiled stats."""
    path = tmp_path / "t.dol"
    path.write_bytes(dol)
    args = [str(path), "--frames", "1", "--stats"]
    for a, n in peeks:
        args += ["--peek", "%08X:%d" % (a, n)]
    r = subprocess.run(gc_run + args, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    interp = parse_peeks(r.stdout)
    env = dict(os.environ, PYTHONPATH=os.path.dirname(HERE))
    subprocess.run([sys.executable, "-m", "gc_tools.recomp", str(path), str(tmp_path / "src")], env=env,
                   check=True, capture_output=True)
    pack = str(tmp_path / "libt.so")
    srcs = sorted(str(p) for p in (tmp_path / "src").glob("*.c"))
    subprocess.run([CC, "-shared", "-fPIC"] + CFLAGS + ["-Wno-unused-label", "-DGC_RC_COUNT", "-I", RUNTIME,
                    "-o", pack] + srcs + ["-lm"], check=True)
    r = subprocess.run(gc_run + args + ["--pack", pack], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    rc = parse_peeks(r.stdout)
    stats = [ln for ln in r.stdout.splitlines() if ln.startswith("stats:")][0].split()
    assert int(stats[2]) > 20, stats  # lifted code ran (after a handler, the interpreter runs to a resume point)
    assert interp == rc, "interpreter %s\nrecompiled  %s" % (interp, rc)
    return interp


def check_log(got, expected):
    assert len(got) >= 5 * len(expected)
    for k, entry in enumerate(expected):
        for f, want in enumerate(entry):
            if want is not ANY:
                assert got[5 * k + f] == want, "log entry %d field %d: %08X, want %08X (%s)" % (
                    k, f, got[5 * k + f], want, [hex(x) for x in got[5 * k:5 * k + 5]])


# ---- address translation ----

def pteg_of(vsid, ea, secondary, sdr1):
    page = (ea >> 12) & 0xFFFF
    h = (vsid & 0x7FFFF) ^ page
    if secondary:
        h = ~h & 0x7FFFF
    org, mask = sdr1 & 0xFFFF0000, sdr1 & 0x1FF
    return (org & 0xFE000000) | ((((org >> 16) & 0x1FF) | ((h >> 10) & mask)) << 16) | ((h & 0x3FF) << 6)


def test_mmu_bats_page_table(gc_run, tmp_path):
    sdr1, vsid4 = 0x00100000, 0x123
    ptes = [(0x40001000, False, 0x00006002),   # read / write
            (0x40002000, False, 0x00007003),   # read only (PP 3, key 0)
            (0x40003000, True, 0x00008002)]    # in the secondary PTEG
    a = Asm(TEXT)
    a.li32(31, OUT)
    a.li32(3, sdr1)
    a(asm.mtspr(25, 3))
    a.li32(3, 0x20000000 | vsid4)                      # SR4: Kp = 1, Ks = 0
    a(asm.mtsr(4, 3))
    a.li32(3, 0x10000456)                              # SR5: no-execute
    a(asm.mtsr(5, 3))
    addrs = []
    for ea, sec, w1 in ptes:
        g = pteg_of(vsid4, ea, sec, sdr1)
        addrs.append(g)
        a.li32(4, 0x80000000 | g)
        a.li32(5, 0x80000000 | vsid4 << 7 | (0x40 if sec else 0) | ((ea >> 22) & 0x3F))
        a(asm.stw(5, 0, 4))
        a.li32(5, w1)
        a(asm.stw(5, 4, 4))
    a(asm.sync())
    a.li32(6, 0x11111111)
    a.li32(7, 0x40001010)
    a(asm.stw(6, 0, 7))
    a.li32(8, 0x80006010)
    a(asm.lwz(9, 0, 8)); out(a, 9, 0)
    a(asm.lwz(9, 0, 7)); out(a, 9, 1)
    a.li32(6, 0x22222222)
    a.li32(8, 0x80007020)
    a(asm.stw(6, 0, 8))
    a.li32(7, 0x40002020)
    a(asm.lwz(9, 0, 7)); out(a, 9, 2)
    a.li32(6, 0x99999999)
    a.ref(asm.bl, "probe_st")                          # DSI: protection
    a(asm.lwz(9, 0, 8)); out(a, 9, 3)
    a.li32(6, 0x33333333)
    a.li32(7, 0x40003004)
    a(asm.stw(6, 0, 7))
    a.li32(8, 0x80008004)
    a(asm.lwz(9, 0, 8)); out(a, 9, 4)
    a.li32(8, 0x80000000 | addrs[0] + 4)
    a(asm.lwz(9, 0, 8)); out(a, 9, 5)                  # R and C set
    a.li32(8, 0x80000000 | addrs[1] + 4)
    a(asm.lwz(9, 0, 8)); out(a, 9, 6)                  # R set, C not
    a.li32(10, 0x5A5A5A5A)
    a.li32(7, 0x40005000)
    a.ref(asm.bl, "probe_ld")                          # DSI: no PTE, r10 unchanged
    out(a, 10, 7)
    a.li32(3, 0x00020002)                              # DBAT2: 0x90000000 (128 KB) -> 0x00020000
    a(asm.mtspr(541, 3))
    a.li32(3, 0x90000002)
    a(asm.mtspr(540, 3))
    a.li32(6, 0x44444444)
    a.li32(7, 0x90000010)
    a(asm.stw(6, 0, 7))
    a.li32(8, 0x80020010)
    a(asm.lwz(9, 0, 8)); out(a, 9, 8)
    a.li32(3, 0x00040001)                              # DBAT3: 0xA0000000 -> 0x00040000, read only
    a(asm.mtspr(543, 3))
    a.li32(3, 0xA0000002)
    a(asm.mtspr(542, 3))
    a.li32(6, 0x55555555)
    a.li32(8, 0x80040000)
    a(asm.stw(6, 0, 8))
    a.li32(7, 0xA0000000)
    a(asm.lwz(9, 0, 7)); out(a, 9, 9)
    a.ref(asm.bl, "probe_st")                          # DSI: protection
    a.li32(3, 0x800003FF)                              # DBAT0 down to 32 MB: MEM1 still mapped, 0x82000000 not
    a(asm.mtspr(536, 3))
    a.li32(7, 0x82000000)
    a.ref(asm.bl, "probe_ld")                          # DSI: no translation
    a.li32(8, 0x80006010)
    a(asm.lwz(9, 0, 8)); out(a, 9, 10)
    a.li32(3, 0x80001FFF)
    a(asm.mtspr(536, 3))
    a.li32(9, 0x40000000)                              # ISI: no PTE
    a(asm.mtctr(9), asm.bctrl())
    a.li32(9, 0x50001000)                              # ISI: no-execute segment
    a(asm.mtctr(9), asm.bctrl())
    a.li32(3, LOGP)
    a(asm.lwz(9, 0, 3)); out(a, 9, 11)
    a.idle()
    # the faulting accesses, in functions of their own: lifted code, and the call's return point resumes lifted code
    a.label("probe_st")
    a(asm.stw(6, 0, 7), asm.blr())
    a.label("probe_ld")
    a(asm.lwz(10, 0, 7), asm.blr())
    dol, _ = make_dol(a)
    res, log = run_both(gc_run, tmp_path, dol, [(OUT, 12), (LOG, 30)])
    L = a.labels
    assert res == [0x11111111, 0x11111111, 0x22222222, 0x22222222, 0x33333333, 0x00006182, 0x00007103, 0x5A5A5A5A,
                   0x44444444, 0x55555555, 0x11111111, (LOG & 0x3FFFFFFF) + 6 * 20]
    check_log(log, [(0x300, L["probe_st"], MSR, 0x0A000000, 0x40002020),
                    (0x300, L["probe_ld"], MSR, 0x40000000, 0x40005000),
                    (0x300, L["probe_st"], MSR, 0x0A000000, 0xA0000000),
                    (0x300, L["probe_ld"], MSR, 0x40000000, 0x82000000),
                    (0x400, 0x40000000, 0x40000000 | MSR, ANY, ANY),
                    (0x400, 0x50001000, 0x10000000 | MSR, ANY, ANY)])


# ---- locked cache ----

def test_locked_cache(gc_run, tmp_path):
    pattern = [0xA0000000 + k * 0x01010101 for k in range(16)]
    a = Asm(TEXT)
    a.li32(31, OUT)
    a(asm.mfspr(3, 920), asm.oris(3, 3, 0x1000), asm.mtspr(920, 3))      # HID2[LCE]
    a.li32(4, 0xE0000000)
    a.li32(5, 0xDEADBEEF)
    a(asm.stw(5, 4, 4), asm.lwz(6, 4, 4)); out(a, 6, 0)
    a(asm.li(7, 0), asm.dcbz_l(4, 7), asm.lwz(6, 4, 4)); out(a, 6, 1)
    a.li32(8, 0x00014000)                                                 # DMA in: 2 lines
    a(asm.mtspr(922, 8))
    a.li32(8, 0xE0000100 | 0x10 | 2 << 2 | 2)
    a(asm.mtspr(923, 8))
    for k, off in enumerate((0x100, 0x11C, 0x120, 0x13C, 0x140)):
        a(asm.lwz(6, off, 4)); out(a, 6, 2 + k)
    a.li32(5, 0x12345678)
    a(asm.stw(5, 0x200, 4))
    a.li32(8, 0x00015000)                                                 # DMA out: 1 line
    a(asm.mtspr(922, 8))
    a.li32(8, 0xE0000200 | 1 << 2 | 2)
    a(asm.mtspr(923, 8))
    a.li32(9, 0x80015000)
    a(asm.lwz(6, 0, 9)); out(a, 6, 7)
    a(asm.mfspr(6, 923)); out(a, 6, 8)
    a(asm.mfspr(6, 920)); out(a, 6, 9)
    a(asm.mfspr(3, 920), asm.rlwinm(3, 3, 0, 4, 2), asm.mtspr(920, 3))  # LCE off
    a.label("dcbzl")
    a(asm.dcbz_l(4, 7))                                                   # program: illegal
    a.label("ld_lc")
    a(asm.lwz(6, 4, 4))                                                   # DSI: no mapping for 0xE0000000
    a.li32(3, LOGP)
    a(asm.lwz(9, 0, 3)); out(a, 9, 10)
    a.idle()
    dol, _ = make_dol(a, [(0x80014000, struct.pack(">16I", *pattern))])
    res, log = run_both(gc_run, tmp_path, dol, [(OUT, 11), (LOG, 10)])
    assert res == [0xDEADBEEF, 0, pattern[0], pattern[7], pattern[8], pattern[15], 0, 0x12345678, 0xE0000204,
                   0xB0000000, (LOG & 0x3FFFFFFF) + 2 * 20]
    check_log(log, [(0x700, a.labels["dcbzl"], 0x00080000 | MSR, ANY, ANY),
                    (0x300, a.labels["ld_lc"], MSR, 0x40000000, 0xE0000004)])


# ---- time base, decrementer ----

def test_time_base_and_decrementer(gc_run, tmp_path):
    a = Asm(TEXT)
    for k in range(10):
        a(asm.divw(21, 22, 23))
    a.label("tb0")
    a(asm.mftb(20))
    a(asm.mullw(21, 22, 23), asm.mulli(21, 22, 3))
    a.label("tb1")
    a(asm.mftb(21))
    a.li32(31, OUT)
    out(a, 20, 0); out(a, 21, 1)
    a(asm.li(3, 1000))
    a.label("mtdec0")
    a(asm.mtdec(3))
    a.label("mfdec0")
    a(asm.mfdec(4)); out(a, 4, 2)
    a(asm.li(3, 100))
    a.label("mtdec1")
    a(asm.mtdec(3), asm.mfmsr(3), asm.ori(3, 3, 0x8000), asm.mtmsr(3))
    a.label("idle1")
    a.ref(asm.b, "idle1")
    a.label("after")
    a(asm.mfmsr(3), asm.rlwinm(3, 3, 0, 17, 15), asm.mtmsr(3))
    a.label("tb2")
    a(asm.mftb(5)); out(a, 5, 3)
    a.li32(3, LOGP)
    a(asm.lwz(9, 0, 3)); out(a, 9, 4)
    a.idle()
    dol, vec = make_dol(a)
    res, log = run_both(gc_run, tmp_path, dol, [(OUT, 5), (LOG, 5)])

    # the expected clock: the cycles of every instruction run, through the idle jump to the decrementer's fire
    words = a.words
    idx = {name: (addr - TEXT) // 4 for name, addr in a.labels.items() if isinstance(name, str)}

    def upto(k):  # cycles once main's instruction k has run (straight-line prefix)
        return sum(cycles(w) for w in words[:k + 1])

    tick = lambda c: c // 12  # noqa: E731
    dec_zero = tick(upto(idx["mtdec1"])) + 100
    fire = (dec_zero + 1) * 12
    assert fire > upto(idx["idle1"])
    hv = vec.resolve()
    h0 = (vec.labels[0x900] - vec.base) // 4
    hw = hv[h0:]
    rfi_at = hw.index(asm.rfi())
    hcyc = [sum(cycles(w) for w in hw[:k + 1]) for k in range(rfi_at + 1)]
    mfdec_at, mftb_at = hw.index(asm.mfdec(4)), hw.index(asm.mftb(4))
    back = fire + hcyc[rfi_at]
    tb2 = back + sum(cycles(w) for w in words[idx["after"]:idx["tb2"] + 1])
    assert res == [tick(upto(idx["tb0"])), tick(upto(idx["tb1"])),
                   (1000 - (tick(upto(idx["mfdec0"])) - tick(upto(idx["mtdec0"])))) & 0xFFFFFFFF,
                   tick(tb2), (LOG & 0x3FFFFFFF) + 20]
    assert (res[0], res[1]) == (15, 16)  # 10 divides + mftb: cycle 191; mullw 4 + mulli 3 + mftb 1: 199
    check_log(log, [(0x900, a.labels["idle1"], 0x8000 | MSR, (dec_zero - tick(fire + hcyc[mfdec_at])) & 0xFFFFFFFF,
                     tick(fire + hcyc[mftb_at]))])


# ---- fres / frsqrte ----

FRES_BASE = [0x7ff800, 0x783800, 0x70ea00, 0x6a0800, 0x638800, 0x5d6200, 0x579000, 0x520800,
             0x4cc800, 0x47ca00, 0x430800, 0x3e8000, 0x3a2c00, 0x360800, 0x321400, 0x2e4a00,
             0x2aa800, 0x272c00, 0x23d600, 0x209e00, 0x1d8800, 0x1a9000, 0x17ae00, 0x14f800,
             0x124400, 0x0fbe00, 0x0d3800, 0x0ade00, 0x088400, 0x065000, 0x041c00, 0x020c00]
FRES_DEC = [0x3e1, 0x3a7, 0x371, 0x340, 0x313, 0x2ea, 0x2c4, 0x2a0, 0x27f, 0x261, 0x245, 0x22a, 0x212, 0x1fb, 0x1e5,
            0x1d1, 0x1be, 0x1ac, 0x19b, 0x18b, 0x17c, 0x16e, 0x15b, 0x15b, 0x143, 0x143, 0x12d, 0x12d, 0x11a, 0x11a,
            0x108, 0x106]
RSQ = [(0x1a7e800, 0x568), (0x17cb800, 0x4f3), (0x1552800, 0x48d), (0x130c000, 0x435), (0x10f2000, 0x3e7),
       (0x0eff000, 0x3a2), (0x0d2e000, 0x365), (0x0b7c000, 0x32e), (0x09e5000, 0x2fc), (0x0867000, 0x2d0),
       (0x06ff000, 0x2a8), (0x05ab800, 0x283), (0x046a000, 0x261), (0x0339800, 0x243), (0x0218800, 0x226),
       (0x0105800, 0x20b), (0x3ffa000, 0x7a4), (0x3c29000, 0x700), (0x38aa000, 0x670), (0x3572000, 0x5f2),
       (0x3279000, 0x584), (0x2fb7000, 0x524), (0x2d26000, 0x4cc), (0x2ac0000, 0x47e), (0x2881000, 0x43a),
       (0x2665000, 0x3fa), (0x2468000, 0x3c2), (0x2287000, 0x38e), (0x20c1000, 0x35e), (0x1f12000, 0x332),
       (0x1d79000, 0x30a), (0x1bf4000, 0x2e6)]


def bits(d):
    return struct.unpack(">Q", struct.pack(">d", d))[0]


def ref_fres(u):
    sign, e, frac = u & (1 << 63), (u >> 52) & 0x7FF, u & ((1 << 52) - 1)
    if e == 0x7FF:
        return u | (1 << 51) if frac else sign
    if e == 0 and frac == 0:
        return sign | 0x7FF0000000000000
    if e < 895:
        return sign | 0x47EFFFFFE0000000
    if e >= 1149:
        return sign
    i = frac >> 37
    f = FRES_BASE[i >> 10] - (FRES_DEC[i >> 10] * (i & 0x3FF) + 1) // 2
    return sign | (0x7FD - e) << 52 | f << 29


def ref_frsqrte(u):
    sign, e, frac = u & (1 << 63), (u >> 52) & 0x7FF, u & ((1 << 52) - 1)
    if e == 0x7FF:
        if frac:
            return u | (1 << 51)
        return 0x7FF8000000000000 if sign else 0
    if e == 0 and frac == 0:
        return sign | 0x7FF0000000000000
    if sign:
        return 0x7FF8000000000000
    if e == 0:
        shift = 53 - frac.bit_length()
        frac, e = (frac << shift) & ((1 << 52) - 1), 1 - shift
    i = ((e & 1) << 15) | (frac >> 37)
    base, dec = RSQ[i >> 11]
    f = base - dec * (i & 0x7FF)
    return (((1022 - ((e - 1023) >> 1)) & 0x7FF) << 52) | f << 26


def to_single_bits(u):
    """A double (bits) rounded to single, nearest even (struct does that; NaNs not used here)."""
    return struct.unpack(">I", struct.pack(">f", struct.unpack(">d", struct.pack(">Q", u))[0]))[0]


ESTIMATE_INPUTS = [1.0, 2.0, 3.0, 0.5, 1.5, 4.0, 0.75, 123.456, math.pi, 1e10, 1e-10, -4.0, -0.3, 0.0, -0.0,
                   math.inf, -math.inf, 5e-324, 2.2e-308, 1e-40, 1e39, 1e300]
EST_NAN = 0x7FF4000000000123          # a signalling NaN: both estimates quiet it, payload kept


def test_estimates_reference_values():
    assert ref_fres(bits(1.0)) == 0x3FEFFF0000000000
    assert ref_frsqrte(bits(1.0)) == 0x3FEFFE8000000000
    assert ref_frsqrte(bits(4.0)) == 0x3FDFFE8000000000
    for x in ESTIMATE_INPUTS:  # estimates within the 750's stated precision of the true value
        if x and math.isfinite(x) and x > 0 and 1e-37 < x < 1e37:
            r = struct.unpack(">d", struct.pack(">Q", ref_fres(bits(x))))[0]
            assert abs(r * x - 1) < 1 / 4096, x
            q = struct.unpack(">d", struct.pack(">Q", ref_frsqrte(bits(x))))[0]
            assert abs(q * math.sqrt(x) - 1) < 1 / 4096, x


def test_fres_frsqrte_tables(gc_run, tmp_path):
    vals = [bits(x) for x in ESTIMATE_INPUTS] + [EST_NAN]
    pairs = [1.0, 2.0, 0.375, 1e30, 3.5, math.inf, 7.25e-3, 0.0]
    a = Asm(TEXT)
    a.li32(3, DATA)
    a.li32(4, OUT)
    for k in range(len(vals)):
        a(asm.lfd(1, 8 * k, 3), asm.fres(2, 1), asm.stfd(2, 16 * k, 4), asm.frsqrte(3, 1), asm.stfd(3, 16 * k + 8, 4))
    a.li32(5, DATA + 0x400)
    a.li32(6, OUT + 0x400)
    for k in range(len(pairs) // 2):
        a(asm.psq_l(5, 8 * k, 5, 0, 0), asm.ps_res(6, 5), asm.psq_st(6, 16 * k, 6, 0, 0),
          asm.ps_rsqrte(7, 5), asm.psq_st(7, 16 * k + 8, 6, 0, 0))
    a.idle()
    data = [(DATA, struct.pack(">%dQ" % len(vals), *vals)), (DATA + 0x400, struct.pack(">%df" % len(pairs), *pairs))]
    dol, _ = make_dol(a, data)
    res, ps = run_both(gc_run, tmp_path, dol, [(OUT, 4 * len(vals)), (OUT + 0x400, 2 * len(pairs))])
    want = []
    for u in vals:
        fr = ref_fres(u) & 0xFFFFFFFFE0000000  # a single-precision result: a NaN's low payload goes
        want += [fr >> 32, fr & 0xFFFFFFFF, ref_frsqrte(u) >> 32, ref_frsqrte(u) & 0xFFFFFFFF]
    assert res == want
    pw = []
    for k in range(0, len(pairs), 2):
        p = [bits(pairs[k]), bits(pairs[k + 1])]
        pw += [to_single_bits(ref_fres(u)) for u in p] + [to_single_bits(ref_frsqrte(u)) for u in p]
    assert ps == pw


# ---- FPSCR ----

def test_fpscr_modes_and_flags(gc_run, tmp_path):
    a = Asm(TEXT)
    a.li32(3, DATA)
    a.li32(4, OUT)
    a(asm.li(5, 0))
    a(asm.lfd(1, 0, 3), asm.lfd(3, 8, 3), asm.lfd(4, 16, 3), asm.lfd(5, 24, 3), asm.lfs(6, 32, 3),
      asm.lfd(0, 40, 3), asm.lfd(7, 48, 3), asm.lfd(8, 56, 3))
    for rn in range(4):                                       # 1/3 single, fctiw +-2.5, 1 + 2^-60
        a(asm.mtfsfi(7, rn), asm.fdivs(9, 1, 3), asm.stfs(9, 0, 4),
          asm.fctiw(10, 4), asm.li(5, 4), asm.stfiwx(10, 4, 5),
          asm.fctiw(10, 5), asm.li(5, 8), asm.stfiwx(10, 4, 5),
          asm.fadd_(11, 1, 8), asm.stfd(11, 16, 4), asm.addi(4, 4, 24))
    a(asm.mtfsfi(7, 4), asm.fmuls(12, 6, 6), asm.stfs(12, 0, 4))       # NI: 2^-140 flushes to 0
    a(asm.mtfsfi(7, 0), asm.fmuls(12, 6, 6), asm.stfs(12, 4, 4))       # IEEE: single denormal
    a(asm.fdiv(15, 1, 0))                                               # inf, for later
    a(asm.mtfsf(0xFF, 0), asm.fdiv(13, 1, 0), asm.mffs(14), asm.stfd(14, 8, 4))   # ZX
    a(asm.mtfsf(0xFF, 0), asm.fdiv(13, 1, 3), asm.mffs(14), asm.stfd(14, 16, 4))  # XX
    a(asm.fmul(13, 7, 7), asm.mffs(14), asm.stfd(14, 24, 4))                      # OX (and XX)
    a(asm.mtfsf(0xFF, 0), asm.fsub(13, 15, 15), asm.mffs(14), asm.stfd(14, 32, 4))  # VX
    a(asm.fadd_(16, 1, 1, 1), asm.mfcr(6), asm.stw(6, 40, 4))           # fadd.: CR1 = FX FEX VX OX
    a(asm.mcrfs(2, 0), asm.mfcr(6), asm.stw(6, 44, 4), asm.mffs(14), asm.stfd(14, 48, 4))
    a(asm.mtfsf(0xFF, 0))
    a.idle()
    data = struct.pack(">4d", 1.0, 3.0, 2.5, -2.5) + struct.pack(">f", 2.0 ** -70) + bytes(4)
    data += struct.pack(">3d", 0.0, 1e300, 2.0 ** -60)
    dol, _ = make_dol(a, [(DATA, data)])
    (res,) = run_both(gc_run, tmp_path, dol, [(OUT, 4 * 6 + 14)])
    one, one_up = (0x3FF00000, 0), (0x3FF00000, 1)
    exp = []
    for third, c25, cm25, sum_ in ((0x3EAAAAAB, 2, -2, one), (0x3EAAAAAA, 2, -2, one),
                                   (0x3EAAAAAB, 3, -2, one_up), (0x3EAAAAAA, 2, -3, one)):
        exp += [third, c25 & 0xFFFFFFFF, cm25 & 0xFFFFFFFF, 0] + list(sum_)  # word 3 is not written
    assert res[:24] == exp
    tail = res[24:]
    assert tail[0] == 0 and tail[1] == 0x00000200
    fpscr = [tail[k] for k in (3, 5, 7, 9, 13)]
    assert [tail[k] for k in (2, 4, 6, 8, 12)] == [0xFFF80000] * 5
    assert fpscr == [0x84000000, 0x82000000, 0x92000000, 0xA0000000, 0x20000000]
    assert tail[10] == 0x0A000000 and tail[11] == 0x0AA00000


# ---- exceptions from lifted code ----

def test_exceptions_from_lifted_code(gc_run, tmp_path):
    a = Asm(TEXT)
    a.li32(31, OUT)
    a(asm.li(3, 5))
    a.label("trap1")
    a(asm.twi(4, 3, 5))                                # trap: equal
    a(asm.twi(4, 3, 6))                                # not taken
    a.label("trap2")
    a(asm.tw(31, 0, 0))                                # trap: always
    a.label("sc1")
    a(asm.sc())
    a.li32_label(3, "user")
    a(asm.mtspr(26, 3), asm.mfmsr(4), asm.ori(4, 4, 0x4000), asm.mtspr(27, 4), asm.rfi())
    a.label("user")
    a(asm.mfmsr(5))                                    # program: privileged
    a(asm.mfmsr(5)); out(a, 5, 0)
    a(asm.mfmsr(4), asm.rlwinm(4, 4, 0, 19, 17), asm.mtmsr(4))
    a.li32(6, DATA)
    a.label("fpu1")
    a(asm.lfs(1, 0, 6))                                # FP unavailable, then again with FP on
    a(asm.stfs(1, 4, 31), asm.mfmsr(5)); out(a, 5, 2)
    a(asm.mfspr(3, 920), asm.rlwinm(3, 3, 0, 3, 1), asm.mtspr(920, 3))   # HID2[PSE] off
    a.label("ps_ill")
    a(asm.ps_add(1, 1, 1))                             # program: illegal
    a(asm.oris(3, 3, 0x2000), asm.mtspr(920, 3))
    a.li32(7, 0xCC003004)                              # PI: EXI interrupts on
    a(asm.li(8, 0x10), asm.stw(8, 0, 7))
    a.li32(7, 0xCC006800)                              # EXI 0: transfer interrupt on, transfer
    a(asm.li(8, 4), asm.stw(8, 0, 7), asm.li(8, 1), asm.stw(8, 12, 7))
    a(asm.mfmsr(4), asm.ori(4, 4, 0x8000))
    a.label("ext")
    a(asm.mtmsr(4))                                    # external, right after
    a(asm.rlwinm(4, 4, 0, 17, 15), asm.mtmsr(4))
    a.li32(3, LOGP)
    a(asm.lwz(9, 0, 3)); out(a, 9, 3)
    a.idle()
    dol, _ = make_dol(a, [(DATA, struct.pack(">f", 6.5))])
    res, log = run_both(gc_run, tmp_path, dol, [(OUT, 4), (LOG, 35)])
    L = a.labels
    assert res == [MSR, 0x40D00000, MSR, (LOG & 0x3FFFFFFF) + 7 * 20]
    check_log(log, [(0x700, L["trap1"], 0x00020000 | MSR, ANY, ANY),
                    (0x700, L["trap2"], 0x00020000 | MSR, ANY, ANY),
                    (0xC00, L["sc1"] + 4, MSR, ANY, ANY),
                    (0x700, L["user"], 0x00040000 | 0x4000 | MSR, ANY, ANY),
                    (0x800, L["fpu1"], MSR & ~0x2000, ANY, ANY),
                    (0x700, L["ps_ill"], 0x00080000 | MSR, ANY, ANY),
                    (0x500, L["ext"] + 4, 0x8000 | MSR, ANY, ANY)])


def test_lifter_emits_the_gekko_guards(tmp_path):
    from .recomp import recompile
    from .dol import Image
    a = Asm(TEXT)
    a(asm.li(3, 1), asm.mftb(4), asm.divw(5, 3, 3), asm.twi(4, 3, 1), asm.lfs(1, 0, 3), asm.ps_add(2, 1, 1),
      asm.mfmsr(6), asm.blr())
    recompile(Image(build([(TEXT, a.data())])), str(tmp_path))
    text = (tmp_path / "gc_rc_000.c").read_text()
    assert "RC_BLOCK(0x80003100u, 26, 8);" in text            # li 1, mftb 1, divw 19, twi 1, lfs 1, ps_add 1, ...
    assert "R(4) = RC_MFSPR_T(268, 24);" in text              # the 24 cycles after mftb
    assert "RC_TRAP(0x8000310Cu, 4);" in text
    assert "RC_GKCHECK_T(0x80003110u, 0x00000000u, 4);" in text
    assert "RC_GKCHECK_T(0x80003114u, 0x20000000u, 3);" in text
    assert "RC_FSET_L(1, ppc_lfs_bits(RC_LWZ(0x80003110u, ea_)));" in text
    assert "RC_PRIV_T(0x80003118u, 2);" in text


# ---- quantisation edges, lfs / stfs without rounding ----

def test_quantisation_edges(gc_run, tmp_path):
    a = Asm(TEXT)
    a.li32(3, DATA)
    a.li32(4, OUT)
    for i, gqr in ((2, 4), (3, 6), (4, 0x3F070000 | 0x3F07)):     # st u8; st s8; ld / st s16 with scale -1
        a.li32(5, gqr)
        a(asm.mtspr(912 + i, 5))
    a(asm.psq_l(1, 0, 3, 0, 0), asm.psq_st(1, 0, 4, 0, 2))             # (300, -5.5) as u8: 255, 0
    a(asm.psq_l(1, 8, 3, 0, 0), asm.psq_st(1, 4, 4, 0, 3))             # (-200, 100.7) as s8: -128, 100
    a(asm.psq_l(1, 16, 3, 0, 0), asm.psq_st(1, 8, 4, 0, 4))            # (1000, -3) * 2^-1 as s16: 500, -1
    a(asm.psq_l(2, 24, 3, 0, 4), asm.psq_st(2, 12, 4, 0, 0))           # s16 (-32768, 32767) * 2: floats
    a(asm.psq_l(3, 0, 3, 1, 0), asm.psq_st(3, 20, 4, 0, 0))            # W = 1: (300, 1.0)
    a(asm.lfd(4, 32, 3), asm.psq_st(4, 28, 4, 1, 0), asm.stfs(4, 32, 4))  # 1 + 3 * 2^-25: truncated, both
    a(asm.lfs(5, 40, 3), asm.stfs(5, 36, 4), asm.psq_st(5, 40, 4, 1, 0))  # a signalling NaN stays signalling
    a(asm.lfs(6, 44, 3), asm.stfs(6, 44, 4))                           # a single denormal, through a double
    a.idle()
    data = struct.pack(">6f", 300.0, -5.5, -200.0, 100.7, 1000.0, -3.0) + struct.pack(">2h", -32768, 32767)
    data += bytes(4) + struct.pack(">d", 1.0 + 3 * 2.0 ** -25) + struct.pack(">2I", 0x7FA00001, 0x00012345)
    dol, _ = make_dol(a, [(DATA, data)])
    (res,) = run_both(gc_run, tmp_path, dol, [(OUT, 12)])
    f = lambda x: struct.unpack(">I", struct.pack(">f", x))[0]  # noqa: E731
    assert res == [0xFF000000, 0x80640000, 0x01F4FFFF, f(-65536.0), f(65534.0), f(300.0), f(1.0), 0x3F800000,
                   0x3F800000, 0x7FA00001, 0x7FA00001, 0x00012345]
