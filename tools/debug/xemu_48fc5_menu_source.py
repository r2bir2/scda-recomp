"""2026-09-09: dynamic check at the exact branch in sub_00048F1D that
decides between a resolved string (presumably the "menu" that ends up
in "...\LMaps\menu\common.lin") and the hardcoded empty-string sentinel
0x5A570C:

    recomp_0002.c:15303-15312 (guest 0x00048FC5-0x00048FD2):
        cmp MEM32(ebp+0x4C), 0
        eax = MEM32(ebp+0x48)
        if (!=0) goto use eax
        else eax = 0x5A570C   (sentinel)

Breaks at guest PC 0x00048FC5 (right after the state value lands in
ebp+0x48/0x4C, before the branch), dumps the stack window ebp+0x40..
ebp+0x58 raw, and -- if ebp+0x48 looks like a plausible guest pointer
(0x10000..0x02000000, this title's typical VA range) -- also tries
reading it as an ANSI C string.
"""
from __future__ import annotations

import socket
import struct

ENTRY = 0x00048FC5
MAX_ATTEMPTS = 60


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


def cont(sock):
    sock.sendall(b"$c#63")
    sock.recv(1)
    wait_stop(sock)


def cont_over_bp(sock, bp_addr):
    packet(sock, f"z0,{bp_addr:x},1")
    sock.sendall(b"$s#73")
    sock.recv(1)
    wait_stop(sock)
    packet(sock, f"Z0,{bp_addr:x},1")
    cont(sock)


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


def read_astr(sock, address, max_chars=40):
    if not address or not (0x10000 <= address < 0x02000000):
        return None
    chars = []
    for i in range(max_chars):
        result = packet(sock, f"m{(address + i) & 0xFFFFFFFF:x},1")
        if result.startswith("E") or len(result) < 2:
            return None
        b = int(result, 16)
        if b == 0:
            break
        chars.append(chr(b) if 0x20 <= b < 0x7f else f"\\x{b:02x}")
    return "".join(chars)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{ENTRY:x},1")
    print(f"armed sub_00048F1D branch point 0x{ENTRY:08X}", flush=True)

    sock.settimeout(120)
    for attempt in range(MAX_ATTEMPTS):
        if attempt == 0:
            cont(sock)
        else:
            cont_over_bp(sock, ENTRY)
        r = regs(sock)
        ebp = r["ebp"]
        window = {off: u32(sock, ebp + off) for off in (0x40, 0x44, 0x48, 0x4C, 0x50, 0x54)}
        val48 = window[0x48]
        val4c = window[0x4C]
        str48 = read_astr(sock, val48)
        print(f"[{attempt}] ebp=0x{ebp:08X} "
              + " ".join(f"+{off:#x}=0x{v if v is not None else 0:08X}" for off, v in window.items())
              + f"  str@ebp+0x48=\"{str48}\"", flush=True)

    packet(sock, f"z0,{ENTRY:x},1")
    print("done", flush=True)


if __name__ == "__main__":
    main()
