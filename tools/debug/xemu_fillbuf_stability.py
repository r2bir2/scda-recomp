"""2026-09-16 (Update 158, verification step): is sub_004BF700's
incoming buffer (EAX at entry) a single persistent buffer reused every
call, or does it vary call to call? Needed to judge whether the
overflow_relwatch2.py cross-platform comparison actually compared the
same logical buffer.
"""
from __future__ import annotations

import socket
import struct

FILL_ENTRY = 0x004BF700
HITS = 6


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
    packet(sock, f"Z0,{FILL_ENTRY:x},1")
    print(f"armed 0x{FILL_ENTRY:08X}", flush=True)

    sock.settimeout(300)
    for i in range(HITS):
        if i == 0:
            cont(sock)
        else:
            cont_over_bp(sock, FILL_ENTRY)
        r = regs(sock)
        print(f"hit {i}: eax(buffer)=0x{r['eax']:08X} ecx=0x{r['ecx']:08X} edx=0x{r['edx']:08X}", flush=True)

    packet(sock, f"z0,{FILL_ENTRY:x},1")
    print("done", flush=True)


if __name__ == "__main__":
    main()
