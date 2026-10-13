"""2026-09-09: sanity check -- xemu_48fc5_menu_source.py's breakpoint at
0x00048FC5 (the branch inside sub_00048F1D deciding sentinel-vs-real
string) never fired in 120s+ of real xemu boot, while the same function's
own later call site into sub_00040FAC (0x00048FF4, independently already
confirmed reached via the common.lin NtCreateFile search) should fire
quickly. Arms BOTH addresses simultaneously to see which (if either)
actually gets hit first, and how many times each fires in the same
short window -- this tells us whether 0x00048FC5 is skipped entirely
(a real branch/call divergence one level up) or just slower to reach.
"""
from __future__ import annotations

import socket
import struct

ADDR_A = 0x00048FC5  # branch point (sentinel vs real string)
ADDR_B = 0x00048FF4  # call-into-sub_00040FAC return address
MAX_ATTEMPTS = 100


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


def regs(sock):
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def step_over(sock, addr):
    packet(sock, f"z0,{addr:x},1")
    sock.sendall(b"$s#73")
    sock.recv(1)
    wait_stop(sock)
    packet(sock, f"Z0,{addr:x},1")


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{ADDR_A:x},1")
    packet(sock, f"Z0,{ADDR_B:x},1")
    print(f"armed both 0x{ADDR_A:08X} (branch) and 0x{ADDR_B:08X} (call-return)", flush=True)

    sock.settimeout(180)
    counts = {ADDR_A: 0, ADDR_B: 0}
    for attempt in range(MAX_ATTEMPTS):
        cont(sock)
        r = regs(sock)
        eip = r["eip"]
        which = "A(branch)" if eip == ADDR_A else ("B(call-return)" if eip == ADDR_B else f"UNKNOWN(0x{eip:08X})")
        counts[eip] = counts.get(eip, 0) + 1
        print(f"[{attempt}] hit eip=0x{eip:08X} -> {which}  totals: A={counts.get(ADDR_A,0)} B={counts.get(ADDR_B,0)}",
              flush=True)
        step_over(sock, eip)

    packet(sock, f"z0,{ADDR_A:x},1")
    packet(sock, f"z0,{ADDR_B:x},1")
    print(f"done: A(branch)={counts.get(ADDR_A,0)} B(call-return)={counts.get(ADDR_B,0)}", flush=True)


if __name__ == "__main__":
    main()
