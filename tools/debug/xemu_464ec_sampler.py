"""Throttled sampler for sub_000464EC: arms the breakpoint, waits for ONE
hit, records it, immediately removes the breakpoint and resumes, then
sleeps before re-arming. This avoids halting xemu on every single hit of
a tight burst (which can effectively freeze the emulator), while still
showing whether/when the function is being called and with what args.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


ENTRY_PC = 0x000464EC


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=180.0, help="total wall-clock seconds to sample")
    parser.add_argument("--interval", type=float, default=1.0, help="seconds to sleep free-running between samples")
    parser.add_argument("--arm-timeout", type=float, default=5.0, help="seconds to wait for a hit once armed before giving up on this sample")
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    t0 = time.time()
    sample = 0
    try:
        while time.time() - t0 < args.duration:
            halt_and_drain(sock)
            r = packet(sock, f"Z0,{ENTRY_PC:x},1")
            sock.sendall(b"$c#63")
            sock.recv(4)  # ack
            sock.settimeout(args.arm_timeout)
            t_arm = time.time()
            hit = False
            try:
                wait_stop(sock)
                hit = True
            except socket.timeout:
                pass
            sock.settimeout(10)
            if hit:
                sample += 1
                regs = get_regs(sock)
                arg1 = read_u32(sock, regs["esp"] + 4)
                arg2 = read_u32(sock, regs["esp"] + 8)
                print(f"sample#{sample} t={time.time()-t0:7.2f}s wait={time.time()-t_arm:5.2f}s "
                      f"ecx={regs['ecx']:08X} arg1={arg1 if arg1 is not None else -1:08X} "
                      f"arg2={arg2 if arg2 is not None else -1:08X}", flush=True)
            else:
                print(f"t={time.time()-t0:7.2f}s no hit within {args.arm_timeout}s", flush=True)
                halt_and_drain(sock)
            # remove breakpoint and free-run for `interval` seconds before re-arming
            halt_and_drain(sock)
            packet(sock, f"z0,{ENTRY_PC:x},1")
            sock.sendall(b"$c#63")
            sock.recv(4)
            time.sleep(args.interval)
    finally:
        try:
            halt_and_drain(sock)
            packet(sock, f"z0,{ENTRY_PC:x},1")
            sock.sendall(b"$c#63")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
