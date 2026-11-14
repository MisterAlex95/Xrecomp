"""`python3 -m n64_tools.coverage ROM ELF [--discovered FILE] [--json OUT]`: how much of a program's code the
discovery finds, measured against the function symbols of its ELF (XBA-169).

Test-only: the ELF (our homebrew keeps it next to the ROM) is read here to MEASURE the discovery, never given to it:
n64_tools.recomp sees the ROM alone. A symbol counts as found when a discovered function starts at its address, and
as covered when its address is inside a lifted entry (lifted functions resume at any instruction, so covered code
runs natively too). Prints a summary and the symbols missed; exit 0.
"""

import argparse
import json
import struct
import sys

from .recomp import build_functions, read_discovered
from .rom import load_rom

SHF_EXECINSTR = 4
STT_FUNC = 2


def elf_functions(path):
    """[(name, addr, size)] of the STT_FUNC symbols with a size in executable sections of a big-endian ELF32."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 2:
        raise ValueError("%s: not a big-endian ELF32" % path)
    shoff = struct.unpack_from(">I", d, 32)[0]
    shentsize, shnum = struct.unpack_from(">HH", d, 46)
    secs = [struct.unpack_from(">10I", d, shoff + k * shentsize) for k in range(shnum)]
    out = []
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for k in range(s[5] // 16):
            name_off, value, size, info, _other, shndx = struct.unpack_from(">IIIBBH", d, s[4] + 16 * k)
            if info & 0xF != STT_FUNC or not size or shndx == 0 or shndx >= shnum:
                continue
            if not secs[shndx][2] & SHF_EXECINSTR:
                continue
            p = strtab[4] + name_off
            name = d[p:d.index(b"\0", p)].decode("ascii", "replace")
            out.append((name, value, size))
    return sorted(set(out), key=lambda t: t[1])


def measure(rom, elf_path, discovered=()):
    funcs, stats = build_functions(rom, list(discovered))
    starts = {f.start for f in funcs}
    ranges = sorted((f.start, f.end) for f in funcs)
    syms = elf_functions(elf_path)
    found, covered, missed = 0, 0, []
    import bisect
    keys = [r[0] for r in ranges]
    for name, addr, size in syms:
        if addr in starts:
            found += 1
            covered += 1
            continue
        k = bisect.bisect_right(keys, addr) - 1
        inside = k >= 0 and ranges[k][0] <= addr < ranges[k][1]
        if inside:
            covered += 1
        missed.append({"name": name, "addr": "0x%08X" % addr, "size": size, "covered": inside})
    n = max(1, len(syms))
    return {"rom": rom.name, "symbols": len(syms), "found": found, "covered": covered,
            "found_percent": round(100.0 * found / n, 2), "covered_percent": round(100.0 * covered / n, 2),
            "functions": len(funcs), "discovered_records": len(list(discovered)), "missed": missed,
            "static": {k: v for k, v in stats.items() if k.startswith("static_")}}


def main(argv=None):
    ap = argparse.ArgumentParser(prog="n64_tools.coverage", description=__doc__.splitlines()[0])
    ap.add_argument("rom")
    ap.add_argument("elf")
    ap.add_argument("--discovered")
    ap.add_argument("--json", help="write the report there")
    args = ap.parse_args(argv)
    rep = measure(load_rom(args.rom), args.elf, read_discovered(args.discovered))
    print("%s: %d / %d function symbols found (%.2f%%), %d covered by lifted code (%.2f%%), %d functions lifted"
          % (rep["rom"], rep["found"], rep["symbols"], rep["found_percent"], rep["covered"], rep["covered_percent"],
             rep["functions"]))
    for m in rep["missed"]:
        print("  missed %s %s (%d bytes)%s" % (m["addr"], m["name"], m["size"], ", covered" if m["covered"] else ""))
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(rep, f, indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
