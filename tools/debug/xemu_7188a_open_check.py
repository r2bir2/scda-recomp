"""2026-09-09: sub_0007188A is the config/INI-file "open, read as UTF-16
text, parse [bracket] sections into a hashtable via sub_0004693B" routine
(confirmed statically: after a successful open+read it strips CR/LF,
looks for a leading '[' / trailing ']', extracts the key, and looks it
up/inserts it via sub_0004693B -> sub_00046A69 -> sub_00046A89, which is
the exact call chain the frozen-boot watchpoint already proved feeds the
first real write into the vtable-0x4F7248 container).

Signature (cdecl, ebp+8/0xC/0x10 at entry once the prologue has run;
before "push ebp" executes -- which is where this breaks -- the args
sit at esp+4/esp+8/esp+0xC):
    sub_0007188A(out_struct, name, manager)

This walks every call to sub_0007188A during boot and prints the name
string being opened plus the manager pointer, so we can find the exact
occurrence where name is empty (the one built from the vtable-0x4F7248
file-manager object's own zeroed fields, i.e. the historical Partition5/
OOM entry) and see what eax (open succeeded/failed) comes back as.
"""
from __future__ import annotations

import socket
import struct

ENTRY = 0x0007188A
MAX_ATTEMPTS = 2000


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
    """Step over a still-armed breakpoint at the current PC, then continue.
    A bare repeated 'c' against a software breakpoint retriggers
    immediately without progress (documented in xemu_778bc_caller_check.py);
    this removes it, single-steps past it, re-arms it, then continues."""
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
    print(f"armed sub_0007188A entry 0x{ENTRY:08X}", flush=True)

    sock.settimeout(300)
    for attempt in range(MAX_ATTEMPTS):
        if attempt == 0:
            cont(sock)
        else:
            cont_over_bp(sock, ENTRY)
        r = regs(sock)
        ret = u32(sock, r["esp"])
        out_ptr = u32(sock, r["esp"] + 4)
        name_ptr = u32(sock, r["esp"] + 8)
        manager_ptr = u32(sock, r["esp"] + 0xC)
        name = read_wstr(sock, name_ptr)
        if attempt < 60 or attempt % 20 == 0 or name == "":
            print(f"[{attempt}] ret=0x{ret:08X} out=0x{out_ptr:08X} "
                  f"name_ptr=0x{name_ptr:08X} name=\"{name}\" manager=0x{manager_ptr:08X}",
                  flush=True)
        if name == "":
            # empty name -- this is our target. Arm a one-shot bp on the
            # return address to capture eax (open succeeded/failed).
            packet(sock, f"z0,{ENTRY:x},1")
            packet(sock, f"Z0,{ret:x},1")
            cont(sock)
            r2 = regs(sock)
            print(f"  >>> EMPTY NAME occurrence found. return eax=0x{r2['eax']:08X} "
                  f"(0=open/read failed, !=0=succeeded)", flush=True)
            packet(sock, f"z0,{ret:x},1")
            print("done (stopped at first empty-name hit)", flush=True)
            return

    packet(sock, f"z0,{ENTRY:x},1")
    print(f"no empty-name occurrence found in {MAX_ATTEMPTS} sub_0007188A calls", flush=True)
    print("done", flush=True)


if __name__ == "__main__":
    main()
