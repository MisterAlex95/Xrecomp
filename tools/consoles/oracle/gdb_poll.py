#!/usr/bin/env python3
"""Polls an XBA result block through a GDB remote stub (XBA-149), for oracles without a scripting API (ares).

    gdb_poll.py --port 9123 --addr 0xA0300000 --endian big --timeout 60

Connects to the stub (retrying until it listens), then reads the 512-byte block with `m` packets every
--interval seconds, without ever halting the target (ares serves memory reads while the game runs). Once the
`status` word (offset 4) is non-zero, or at the timeout, prints one line « XBA-BLOCK <seconds> <1024 hex digits> »
and exits 0; exits 2 if the stub never answered. Standard library only.
"""
import argparse
import socket
import sys
import time

SIZE = 512


def packet(payload: str) -> bytes:
    return f"${payload}#{sum(payload.encode()) & 0xFF:02x}".encode()


class Stub:
    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buf = b""

    def _byte(self) -> int:
        while not self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("stub closed the connection")
            self.buf += chunk
        b, self.buf = self.buf[0], self.buf[1:]
        return b

    def command(self, payload: str) -> str:
        self.sock.sendall(packet(payload))
        while True:  # skip acks ('+') and anything before the reply packet
            if self._byte() == ord("$"):
                break
        out = bytearray()
        while (b := self._byte()) != ord("#"):
            out.append(b)
        self._byte(), self._byte()  # checksum
        self.sock.sendall(b"+")
        return out.decode()

    def read(self, addr: int, size: int) -> bytes:
        data = bytearray()
        while len(data) < size:  # small packets: stubs cap their reply size
            n = min(256, size - len(data))
            reply = self.command(f"m{addr + len(data):x},{n:x}")
            if len(reply) != 2 * n:
                raise ValueError(f"bad reply to m{addr + len(data):x},{n:x}: {reply[:40]!r}")
            data += bytes.fromhex(reply)
        return bytes(data)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--addr", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--endian", choices=("big", "little"), required=True)
    ap.add_argument("--timeout", type=float, default=60)
    ap.add_argument("--interval", type=float, default=0.25)
    args = ap.parse_args()

    start = time.monotonic()
    sock = None
    while time.monotonic() - start < args.timeout:
        try:
            sock = socket.create_connection((args.host, args.port), timeout=10)
            break
        except OSError:
            time.sleep(0.5)
    if sock is None:
        print("gdb_poll: the GDB stub never answered", file=sys.stderr)
        return 2
    sock.sendall(b"+")  # GDB clients open with an ack; ares drops any client whose first byte is not '+'
    stub = Stub(sock)
    block = b""
    while True:
        try:
            block = stub.read(args.addr, SIZE)
        except (OSError, ValueError, ConnectionError) as e:
            print(f"gdb_poll: {e}", file=sys.stderr)
            if not block:
                return 2
            break
        status = int.from_bytes(block[4:8], args.endian)
        if status or time.monotonic() - start >= args.timeout:
            break
        time.sleep(args.interval)
    print(f"XBA-BLOCK {time.monotonic() - start:.1f}s {block.hex()}")
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
