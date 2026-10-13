"""2026-09-09: scda_recomp's own NtCreateFile trace shows it tries 18
candidate paths for "common.lin" (DataXb\LMaps\menu\common.lin, via D:/T:/Z:
with/without a "Default\" prefix -- including a "D:\Default\Default\..."
double-prefixed candidate) and then NEVER calls NtCreateFile again,
apparently stalling before it ever reaches SplinterCell4xbox.ini (xemu's
very first successful open).

This does the same real-kernel NtCreateFile trace on xemu (ordinal-190
thunk resolution, same as xemu_ntcreatefile_partition5.py), filtered for
any path containing "common.lin" (case-insensitive), to see: does xemu
try the same candidates, does one of them succeed, and where does
common.lin sit relative to SplinterCell4xbox.ini in the real boot order?
"""
from __future__ import annotations

import socket
import struct

ENTRY_XBE = 0x00054775
THUNK_SLOT_190 = 0x004F5260
MAX_ATTEMPTS = 6000


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
    seen_commonlin = 0
    seen_splintercell = False
    for attempt in range(MAX_ATTEMPTS):
        if attempt == 0:
            cont(sock)
        else:
            cont_over_bp(sock, nt_create_file)
        r = regs(sock)
        obj_attrs_ptr = u32(sock, r["esp"] + 0xC)
        path = read_ansi_path(sock, obj_attrs_ptr)
        lower = (path or "").lower()
        if "common.lin" in lower:
            seen_commonlin += 1
            print(f"[{attempt}] (splintercell4xbox.ini seen so far: {seen_splintercell}) "
                  f"path=\"{path}\"", flush=True)
        if "splintercell4xbox.ini" in lower and not seen_splintercell:
            seen_splintercell = True
            print(f"[{attempt}] *** first SplinterCell4xbox.ini open ***", flush=True)
        if attempt % 500 == 0:
            print(f"  ...attempt {attempt}: path=\"{path}\"", flush=True)

    packet(sock, f"z0,{nt_create_file:x},1")
    print(f"done: {seen_commonlin} common.lin occurrences in {MAX_ATTEMPTS} calls, "
          f"SplinterCell4xbox.ini seen: {seen_splintercell}", flush=True)


if __name__ == "__main__":
    main()
