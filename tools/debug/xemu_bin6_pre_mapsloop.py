"""Single, unambiguous checkpoint: sub_001D589D's own entry (called
exactly once per static analysis -- unlike sub_001D5208, which showed
an ambiguous, possibly-artifact early hit). Arms a breakpoint there,
waits, and on hit walks bin[6]'s free chain the same way
xemu_bin6_walk_now.py does, to compare headroom against scda_recomp's
own [SCDA-BIN6-PRE-MAPSLOOP] measurement (chain_len=39)."""

from __future__ import annotations

import argparse
import socket
import struct
import time


TARGET_PC = 0x001D589D
BIN6_ADDR = 0x0064D2B4
CHAIN_CAP = 64


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


def walk_bin6(sock: socket.socket) -> tuple[int, int, str]:
    head = read_u32(sock, BIN6_ADDR)
    cur = head
    chain = []
    seen = set()
    status = "ok"
    while cur:
        if cur in seen:
            status = "cycle"
            break
        seen.add(cur)
        chain.append(cur)
        if len(chain) >= CHAIN_CAP:
            status = "cap-reached"
            break
        nxt = read_u32(sock, cur + 8)
        if nxt is None:
            status = "unreadable-link"
            break
        cur = nxt
    return head or 0, len(chain), status


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    # Keep the timeout short for the initial halt-and-drain: if the
    # target is already stopped (e.g. fresh -S launch), sending \x03
    # gets no reply at all, and inheriting --timeout here would block
    # this whole script for that long before even arming the
    # breakpoint. Only widen the timeout for the real wait below.
    halt_and_drain(sock)
    r1 = packet(sock, f"Z0,{TARGET_PC:x},1")
    print(f"bp result: {r1}", flush=True)
    t0 = time.time()
    send_continue(sock)
    sock.settimeout(args.timeout)
    try:
        wait_stop(sock)
    except socket.timeout:
        print(f"NO HIT within {args.timeout}s", flush=True)
        return 0
    t = time.time() - t0
    r = get_regs(sock)
    head, chain_len, status = walk_bin6(sock)
    print(f"HIT at t={t:.2f}s pc={r['eip']:08X} esp={r['esp']:08X} "
          f"ecx={r['ecx']:08X} | bin6 head=0x{head:08X} chain_len={chain_len} status={status}",
          flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
