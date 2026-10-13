"""2026-09-09 (session 2): sub_000778BC (a generic grow/memcpy-buffer
helper, called only from sub_00077B61 per static call-graph) performs
the REP MOVSD that populates our nested file-manager-entry container
object (0x0082C784 this boot) at guest eip 0x00077A2C, t~=1.49s.

sub_00077B61 itself has three static callers (0x0003EDCC, 0x00366863,
0x004518CC) -- this script identifies, via a REAL breakpoint at
sub_000778BC's own entry (reading [esp] for the immediate return
address, which is reliable regardless of frame type since it's set by
the CALL instruction, not a stack-walk), which exact call reaches our
object, by checking ecx (the "this"/container pointer convention) at
entry against the known object address.
"""
from __future__ import annotations

import socket
import struct
import time

ENTRY = 0x000778BC
OBJ = 0x0082C784
MAX_HITS = 400


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


def step(sock):
    sock.sendall(b"$s#73")
    sock.recv(1)
    wait_stop(sock)


def cont_over_bp(sock, bp_addr):
    """Step off a just-hit software breakpoint before re-continuing, so the
    same address doesn't retrigger immediately without making progress."""
    packet(sock, f"z0,{bp_addr:x},1")
    step(sock)
    packet(sock, f"Z0,{bp_addr:x},1")
    sock.sendall(b"$c#63")
    sock.recv(1)
    wait_stop(sock)


def regs(sock):
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def u32(sock, address):
    address &= 0xFFFFFFFF
    result = packet(sock, f"m{address:x},4")
    if result.startswith("E") or len(result) < 8:
        return None
    return struct.unpack("<I", bytes.fromhex(result))[0]


def main():
    t0 = time.monotonic()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{ENTRY:x},1")
    print(f"armed entry 0x{ENTRY:08X}", flush=True)

    sock.settimeout(60)
    for hit_no in range(MAX_HITS):
        if hit_no == 0:
            cont(sock)
        else:
            cont_over_bp(sock, ENTRY)
        r = regs(sock)
        ret = u32(sock, r["esp"])
        t = time.monotonic() - t0
        # args, per cdecl/thiscall guess: [esp+4], [esp+8] (first two stack args)
        arg0 = u32(sock, r["esp"] + 4)
        arg1 = u32(sock, r["esp"] + 8)
        near_obj = (OBJ - 0x40) <= r["ecx"] <= (OBJ + 0x40)
        if hit_no < 5 or hit_no % 20 == 0 or near_obj:
            print(f"HIT#{hit_no} t={t:.2f}s ret=0x{ret:08X} ecx=0x{r['ecx']:08X} "
                  f"eax=0x{r['eax']:08X} edx=0x{r['edx']:08X} esi=0x{r['esi']:08X} "
                  f"edi=0x{r['edi']:08X} arg[esp+4]=0x{arg0:08X} arg[esp+8]=0x{arg1:08X}",
                  flush=True)
        if r["ecx"] == OBJ or arg0 == OBJ or arg1 == OBJ or near_obj:
            print(f"  *** MATCH: this call's this/arg equals our known object 0x{OBJ:08X} ***", flush=True)
            break

    packet(sock, f"z0,{ENTRY:x},1")
    print("done", flush=True)


if __name__ == "__main__":
    main()
