"""One-shot: halt xemu right now (wherever it currently is), walk bin[6]'s
free chain, print the result, then resume. Does not require reaching any
specific PC -- just answers "how healthy is bin[6] right now".
"""

from __future__ import annotations

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


def main() -> int:
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    sock.sendall(b"\x03")
    time.sleep(0.3)
    try:
        wait_stop(sock)
    except socket.timeout:
        pass

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

    # Also report a couple of neighboring size classes for context.
    for label, addr in (("bin[5] (40B)", 0x0064D298), ("bin[7] (56B)", 0x0064D2A0)):
        h = read_u32(sock, addr)
        cnt = 0
        c = h
        seen2 = set()
        while c and c not in seen2 and cnt < CHAIN_CAP:
            seen2.add(c)
            cnt += 1
            c = read_u32(sock, c + 8)
        if h:
            print(f"{label} head={h:08X} approx_len={cnt}")
        else:
            print(f"{label} empty")

    sock.sendall(b"$c#63")
    print("resumed", sock.recv(4))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
