"""2026-09-07 (continued 66): does real hardware's scratch object (the one
MEM32(0x5B57FC) points to when sub_00071B49 calls into sub_00045ADE via
0x71C65) have a populated name table ([this+0x20]/[0x24]/[0x2C]), or is it
also all-zero like scda_recomp's -- and what does [this+4] (the field the
FAILURE-path tail dispatch reads as a nested vtable pointer) hold?

Breaks once at 0x71C56 (same checkpoint scda_recomp's own probe used).
"""
from __future__ import annotations

import socket
import struct

CHECKPOINT = 0x00071C56


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


def regs(sock):
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def u32(sock, address):
    address &= 0xFFFFFFFF
    result = packet(sock, f"m{address:x},4")
    if result.startswith("E") or len(result) < 8:
        return None
    return struct.unpack("<I", bytes.fromhex(result))[0]


def read_wstring(sock, address, maxlen=32):
    out = []
    for i in range(maxlen):
        result = packet(sock, f"m{(address + i*2) & 0xFFFFFFFF:x},2")
        if result.startswith("E") or len(result) < 4:
            break
        wc = struct.unpack("<H", bytes.fromhex(result))[0]
        if wc == 0:
            break
        out.append(chr(wc) if 32 <= wc < 127 else f"\\x{wc:04x}")
    return "".join(out)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{CHECKPOINT:x},1")
    print(f"armed 0x{CHECKPOINT:08X}", flush=True)

    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(120)
    wait_stop(sock)

    r = regs(sock)
    obj = u32(sock, 0x005B57FC)
    field4 = u32(sock, obj + 4) if obj else None
    field20 = u32(sock, obj + 0x20) if obj else None
    field24 = u32(sock, obj + 0x24) if obj else None
    field2c = u32(sock, obj + 0x2C) if obj else None
    strptr = (r["ebp"] - 1036) & 0xFFFFFFFF
    name = read_wstring(sock, strptr)
    print(
        f"HIT pc=0x{r['eip']:08X} obj=0x{obj:08X} field4=0x{field4:08X} "
        f"field20(base)=0x{field20:08X} field24(count)=0x{field24:08X} "
        f"field2c(gate)=0x{field2c:08X} strptr=0x{strptr:08X} name=\"{name}\"",
        flush=True,
    )

    packet(sock, f"z0,{CHECKPOINT:x},1")
    print("done", flush=True)


if __name__ == "__main__":
    main()
