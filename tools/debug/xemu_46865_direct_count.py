"""Directly count hits at guest PC 0x00046865 (the loop-back call site
inside sub_000464EC that calls sub_00036116) from process boot up to
sub_001D589D, via xemu's GDB stub -- a direct breakpoint on the guest PC
itself, avoiding the noise of counting every sub_00036116 call.
"""

from __future__ import annotations

import argparse
import socket
import time


TARGET_PC = 0x00046865
STOP_PC = 0x001D589D


def packet(sock: socket.socket, body: str) -> str:
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


def send_continue(sock: socket.socket) -> None:
    body = b"c"
    sock.sendall(b"$" + body + b"#63")
    if sock.recv(1) != b"+":
        raise RuntimeError("GDB stub did not acknowledge continue")


def wait_stop(sock: socket.socket) -> str:
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


def get_pc(sock: socket.socket) -> int:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    import struct
    return struct.unpack_from("<I", data, 8 * 4)[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=1200.0)
    parser.add_argument("--max-hits", type=int, default=5000)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(args.timeout)
    try:
        r1 = packet(sock, f"Z0,{TARGET_PC:x},1")
        r2 = packet(sock, f"Z0,{STOP_PC:x},1")
        print(f"bp results: {r1} {r2}")
        send_continue(sock)
        hits = 0
        while hits < args.max_hits:
            stop = wait_stop(sock)
            pc = get_pc(sock)
            if pc == STOP_PC:
                print(f"REACHED sub_001D589D after {hits} hits at 0x{TARGET_PC:08X}")
                break
            if pc == TARGET_PC:
                hits += 1
            else:
                print(f"unexpected stop pc={pc:08X}")
                break
            send_continue(sock)
        else:
            print(f"max-hits reached ({hits}) without reaching sub_001D589D")
    finally:
        try:
            sock.sendall(b"\x03")
            time.sleep(0.3)
            try:
                wait_stop(sock)
            except socket.timeout:
                pass
            packet(sock, f"z0,{TARGET_PC:x},1")
            packet(sock, f"z0,{STOP_PC:x},1")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
