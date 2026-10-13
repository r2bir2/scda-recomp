"""2026-09-09: dynamic verification of sub_000457AE, reached via the
NOW-PROVEN-REAL call chain sub_00047797 -> sub_0004721E -> sub_000464EC
-> sub_0007188A -> sub_00045693 (vtable+8 "Open") -> sub_000457AE.

sub_00045693's logic:
    edi = sub_000457AE(ecx=this, name=MEM32(ebp+8))
    if (edi == 0) delegate Open to esi+4's own vtable+8 (fallback chain)
    else wrap a found-in-archive view object and return it

This filters sub_000457AE entries by return address == 0x000456A6 (the
one real caller on this path, as opposed to the unrelated 0x00045AEA
caller inside sub_00045ADE, which belongs to the old/misattributed
0x47521 enumeration-loop chain).

At entry (before "push ebp" executes): MEM32(esp) = return address,
MEM32(esp+4) = the name-pointer argument (esi in the callee).
Captures the name string and then arms a one-shot breakpoint at the
return address (0x000456A6) to read eax (the actual return value,
copied to "edi" by the caller) at the moment of return.
"""
from __future__ import annotations

import socket
import struct

ENTRY = 0x000457AE
CALLER_RET = 0x000456A6
MAX_ATTEMPTS = 4000


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


def read_wstr(sock, address, max_chars=64):
    chars = []
    for i in range(max_chars):
        v = u32(sock, address + i * 2)
        if v is None:
            break
        ch = v & 0xFFFF
        if ch == 0:
            break
        chars.append(chr(ch) if 0x20 <= ch < 0x7f else f"\\u{ch:04x}")
    return "".join(chars)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{ENTRY:x},1")
    print(f"armed entry 0x{ENTRY:08X}, filtering ret==0x{CALLER_RET:08X}", flush=True)

    sock.settimeout(120)
    found = False
    for attempt in range(MAX_ATTEMPTS):
        cont(sock)
        r = regs(sock)
        ret = u32(sock, r["esp"])
        if ret == CALLER_RET:
            name_ptr = u32(sock, r["esp"] + 4)
            this_ptr = r["ecx"]
            name = read_wstr(sock, name_ptr) if name_ptr else "(null)"
            print(f"MATCH at attempt {attempt}: this=0x{this_ptr:08X} name_ptr=0x{name_ptr:08X} "
                  f"name=\"{name}\"", flush=True)
            # now arm the return-address breakpoint to catch eax on return
            packet(sock, f"z0,{ENTRY:x},1")
            packet(sock, f"Z0,{CALLER_RET:x},1")
            cont(sock)
            r2 = regs(sock)
            print(f"RETURN at 0x{r2['eip']:08X}: eax=0x{r2['eax']:08X} "
                  f"(0=not-found/delegate, !=0=found-in-archive)", flush=True)
            packet(sock, f"z0,{CALLER_RET:x},1")
            found = True
            break
        if attempt % 200 == 0:
            print(f"  ...attempt {attempt}: ret=0x{ret if ret else 0:08X} (no match yet)", flush=True)

    if not found:
        packet(sock, f"z0,{ENTRY:x},1")
        print(f"never matched ret==0x{CALLER_RET:08X} in {MAX_ATTEMPTS} sub_000457AE calls", flush=True)
    print("done", flush=True)


if __name__ == "__main__":
    main()
