"""One-shot: halt xemu (wherever paused) and read MEM32(0x81BE80) --
the newly-allocated object's own vtable pointer -- plus the vtable
slot at +0x50, to identify exactly which concrete method real
hardware calls there."""
from __future__ import annotations
import socket
import struct
import time

OBJ = 0x0081BE80


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
    if address == 0:
        return None
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

vtable = read_u32(sock, OBJ)
print(f"MEM32(0x{OBJ:08X}) [vtable ptr] = 0x{vtable if vtable is not None else -1:08X}")
if vtable:
    slot50 = read_u32(sock, vtable + 0x50)
    print(f"MEM32(vtable+0x50) = 0x{slot50 if slot50 is not None else -1:08X}")
    for off in range(0, 0x60, 4):
        v = read_u32(sock, vtable + off)
        print(f"  vtable+0x{off:02X} = 0x{v if v is not None else -1:08X}")

sock.sendall(b"$c#63")
print("resumed", sock.recv(4))
