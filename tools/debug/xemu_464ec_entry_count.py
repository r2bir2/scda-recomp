"""Count how many times guest PC 0x000464EC (sub_000464EC's own entry,
not the internal loop label) is hit before reaching sub_001D589D. This
distinguishes "one call that loops many times" from "many separate
calls", each contributing to the earlier 0x00046865 hit count.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


TARGET_PC = 0x000464EC
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


def read_u32(sock: socket.socket, address: int) -> int | None:
    if address == 0:
        return None
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def get_regs(sock: socket.socket) -> dict:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=1200.0)
    parser.add_argument("--max-hits", type=int, default=200)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(args.timeout)
    try:
        # Sending any command while the target is still running makes the
        # stub implicitly halt it first, and that halt's own stop-reply
        # sits ahead of the command's real response in the stream,
        # desyncing packet framing. Force a halt (idempotent if already
        # stopped) and drain its stop-reply before issuing real commands.
        sock.sendall(b"\x03")
        time.sleep(0.3)
        try:
            wait_stop(sock)
        except socket.timeout:
            pass
        r1 = packet(sock, f"Z0,{TARGET_PC:x},1")
        r2 = packet(sock, f"Z0,{STOP_PC:x},1")
        print(f"bp results: {r1} {r2}")
        send_continue(sock)
        hits = 0
        while hits < args.max_hits:
            stop = wait_stop(sock)
            r = get_regs(sock)
            pc = r["eip"]
            if pc == STOP_PC:
                print(f"REACHED sub_001D589D after {hits} sub_000464EC entries")
                break
            if pc == TARGET_PC:
                hits += 1
                ecx = r["ecx"]
                arg1 = read_u32(sock, r["esp"] + 4)
                arg2 = read_u32(sock, r["esp"] + 8)
                print(f"#{hits} ecx={ecx:08X} arg1={arg1 if arg1 is not None else -1:08X} "
                      f"arg2={arg2 if arg2 is not None else -1:08X}")
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
