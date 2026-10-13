"""Trace every REAL disc-refill attempt (sub_000516B7 entry, the function
that actually issues Xbox disc I/O beneath sub_00040821's ring buffer) on
real hardware, for the same early sub_00048B15/sub_0004438C load path
checked by xemu_48b15_fatal_check.py.

Much rarer than sub_0004438C itself (that one is hit on every "pump this
stream a little" tick; sub_000516B7 only fires when the ring buffer
actually needs to top up from disc), so this can run much longer within
the same real-time budget and show whether/when a short read, a file-end
condition, or the fatal screen (sub_0003FCA5) ever occurs.

Run xemu with ``-s -S`` first, then invoke this script.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time

REFILL_516B7 = 0x000516B7
FATAL_3FCA5 = 0x0003FCA5


def packet(sock: socket.socket, body: str) -> str:
    raw = body.encode("ascii")
    sock.sendall(b"$" + raw + b"#" + f"{sum(raw) & 0xff:02x}".encode("ascii"))
    first = sock.recv(1)
    if first == b"+":
        first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    response = bytearray()
    while True:
        byte = sock.recv(1)
        if byte == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return response.decode("ascii")
        response.extend(byte)


def send_continue(sock: socket.socket) -> None:
    body = b"c"
    sock.sendall(b"$" + body + b"#63")
    if sock.recv(1) != b"+":
        raise RuntimeError("GDB stub did not acknowledge continue")


def wait_stop(sock: socket.socket) -> str:
    first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    body = bytearray()
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return body.decode("ascii")
        body.extend(char)


def regs(sock: socket.socket) -> dict[str, int]:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def read_u32(sock: socket.socket, address: int) -> int | None:
    if address == 0:
        return None
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    started = time.monotonic()
    hit_refill = 0
    hit_fatal = 0
    try:
        for addr in (REFILL_516B7, FATAL_3FCA5):
            r = packet(sock, f"Z0,{addr:x},1")
            print(f"arm bp 0x{addr:08X}: {r}", flush=True)

        sock.settimeout(args.timeout)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            send_continue(sock)
            stop = wait_stop(sock)
            t = time.monotonic() - started
            r = regs(sock)
            pc = r["eip"]
            if pc == REFILL_516B7:
                hit_refill += 1
                # cdecl push order seen in sub_00040821 (Update 179):
                # push file_obj(=MEM32(ebx+0x3C)); push dest; push size;
                # push &bytes_read; push 0(offset_hi) -- so at entry,
                # [esp+4]=file_obj, [esp+8]=dest, [esp+0xC]=size.
                file_obj = read_u32(sock, r["esp"] + 4) or 0
                dest = read_u32(sock, r["esp"] + 8) or 0
                size = read_u32(sock, r["esp"] + 0xC) or 0
                print(f"[REFILL] #{hit_refill} t={t:.3f}s file_obj=0x{file_obj:08X} "
                      f"dest=0x{dest:08X} size=0x{size:08X}", flush=True)
            elif pc == FATAL_3FCA5:
                hit_fatal += 1
                print(f"[FATAL-3FCA5-HIT] #{hit_fatal} t={t:.3f}s -- REAL HARDWARE REACHED "
                      f"THE FATAL SCREEN after {hit_refill} refills.", flush=True)
                break
            else:
                print(f"[UNEXPECTED] t={t:.3f}s eip=0x{pc:08X} stop={stop}", flush=True)
            if pc in (REFILL_516B7, FATAL_3FCA5):
                packet(sock, f"z0,{pc:x},1")
                packet(sock, "s")
                packet(sock, f"Z0,{pc:x},1")
        print(f"SUMMARY: refill hits={hit_refill} FATAL-3FCA5 hits={hit_fatal} "
              f"elapsed={time.monotonic()-started:.1f}s", flush=True)
    finally:
        for addr in (REFILL_516B7, FATAL_3FCA5):
            try:
                packet(sock, f"z0,{addr:x},1")
            except OSError:
                pass
        try:
            packet(sock, "D")
        except OSError:
            pass
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
