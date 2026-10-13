"""Fixed variant: disarm the SPECIFIC breakpoint just hit before
continuing (rather than leaving all 4 armed simultaneously), since
continuing with multiple breakpoints active at this address class
appeared to re-trigger the same PC without progress."""

from __future__ import annotations

import argparse
import socket
import struct
import time


BREAKPOINTS = {
    0x000366A2: "366A2-ENTRY",
    0x001CC66E: "ALLOC-RESULT(esi=eax)",
    0x001CC6A3: "SKIP-PATH(esi==0)",
    0x001CC69A: "PROCEED(vtable+0x50-call)",
}


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


def get_regs(sock: socket.socket) -> dict:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--max-hits", type=int, default=10)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    armed = set()
    for addr in BREAKPOINTS:
        r = packet(sock, f"Z0,{addr:x},1")
        print(f"arm 0x{addr:08X} ({BREAKPOINTS[addr]}): {r}", flush=True)
        armed.add(addr)
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
        pc = r["eip"]
        label = BREAKPOINTS.get(pc, f"UNKNOWN(0x{pc:08X})")
        print(f"HIT#{hits} t={t:.2f}s [{label}] eax=0x{r['eax']:08X} esi=0x{r['esi']:08X} "
              f"ecx=0x{r['ecx']:08X} esp=0x{r['esp']:08X}", flush=True)
        if pc in armed:
            packet(sock, f"z0,{pc:x},1")
            armed.discard(pc)
        send_continue(sock)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
