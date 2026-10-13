"""Locate the *specific* SCDA callers that open the two optional config INIs.

This deliberately breaks at the shared guest file-open helper (0x511E7),
reads its stack argument before the helper builds a frame, and filters on the
two names.  It therefore avoids confusing an unrelated use of the helper
with the post-startup fallback path.  The return address printed for a match
is the immediate original-XBE caller.
"""
from __future__ import annotations

import socket
import struct
import time

OPEN_HELPER = 0x000511E7
NAMES = (b"splintercell4xbox.ini", b"splintercell4user.ini")


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


def wait_stop(sock: socket.socket) -> None:
    first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    while True:
        byte = sock.recv(1)
        if byte == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return


def cont(sock: socket.socket, timeout: float) -> None:
    sock.sendall(b"$c#63")
    sock.recv(1)  # acknowledgement
    sock.settimeout(timeout)
    wait_stop(sock)


def regs(sock: socket.socket) -> dict[str, int]:
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def mem(sock: socket.socket, address: int, size: int) -> bytes:
    raw = packet(sock, f"m{address:x},{size:x}")
    return b"" if raw.startswith("E") else bytes.fromhex(raw)


def cstring(sock: socket.socket, address: int) -> bytes:
    raw = mem(sock, address, 260)
    return raw.split(b"\0", 1)[0]


def main() -> None:
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{OPEN_HELPER:x},1")
    print(f"armed helper 0x{OPEN_HELPER:08X}; filtering only target INI opens", flush=True)
    started = time.time()
    hits = 0
    matches = 0
    # Early boot performs thousands of unrelated opens.  Keep the probe
    # narrow by filtering the path, but allow enough helper hits to reach
    # the late INI fallback without restarting xemu in the middle of boot.
    while time.time() - started < 300 and hits < 20000:
        try:
            cont(sock, min(45, 301 - (time.time() - started)))
        except socket.timeout:
            print("timeout without another helper entry", flush=True)
            break
        r = regs(sock)
        hits += 1
        # At helper entry: [ESP]=return address, [ESP+4]=first (path) arg.
        words = mem(sock, r["esp"], 16)
        if len(words) < 8:
            continue
        ret, path_ptr = struct.unpack_from("<II", words)
        path = cstring(sock, path_ptr)
        lower = path.lower()
        if any(name in lower for name in NAMES):
            matches += 1
            print(
                f"MATCH #{matches} t={time.time()-started:.1f}s pc=0x{r['eip']:08X} "
                f"caller_return=0x{ret:08X} esp=0x{r['esp']:08X} path_ptr=0x{path_ptr:08X} "
                f"path={path.decode('latin1', 'replace')} eax=0x{r['eax']:08X} "
                f"ecx=0x{r['ecx']:08X} ebx=0x{r['ebx']:08X} esi=0x{r['esi']:08X} edi=0x{r['edi']:08X}",
                flush=True,
            )
            if matches >= 4:
                break
    packet(sock, f"z0,{OPEN_HELPER:x},1")
    print(f"done: helper_hits={hits}, target_matches={matches}", flush=True)


if __name__ == "__main__":
    main()
