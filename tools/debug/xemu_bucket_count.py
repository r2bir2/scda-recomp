"""Arm 0x00045FB0 (sub_00045FB0 entry) and wait specifically for
ecx==0x0109B498 (the exact hashtable object scda_recomp resolved for
the bucket-array-teardown case, no-ASLR so the same address is
expected on real hardware if reached the same way). Reads
MEM32(ecx)=bucket_array and MEM32(ecx+4)=bucket_count directly."""

from __future__ import annotations

import argparse
import socket
import struct
import time


TARGET_PC = 0x00045FB0
WANT_ECX = 0x0109B498


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
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--max-hits", type=int, default=5000)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    halt_and_drain(sock)
    r1 = packet(sock, f"Z0,{TARGET_PC:x},1")
    print(f"bp result: {r1}", flush=True)
    t0 = time.time()
    send_continue(sock)
    sock.settimeout(args.timeout)
    hits = 0
    while hits < args.max_hits:
        try:
            wait_stop(sock)
        except socket.timeout:
            print(f"no (more) hits within {args.timeout}s (had {hits})", flush=True)
            break
        hits += 1
        r = get_regs(sock)
        t = time.time() - t0
        if r["ecx"] == WANT_ECX:
            bucket_array = read_u32(sock, r["ecx"])
            bucket_count = read_u32(sock, r["ecx"] + 4)
            print(f"MATCH at t={t:.2f}s hit#{hits} container=0x{r['ecx']:08X} "
                  f"bucket_array=0x{bucket_array if bucket_array is not None else -1:08X} "
                  f"bucket_count={bucket_count}", flush=True)
            break
        else:
            print(f"hit#{hits} t={t:.2f}s ecx=0x{r['ecx']:08X} (not the target container)", flush=True)
        send_continue(sock)
    else:
        print(f"reached max-hits ({hits}) without matching container", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
