"""2026-09-09: sub_00048F1D/sub_00040FAC (scda_recomp's own code path for
opening "...\LMaps\common.lin") is proven NOT reached by xemu at all
(two breakpoints inside/around it never fired in 180s+ of real, actively
computing boot). This walks the GUEST EBP chain at the real kernel
NtCreateFile entry, filtered for the correct (non-"menu") common.lin
path, to find which DIFFERENT guest function(s) xemu actually uses.

At NtCreateFile's entry (before its own "push ebp" executes), the CPU's
current ebp register still belongs to the immediate CALLER's frame (the
function that executed "call [thunk_slot_190]"), and [esp] is the
return address into that same caller, right after the call instruction.
Standard x86 cdecl prologue ("push ebp; mov ebp,esp") means each frame's
saved ebp is at [ebp+0] and its own return address is at [ebp+4], so
this walks that chain upward for several levels -- exactly the "one-hop
frame walk" pattern already validated in xemu_47505_first_write.py,
extended to multiple hops.
"""
from __future__ import annotations

import socket
import struct

ENTRY_XBE = 0x00054775
THUNK_SLOT_190 = 0x004F5260
MAX_ATTEMPTS = 3000
CHAIN_DEPTH = 10


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


def u16(sock, address):
    address &= 0xFFFFFFFF
    result = packet(sock, f"m{address:x},2")
    if result.startswith("E") or len(result) < 4:
        return None
    return struct.unpack("<H", bytes.fromhex(result))[0]


def read_bytes(sock, address, n):
    address &= 0xFFFFFFFF
    result = packet(sock, f"m{address:x},{n:x}")
    if result.startswith("E"):
        return None
    return bytes.fromhex(result)


def read_ansi_path(sock, obj_attrs_ptr):
    if not obj_attrs_ptr:
        return None
    name_ptr = u32(sock, obj_attrs_ptr + 4)
    if not name_ptr:
        return None
    length = u16(sock, name_ptr)
    buf_ptr = u32(sock, name_ptr + 4)
    if length is None or not buf_ptr:
        return None
    raw = read_bytes(sock, buf_ptr, min(length, 260))
    return raw.decode("latin-1", errors="replace") if raw else None


def walk_ebp_chain(sock, ret0, ebp0, depth):
    """ret0 = return address into the immediate caller (already known).
    ebp0 = that caller's own frame pointer (register value at break time).
    Returns a list of (frame_ebp, return_address) pairs, frame 0 first."""
    chain = [(ebp0, ret0)]
    ebp = ebp0
    for _ in range(depth - 1):
        saved_ebp = u32(sock, ebp)
        ret = u32(sock, ebp + 4)
        if saved_ebp is None or ret is None or saved_ebp == 0:
            break
        chain.append((saved_ebp, ret))
        if saved_ebp <= ebp:  # stack must grow "up" (toward higher addrs) frame by frame
            break
        ebp = saved_ebp
    return chain


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    packet(sock, f"Z0,{ENTRY_XBE:x},1")
    print(f"armed XBE entry 0x{ENTRY_XBE:08X}, waiting for kernel init...", flush=True)
    sock.settimeout(60)
    cont(sock)
    packet(sock, f"z0,{ENTRY_XBE:x},1")

    nt_create_file = u32(sock, THUNK_SLOT_190)
    print(f"resolved NtCreateFile guest address: 0x{nt_create_file:08X}", flush=True)

    packet(sock, f"Z0,{nt_create_file:x},1")
    sock.settimeout(180)
    for attempt in range(MAX_ATTEMPTS):
        if attempt == 0:
            cont(sock)
        else:
            cont_over_bp(sock, nt_create_file)
        r = regs(sock)
        obj_attrs_ptr = u32(sock, r["esp"] + 0xC)
        path = read_ansi_path(sock, obj_attrs_ptr)
        lower = (path or "").lower()
        if "lmaps" in lower and "common.lin" in lower:
            ret0 = u32(sock, r["esp"])
            print(f"[{attempt}] MATCH path=\"{path}\" ebp(caller frame)=0x{r['ebp']:08X} "
                  f"ret0=0x{ret0:08X}", flush=True)
            chain = walk_ebp_chain(sock, ret0, r["ebp"], CHAIN_DEPTH)
            for i, (frame_ebp, ret) in enumerate(chain):
                print(f"  frame[{i}] ebp=0x{frame_ebp:08X} return_addr=0x{ret:08X}", flush=True)
            packet(sock, f"z0,{nt_create_file:x},1")
            print("done (stopped at first LMaps\\common.lin match)", flush=True)
            return
        if attempt % 200 == 0:
            print(f"  ...attempt {attempt}: path=\"{path}\"", flush=True)

    packet(sock, f"z0,{nt_create_file:x},1")
    print(f"no LMaps/common.lin match found in {MAX_ATTEMPTS} calls", flush=True)


if __name__ == "__main__":
    main()
