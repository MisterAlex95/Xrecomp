"""chm_extract.py FILE.chm OUT.lzx NAME -> JSON on stdout (XBA-270 test vectors).

Takes the LZX stream out of a CHM made by Free Pascal's chmcmd: the MSCompressed section's Content, its LZXC control
data (window, reset interval) and reset table (uncompressed length), and where file NAME sits in the decompressed
section. Our own reader of the CHM container (ITSF / ITSP / PMGL), written from the format's public descriptions."""

import json
import struct
import sys

PFX = "::DataSpace/Storage/MSCompressed/"
RESET = PFX + "Transform/{7FC28940-9D31-11D0-9B27-00A0C91E9C7C}/InstanceData/ResetTable"


def encint(b, o):
    v = 0
    while True:
        c = b[o]
        o += 1
        v = (v << 7) | (c & 0x7F)
        if not c & 0x80:
            return v, o


def directory(b):
    assert b[:4] == b"ITSF"
    dir_off, dir_len = struct.unpack_from("<QQ", b, 0x48)
    content = struct.unpack_from("<Q", b, 0x58)[0]
    d = b[dir_off:dir_off + dir_len]
    assert d[:4] == b"ITSP"
    hdr_len = struct.unpack_from("<I", d, 8)[0]
    chunk = struct.unpack_from("<I", d, 0x10)[0]
    entries = {}
    o = hdr_len
    while o + chunk <= len(d):
        c = d[o:o + chunk]
        if c[:4] == b"PMGL":
            free = struct.unpack_from("<I", c, 4)[0]
            p = 0x14
            while p < chunk - free:
                n, p = encint(c, p)
                name = c[p:p + n].decode("utf-8")
                p += n
                sec, p = encint(c, p)
                off, p = encint(c, p)
                ln, p = encint(c, p)
                entries[name] = (sec, off, ln)
        o += chunk
    return entries, content


def main():
    path, out, name = sys.argv[1:4]
    b = open(path, "rb").read()
    entries, content = directory(b)

    def sec0(n):
        s, off, ln = entries[n]
        assert s == 0
        return b[content + off:content + off + ln]

    stream = sec0(PFX + "Content")
    ctl = sec0(PFX + "ControlData")
    assert ctl[4:8] == b"LZXC"
    version, reset, window = struct.unpack_from("<III", ctl, 8)
    if version == 2:
        reset *= 0x8000
        window *= 0x8000
    rt = sec0(RESET)
    _ver, _n, _esize, _hlen, ulen, clen, block = struct.unpack_from("<IIIIQQQ", rt, 0)
    s, off, ln = entries["/" + name]
    assert s == 1
    with open(out, "wb") as f:
        f.write(stream)
    print(json.dumps({"window_bits": window.bit_length() - 1, "reset_interval": reset // block,
                      "out_size": ulen, "compressed_size": clen, "file_offset": off, "file_length": ln}))


if __name__ == "__main__":
    main()
