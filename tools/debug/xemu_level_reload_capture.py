"""Capture sub_000464EC entries (with args) during a level (re)load, with
wall-clock timestamps so pacing can be judged afterward. bin[6] write
watching was dropped from this script -- during live gameplay it fires
continuously (per-frame allocation churn) and would drown the log; that
question is answered separately by xemu_bin6_walk_now.py snapshots.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


ENTRY_PC = 0x000464EC


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
    parser.add_argument("--timeout", type=float, default=1800.0)
    parser.add_argument("--max-hits", type=int, default=5000)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(args.timeout)
    try:
        sock.sendall(b"\x03")
        time.sleep(0.3)
        try:
            wait_stop(sock)
        except socket.timeout:
            pass
        r1 = packet(sock, f"Z0,{ENTRY_PC:x},1")
        print(f"bp results: entry={r1}", flush=True)
        t0 = time.time()
        send_continue(sock)
        seq = 0
        while seq < args.max_hits:
            wait_stop(sock)
            r = get_regs(sock)
            pc = r["eip"]
            seq += 1
            t = time.time() - t0
            if pc == ENTRY_PC:
                arg1 = read_u32(sock, r["esp"] + 4)
                arg2 = read_u32(sock, r["esp"] + 8)
                print(f"#{seq} t={t:7.2f}s ecx={r['ecx']:08X} "
                      f"arg1={arg1 if arg1 is not None else -1:08X} "
                      f"arg2={arg2 if arg2 is not None else -1:08X}", flush=True)
            else:
                print(f"#{seq} t={t:7.2f}s unexpected pc={pc:08X}", flush=True)
            send_continue(sock)
        else:
            print(f"max-hits reached ({seq})", flush=True)
    finally:
        try:
            sock.sendall(b"\x03")
            time.sleep(0.3)
            try:
                wait_stop(sock)
            except socket.timeout:
                pass
            packet(sock, f"z0,{ENTRY_PC:x},1")
            sock.sendall(b"$c#63")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
