"""2026-09-16 (Update 159 continuation): decisive xemu/recomp comparison
at the exact call site (guest VA 0x004C173B, immediately before
`call sub_004BF700` for the V-plane fill within sub_004C15D0).

On recomp, at the equivalent flushed point, this call showed:
  esi (width)  = 0x140 (320)
  edi (height) = 0xB8  (184)
  ebx          = 0x140 (320)
  eax (buffer, from the correlated fill loop) = 0x1275BE0

This reads the real x86 registers at the identical VA on real hardware,
first hit, to determine whether width/height/etc. already diverge here
(the caller feeding wrong values) or match (pointing at sub_004BF700's
own internal semantics, or the allocation size itself, as the true
divergence).
"""
from __future__ import annotations

import socket
import struct

ENTRY = 0x004C173B


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


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(30)
    packet(sock, f"Z0,{ENTRY:x},1")
    print(f"armed 0x{ENTRY:08X}", flush=True)

    sock.settimeout(600)
    cont(sock)
    r = regs(sock)
    print(f"eax(buffer)=0x{r['eax']:08X} ecx=0x{r['ecx']:08X} edx=0x{r['edx']:08X} "
          f"ebx=0x{r['ebx']:08X} esi(width?)=0x{r['esi']:08X} edi(height?)=0x{r['edi']:08X} "
          f"ebp=0x{r['ebp']:08X} esp=0x{r['esp']:08X}", flush=True)

    packet(sock, f"z0,{ENTRY:x},1")
    print("done", flush=True)


if __name__ == "__main__":
    main()
