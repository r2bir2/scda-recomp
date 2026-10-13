"""Count real calls into sub_00036116 whose guest return address is
0x00046865 (the loop-back call site inside sub_000464EC), from process
boot up to sub_001D589D, via xemu's GDB stub. Mirrors the CDB technique
already used on the scda_recomp side (cdb_36116_guestret.txt).
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


TARGET_FUNC = 0x00036116
STOP_PC = 0x001D589D
TARGET_RET = 0x00046865


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


def read_u32(sock: socket.socket, address: int) -> int | None:
    if address == 0:
        return None
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def regs(sock: socket.socket) -> dict[str, int]:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=1200.0)
    parser.add_argument("--max-hits", type=int, default=2000)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(args.timeout)
    try:
        r1 = packet(sock, f"Z0,{TARGET_FUNC:x},1")
        r2 = packet(sock, f"Z0,{STOP_PC:x},1")
        print(f"bp results: {r1} {r2}")
        send_continue(sock)
        hits = 0
        target_hits = 0
        while hits < args.max_hits:
            stop = wait_stop(sock)
            r = regs(sock)
            pc = r["eip"]
            if pc == STOP_PC:
                print(f"REACHED sub_001D589D after {hits} sub_00036116 calls "
                      f"({target_hits} from guest ret 0x{TARGET_RET:08X})")
                break
            ret = read_u32(sock, r["esp"])
            hits += 1
            if ret == TARGET_RET:
                target_hits += 1
            print(f"#{hits} ecx={r['ecx']:08X} ret={ret if ret is not None else -1:08X}")
            send_continue(sock)
    finally:
        try:
            sock.sendall(b"\x03")
            time.sleep(0.3)
            try:
                wait_stop(sock)
            except socket.timeout:
                pass
            packet(sock, f"z0,{TARGET_FUNC:x},1")
            packet(sock, f"z0,{STOP_PC:x},1")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
