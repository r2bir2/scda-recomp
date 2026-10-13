"""2026-09-09 (new session, correct-disc re-run): same as
xemu_47505_container_check.py but adds wall-clock elapsed timing (the
prior run's dvd_path had been pointing at a modified SC4_xbe_hack test
ISO, not the retail scda.iso -- this rerun is against the corrected
xemu.toml). Confirms object address, container field values, AND the
elapsed real seconds from process launch to the match, so a follow-up
frozen-boot write-watch run knows roughly where in time to look.
"""
from __future__ import annotations

import socket
import struct
import time

ENTRY = 0x0004721E
FIELDS_PC = 0x00047858
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
    t0 = time.monotonic()
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
            t_match = time.monotonic() - t0
            print(f"MATCH at attempt {attempt}, t={t_match:.2f}s: obj=0x{obj:08X} vtbl=0x{vtbl:08X}", flush=True)
            found = True
            break
        if attempt % 50 == 0:
            print(f"  ...attempt {attempt} t={time.monotonic()-t0:.2f}s: obj=0x{obj:08X} vtbl=0x{vtbl if vtbl else 0:08X}", flush=True)

    packet(sock, f"z0,{ENTRY:x},1")

    if not found:
        print(f"never matched vtable 0x{TARGET_VTABLE:08X} in {MAX_ATTEMPTS} sub_0004721E calls", flush=True)
        return

    packet(sock, f"Z0,{FIELDS_PC:x},1")
    cont(sock)
    r2 = regs(sock)
    esi = r2["esi"]
    base = u32(sock, esi)
    buckets = u32(sock, esi + 0x0C)
    mask = u32(sock, esi + 0x10)
    t_fields = time.monotonic() - t0
    print(f"FIELDS t={t_fields:.2f}s pc=0x{r2['eip']:08X} esi=0x{esi:08X} "
          f"container_base=0x{base:08X} container_buckets=0x{buckets:08X} "
          f"container_mask=0x{mask:08X}", flush=True)
    packet(sock, f"z0,{FIELDS_PC:x},1")
    print("done", flush=True)


if __name__ == "__main__":
    main()
