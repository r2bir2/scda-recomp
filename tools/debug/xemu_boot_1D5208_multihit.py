"""Re-verification: is the very-early (~1.4s post-reset) hit on
0x001D5208 genuine SCDA execution or a spurious/false-positive hit?
Catches several hits in a row and reports ecx/esi/edi progression --
genuine execution of the sub_001D5B9B loop should show a small,
structured increment in ecx (~0x8C-0xC4 bytes) between consecutive
hits, matching the pattern already confirmed in scda_recomp's own log.
Meant to be run against a FRESH xemu launched with -s -S, continued
immediately (no title-screen wait)."""

from __future__ import annotations

import argparse
import socket
import struct
import time


TARGET_PC = 0x001D5208


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--max-hits", type=int, default=15)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(args.timeout)
    r1 = packet(sock, f"Z0,{TARGET_PC:x},1")
    print(f"bp result: {r1}", flush=True)
    t0 = time.time()
    send_continue(sock)
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
        # First dword of the retaddr on stack tells us who called this.
        ret = read_u32(sock, r["esp"])
        print(f"HIT#{hits} t={t:.2f}s ecx={r['ecx']:08X} esi={r['esi']:08X} "
              f"edi={r['edi']:08X} esp={r['esp']:08X} ret={ret if ret is not None else -1:08X}",
              flush=True)
        send_continue(sock)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
