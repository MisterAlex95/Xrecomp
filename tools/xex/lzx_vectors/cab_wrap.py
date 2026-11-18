"""cab_wrap.py STREAM.lzx OUT.cab WINDOW_BITS OUT_SIZE -- one LZX stream (CAB framing, no reset) as a cabinet holding
one file, content.bin, so cabextract (libmspack) can decode it independently (XBA-270 test vectors).

A CAB cuts an LZX folder into data blocks of 32 KB of output each, so the stream is cut where each frame ends; those
offsets come from tools/xex/lzx.py (stats "frame_ends"): if they were wrong, cabextract would fail, it does not
take our decoder's word for the content. Checksums are left 0 (allowed: not checked).
chmcmd encodes its last frame whole (zeros past the content), so the folder is whole frames and the file is cut."""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
from tools.xex.lzx import FRAME, decompress  # noqa: E402


def main():
    src, dst, bits, size = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    stream = open(src, "rb").read()
    st = {}
    # chmcmd encodes the last frame whole (zeros after the content): the folder holds whole frames, the file is cut
    # (exact: the stream ends with the content, as tools/xex/lzx.py store() writes it)
    exact = len(sys.argv) > 5 and sys.argv[5] == "exact"
    padded = size if exact else -(-size // FRAME) * FRAME
    decompress(stream, bits, padded, "cab", stats=st)
    cuts = [0] + st["frame_ends"]
    cuts[-1] = len(stream)
    blocks = []
    for k in range(len(cuts) - 1):
        comp = stream[cuts[k]:cuts[k + 1]]
        unc = min(FRAME, padded - FRAME * k)
        blocks.append(struct.pack("<IHH", 0, len(comp), unc) + comp)
    name = b"content.bin\0"
    hdr_size, folder_size = 36, 8
    file_entry = struct.pack("<IIHHHH", size, 0, 0, 0x5A21, 0, 0x20) + name
    data_off = hdr_size + folder_size + len(file_entry)
    body = b"".join(blocks)
    total = data_off + len(body)
    out = struct.pack("<4sIIIIIBBHHHHH", b"MSCF", 0, total, 0, hdr_size + folder_size, 0, 3, 1, 1, 1, 0, 0, 0)
    out += struct.pack("<IHH", data_off, len(blocks), 3 | (bits << 8))
    out += file_entry + body
    open(dst, "wb").write(out)


if __name__ == "__main__":
    main()
