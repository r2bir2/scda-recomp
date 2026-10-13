"""Capture every hit of guest PC 0x00045599 (vector::clear(), the
generic destructor-chain entry) whose return address is 0x00045F42
(the sub_00045F37 bucket-array teardown, NOT the sub_000464EC config-
cache path, which is separately confirmed to always pass count=0).
Prints container/data/count/capacity for direct comparison against
scda_recomp's own [SCDA-45599-CALLER] log lines."""

from __future__ import annotations

import argparse
import socket
import struct
import time


TARGET_PC = 0x00045599
WANT_RET = 0x00045F42


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
    sock.sendall(b"$c#63")
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


def halt_and_drain(sock: socket.socket) -> None:
    sock.sendall(b"\x03")
    time.sleep(0.3)
    try:
        wait_stop(sock)
    except socket.timeout:
        pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--max-hits", type=int, default=200)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    halt_and_drain(sock)
    r1 = packet(sock, f"Z0,{TARGET_PC:x},1")
    print(f"bp result: {r1}", flush=True)
    send_continue(sock)
    sock.settimeout(args.timeout)
    hits = 0
    matched = 0
    while hits < args.max_hits:
        try:
            wait_stop(sock)
        except socket.timeout:
            print(f"no (more) hits within {args.timeout}s (had {hits} total, {matched} matched)", flush=True)
            break
        hits += 1
        r = get_regs(sock)
        ret = read_u32(sock, r["esp"])
        if ret == WANT_RET:
            matched += 1
            ecx = r["ecx"]
            data = read_u32(sock, ecx)
            count = read_u32(sock, ecx + 4)
            cap = read_u32(sock, ecx + 8)
            print(f"MATCH#{matched} container=0x{ecx:08X} data=0x{data if data is not None else -1:08X} "
                  f"count={count} cap={cap} esi=0x{r['esi']:08X}", flush=True)
        else:
            print(f"skip#{hits} ret=0x{ret if ret is not None else -1:08X} ecx=0x{r['ecx']:08X}", flush=True)
        send_continue(sock)
    else:
        print(f"reached max-hits ({hits} total, {matched} matched)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
