"""Second attempt: game title screen already visible (confirmed via
screenshot), so the XBE's .text is definitely mapped now. Re-arm
0x001D5208 and wait for a hit with sane (non-kernel-range) register
values."""

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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--max-hits", type=int, default=20)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(args.timeout)
    halt_and_drain(sock)
    r1 = packet(sock, f"Z0,{TARGET_PC:x},1")
    print(f"bp result: {r1}", flush=True)
    t0 = time.time()
    send_continue(sock)
    hits = 0
    while hits < args.max_hits:
        try:
            wait_stop(sock)
        except socket.timeout:
            print(f"NO (MORE) HITS within {args.timeout}s (had {hits})", flush=True)
            break
        hits += 1
        r = get_regs(sock)
        t = time.time() - t0
        print(f"HIT#{hits} at t={t:.2f}s pc={r['eip']:08X} ecx={r['ecx']:08X} "
              f"esi={r['esi']:08X} edi={r['edi']:08X} esp={r['esp']:08X}", flush=True)
        send_continue(sock)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
