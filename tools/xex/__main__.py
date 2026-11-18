"""python3 -m tools.xex FILE.xex [--key-file KEY]... [--json FILE] [--image OUT] [--plain OUT.xex]

Prints what the XEX2 holds (headers, imports by library and ordinal, sections, .pdata). Keys are never stored in the
repository: --key-file takes a file of your own (16 raw bytes or 32 hex digits), as does $XEX_KEY_FILE. --plain
writes the XEX back with its image decrypted and expanded (what consoles/x360's x360_run loads).
"""
import argparse
import json
import sys

from .xex import IMPORT_THUNK, XexError, load_keys, parse_xex, plain_xex, summary, version_str


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python3 -m tools.xex", description=__doc__.split("\n\n")[1])
    ap.add_argument("xex")
    ap.add_argument("--key-file", action="append", default=[], help="AES key file (repeatable)")
    ap.add_argument("--json", help="write the summary as JSON")
    ap.add_argument("--image", help="write the decrypted, expanded PE image")
    ap.add_argument("--plain", help="write the XEX2 with its image decrypted and expanded (for x360_run)")
    a = ap.parse_args(argv)
    try:
        keys = load_keys(a.key_file)
        with open(a.xex, "rb") as f:
            raw = f.read()
        img = parse_xex(raw, keys)
    except (OSError, XexError) as ex:
        print("error: %s" % ex, file=sys.stderr)
        return 1
    s = summary(img)
    print("XEX2 %s: entry %08X, base %08X, image %d bytes, %s, compression %s, %d pages of %d KB" % (
        a.xex, img.entry, img.base, img.image_size, "encrypted" if img.encryption else "not encrypted",
        s["compression"], len(img.pages), img.page_size // 1024))
    if img.execution:
        e = img.execution
        print("  title %08X media %08X version %s disc %d/%d" % (e["title_id"], e["media_id"], e["version"],
                                                                e["disc"], e["disc_count"]))
    if img.original_name:
        print("  original PE name: %s" % img.original_name)
    if img.tls:
        print("  TLS: %(slots)d slots, raw data %(raw_data_address)08X (%(data_size)d bytes)" % img.tls)
    print("  stack %#x, headers: %s" % (img.stack_size, ", ".join(s["headers"])))
    for name, ver in img.static_libraries:
        print("  static library %s %s" % (name, ver))
    for lib in img.libraries:
        thunks = sum(1 for r in lib.records if r.kind == IMPORT_THUNK)
        print("  import %s %s: %d records (%d thunks)" % (lib.name, version_str(lib.version), len(lib.record_addrs),
                                                         thunks))
        if lib.records:
            print("    ordinals: " + " ".join("%d%s" % (r.ordinal, "()" if r.kind == IMPORT_THUNK else "")
                                          for r in lib.records))
    for sec in img.sections:
        print("  section %-8s %08X %8X %08X%s" % (sec.name, sec.va, sec.size, sec.flags, " code" if sec.code else ""))
    if img.image is not None:
        print("  .pdata: %d functions" % len(img.pdata))
    else:
        print("  image not available: %s" % img.image_note)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(s, f, indent=1)
    if a.image and img.image is not None:
        with open(a.image, "wb") as f:
            f.write(img.image)
    if a.plain:
        if img.image is None:
            print("error: --plain: %s" % img.image_note, file=sys.stderr)
            return 1
        with open(a.plain, "wb") as f:
            f.write(plain_xex(raw, img))
    return 0


sys.exit(main())
