"""Fixed variant: 0x00371D06 immediately follows 0x00371D02 in the
disassembly with no intervening call (eax=0/non-negative always falls
straight through), so after catching a STATUS hit, continue WITHOUT
disarming to also catch the immediate RETVAL hit, before throttling."""

from __future__ import annotations

import argparse
import socket
import struct
import time


TARGET_PC = 0x00371D02
TARGET_PC2 = 0x00371D06


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


def halt_and_drain(sock: socket.socket) -> None:
    sock.sendall(b"\x03")
    time.sleep(0.3)
    try:
        wait_stop(sock)
    except socket.timeout:
        pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=300.0)
    parser.add_argument("--free-run", type=float, default=0.2)
    parser.add_argument("--arm-timeout", type=float, default=1.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    t0 = time.time()
    sample = 0
    pairs = 0
    try:
        while time.time() - t0 < args.duration:
            halt_and_drain(sock)
            packet(sock, f"Z0,{TARGET_PC:x},1")
            packet(sock, f"Z0,{TARGET_PC2:x},1")
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
                pc = r["eip"]
                print(f"SAMPLE#{sample} t={t:.2f}s pc=0x{pc:08X} eax=0x{r['eax']:08X} "
                      f"ecx=0x{r['ecx']:08X} esp=0x{r['esp']:08X}", flush=True)
                if pc == TARGET_PC:
                    # continue immediately (keep both armed) to catch the
                    # very next instruction's hit without a throttle gap.
                    sock.sendall(b"$c#63")
                    sock.recv(4)
                    sock.settimeout(2.0)
                    try:
                        wait_stop(sock)
                        r2 = get_regs(sock)
                        t2 = time.time() - t0
                        sample += 1
                        print(f"SAMPLE#{sample} t={t2:.2f}s pc=0x{r2['eip']:08X} "
                              f"eax=0x{r2['eax']:08X} ecx=0x{r2['ecx']:08X} "
                              f"esp=0x{r2['esp']:08X} (chained)", flush=True)
                        if r2["eip"] == TARGET_PC2:
                            pairs += 1
                    except socket.timeout:
                        print("  (no immediate chained hit)", flush=True)
                    sock.settimeout(10)
            halt_and_drain(sock)
            packet(sock, f"z0,{TARGET_PC:x},1")
            packet(sock, f"z0,{TARGET_PC2:x},1")
            sock.sendall(b"$c#63")
            sock.recv(4)
            if pairs >= 3:
                print("collected 3 status+retval pairs -- stopping", flush=True)
                break
            time.sleep(args.free_run)
        else:
            print(f"duration exhausted ({sample} samples, {pairs} pairs)", flush=True)
    finally:
        try:
            halt_and_drain(sock)
            packet(sock, f"z0,{TARGET_PC:x},1")
            packet(sock, f"z0,{TARGET_PC2:x},1")
            sock.sendall(b"$c#63")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
