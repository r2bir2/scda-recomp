"""Continuing from the current xemu state (paused right at 0x001CC69A,
the vtable+0x50 destructor call for the sub_000366A2-allocated object),
arm sub_00047150 (registry teardown) and sub_00045FB0 (per-entry
hashtable teardown) to see whether/when they fire as a DIRECT
consequence of this specific, now-proven-reached destructor call."""

from __future__ import annotations

import argparse
import socket
import struct
import time


BREAKPOINTS = {
    0x00047150: "47150-REGISTRY-TEARDOWN",
    0x00045FB0: "45FB0-HASHTABLE",
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    for addr in BREAKPOINTS:
        r = packet(sock, f"Z0,{addr:x},1")
        print(f"arm 0x{addr:08X} ({BREAKPOINTS[addr]}): {r}", flush=True)
    # A breakpoint left armed by the previous script (0x001CC69A) is
    # still set and we are currently halted exactly there -- disarm it
    # first so our continue doesn't just re-trigger the same old stop.
    stale = 0x001CC69A
    r = packet(sock, f"z0,{stale:x},1")
    print(f"disarm stale 0x{stale:08X}: {r}", flush=True)
    t0 = time.time()
    sock.settimeout(args.timeout)
    hits = 0
    send_continue(sock)
    while True:
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
        bucket_array = read_u32(sock, r["ecx"])
        bucket_count = read_u32(sock, r["ecx"] + 4)
        print(f"HIT#{hits} t={t:.2f}s [{label}] ecx=0x{r['ecx']:08X} bucket_array=0x{bucket_array if bucket_array else 0:08X} "
              f"bucket_count={bucket_count} esp=0x{r['esp']:08X}", flush=True)
        if pc in BREAKPOINTS:
            packet(sock, f"z0,{pc:x},1")
            print("STOPPING HERE (xemu left paused for further inspection)", flush=True)
            return 0
        send_continue(sock)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
