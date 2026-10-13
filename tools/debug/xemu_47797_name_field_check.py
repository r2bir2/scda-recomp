"""2026-09-10 (new session): does the vtable-0x004F7248 file-manager
object's own "name" field (offset +0x1C, the one sub_0004721E's very
first branch checks: `if (MEM32(ecx+0x1C)==0) use hardcoded empty
string 0x5A570C else use MEM32(ecx+0x18)`) hold a real value on real
hardware, at the exact point sub_0004721E is entered for this object?

On scda_recomp (same-session live cdb dump, known deterministic object
address 0x010D9408): MEM32(obj+0x18)=0, MEM32(obj+0x1C)=0 -- both zero,
forcing the hardcoded-empty-string fallback. This script checks the
identical two fields on xemu for the equivalent object (matched by the
already-proven vtable filter from xemu_47505_container_check.py/
xemu_47505_container_check2.py, object address 0x0081BE80 on the
corrected retail disc).
"""
from __future__ import annotations

import socket
import struct

ENTRY = 0x0004721E
TARGET_VTABLE = 0x004F7248
MAX_ATTEMPTS = 400


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
    sock.settimeout(10)
    packet(sock, f"Z0,{ENTRY:x},1")
    print(f"armed entry 0x{ENTRY:08X}, filtering for vtable 0x{TARGET_VTABLE:08X}", flush=True)

    sock.settimeout(120)
    found = False
    for attempt in range(MAX_ATTEMPTS):
        cont(sock)
        r = regs(sock)
        obj = r["ecx"]
        vtbl = u32(sock, obj)
        if vtbl == TARGET_VTABLE:
            field18 = u32(sock, obj + 0x18)
            field1c = u32(sock, obj + 0x1C)
            print(f"MATCH at attempt {attempt}: obj=0x{obj:08X} vtbl=0x{vtbl:08X} "
                  f"field+0x18=0x{field18:08X} field+0x1C=0x{field1c:08X}", flush=True)
            found = True
            break
        if attempt % 50 == 0:
            print(f"  ...attempt {attempt}: obj=0x{obj:08X} vtbl=0x{vtbl if vtbl else 0:08X} (no match yet)", flush=True)

    packet(sock, f"z0,{ENTRY:x},1")
    if not found:
        print(f"never matched vtable 0x{TARGET_VTABLE:08X} in {MAX_ATTEMPTS} sub_0004721E calls", flush=True)
    print("done", flush=True)


if __name__ == "__main__":
    main()
