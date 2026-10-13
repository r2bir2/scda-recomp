"""2026-09-09: catch the real Xbox kernel's own NtCreateFile (ordinal 190)
on xemu, filtered for a path that resolves to the raw Partition5 device
with no filename after it (the historical continued-58-61 OOM trigger:
an empty config-entry name causes kernel_path.c's device-root fallback
to open the bare partition as if it were a file).

There is no guest-side "bridge" on xemu the way our own recomp has
kernel_bridge.c -- xemu boots the real xboxkrnl.exe as guest code, so
NtCreateFile is a real guest function at whatever address the real
kernel loader patched into this XBE's own kernel-import thunk table.
That address is universal for a given kernel version and NOT XOR-keyed
per-title, so it is resolved dynamically here rather than guessed:

    thunk slot for ordinal 190 (NtCreateFile) = 0x004F5260
    (from tools/disasm/loader.py's kernel_imports on the same
    default.xbe used for both platforms)

The Xbox kernel loader patches this slot with the real function address
during early boot, before the title's own entry point runs -- so we
break at the XBE entry point first (0x00054775), read the patched
value, then move the real breakpoint there.

NtCreateFile (__stdcall, XBOX_OBJECT_ATTRIBUTES uses ANSI_STRING, not
UNICODE_STRING):
    NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes,
                 IoStatusBlock, AllocationSize, FileAttributes,
                 ShareAccess, CreateDisposition, CreateOptions)
At entry (before "push ebp"): esp+4=FileHandle, esp+8=DesiredAccess,
esp+0xC=ObjectAttributes(*XBOX_OBJECT_ATTRIBUTES).
XBOX_OBJECT_ATTRIBUTES = { RootDirectory@0, ObjectName(*XBOX_ANSI_STRING)@4,
                            Attributes@8 }.
XBOX_ANSI_STRING = { Length(u16)@0, MaximumLength(u16)@2, Buffer(char*)@4 }.
"""
from __future__ import annotations

import socket
import struct

ENTRY_XBE = 0x00054775
THUNK_SLOT_190 = 0x004F5260
MAX_ATTEMPTS = 3000


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
        return None, None, None
    root_dir = u32(sock, obj_attrs_ptr)
    name_ptr = u32(sock, obj_attrs_ptr + 4)
    if not name_ptr:
        return root_dir, None, None
    length = u16(sock, name_ptr)
    buf_ptr = u32(sock, name_ptr + 4)
    if length is None or not buf_ptr:
        return root_dir, length, None
    raw = read_bytes(sock, buf_ptr, min(length, 260))
    text = raw.decode("latin-1", errors="replace") if raw else None
    return root_dir, length, text


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    # 1. Break at the title's own entry point so the kernel has already
    #    patched its import thunk table.
    packet(sock, f"Z0,{ENTRY_XBE:x},1")
    print(f"armed XBE entry 0x{ENTRY_XBE:08X}, waiting for kernel init to finish...", flush=True)
    sock.settimeout(60)
    cont(sock)
    r = regs(sock)
    print(f"hit XBE entry at eip=0x{r['eip']:08X}", flush=True)
    packet(sock, f"z0,{ENTRY_XBE:x},1")

    # 2. Resolve the real (patched) NtCreateFile guest address.
    nt_create_file = u32(sock, THUNK_SLOT_190)
    print(f"resolved NtCreateFile guest address: 0x{nt_create_file:08X}", flush=True)
    if not nt_create_file:
        print("thunk slot still zero -- kernel has not patched it yet, aborting", flush=True)
        return

    # 3. Arm the real breakpoint and scan every call for a Partition5-root path.
    packet(sock, f"Z0,{nt_create_file:x},1")
    sock.settimeout(120)
    for attempt in range(MAX_ATTEMPTS):
        if attempt == 0:
            cont(sock)
        else:
            cont_over_bp(sock, nt_create_file)
        r = regs(sock)
        ret = u32(sock, r["esp"])
        obj_attrs_ptr = u32(sock, r["esp"] + 0xC)
        desired_access = u32(sock, r["esp"] + 8)
        root_dir, length, path = read_ansi_path(sock, obj_attrs_ptr)
        is_partition5_root = bool(path) and path.rstrip("\\").upper().endswith(
            "\\DEVICE\\HARDDISK0\\PARTITION5")
        if attempt < 20 or attempt % 100 == 0 or is_partition5_root:
            print(f"[{attempt}] ret=0x{ret:08X} access=0x{desired_access:08X} "
                  f"root_dir=0x{root_dir if root_dir else 0:08X} len={length} "
                  f"path=\"{path}\"{'  <<< PARTITION5 ROOT' if is_partition5_root else ''}",
                  flush=True)
        if is_partition5_root:
            packet(sock, f"z0,{nt_create_file:x},1")
            print(f"STOPPING: Partition5-root open at attempt {attempt}, caller ret=0x{ret:08X}",
                  flush=True)
            print(f"  full regs: {r}", flush=True)
            return

    packet(sock, f"z0,{nt_create_file:x},1")
    print(f"no Partition5-root open found in {MAX_ATTEMPTS} NtCreateFile calls", flush=True)


if __name__ == "__main__":
    main()
