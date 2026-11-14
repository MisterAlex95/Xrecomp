"""check_mcard.py CARD [--empty] -- checks a 128 KB PS1 memory card image (.mcr) after the mcard homebrew
(consoles/ps1/tests/homebrew/mcard) ran on it (XBA-167): the header frame ('MC', XOR checksum), the 15 directory frames
and the 20 broken-sector frames with their checksums, exactly one file XBAMCARD of one 8 KB block (state 51h, size
8192, no next block) whose data block holds the homebrew's pattern, every other block free (A0h). Layout and checksum
from psx-spx « Memory Card Data Format ». --empty: the card is a freshly formatted one instead (no file). Exit 0 / 1."""

import sys


def pattern(i):
    return ((i * 7 + 0x5A) ^ (i >> 8)) & 0xFF


def xor(frame):
    v = 0
    for b in frame[:0x7F]:
        v ^= b
    return v


def check(d, empty):
    if len(d) != 0x20000:
        return "size %d, not 131072" % len(d)
    frames = [d[i * 128:(i + 1) * 128] for i in range(1024)]
    if frames[0][:2] != b"MC" or frames[0][0x7F] != xor(frames[0]):
        return "header frame: no 'MC' or a bad checksum"
    for f in range(1, 36):
        if frames[f][0x7F] != xor(frames[f]):
            return "frame %d: bad checksum %02X (want %02X)" % (f, frames[f][0x7F], xor(frames[f]))
    files = []
    for b in range(15):
        e = frames[1 + b]
        state = e[0]
        if state == 0x51:
            size = int.from_bytes(e[4:8], "little")
            nxt = int.from_bytes(e[8:10], "little")
            name = e[10:30].split(b"\0")[0].decode("latin-1")
            files.append((b, name, size, nxt))
        elif state not in (0xA0, 0xA1, 0xA2, 0xA3):
            return "directory frame %d: state %02X (only one file, else free)" % (1 + b, state)
    if empty:
        return "files on an empty card: %r" % files if files else None
    if len(files) != 1:
        return "%d files, want XBAMCARD alone: %r" % (len(files), files)
    b, name, size, nxt = files[0]
    if name != "XBAMCARD" or size != 8192 or nxt != 0xFFFF:
        return "file %r size %d next %04X (want XBAMCARD, 8192, FFFF)" % (name, size, nxt)
    data = d[(b + 1) * 0x2000:(b + 2) * 0x2000]
    bad = [i for i in range(8192) if data[i] != pattern(i)]
    if bad:
        return "data block %d: %d bytes differ from the pattern (first at %d)" % (b + 1, len(bad), bad[0])
    return None


def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        d = f.read()
    why = check(d, "--empty" in sys.argv[2:])
    if why:
        print("FAIL: %s: %s" % (path, why))
        return 1
    print("card OK: %s" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
