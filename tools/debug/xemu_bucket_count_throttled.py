"""Throttled variant of xemu_bucket_count.py: instead of continuing
immediately after every non-matching hit (which appears to freeze real
hardware in a spurious BIOS-era spin loop at this low address range,
the same symptom seen for 0x00045599), remove the breakpoint, let the
VM run completely freely for a short real interval, then re-arm. This
gives any genuine hardware wait condition a chance to actually resolve
instead of being paused into it repeatedly."""

from __future__ import annotations

import argparse
import socket
import struct
import time


TARGET_PC = 0x00045FB0
WANT_ECX = 0x0109B498


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
    parser.add_argument("--duration", type=float, default=180.0)
    parser.add_argument("--free-run", type=float, default=0.5, help="seconds to run free between arm attempts")
    parser.add_argument("--arm-timeout", type=float, default=2.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    t0 = time.time()
    sample = 0
    try:
        while time.time() - t0 < args.duration:
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
                if r["ecx"] == WANT_ECX:
                    bucket_array = read_u32(sock, r["ecx"])
                    bucket_count = read_u32(sock, r["ecx"] + 4)
                    print(f"MATCH sample#{sample} t={t:.2f}s container=0x{r['ecx']:08X} "
                          f"bucket_array=0x{bucket_array if bucket_array is not None else -1:08X} "
                          f"bucket_count={bucket_count}", flush=True)
                    halt_and_drain(sock)
                    packet(sock, f"z0,{TARGET_PC:x},1")
                    sock.sendall(b"$c#63")
                    sock.recv(4)
                    return 0
                else:
                    print(f"sample#{sample} t={t:.2f}s ecx=0x{r['ecx']:08X} (not target)", flush=True)
            else:
                print(f"t={time.time()-t0:.2f}s no hit within {args.arm_timeout}s", flush=True)
                halt_and_drain(sock)
            halt_and_drain(sock)
            packet(sock, f"z0,{TARGET_PC:x},1")
            sock.sendall(b"$c#63")
            sock.recv(4)
            time.sleep(args.free_run)
        print("duration exhausted without a match", flush=True)
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
