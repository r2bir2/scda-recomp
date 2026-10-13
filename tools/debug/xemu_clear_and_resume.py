"""Halt, remove the known breakpoints/watchpoints from prior capture scripts,
and resume -- safety cleanup in case a capture script was killed mid-run."""
from __future__ import annotations
import socket
import time

ENTRY_PC = 0x000464EC
STOP_PC = 0x001D589D
BIN6_ADDR = 0x0064D2B4


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


sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
sock.settimeout(10)
sock.sendall(b"\x03")
time.sleep(0.3)
try:
    wait_stop(sock)
except socket.timeout:
    pass
for cmd in (f"z0,{ENTRY_PC:x},1", f"z0,{STOP_PC:x},1", f"z2,{BIN6_ADDR:x},4"):
    print(cmd, packet(sock, cmd))
sock.sendall(b"$c#63")
print("resumed", sock.recv(4))
