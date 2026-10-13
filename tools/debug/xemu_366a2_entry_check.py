"""sub_000366A2 is called unconditionally from sub_00048B15 (the same
main initializer that reliably calls sub_001D589D at ~1.3s post-reset).
Check whether/when it fires on real hardware at the equivalent early
point, and read the object/registers at entry, plus follow forward to
the internal MEM32(0x5B57FC) check inside sub_001CC602 (reached via
sub_000366A2's own call at loc_000366B5) to see whether real hardware
takes the same "proceed" branch scda_recomp does.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


TARGET_PC = 0x000366A2


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
    sock.sendall(b"$c#63")
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


def get_regs(sock: socket.socket) -> dict:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=600.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    r1 = packet(sock, f"Z0,{TARGET_PC:x},1")
    print(f"bp result: {r1}", flush=True)
    t0 = time.time()
    send_continue(sock)
    sock.settimeout(args.timeout)
    try:
        wait_stop(sock)
    except socket.timeout:
        print(f"NO HIT within {args.timeout}s", flush=True)
        return 0
    t = time.time() - t0
    r = get_regs(sock)
    ret = read_u32(sock, r["esp"])
    mem5B57FC = read_u32(sock, 0x5B57FC)
    print(f"HIT at t={t:.2f}s pc={r['eip']:08X} ret={ret if ret else 0:08X} "
          f"ecx={r['ecx']:08X} esp={r['esp']:08X} mem5B57FC={mem5B57FC if mem5B57FC is not None else -1:08X}",
          flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
