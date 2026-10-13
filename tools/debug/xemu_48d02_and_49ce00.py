"""Real-hardware ground truth for two open questions from continued 36-39:

1. sub_00048B15's icall at 0x00048D02 (loc_00048CF8: call dword ptr [eax],
   vtable slot 0 of the "0x5B5800 transient object") -- what does real
   hardware resolve this to? scda_recomp resolves it to sub_00045D0B,
   which cascades into the sub_004A12B0 spin. If real hardware resolves
   to something else (a different vtable/object state), that's the bug.

2. sub_0049CE00's entry (0x0049CE00: mov ebp, [0x4AB668]) -- confirmed in
   continued 37 that real hardware never reaches this with a null device
   pointer. This script re-confirms that AND, if ebp is ever observed
   near zero, dumps [ebp+0x1A04]/[ebp+0x1A08] to settle continued 36's
   "explanation 2" (real-vs-zeroed low memory) definitively either way.

Run xemu with ``-s -S`` first (frozen at reset), then invoke this script.
Read-only: only places temporary software breakpoints and reads guest
registers/memory. All breakpoints are removed before detaching.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


POINTS = {
    0x00048D02: "sub_00048B15-icall-0x48CF8",
    0x0049CE00: "sub_0049CE00-entry",
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
    body = b"c"
    sock.sendall(b"$" + body + b"#63")
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


def regs(sock: socket.socket) -> dict[str, int]:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--max-hits", type=int, default=40)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    active = list(POINTS)
    try:
        for address in active:
            result = packet(sock, f"Z0,{address:x},1")
            if result != "OK":
                raise RuntimeError(f"could not set {address:08X}: {result}")
        sock.settimeout(args.timeout)
        send_continue(sock)
        deadline = time.monotonic() + args.timeout
        hits = 0
        seen_null_device = False
        while time.monotonic() < deadline and hits < args.max_hits:
            stop = wait_stop(sock)
            r = regs(sock)
            pc = r["eip"]
            name = POINTS.get(pc, "unexpected-stop")
            if pc == 0x00048D02:
                # eax already holds the resolved vtable base (per the C:
                # `eax = MEM32(ecx)` before the call); MEM32(eax) is the
                # actual call target about to be dispatched.
                target = read_u32(sock, r["eax"])
                print(f"[XEMU-48D02] stop={stop} vtbl={r['eax']:08X} "
                      f"target=0x{target if target else 0:08X} "
                      f"ecx={r['ecx']:08X} esp={r['esp']:08X}", flush=True)
            elif pc == 0x0049CE00:
                device = read_u32(sock, 0x004AB668) or 0
                line = f"[XEMU-49CE00] stop={stop} device_ptr=0x{device:08X}"
                if device == 0:
                    seen_null_device = True
                    low_1a04 = read_u32(sock, 0x00001A04) or 0
                    low_1a08 = read_u32(sock, 0x00001A08) or 0
                    line += (f" **NULL DEVICE** [0x1A04]=0x{low_1a04:08X} "
                             f"[0x1A08]=0x{low_1a08:08X}")
                print(line, flush=True)
            else:
                print(f"[XEMU-UNEXPECTED] stop={stop} pc={pc:08X}", flush=True)
            hits += 1
            if pc in POINTS:
                packet(sock, f"z0,{pc:x},1")
                packet(sock, "s")
                packet(sock, f"Z0,{pc:x},1")
            send_continue(sock)
        print(f"done: {hits} hits, ever saw null device at 0x0049CE00 = {seen_null_device}",
              flush=True)
    finally:
        for address in active:
            try:
                packet(sock, f"z0,{address:x},1")
            except OSError:
                pass
        try:
            packet(sock, "D")
        except OSError:
            pass
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
