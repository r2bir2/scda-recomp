"""Trace the exact config-path-builder result in the SCDA retail XBE.

0x40347 is immediately after sub_000725A5 has built the path in EAX and
immediately before the resulting string is passed to sub_000511E7.  This is
far less frequent than the generic file-open helper and gives an unambiguous
ground-truth point for the two optional INI probes.
"""
from __future__ import annotations

import socket
import struct
import time

POINT = 0x00040347
NAMES = (b"splintercell4xbox.ini", b"splintercell4user.ini")


def packet(sock: socket.socket, body: str) -> str:
    raw = body.encode("ascii")
    sock.sendall(b"$" + raw + b"#" + f"{sum(raw) & 0xff:02x}".encode("ascii"))
    first = sock.recv(1)
    if first == b"+":
        first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    result = bytearray()
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return result.decode("ascii")
        result.extend(char)


def wait_stop(sock: socket.socket) -> None:
    first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return


def continue_to_stop(sock: socket.socket, timeout: float) -> None:
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)


def regs(sock: socket.socket) -> dict[str, int]:
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def cstring(sock: socket.socket, address: int) -> bytes:
    response = packet(sock, f"m{address:x},104")
    raw = b"" if response.startswith("E") else bytes.fromhex(response)
    return raw.split(b"\0", 1)[0]


def main() -> None:
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{POINT:x},1")
    print(f"armed path-builder return 0x{POINT:08X}", flush=True)
    start = time.time()
    hits = 0
    matches = 0
    # The same high-level loader builds many non-INI paths during early boot.
    # The name filter is exact, so retain enough samples to reach the late
    # fallback probes rather than mistaking an early subset for absence.
    while time.time() - start < 300 and hits < 20000:
        try:
            continue_to_stop(sock, min(60, 301 - (time.time() - start)))
        except socket.timeout:
            print("timeout", flush=True)
            break
        r = regs(sock)
        hits += 1
        path = cstring(sock, r["eax"])
        if any(name in path.lower() for name in NAMES):
            matches += 1
            print(
                f"MATCH #{matches} t={time.time()-start:.1f}s path={path.decode('latin1', 'replace')} "
                f"eax=0x{r['eax']:08X} ebp=0x{r['ebp']:08X} esp=0x{r['esp']:08X} "
                f"ebx=0x{r['ebx']:08X} esi=0x{r['esi']:08X} edi=0x{r['edi']:08X}",
                flush=True,
            )
            if matches >= 6:
                break
    packet(sock, f"z0,{POINT:x},1")
    print(f"done hits={hits} matches={matches}", flush=True)


if __name__ == "__main__":
    main()
