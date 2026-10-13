"""One-off: interrupt a running xemu (paused via GDB break-in), read
arbitrary memory addresses, print them, then resume."""
from __future__ import annotations
import socket
import struct
import sys


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


def main():
    addrs = [0x5B1D90, 0x5B1D94]
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    # break-in
    sock.sendall(b"\x03")
    try:
        wait_stop(sock)
    except socket.timeout:
        pass
    for addr in addrs:
        response = packet(sock, f"m{addr:x},4")
        data = bytes.fromhex(response)
        val = struct.unpack("<I", data)[0]
        print(f"0x{addr:08X} = 0x{val:08X}")
    # resume
    sock.sendall(b"$c#63")
    sock.recv(1)


if __name__ == "__main__":
    main()
