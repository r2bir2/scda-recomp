"""Single-step variant: after hitting 0x00371D02, use GDB single-step
('s') instead of continue to force exactly one instruction of real
forward progress (continuing appeared to re-trigger the same PC
without any progress at this address, matching a pattern seen at other
low addresses this session)."""

from __future__ import annotations

import argparse
import socket
import struct
import time


TARGET_PC = 0x00371D02


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


def get_regs(sock: socket.socket) -> dict:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def read_u32(sock: socket.socket, address: int) -> int | None:
    if address == 0:
        return None
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def halt_and_drain(sock: socket.socket) -> None:
    sock.sendall(b"\x03")
    time.sleep(0.3)
    try:
        wait_stop(sock)
    except socket.timeout:
        pass


def single_step(sock: socket.socket) -> dict:
    sock.sendall(b"$s#73")
    ack = sock.recv(1)
    wait_stop(sock)
    return get_regs(sock)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=240.0)
    parser.add_argument("--free-run", type=float, default=0.2)
    parser.add_argument("--arm-timeout", type=float, default=1.0)
    parser.add_argument("--max-samples", type=int, default=3)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    t0 = time.time()
    sample = 0
    try:
        while time.time() - t0 < args.duration and sample < args.max_samples:
            halt_and_drain(sock)
            packet(sock, f"Z0,{TARGET_PC:x},1")
            sock.sendall(b"$c#63")
            sock.recv(4)
            sock.settimeout(args.arm_timeout)
            hit = False
            try:
                wait_stop(sock)
                hit = True
            except socket.timeout:
                pass
            sock.settimeout(10)
            if hit:
                sample += 1
                r = get_regs(sock)
                t = time.time() - t0
                print(f"SAMPLE#{sample} t={t:.2f}s pc=0x{r['eip']:08X} eax=0x{r['eax']:08X} "
                      f"ecx=0x{r['ecx']:08X} esp=0x{r['esp']:08X}", flush=True)
                # remove the breakpoint so single-stepping doesn't re-trigger it
                packet(sock, f"z0,{TARGET_PC:x},1")
                # single-step forward a few instructions to reach 0x00371D06
                # and read eax there (the actual sub_00371CE7 return value).
                for i in range(6):
                    rs = single_step(sock)
                    print(f"  step#{i} pc=0x{rs['eip']:08X} eax=0x{rs['eax']:08X}", flush=True)
                    if rs["eip"] == 0x00371D06:
                        print(f"  ==> RETVAL at 0x00371D06: eax=0x{rs['eax']:08X}", flush=True)
                        break
            halt_and_drain(sock)
            packet(sock, f"z0,{TARGET_PC:x},1")
            sock.sendall(b"$c#63")
            sock.recv(4)
            time.sleep(args.free_run)
        else:
            print(f"stopped ({sample} samples)", flush=True)
    finally:
        try:
            halt_and_drain(sock)
            packet(sock, f"z0,{TARGET_PC:x},1")
            sock.sendall(b"$c#63")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
