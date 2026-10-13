"""Clear the 0x00045599 breakpoint and resume."""
from __future__ import annotations
import socket

TARGET_PC = 0x00045599


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


sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
sock.settimeout(10)
print("clear:", packet(sock, f"z0,{TARGET_PC:x},1"))
sock.sendall(b"$c#63")
print("resumed", sock.recv(4))
