"""tools/disasm/ranges.py and BinaryImage.get_section_at_va by bisection: the same answers as the linear scans they
replace (an XBE of hundreds of sections made those scans most of the disassembler's time)."""

import random

from tools.disasm import ranges
from tools.disasm.loader import BinaryImage, SectionInfo


def test_contains_fn_matches_any():
    rng = random.Random(7)
    for _ in range(200):
        rs = []
        for _ in range(rng.randrange(0, 12)):
            lo = rng.randrange(0, 400)
            rs.append((lo, lo + rng.randrange(-3, 60)))          # empty, touching and overlapping ones included
        f = ranges.contains_fn(rs)
        for a in range(-5, 480):
            assert f(a) == any(lo <= a < hi for lo, hi in rs), (rs, a)


def _image(sections):
    return BinaryImage(filepath="x", raw_data=b"", base_address=0x10000, image_size=0x100000, entry_point=0,
                       kernel_thunk_addr=0, sections=sections)


def test_section_at_va_matches_scan():
    rng = random.Random(11)
    for _ in range(60):
        secs = []
        for k in range(rng.randrange(0, 10)):
            va = rng.randrange(0x1000, 0x9000, 0x10)
            secs.append(SectionInfo(name="s%d" % k, virtual_addr=va, virtual_size=rng.randrange(0, 0x1800),
                                    raw_addr=0, raw_size=0, writable=False, executable=rng.random() < 0.5,
                                    flags="PRE" if rng.random() < 0.3 else ""))
        img = _image(secs)
        for va in list(range(0xF00, 0xB000, 0x2F)) + [s.virtual_addr for s in secs] + \
                [s.virtual_addr + s.virtual_size for s in secs]:
            assert img.get_section_at_va(va) is img._section_at_va_scan(va), va
    img = _image([])
    assert img.get_section_at_va(0x2000) is None
    img.sections.append(SectionInfo(name="late", virtual_addr=0x2000, virtual_size=0x100, raw_addr=0, raw_size=0,
                                    writable=False, executable=True, flags=""))
    assert img.get_section_at_va(0x2000).name == "late"    # a section added after a lookup is seen
