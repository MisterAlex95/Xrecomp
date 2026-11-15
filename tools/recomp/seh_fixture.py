"""The synthetic SEH title of XBA-112: build it, lift it, write the fixture.

tests/seh_dispatch/fixture.s is our own x86 (no CRT or XDK code): EH3 frames
with __try/__except and __try/__finally, C++ frames with a catch-all and a
destructor, and minimal frame handlers of our own that do what
_except_handler3 and __CxxFrameHandler do. This module

  build   assembles it (as --32, ld -m elf_i386) into fixture.json: the bytes
          of .text and .rdata and the function bounds. Needs binutils; the
          JSON is checked in, so nothing else does.
  lift    runs the real pipeline (FunctionTranslator, the funclet entries)
          on fixture.json and returns the C, with a dispatch table and the
          image bytes, for tests/seh_dispatch (CMake, x86-64 and arm64) and
          tools/recomp/test_seh_dispatch.py.

  python -m tools.recomp.seh_fixture build   # after editing fixture.s
  python -m tools.recomp.seh_fixture write   # regenerate fixture_gen.c
"""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FIXTURE_DIR = os.path.join(ROOT, "tests", "seh_dispatch")
SOURCE = os.path.join(FIXTURE_DIR, "fixture.s")
FIXTURE_JSON = os.path.join(FIXTURE_DIR, "fixture.json")
GENERATED_C = os.path.join(FIXTURE_DIR, "fixture_gen.c")

TEXT_VA = 0x00010000
RDATA_VA = 0x00020000
FUNCTIONS = ("g_raises", "g_throw", "f_outer", "f_inner", "my_handler3",
             "f_cxx", "f_cxx_inner", "ehstub_cxx", "ehstub_inner",
             "my_cxx_handler", "f_div", "f_probe", "my_lj_unwind")


def build(source=SOURCE, out=FIXTURE_JSON):
    with tempfile.TemporaryDirectory() as tmp:
        obj, elf = os.path.join(tmp, "f.o"), os.path.join(tmp, "f.elf")
        subprocess.run(["as", "--32", "-o", obj, source], check=True)
        subprocess.run(["ld", "-m", "elf_i386", "-e", "0",
                        f"-Ttext=0x{TEXT_VA:X}",
                        f"--section-start=.rdata=0x{RDATA_VA:X}",
                        "-o", elf, obj], check=True)
        blobs = {}
        for sect in (".text", ".rdata"):
            path = os.path.join(tmp, sect.strip(".") + ".bin")
            subprocess.run(["objcopy", "-O", "binary", f"--only-section={sect}",
                            elf, path], check=True)
            with open(path, "rb") as f:
                blobs[sect] = f.read()
        syms = {}
        for line in subprocess.run(["nm", elf], check=True, capture_output=True,
                                   text=True).stdout.splitlines():
            parts = line.split()
            if len(parts) == 3:
                syms[parts[2]] = int(parts[0], 16)
    starts = sorted(syms[n] for n in FUNCTIONS) + [syms["text_end"]]
    functions = {n: [syms[n], starts[starts.index(syms[n]) + 1]]
                 for n in FUNCTIONS}
    data = {"text_va": TEXT_VA, "text": blobs[".text"].hex(),
            "rdata_va": RDATA_VA, "rdata": blobs[".rdata"].hex(),
            "functions": functions}
    with open(out, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, sort_keys=True)
        f.write("\n")
    return data


def load(path=FIXTURE_JSON):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def lift(data=None, *, seh_anchors=True):
    """C for the fixture: the lifted functions, their funclet entries, a
    dispatch table (recomp_lookup) and the image bytes."""
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator, _func_ident

    data = data or load()
    text, rdata = bytes.fromhex(data["text"]), bytes.fromhex(data["rdata"])
    image = text + rdata
    saved = {k: getattr(config, k) for k in (
        "_SECTIONS", "SECTIONS", "_configured_from", "TEXT_VA_START",
        "TEXT_VA_END", "RDATA_VA_START", "RDATA_VA_END", "DATA_VA_START",
        "DATA_VA_END", "KERNEL_THUNK_ADDR", "ENTRY_POINT")}
    try:
        config._install(
            [config.Section(".text", data["text_va"], len(text), 0, len(text), True),
             config.Section(".rdata", data["rdata_va"], len(rdata), len(text),
                            len(rdata), False)],
            entry_point=data["text_va"], kernel_thunk_addr=data["rdata_va"],
            origin="seh-fixture")
        db = {}
        for name, (start, end) in data["functions"].items():
            db[start] = {"name": name, "start": hex(start), "end": end,
                         "_addr": start, "size": end - start}
        tr = FunctionTranslator(image, db)
        tr.seh_anchors = seh_anchors
        bodies, entries = [], []
        for addr in sorted(db):
            info = db[addr]
            name = _func_ident(addr, info["name"])
            code = tr.translate_function(addr, info)
            assert code, f"{name} did not translate"
            bodies.append(code)
            entries.append((addr, name))
            for va, ename, ecode in tr.seh_funclet_entries(
                    addr, name, code, tr._seh_frame):
                bodies.append(ecode)
                entries.append((va, ename))
    finally:
        for k, v in saved.items():
            setattr(config, k, v)

    entries.sort()
    out = [
        "/* Generated by tools/recomp/seh_fixture.py from fixture.s -- do not",
        " * edit; regenerate with `python -m tools.recomp.seh_fixture write`. */",
        "#define RECOMP_GENERATED_CODE",
        '#include "recomp_types.h"',
        "#include <math.h>",
        "",
        "typedef void (*recomp_func_t)(void);",
    ]
    out += [f"void {name}(void);" for _, name in entries]
    out += ["void sub_0001F000(void);  /* RAISE, the harness */",
            "void sub_0001F010(void);  /* UNWIND, the harness */",
            "void sub_0001F020(void);  /* COMMIT, the harness */", ""]
    out += bodies
    out += ["", "static const struct { uint32_t va; recomp_func_t fn; } "
            "seh_fixture_table[] = {"]
    out += [f"    {{ 0x{va:08X}u, {name} }}," for va, name in entries]
    out += ["};", "",
            "recomp_func_t recomp_lookup(uint32_t va)",
            "{",
            "    size_t i;",
            "    for (i = 0; i < sizeof seh_fixture_table / sizeof seh_fixture_table[0]; i++)",
            "        if (seh_fixture_table[i].va == va)",
            "            return seh_fixture_table[i].fn;",
            "    return 0;",
            "}", "",
            "/* The harness's own guest functions (a stack switch, a fiber body). */",
            "recomp_func_t seh_fixture_lookup_harness(uint32_t va);",
            "recomp_func_t recomp_lookup_manual(uint32_t va) { return seh_fixture_lookup_harness(va); }",
            "",
            "/* No flat table (span 0): every indirect call takes the lookups above. */",
            "recomp_flat_dispatch_t g_recomp_flat;",
            ""]
    for label, va, blob in (("text", data["text_va"], text),
                            ("rdata", data["rdata_va"], rdata)):
        out.append(f"const uint32_t seh_fixture_{label}_va = 0x{va:08X}u;")
        out.append(f"const uint32_t seh_fixture_{label}_size = {len(blob)}u;")
        out.append(f"const unsigned char seh_fixture_{label}[] = {{")
        for i in range(0, len(blob), 16):
            out.append("    " + ", ".join(f"0x{b:02X}" for b in blob[i:i + 16]) + ",")
        out.append("};")
    out += [f"const uint32_t seh_fixture_{n} = 0x{s:08X}u;"
            for n, (s, _) in sorted(data["functions"].items())]
    out.append("")
    return "\n".join(out)


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "write"
    if cmd == "build":
        build()
        cmd = "write"
    if cmd == "write":
        with open(GENERATED_C, "w", encoding="utf-8", newline="\n") as f:
            f.write(lift())
        print(f"wrote {GENERATED_C}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
