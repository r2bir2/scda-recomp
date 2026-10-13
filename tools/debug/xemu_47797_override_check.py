"""2026-09-09: does the ONE proven sub_0004721E invocation for the
vtable-0x4F7248 file-manager object (ecx's own vtable == 0x004F7248,
matched identically to xemu_47797_name_field_check.py) go down the
"explicit caller-supplied override name" branch, or the "object's own
esi+0x18/esi+0x1C fields" branch (which xemu_47797_name_field_check.py
already proved are both zero here, forcing the hardcoded empty-string
sentinel 0x5A570C)?

At sub_0004721E's entry (before "push ebp" executes): MEM32(esp) =
return address, MEM32(esp+4) = the explicit override-name argument
(this becomes MEM32(ebp+8) once the prologue runs -- see
sub_0004721E's own first instructions). If that pointer is non-null
and its string is non-empty, sub_0004721E's very first branch skips
the esi+0x18/0x1C check entirely and uses this instead.
"""
from __future__ import annotations

import socket
import struct

ENTRY = 0x0004721E
TARGET_VTABLE = 0x004F7248
MAX_ATTEMPTS = 5000


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


def read_wstr(sock, address, max_chars=80):
    if not address:
        return "(null)"
    chars = []
    for i in range(max_chars):
        v = u32(sock, address + i * 2)
        if v is None:
            return "(unreadable)"
        ch = v & 0xFFFF
        if ch == 0:
            break
        chars.append(chr(ch) if 0x20 <= ch < 0x7f else f"\\u{ch:04x}")
    return "".join(chars)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{ENTRY:x},1")
    print(f"armed entry 0x{ENTRY:08X}, filtering for vtable 0x{TARGET_VTABLE:08X}", flush=True)

    sock.settimeout(300)
    found = 0
    for attempt in range(MAX_ATTEMPTS):
        if attempt == 0:
            cont(sock)
        else:
            cont_over_bp(sock, ENTRY)
        r = regs(sock)
        obj = r["ecx"]
        vtbl = u32(sock, obj)
        if vtbl == TARGET_VTABLE:
            ret = u32(sock, r["esp"])
            override_ptr = u32(sock, r["esp"] + 4)
            override_str = read_wstr(sock, override_ptr) if override_ptr else "(null)"
            field18 = u32(sock, obj + 0x18)
            field1c = u32(sock, obj + 0x1C)
            is_empty_name = (override_str == "" and field18 == 0 and field1c == 0)
            if is_empty_name or found < 5 or found % 25 == 0:
                print(f"MATCH[{found}] at attempt {attempt}: obj=0x{obj:08X} ret=0x{ret:08X} "
                      f"override_ptr=0x{override_ptr:08X} override=\"{override_str}\" "
                      f"field+0x18=0x{field18:08X} field+0x1C=0x{field1c:08X}"
                      f"{'  <<< EMPTY NAME' if is_empty_name else ''}", flush=True)
            found += 1
            if is_empty_name:
                print(f"STOPPING: found empty-name instance at attempt {attempt}, obj=0x{obj:08X}", flush=True)
                break
        if attempt % 200 == 0:
            print(f"  ...attempt {attempt}: obj=0x{obj:08X} vtbl=0x{vtbl if vtbl else 0:08X} (no match yet)", flush=True)

    packet(sock, f"z0,{ENTRY:x},1")
    if found == 0:
        print(f"never matched vtable 0x{TARGET_VTABLE:08X} in {MAX_ATTEMPTS} sub_0004721E calls", flush=True)
    print(f"done ({found} matches)", flush=True)


if __name__ == "__main__":
    main()
