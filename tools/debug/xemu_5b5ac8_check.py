"""Halt xemu wherever it currently is and read MEM32(0x5B5AC4)/MEM32(0x5B5AC8)
-- the settings-struct fields read right before the "True"/string-select
branch inside sub_001CC602 -- to compare against scda_recomp's observed
eax=1 at that point."""
from __future__ import annotations
import socket
import struct
import time

TARGETS = [0x5B5A88, 0x5B5AB0, 0x5B5AC4, 0x5B5AC8]


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


sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
sock.settimeout(10)
sock.sendall(b"\x03")
time.sleep(0.3)
try:
    wait_stop(sock)
except socket.timeout:
    pass

for addr in TARGETS:
    v = read_u32(sock, addr)
    print(f"MEM32(0x{addr:08X}) = 0x{v if v is not None else -1:08X}")

sock.sendall(b"$c#63")
print("resumed", sock.recv(4))
