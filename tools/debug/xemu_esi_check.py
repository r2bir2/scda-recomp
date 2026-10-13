"""Check esi's value on real hardware at loc_00047AA4 (right after
sub_00046F46 returns inside sub_00047A92), to determine whether
scda_recomp's esi=0 there is a genuine stack/register-restoration bug
(scda_recomp-specific) or matches real hardware (in which case esi=0
is expected/correct there and the true divergence lies elsewhere)."""
from __future__ import annotations
import socket
import struct
import time

MARKER_A = 0x000366A2
TARGET = 0x00047AA4


def packet(sock, body):
    raw = body.encode("ascii")
    sock.sendall(b"$" + raw + b"#" + f"{sum(raw) & 0xff:02x}".encode("ascii"))
    first = sock.recv(1)
    if first == b"+":
        first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    response = bytearray()
    while True:
        byte = sock.recv(1)
        if byte == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return response.decode("ascii")
        response.extend(byte)


def wait_stop(sock):
    first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    body = bytearray()
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return body.decode("ascii")
        body.extend(char)


def read_u32(sock, address):
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def get_regs(sock):
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def cont_and_wait(sock, timeout=60):
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)
    return get_regs(sock)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    packet(sock, f"Z0,{MARKER_A:x},1")
    regs = cont_and_wait(sock, 120)
    assert regs["eip"] == MARKER_A
    print("MARKER A HIT", flush=True)
    packet(sock, f"z0,{MARKER_A:x},1")

    packet(sock, f"Z0,{TARGET:x},1")
    print(f"armed 0x{TARGET:08X}", flush=True)
    regs = cont_and_wait(sock, 120)
    assert regs["eip"] == TARGET
    esi = regs["esi"]
    field24 = read_u32(sock, esi + 0x24) if esi else None
    field24_size = read_u32(sock, esi + 0x28) if esi else None
    field24_cap = read_u32(sock, esi + 0x2C) if esi else None
    ecx = regs["ecx"]
    print(f"AT 0x{TARGET:08X}: esi=0x{esi:08X} ecx=0x{ecx:08X} esp=0x{regs['esp']:08X} "
          f"field24_data=0x{field24 if field24 else 0:08X} "
          f"field24_size={field24_size} field24_cap={field24_cap}", flush=True)


if __name__ == "__main__":
    main()
