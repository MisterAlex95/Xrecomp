"""AES-128 in pure Python (XBA-230): enough for XEX2 (ECB for the session key, CBC with a zero IV for the image).

No third-party module: the CI image has none, and the XEX parser must run anywhere. When the `cryptography` package
is installed it is used instead (much faster on large images); results are identical. Table-driven (T-tables), about
0.5 MB/s in CPython: fine for tests and acceptable for a one-off parse of a title.

No key lives here or anywhere in the repository: callers pass the keys.
"""

import os
import struct
import subprocess
import sys
import threading

try:  # optional fast path
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # type: ignore
except Exception:  # pragma: no cover - depends on the host
    Cipher = None


def _xt(a):
    a <<= 1
    return a ^ 0x11B if a & 0x100 else a


def _mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = _xt(a)
        b >>= 1
    return r


def _tables():
    sbox = [0] * 256
    inv = [0] * 256
    p = q = 1
    while True:  # walk the multiplicative group with generator 3, its inverse with 0xF6
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        x = (x ^ 0x63) & 0xFF
        sbox[p] = x
        inv[x] = p
        if p == 1:
            break
    sbox[0] = 0x63
    inv[0x63] = 0
    te = [[0] * 256 for _ in range(4)]
    td = [[0] * 256 for _ in range(4)]
    for i in range(256):
        s = sbox[i]
        w = (_mul(s, 2) << 24) | (s << 16) | (s << 8) | _mul(s, 3)
        si = inv[i]
        v = (_mul(si, 14) << 24) | (_mul(si, 9) << 16) | (_mul(si, 13) << 8) | _mul(si, 11)
        for k in range(4):
            te[k][i] = ((w >> (8 * k)) | (w << (32 - 8 * k))) & 0xFFFFFFFF
            td[k][i] = ((v >> (8 * k)) | (v << (32 - 8 * k))) & 0xFFFFFFFF
    return sbox, inv, te, td


SBOX, INV_SBOX, TE, TD = _tables()


def _expand(key):
    if len(key) != 16:
        raise ValueError("AES-128 key must be 16 bytes")
    w = [int.from_bytes(key[4 * i:4 * i + 4], "big") for i in range(4)]
    rcon = 1
    for i in range(4, 44):
        t = w[i - 1]
        if i % 4 == 0:
            t = ((t << 8) | (t >> 24)) & 0xFFFFFFFF
            t = (SBOX[t >> 24] << 24) | (SBOX[(t >> 16) & 255] << 16) | (SBOX[(t >> 8) & 255] << 8) | SBOX[t & 255]
            t ^= rcon << 24
            rcon = _xt(rcon)
        w.append(w[i - 4] ^ t)
    return w


def _inv_mix(x):
    b = [(x >> 24) & 255, (x >> 16) & 255, (x >> 8) & 255, x & 255]
    return TD[0][SBOX[b[0]]] ^ TD[1][SBOX[b[1]]] ^ TD[2][SBOX[b[2]]] ^ TD[3][SBOX[b[3]]]


class AES128:
    def __init__(self, key):
        self.key = bytes(key)
        self.ek = _expand(self.key)
        dk = [0] * 44
        for r in range(11):
            for c in range(4):
                v = self.ek[4 * (10 - r) + c]
                dk[4 * r + c] = v if r in (0, 10) else _inv_mix(v)
        self.dk = dk

    def encrypt_block(self, blk):
        k = self.ek
        s0, s1, s2, s3 = (int.from_bytes(blk[i:i + 4], "big") ^ k[i // 4] for i in range(0, 16, 4))
        t0, t1, t2, t3 = TE
        for r in range(1, 10):
            o = 4 * r
            s0, s1, s2, s3 = (
                t0[s0 >> 24] ^ t1[(s1 >> 16) & 255] ^ t2[(s2 >> 8) & 255] ^ t3[s3 & 255] ^ k[o],
                t0[s1 >> 24] ^ t1[(s2 >> 16) & 255] ^ t2[(s3 >> 8) & 255] ^ t3[s0 & 255] ^ k[o + 1],
                t0[s2 >> 24] ^ t1[(s3 >> 16) & 255] ^ t2[(s0 >> 8) & 255] ^ t3[s1 & 255] ^ k[o + 2],
                t0[s3 >> 24] ^ t1[(s0 >> 16) & 255] ^ t2[(s1 >> 8) & 255] ^ t3[s2 & 255] ^ k[o + 3])
        S = SBOX
        out = bytearray()
        for a, b, c, d, kk in ((s0, s1, s2, s3, k[40]), (s1, s2, s3, s0, k[41]), (s2, s3, s0, s1, k[42]),
                               (s3, s0, s1, s2, k[43])):
            v = (S[a >> 24] << 24 | S[(b >> 16) & 255] << 16 | S[(c >> 8) & 255] << 8 | S[d & 255]) ^ kk
            out += v.to_bytes(4, "big")
        return bytes(out)

    def decrypt_block(self, blk):
        k = self.dk
        s0, s1, s2, s3 = (int.from_bytes(blk[i:i + 4], "big") ^ k[i // 4] for i in range(0, 16, 4))
        t0, t1, t2, t3 = TD
        for r in range(1, 10):
            o = 4 * r
            s0, s1, s2, s3 = (
                t0[s0 >> 24] ^ t1[(s3 >> 16) & 255] ^ t2[(s2 >> 8) & 255] ^ t3[s1 & 255] ^ k[o],
                t0[s1 >> 24] ^ t1[(s0 >> 16) & 255] ^ t2[(s3 >> 8) & 255] ^ t3[s2 & 255] ^ k[o + 1],
                t0[s2 >> 24] ^ t1[(s1 >> 16) & 255] ^ t2[(s0 >> 8) & 255] ^ t3[s3 & 255] ^ k[o + 2],
                t0[s3 >> 24] ^ t1[(s2 >> 16) & 255] ^ t2[(s1 >> 8) & 255] ^ t3[s0 & 255] ^ k[o + 3])
        S = INV_SBOX
        out = bytearray()
        for a, b, c, d, kk in ((s0, s3, s2, s1, k[40]), (s1, s0, s3, s2, k[41]), (s2, s1, s0, s3, k[42]),
                               (s3, s2, s1, s0, k[43])):
            v = (S[a >> 24] << 24 | S[(b >> 16) & 255] << 16 | S[(c >> 8) & 255] << 8 | S[d & 255]) ^ kk
            out += v.to_bytes(4, "big")
        return bytes(out)


def _fast(key, mode, data, decrypt):
    c = Cipher(algorithms.AES(bytes(key)), mode)
    op = c.decryptor() if decrypt else c.encryptor()
    return op.update(bytes(data)) + op.finalize()


def ecb_decrypt(key, data):
    if Cipher is not None:
        return _fast(key, modes.ECB(), data, True)
    a = AES128(key)
    return b"".join(a.decrypt_block(data[i:i + 16]) for i in range(0, len(data), 16))


def ecb_encrypt(key, data):
    if Cipher is not None:
        return _fast(key, modes.ECB(), data, False)
    a = AES128(key)
    return b"".join(a.encrypt_block(data[i:i + 16]) for i in range(0, len(data), 16))


def cbc_decrypt(key, data, iv=bytes(16)):
    """Whole 16-byte blocks only; a trailing partial block is returned as is (XEX images are block-aligned)."""
    n = len(data) & ~15
    if Cipher is not None:
        return _fast(key, modes.CBC(bytes(iv)), data[:n], True) + bytes(data[n:])
    if n >= PARALLEL_MIN:
        out = _cbc_decrypt_parallel(key, data, n, iv)
        if out is not None:
            return out + bytes(data[n:])
    return _cbc_decrypt_here(key, data, n, iv)


# CBC decryption needs no result of the block before, only its ciphertext: a large image is cut into parts that
# python -m tools.xex.aes decrypts side by side (subprocesses, as tools.prepare runs every step: the multiprocessing
# module needs sem_open, which Android lacks). 15 MB of XEX took 17 s on one desktop core, about three times that
# on a phone's; with 4 parts a quarter, plus the interpreters' start. Anything going wrong there (no interpreter to
# start, a part of the wrong size) falls back to decrypting here, the same bytes either way.
PARALLEL_MIN = 1 << 20
PARALLEL_JOBS = 4
PYTHON = None      # the interpreter for the parts (tools.prepare sets its --python); else $XBA_PYTHON, sys.executable


def _python():
    exe = PYTHON or os.environ.get("XBA_PYTHON") or sys.executable
    return exe if exe and os.path.isfile(exe) else None


def _cbc_decrypt_parallel(key, data, n, iv):
    jobs = min(PARALLEL_JOBS, os.cpu_count() or 1)
    exe = _python()
    if jobs < 2 or exe is None:
        return None
    blocks = n // 16
    cuts = [16 * (blocks * k // jobs) for k in range(jobs + 1)]
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    env = dict(os.environ)
    env["PYTHONPATH"] = root + (os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else "")
    parts = [None] * jobs
    procs = []

    def one(k, p, chunk):
        try:
            out, _ = p.communicate(chunk)
            if p.returncode == 0 and len(out) == len(chunk) - 32:
                parts[k] = out
        except (OSError, ValueError):
            pass
    try:
        threads = []
        for k in range(jobs):
            lo, hi = cuts[k], cuts[k + 1]
            prev = bytes(iv) if lo == 0 else bytes(data[lo - 16:lo])
            p = subprocess.Popen([exe, "-m", "tools.xex.aes"], cwd=root, env=env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            procs.append(p)
            t = threading.Thread(target=one, args=(k, p, bytes(key) + prev + bytes(data[lo:hi])))
            t.start()
            threads.append(t)
        for t in threads:
            t.join()
    except (OSError, ValueError):
        for p in procs:
            try:
                p.kill()
                p.wait()
            except OSError:
                pass
        return None
    if any(x is None for x in parts):
        return None
    return b"".join(parts)


def _part_main(argv):
    """python -m tools.xex.aes: stdin is the key, the IV (16 bytes each: the key never on a command line) and
    the blocks, CBC-decrypted to stdout; the part's IV is the ciphertext block before it (cbc_decrypt's
    parallel path)."""
    data = sys.stdin.buffer.read()
    if len(data) < 32:
        return 2
    key, iv, data = data[:16], data[16:32], data[32:]
    sys.stdout.buffer.write(_cbc_decrypt_here(key, data, len(data) & ~15, iv))
    sys.stdout.buffer.flush()
    return 0


def _cbc_decrypt_here(key, data, n, iv):
    """decrypt_block's rounds inline, 64 KB of words unpacked at a time (about 15 % faster than a call per block)."""
    k = AES128(key).dk
    t0, t1, t2, t3 = TD
    S = INV_SBOX
    k0, k1, k2, k3 = k[0:4]
    k40, k41, k42, k43 = k[40:44]
    rounds = [tuple(k[4 * r:4 * r + 4]) for r in range(1, 10)]
    p0, p1, p2, p3 = struct.unpack(">4I", bytes(iv))
    out = bytearray()
    mv = memoryview(data)
    for c in range(0, n, 1 << 16):
        w = struct.unpack(">%dI" % (min(1 << 16, n - c) >> 2), mv[c:c + min(1 << 16, n - c)])
        o = [0] * len(w)
        for j in range(0, len(w), 4):
            c0, c1, c2, c3 = w[j], w[j + 1], w[j + 2], w[j + 3]
            s0, s1, s2, s3 = c0 ^ k0, c1 ^ k1, c2 ^ k2, c3 ^ k3
            for r0, r1, r2, r3 in rounds:
                s0, s1, s2, s3 = (
                    t0[s0 >> 24] ^ t1[(s3 >> 16) & 255] ^ t2[(s2 >> 8) & 255] ^ t3[s1 & 255] ^ r0,
                    t0[s1 >> 24] ^ t1[(s0 >> 16) & 255] ^ t2[(s3 >> 8) & 255] ^ t3[s2 & 255] ^ r1,
                    t0[s2 >> 24] ^ t1[(s1 >> 16) & 255] ^ t2[(s0 >> 8) & 255] ^ t3[s3 & 255] ^ r2,
                    t0[s3 >> 24] ^ t1[(s2 >> 16) & 255] ^ t2[(s1 >> 8) & 255] ^ t3[s0 & 255] ^ r3)
            o[j] = (S[s0 >> 24] << 24 | S[(s3 >> 16) & 255] << 16 | S[(s2 >> 8) & 255] << 8 | S[s1 & 255]) ^ k40 ^ p0
            o[j + 1] = (S[s1 >> 24] << 24 | S[(s0 >> 16) & 255] << 16 | S[(s3 >> 8) & 255] << 8
                        | S[s2 & 255]) ^ k41 ^ p1
            o[j + 2] = (S[s2 >> 24] << 24 | S[(s1 >> 16) & 255] << 16 | S[(s0 >> 8) & 255] << 8
                        | S[s3 & 255]) ^ k42 ^ p2
            o[j + 3] = (S[s3 >> 24] << 24 | S[(s2 >> 16) & 255] << 16 | S[(s1 >> 8) & 255] << 8
                        | S[s0 & 255]) ^ k43 ^ p3
            p0, p1, p2, p3 = c0, c1, c2, c3
        out += struct.pack(">%dI" % len(o), *o)
    return bytes(out) + bytes(data[n:])


def cbc_encrypt(key, data, iv=bytes(16)):
    if len(data) % 16:
        raise ValueError("CBC input must be a multiple of 16 bytes")
    if Cipher is not None:
        return _fast(key, modes.CBC(bytes(iv)), data, False)
    a = AES128(key)
    out = bytearray()
    prev = bytes(iv)
    for i in range(0, len(data), 16):
        x = (int.from_bytes(data[i:i + 16], "big") ^ int.from_bytes(prev, "big")).to_bytes(16, "big")
        prev = a.encrypt_block(x)
        out += prev
    return bytes(out)


if __name__ == "__main__":
    sys.exit(_part_main(sys.argv[1:]))
