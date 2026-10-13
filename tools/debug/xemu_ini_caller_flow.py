"""Trace the true post-INI callers on a cold xemu SCDA boot.

The three failed open returns in sub_00040FAC are shared.  The relevant
instances return to 0x71C68 (Xbox settings) and 0x71D11 (User settings),
not to the later, unrelated 0x4745x cache dispatch.  This script captures
those exact frames and the immediately following signed-status branches.
"""
from __future__ import annotations

import socket
import struct
import time

POINTS = (
    0x00040FAC, 0x0004108A, 0x0004119F, 0x000412B4, 0x000412F4,
    0x00071C68, 0x00071C6A, 0x00071C6E, 0x00071C71, 0x00071C73,
    0x00071C79, 0x00071CB9,
    0x00071D11, 0x00071D13, 0x00071D15, 0x00071D1A, 0x00071D1C,
    0x00071D22, 0x00071D62,
)


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


def regs(sock: socket.socket) -> dict[str, int]:
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, index * 4)[0]
            for index, name in enumerate(names)}


def u32(sock: socket.socket, address: int) -> int | None:
    response = packet(sock, f"m{address:x},4")
    return None if response.startswith("E") else struct.unpack("<I", bytes.fromhex(response))[0]


def step_past_breakpoint(sock: socket.socket, point: int) -> None:
    packet(sock, f"z0,{point:x},1")
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(10)
    wait_stop(sock)
    packet(sock, f"Z0,{point:x},1")


def main() -> None:
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    for point in POINTS:
        packet(sock, f"Z0,{point:x},1")
    print("armed", ", ".join(f"0x{point:08X}" for point in POINTS), flush=True)
    start = time.time()
    relevant_entries: list[int] = []
    for hit in range(80):
        try:
            sock.sendall(b"$c#63")
            sock.recv(1)
            sock.settimeout(90)
            wait_stop(sock)
        except socket.timeout:
            print("timeout", flush=True)
            break
        r = regs(sock)
        pc = r["eip"]
        stack0 = u32(sock, r["esp"])
        caller = u32(sock, (r["ebp"] + 4) & 0xffffffff)
        if pc == 0x00040FAC:
            relevant_entries.append(stack0 or 0)
        status140 = u32(sock, 0x005ED140)
        arg28 = u32(sock, (r["ebp"] + 0x28) & 0xffffffff)
        print(
            f"HIT#{hit+1} t={time.time()-start:.1f}s pc=0x{pc:08X} "
            f"eax=0x{r['eax']:08X} ecx=0x{r['ecx']:08X} edx=0x{r['edx']:08X} "
            f"ebx=0x{r['ebx']:08X} esi=0x{r['esi']:08X} edi=0x{r['edi']:08X} "
            f"esp=0x{r['esp']:08X} ebp=0x{r['ebp']:08X} eflags=0x{r['eflags']:08X} "
            f"stack0={('0x%08X' % stack0) if stack0 is not None else 'NA'} "
            f"caller={('0x%08X' % caller) if caller is not None else 'NA'} "
            f"arg28={('0x%08X' % arg28) if arg28 is not None else 'NA'} "
            f"status140={('0x%08X' % status140) if status140 is not None else 'NA'} "
            f"seen_40fac_returns={[hex(value) for value in relevant_entries]}",
            flush=True,
        )
        step_past_breakpoint(sock, pc)
        if pc == 0x00071D62:
            print("DONE: captured both post-INI signed-status paths", flush=True)
            break
    for point in POINTS:
        packet(sock, f"z0,{point:x},1")


if __name__ == "__main__":
    main()
