"""One-shot: walk bin[6]'s free chain the moment sub_001D589D (the
system-info property-dump routine) is entered, via xemu's GDB stub.

Run xemu with ``-s -S`` first, then invoke this script; it resumes
execution itself, halts at the breakpoint, walks the chain, and reports
the count -- for direct comparison against the same walk on scda_recomp
(cdb_bin6_walk_manual4.txt) at the same guest PC.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


BIN6_ADDR = 0x0064D2B4
CHAIN_CAP = 60


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


def read_u32(sock: socket.socket, address: int) -> int | None:
    if address == 0:
        return None
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def regs(sock: socket.socket) -> dict[str, int]:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=600.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(args.timeout)
    bp_addr = 0x001D589D
    try:
        result = packet(sock, f"Z0,{bp_addr:x},1")
        if result != "OK":
            raise RuntimeError(f"could not set breakpoint: {result}")
        send_continue(sock)
        stop = wait_stop(sock)
        r = regs(sock)
        pc = r["eip"]
        print(f"stop={stop} pc={pc:08X} eax={r['eax']:08X} ecx={r['ecx']:08X}")
        if pc != bp_addr:
            print("UNEXPECTED STOP PC -- not sub_001D589D")
            return 1
        chain = []
        head = read_u32(sock, BIN6_ADDR)
        cur = head
        seen = set()
        status = "ok"
        while cur:
            if cur in seen:
                status = "cycle"
                break
            seen.add(cur)
            chain.append(cur)
            if len(chain) >= CHAIN_CAP:
                status = "cap-reached"
                break
            nxt = read_u32(sock, cur + 8)
            if nxt is None:
                status = "unreadable-link"
                break
            cur = nxt
        print(f"bin6 chain length = {len(chain)} status={status}")
        for i, c in enumerate(chain):
            print(f"  [{i}] {c:08X}")
        packet(sock, f"z0,{bp_addr:x},1")
    finally:
        try:
            sock.sendall(b"\x03")
            time.sleep(0.3)
            try:
                wait_stop(sock)
            except socket.timeout:
                pass
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
