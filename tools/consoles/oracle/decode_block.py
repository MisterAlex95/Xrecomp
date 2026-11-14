#!/usr/bin/env python3
"""Decodes the « XBA-BLOCK <when> <hex> » line an oracle printed (XBA-149) into a verdict.

    <oracle output> | decode_block.py --endian little --name cpu --test-id "CPU "

Layout: consoles/<console>/tests/homebrew/README.md, « Result protocol ». Prints
« RESULT <name> PASS|FAIL|TIMEOUT|NOBLOCK ... » then the header fields and the table. Exit 0 on PASS, 1 otherwise.
"""
import argparse
import sys

MAGIC, PASS, FAIL = 0x58424152, 0x50415353, 0x4641494C


def fourcc(v: int) -> str:
    s = v.to_bytes(4, "big").decode("latin-1")
    return s if s.isprintable() else f"0x{v:08x}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--endian", choices=("big", "little"), required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--test-id", default=None, help="expected fourcc")
    args = ap.parse_args()

    line = next((ln.split() for ln in sys.stdin if ln.startswith("XBA-BLOCK ")), None)
    if not line or len(line) < 3 or len(line[2]) != 1024:
        print(f"RESULT {args.name} NOBLOCK (the oracle printed no result block)")
        return 1
    when, raw = line[1], bytes.fromhex(line[2])
    w = [int.from_bytes(raw[i:i + 4], args.endian) for i in range(0, len(raw), 4)]
    magic, status, test_id, detail, checksum, count, frames, stage = w[:8]
    table = w[8:]

    if magic != MAGIC:
        verdict = "NOBLOCK (no XBAR magic: main never ran)"
    elif status == PASS:
        verdict = "PASS"
    elif status == FAIL:
        verdict = f"FAIL (detail {detail}: first failing check #{detail - 1})"
    elif status == 0:
        verdict = "TIMEOUT (status still 0)"
    else:
        verdict = f"FAIL (unknown status 0x{status:08x})"
    if verdict == "PASS" and args.test_id and fourcc(test_id) != args.test_id:
        verdict = f"FAIL (test_id {fourcc(test_id)!r}, expected {args.test_id!r})"

    print(f"RESULT {args.name} {verdict} after {when}")
    print(f"  test_id {fourcc(test_id)!r} detail {detail} checksum 0x{checksum:08x} count {count} "
          f"frames {frames} stage 0x{stage:08x}")
    n = min(count, len(table))
    for i in range(0, n, 8):
        print(f"  table[{i:3}] " + " ".join(f"{v:08x}" for v in table[i:min(i + 8, n)]))
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
