"""
Synthetic XBEs of our own for the discovery tests (XBA-137): several sections
(.text, .rdata, .data, ...) laid out from hand-written x86 with labels, so a
test can say "a vtable in .rdata naming a method only it reaches" without
counting bytes. No game data: every byte comes from the test.

    t = Title()
    text, rdata = t.section(".text"), t.section(".rdata", flags=DATA)
    text.label("entry").hex("558bec").call("ctor").hex("ebfe")
    text.label("ctor").hex("c701").abs("vtbl").hex("c3")
    rdata.label("vtbl").abs("method")
    ...
    path = t.write(tmp / "default.xbe", entry="entry")
    t.va("method")          # the address a label landed at

The header page follows tools.xbe_ident's make_xbe (retail-encoded entry and
thunk table, certificate, a library table); up to four section headers.
"""

import struct

BASE = 0x10000
HDR = 0x1000
CODE = 0x6           # preload | executable
DATA = 0x3           # writable | preload (a loader treats .data/.rdata as data by name anyway)
THUNK_VA = BASE + 0x800


class Section:
    def __init__(self, title, name, flags):
        self.title, self.name, self.flags = title, name, flags
        self.buf = bytearray()
        self.labels = {}
        self.fixups = []        # (offset, kind, label, addend)
        self.va = None

    # Emitters, chainable.
    def label(self, name):
        assert name not in self.title.labels(), name
        self.labels[name] = len(self.buf)
        return self

    def hex(self, s):
        self.buf += bytes.fromhex(s.replace(" ", ""))
        return self

    def raw(self, b):
        self.buf += b
        return self

    def u32(self, v):
        self.buf += struct.pack("<I", v & 0xFFFFFFFF)
        return self

    def f32(self, v):
        self.buf += struct.pack("<f", v)
        return self

    def abs(self, name, addend=0):
        """The absolute address of `name` (+addend), as a dword."""
        self.fixups.append((len(self.buf), "abs", name, addend))
        self.buf += b"\0\0\0\0"
        return self

    def rva(self, name):
        """`name` minus the image base, as a dword: half of a computed
        address (mov eax, BASE ; add eax, rva), which no scan can see."""
        self.fixups.append((len(self.buf), "rva", name, 0))
        self.buf += b"\0\0\0\0"
        return self

    def rel(self, name):
        """A rel32 to `name` from the end of this dword."""
        self.fixups.append((len(self.buf), "rel", name, 0))
        self.buf += b"\0\0\0\0"
        return self

    def rel8(self, name):
        """A rel8 to `name` from the end of this byte (jcc/jmp short)."""
        self.fixups.append((len(self.buf), "rel8", name, 0))
        self.buf += b"\0"
        return self

    def call(self, name):
        return self.hex("e8").rel(name)

    def jmp(self, name):
        return self.hex("e9").rel(name)

    def push(self, name, addend=0):
        return self.hex("68").abs(name, addend)

    def call_kernel(self, slot):
        """call dword ptr [thunk + 4*slot]: the slot-th import of the title."""
        return self.hex("ff15").u32(THUNK_VA + 4 * slot)

    def align(self, n, fill=0xCC):
        while len(self.buf) % n:
            self.buf.append(fill)
        return self

    def pad(self, count, fill=0xCC):
        self.buf += bytes([fill]) * count
        return self


class Title:
    def __init__(self):
        self.sections = []

    def section(self, name, flags=CODE):
        s = Section(self, name, flags)
        self.sections.append(s)
        return s

    def labels(self):
        out = {}
        for s in self.sections:
            out.update(s.labels)
        return out

    def _layout(self):
        off = HDR
        for s in self.sections:
            s.va = BASE + off
            s.raw_off = off
            off += (len(s.buf) + 16 + 0xFFF) & ~0xFFF
        return off

    def va(self, name):
        if self.sections[0].va is None:
            self._layout()
        for s in self.sections:
            if name in s.labels:
                return s.va + s.labels[name]
        raise KeyError(name)

    def write(self, path, entry, imports=(), libs=(("XAPILIB", 5849, 0),),
              title_id=0x58424137, title="Discovery"):
        assert len(self.sections) <= 4 and len(imports) < 64
        end = self._layout()
        for s in self.sections:
            for off, kind, name, addend in s.fixups:
                target = self.va(name) + addend
                if kind == "rel8":
                    v = target - (s.va + off + 1)
                    assert -128 <= v < 128, (name, v)
                    struct.pack_into("<b", s.buf, off, v)
                    continue
                v = (target if kind == "abs" else target - BASE if kind == "rva"
                     else target - (s.va + off + 4))
                struct.pack_into("<I", s.buf, off, v & 0xFFFFFFFF)
        buf = bytearray(end)
        buf[0:4] = b"XBEH"
        struct.pack_into("<4I", buf, 0x104, BASE, HDR, end, 0x178)
        struct.pack_into("<I", buf, 0x118, BASE + 0x200)
        struct.pack_into("<II", buf, 0x11C, len(self.sections), BASE + 0x600)
        struct.pack_into("<I", buf, 0x128, self.va(entry) ^ 0xA8FC57AB)
        struct.pack_into("<I", buf, 0x158, THUNK_VA ^ 0x5B6D40B6)
        for i, o in enumerate(imports):
            struct.pack_into("<I", buf, 0x800 + 4 * i, 0x80000000 | o)
        lib_va = BASE + 0x900
        struct.pack_into("<II", buf, 0x160, len(libs), lib_va)
        if libs:
            struct.pack_into("<I", buf, 0x16C, lib_va)
        for i, (name, build, flags) in enumerate(libs):
            o = 0x900 + 16 * i
            buf[o:o + 8] = name.encode().ljust(8, b"\0")
            struct.pack_into("<4H", buf, o + 8, 1, 0, build, flags)
        struct.pack_into("<I", buf, 0x200 + 8, title_id)
        t = title.encode("utf-16-le")
        buf[0x20C:0x20C + len(t)] = t
        for i, s in enumerate(self.sections):
            raw = bytes(s.buf) + b"\0" * 16
            name_va = BASE + 0x700 + 16 * i
            struct.pack_into("<6I", buf, 0x600 + 56 * i, s.flags, s.va, len(raw), s.raw_off, len(raw), name_va)
            buf[0x700 + 16 * i:0x700 + 16 * i + len(s.name) + 1] = s.name.encode() + b"\0"
            buf[s.raw_off:s.raw_off + len(raw)] = raw
        with open(path, "wb") as f:
            f.write(bytes(buf))
        return str(path)


# Kernel imports of the corpus, by thunk slot.
CORPUS_IMPORTS = (255, 107, 206, 49)    # PsCreateSystemThreadEx, KeInitializeDpc, NtQueueApcThread, HalReturnToFirmware
PS_CREATE, KE_DPC, NT_APC, HAL_RETURN = range(4)


def discovery_corpus(al=16, fill=0xCC):
    """A title with every shape static discovery looks for (XBA-137), its
    functions aligned to `al` with `fill`. Labels starting with T_ are code
    only a run (or static discovery) finds; D_ labels are decoys, data that
    looks like a pointer into code, never to become a function; every other
    .text label not in DATA_LABELS is ordinary code.

    Returns (title, decoys: {name: va}) once laid out (call write())."""
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    d = t.section(".data", DATA)
    a = lambda: x.align(al, fill)

    x.label("entry").hex("558bec")
    for f in ("init_threads", "make_obj", "use_tables", "computed", "sorter",
              "guarded", "switcher", "regswitch", "exit_if"):
        x.call(f)
    x.hex("ebfe")
    a()
    # Callbacks handed to kernel calls: a thread (immediate), a DPC, an APC
    # routine through a register, and a second thread routine packed straight
    # after a noreturn kernel call (fatal) -- swallowed by it without
    # noreturn, and an immediate inside a body is no evidence to the passes
    # before discovery.
    x.label("init_threads").push("T_thread").hex("6a00" * 9).call_kernel(PS_CREATE)
    x.push("T_thread2").hex("6a00" * 9).call_kernel(PS_CREATE)
    x.hex("6a00").push("T_dpc").push("dpc_obj").call_kernel(KE_DPC)
    x.hex("b9").abs("T_apc").hex("6a00 6a00 6a00 51 6afe").call_kernel(NT_APC)
    x.hex("c3")
    a()
    # A constructor installs the vtable; the methods are reached through it.
    x.label("make_obj").hex("b9").abs("obj").hex("c701").abs("vtbl").hex("8bc1c3")
    a()
    x.label("use_tables").hex("8b442404ff1485").abs("ftable").hex("a1").abs("lone_ptr").hex("ffd0c3")
    a()
    # A computed address: base + offset, in no word of the image.
    x.label("computed").hex("b8").u32(BASE).hex("05").rva("T_gap").hex("ffd0c3")
    a()
    x.label("sorter").push("T_cmp").hex("6a04 6a08").push("arr").call("qsort_impl").hex("83c410")
    # Strings and constants referenced by code: immediates that are data.
    x.push("D_str").hex("58").hex("b8").abs("D_blob").hex("c3")
    a()
    x.label("qsort_impl").hex("8b442410 6a00 6a00 ffd0 83c408 c3")
    a()
    # An inline EH3 frame (__try/__except): the filter and the __except block
    # sit after the ret and are named only by the scope table.
    x.label("guarded").hex("558bec 6aff").push("scopetbl").push("eh3")
    x.hex("64a100000000 50 648925 00000000 83ec08 c745fc00000000 33c0 c745fcffffffff")
    x.label("L_ret").hex("8b4df0 64890d00000000 8be5 5d c3")
    x.label("H_filter").hex("b801000000 c3")
    x.label("H_resume").hex("8b65e8 eb").rel8("L_ret")
    a()
    x.label("eh3").hex("b801000000 c3")
    a()
    # A switch whose table is in .rdata, its arms after the default's ret.
    x.label("switcher").hex("8b442404 83f802 77").rel8("S_def").hex("ff2485").abs("rtbl")
    x.label("S_def").hex("33c0 c3")
    x.label("S_c0").hex("b80a000000 c3")
    x.label("S_c1").hex("b80b000000 c3")
    x.label("S_c2").hex("b80c000000 c3")
    a()
    # A switch dispatched through a register, its table inline.
    x.label("regswitch").hex("8b4c2404 83f902 77").rel8("R_def").hex("8b048d").abs("itbl").hex("ffe0")
    x.label("itbl").abs("R_c0").abs("R_c1").abs("R_c2")
    x.label("R_def").hex("33c0 c3")
    x.label("R_c0").hex("b814000000 c3")
    x.label("R_c1").hex("b815000000 c3")
    x.label("R_c2").hex("b816000000 c3")
    a()
    x.label("exit_if").hex("8b442404 85c0 74").rel8("E_ret").call("die")
    x.label("E_ret").hex("c3")
    a()
    # fatal never returns (HalReturnToFirmware); the thread routine follows.
    x.label("fatal").hex("6a00").call_kernel(HAL_RETURN)
    x.label("T_thread2").hex("8b442404 c20400")
    a()
    # die never returns either (it calls fatal), and a method follows it.
    x.label("die").hex("6a02").call("fatal")
    x.label("T_vm3").hex("8b4108 40 c3")
    a()
    x.label("T_thread").hex("558bec ebfe")
    a()
    x.label("T_dpc").hex("558bec 5d c21000")
    a()
    x.label("T_apc").hex("8b442404 c20c00")
    a()
    x.label("T_vm0").hex("558bec 8b4508 5d c20400")
    a()
    x.label("T_vm1").hex("8b4104 c3")
    a()
    x.label("T_vm2").hex("33c0 c3")
    a()
    x.label("T_ft0").hex("b801000000c3")
    a()
    x.label("T_ft1").hex("56 8b742408 8bc6 5e c3")
    a()
    x.label("T_ft2").hex("83ec08 8b0424 83c408 c3")
    a()
    x.label("T_lone").hex("6a01 58 c3")
    a()
    x.label("T_gap").hex("56 8b742408 8b06 5e c3")
    a()
    x.label("T_cmp").hex("8b442404 8b00 8b4c2408 2b01 c3")
    a()
    # Data parked in .text: floats, a string, colours, small integers.
    x.label("D_blob").f32(1.0).f32(0.5).f32(2.0).f32(-1.0)
    a()
    x.label("D_str").raw(b"Video settings\0")
    a()
    x.label("D_rgba").u32(0xFF8040C0).u32(0x80FFFFFF).u32(0x00102030).u32(0x7F7F7F7F)
    a()
    x.label("D_ints").raw(bytes(range(1, 17)))
    a()
    x.label("D_text").raw(b"VWS\x8b\xecPQR the quick brown fox\0")
    a()
    # Code after an unconditional jump, with no reference at all.
    x.label("spin").hex("eb fe")
    x.label("T_after_jmp").hex("57 8b7c2404 8bc7 5f c3")
    a()
    x.label("last").hex("c3")

    # .rdata: the vtable, switch table, scope table, decoys.
    r.label("vtbl").abs("T_vm0").abs("T_vm1").abs("T_vm2").abs("T_vm3").u32(0)
    r.label("rtbl").abs("S_c0").abs("S_c1").abs("S_c2")
    r.u32(0)
    r.label("scopetbl").u32(0xFFFFFFFF).abs("H_filter").abs("H_resume")
    r.label("decoys")
    decoy_refs = (("D_mid", "T_vm0", 1), ("D_in", "T_vm0", 3), ("D_in2", "guarded", 3),
                  ("D_blob4", "D_blob", 4), ("D_str0", "D_str", 0), ("D_str6", "D_str", 6),
                  ("D_rgba0", "D_rgba", 0), ("D_ints2", "D_ints", 2), ("D_text0", "D_text", 0),
                  ("D_text3", "D_text", 3), ("D_ret1", "L_ret", 3), ("D_case", "S_c1", 1))
    for _, name, add in decoy_refs:
        r.abs(name, add)
    r.f32(1.0).f32(0.25).u32(0xFF0000FF).u32(0x3F800000)
    d.label("ftable").abs("T_ft0").abs("T_ft1").abs("T_ft2").u32(0)
    d.label("lone_ptr").abs("T_lone").u32(0)
    d.label("obj").u32(0).u32(0)
    d.label("dpc_obj").pad(32, 0)
    d.label("arr").pad(32, 0)
    t._layout()
    decoys = {k: t.va(name) + add for k, name, add in decoy_refs}
    for name in ("D_blob", "D_str", "D_rgba", "D_ints", "D_text"):
        decoys[name] = t.va(name)
    return t, decoys


DATA_LABELS = ("itbl",)


def code_labels(t):
    """{va: name} of the .text labels that are code (decoys and tables out)."""
    out = {}
    s = t.sections[0]
    for name, off in s.labels.items():
        if not name.startswith("D_") and name not in DATA_LABELS:
            out[s.va + off] = name
    return out


E2E_IMPORTS = (255, 49)                 # PsCreateSystemThreadEx, HalReturnToFirmware


def discovery_e2e_title():
    """A title that boots (tools/compat/test_e2e.py) and reaches, at run
    time, only code the analysis before XBA-137 could miss:

      entry      calls a vtable method through an object a constructor made,
                 a gap function through a computed address (base + offset),
                 starts a thread, then spins
      thread_fn  packed straight after `fatal` (push 0; call
                 [HalReturnToFirmware]): without the noreturn rule the body
                 of fatal swallows it; waits on a zero dword for ever
      m1         the vtable's second slot, the one called
      gap_fn     after zero fill, named nowhere: only the computed call
      decoys     an .rdata table of words into the middle of that code

    With static discovery the first prepare finds all of it: the run records
    nothing to seed (0 healing rounds). Returns the laid-out Title."""
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    d = t.section(".data", DATA)
    x.label("entry").hex("558bec")
    x.hex("6a00").call("exit_if").hex("83c404")                 # exit_if(0): returns
    x.call("make_obj").hex("8bc8 8b11 ff5204")                  # obj->vtbl[1]()
    x.hex("b8").u32(BASE).hex("05").rva("gap_fn").hex("ffd0")   # computed call
    x.push("thread_fn").hex("6a00" * 9).call_kernel(0)          # PsCreateSystemThreadEx
    x.label("spin").hex("ebfe").align(16)
    x.label("make_obj").hex("b9").abs("obj").hex("c701").abs("vtbl").hex("8bc1 c3").align(16)
    x.label("exit_if").hex("8b442404 85c0 74").rel8("E_ret").call("fatal")
    x.label("E_ret").hex("c3").align(16)
    x.label("fatal").hex("6a00").call_kernel(1)
    x.label("thread_fn").hex("833d").abs("flag").hex("00 74f7 c20400").align(16)
    x.label("m0").hex("33c0 c3").align(16)
    x.label("m1").hex("b801000000 c3").align(16, 0x00)
    x.label("gap_fn").hex("56 33f6 46 8bc6 5e c3").align(16, 0x00)
    x.label("end").hex("c3")
    r.label("vtbl").abs("m0").abs("m1").u32(0)
    r.label("decoys").abs("thread_fn", 2).abs("gap_fn", 1).abs("m1", 1).abs("fatal", 2).abs("entry", 1)
    r.f32(1.0).u32(0xFF0000FF)
    d.label("obj").u32(0).u32(0)
    d.label("flag").u32(0)
    t._layout()
    return t


E2E_DECOYS = (("thread_fn", 2), ("gap_fn", 1), ("m1", 1), ("fatal", 2), ("entry", 1))


# Frameless x87 routines (XBA-140): a run-time math library's handlers, the
# shape of the MSVC CRT's hand-written math code. No prologue, no padding
# between them, reached only through a dispatch table. (name, hex)
X87_ROUTINES = (
    ("X_sin", "d9fe c3"),                                   # fsin; ret
    ("X_cos", "d9ff c3"),                                   # fcos; ret
    ("X_tan", "d9f2 ddd8 c3"),                              # fptan; fstp st0; ret
    ("X_sqrt", "d9fa c3"),                                  # fsqrt; ret
    # fxam; fwait; fnstsw ax; sahf; jp nan; fsin; ret; nan: fstp st0; fldz; ret
    ("X_cls", "d9e5 9b dfe0 9e 7a03 d9fe c3 ddd8 d9ee c3"),
    ("X_arg", "dd442404 d9fe c3"),                          # fld qword [esp+4]; fsin; ret
    ("X_zero", "ddd8 d9ee c3"),                             # fstp st0; fldz; ret
    ("X_nan", "ddd8 d9e8 d9ee def9 c3"),                    # 1/0 -> inf; ret
    ("X_pow", "d9c9 d9f1 d9e8 d9c1 d9f8 c3"),               # fxch; fyl2x; ...
)


RGBA_DECOYS = (0x03, 0x11, 0x27, 0x35, 0x49, 0x5b)


def packed_table_title(al=16, fill=0xCC):
    """A title with packed (unaligned) dispatch tables naming frameless x87
    routines (XBA-140), and decoys that look like such tables. Labels X_ are
    the routines, the targets; nothing calls them directly (X_in1 and
    X_in2 lie inside the body of owner, whose branch runs past them).

    The tables, in .rdata:
      mtab   records of 13 bytes {char name[4]; u8 kind; void *f1, *f2},
             packed: the pointer fields fall at every phase of a dword
      ntab   a variable-length name followed by four contiguous handler
             pointers, the run starting off a dword boundary
      otab   records of 6 bytes {u16 id; void *f}
    Decoys (never a function): a string holding one address, an unaligned
    run of words into the middle of code, colours whose unaligned windows
    read as code addresses, an unaligned float array.

    Returns (title, decoy addresses) once laid out."""
    t = Title()
    x = t.section(".text")
    r = t.section(".rdata", DATA)
    a = lambda: x.align(al, fill)
    x.label("entry").hex("558bec").call("user").call("owner").hex("ebfe")
    a()
    # The dispatcher: the record's address as an immediate, then an indirect
    # call through the field (`call [edx+5]`): no scan sees the target.
    x.label("user").hex("ba").abs("mtab").hex("8b442404 6bc00d 03d0 ff5205 c3")
    # The routines, packed straight after user's ret.
    for name, code in X87_ROUTINES:
        x.label(name).hex(code)
    a()
    x.label("after").hex("558bec 8b4508 5d c3")
    a()
    # Two routines inside a body: owner's branch runs past them, so the body
    # covers them and may not be cut there (they become aliases).
    x.label("owner").hex("8b442404 85c0 74").rel8("O_tail").hex("c3")
    x.label("X_in1").hex("d9e1 c3")                         # fabs; ret
    x.label("X_in2").hex("d9e0 c3")                         # fchs; ret
    x.label("O_tail").hex("33c0 c3")
    a()
    x.label("last").hex("c3")

    r.raw(b"\x01")                       # the tables start off a dword boundary
    r.label("mtab")
    for name, f1, f2 in ((b"sin\0", "X_sin", "X_nan"), (b"cos\0", "X_cos", "X_nan"),
                         (b"tan\0", "X_tan", "X_cls"), (b"sqr\0", "X_sqrt", "X_zero"),
                         (b"arg\0", "X_arg", "X_cls")):
        r.raw(name).raw(b"\x07").abs(f1).abs(f2)
    r.u32(0)
    r.raw(b"pow\0\x02").label("ntab").abs("X_pow").abs("X_zero").abs("X_nan").abs("X_cls")
    r.u32(0).raw(b"\x00")
    # otab: records of 6 bytes {u16 id; void *f}, from offset 2 of a dword.
    while len(r.buf) % 4 != 0:
        r.raw(b"\x00")
    r.raw(b"\x00\x00").label("otab")
    for n, f in enumerate(("X_in1", "X_in2", "X_sin")):
        r.raw(bytes([n, 0])).abs(f)
    r.u32(0)
    # Decoys, every word of them off a dword boundary (an aligned one is the
    # older table-word pass's to judge).
    def phase(p):
        while len(r.buf) % 4 != p:
            r.raw(b"_")
    r.raw(b"C:\\path\\")
    phase(3)
    r.label("D_strptr").abs("X_cls", 2).raw(b"file.txt\0")
    phase(1)
    r.label("D_run").abs("user", 3).abs("user", 6).abs("X_cls", 2).abs("after", 1)
    r.raw(b"\x00\x00\x09")
    # Colours 0x0110GG00: their windows one byte in read 0x000110GG, in .text.
    phase(0)
    r.label("D_rgba")
    for gg in RGBA_DECOYS:
        r.u32(0x01000000 | (((BASE + HDR) >> 8 & 0xFF) << 16) | (gg << 8))
    r.raw(b"\x00\x00\x00")
    phase(2)
    r.label("D_floats")
    for v in (0.0, 1.0, 0.5, -1.0, 3.14159, 1e-3):
        r.f32(v)
    t._layout()
    decoys = {"D_strptr": t.va("X_cls") + 2, "D_run0": t.va("user") + 3,
              "D_run1": t.va("user") + 6, "D_run2": t.va("X_cls") + 2,
              "D_run3": t.va("after") + 1}
    text = t.sections[0]
    entries = {text.va + off for off in text.labels.values()}
    for gg in RGBA_DECOYS:
        if text.va + gg not in entries:     # (a window may name a real entry, by layout)
            decoys["D_rgba%02x" % gg] = text.va + gg      # what the window reads
    return t, decoys
