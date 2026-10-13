"""2026-09-09: does real hardware's sub_0003EAE3 call (from sub_00048B15,
right after MEM32(0x5ED144) is explicitly zeroed) leave it non-zero, taking
the sub_00048856 factory-registration branch scda_recomp's own build never
takes (scda_recomp: MEM32(0x5ED144)==0 at this point, confirmed via cdb)?

Breaks once at 0x00048BBE (the branch's own checkpoint, right after
sub_0003EAE3 returns) and reads MEM32(0x5ED140)/MEM32(0x5ED144) directly.
"""
from __future__ import annotations

import socket
import struct

CHECKPOINT = 0x00048BBE


def packet(sock, body):
    raw = body.encode("ascii")
    sock.sendall(b"$" + raw + b"#" + f"{sum(raw) & 0xff:02x}".encode("ascii"))
    first = sock.recv(1)
    if first == b"+":
        first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    result = bytearray()
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return result.decode("ascii")
        result.extend(char)


def wait_stop(sock):
    first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return


def u32(sock, address):
    address &= 0xFFFFFFFF
    result = packet(sock, f"m{address:x},4")
    if result.startswith("E") or len(result) < 8:
        return None
    return struct.unpack("<I", bytes.fromhex(result))[0]


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{CHECKPOINT:x},1")
    print(f"armed 0x{CHECKPOINT:08X}", flush=True)

    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(300)
    wait_stop(sock)

    status140 = u32(sock, 0x005ED140)
    status144 = u32(sock, 0x005ED144)
    print(f"HIT pc=0x{CHECKPOINT:08X} status140=0x{status140:08X} status144=0x{status144:08X}",
          flush=True)

    packet(sock, f"z0,{CHECKPOINT:x},1")
    print("done", flush=True)


if __name__ == "__main__":
    main()
