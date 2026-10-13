"""2026-09-16 (Update 156/157): xemu ground truth for the size requested
at VA 0x000771F9 (sub_00077194's call into sub_00076B70, forwarding a
size into NtAllocateVirtualMemory / xbox_HeapAlloc's chain).

On recomp, this exact breakpoint (filtered to size > 0x1000) fires
EXACTLY ONCE before the crash, with edi=0xAD000 -- the block-8
allocation whose decode fill loop (sub_004BF700) later overflows into
the adjacent block 9 (the Bink stream header object), per Update 155.

This checks what real hardware requests for the SAME call site: if it
also requests exactly 0xAD000, the bug is in the fill loop itself
(too many iterations). If it requests something LARGER, the bug is in
the size formula (or its width/height/format inputs) further up the
call chain.
"""
from __future__ import annotations

import socket
import struct

ENTRY = 0x000771F9
MAX_HITS = 6


def packet(sock, body):
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


def wait_stop(sock):
    first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return


def cont(sock):
    sock.sendall(b"$c#63")
    sock.recv(1)
    wait_stop(sock)


def step_once(sock):
    sock.sendall(b"$s#73")
    sock.recv(1)
    wait_stop(sock)


def cont_over_bp(sock, bp_addr):
    packet(sock, f"z0,{bp_addr:x},1")
    step_once(sock)
    packet(sock, f"Z0,{bp_addr:x},1")
    cont(sock)


def regs(sock):
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(30)
    packet(sock, f"Z0,{ENTRY:x},1")
    print(f"armed 0x{ENTRY:08X}", flush=True)

    sock.settimeout(600)
    hit = 0
    tries = 0
    while hit < MAX_HITS and tries < 500:
        tries += 1
        if hit == 0 and tries == 1:
            cont(sock)
        else:
            cont_over_bp(sock, ENTRY)
        r = regs(sock)
        edi = r["edi"]
        if edi > 0x1000:
            hit += 1
            print(f"hit {hit} (try {tries}): edi=0x{edi:08X} esp=0x{r['esp']:08X}", flush=True)

    packet(sock, f"z0,{ENTRY:x},1")
    print("done", flush=True)


if __name__ == "__main__":
    main()
