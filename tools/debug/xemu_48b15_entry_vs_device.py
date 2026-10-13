"""Settle the ordering question directly: on real hardware, does the device
pointer at 0x4AB668 become valid BEFORE or AFTER sub_00048B15 is entered?

Arms both a breakpoint on sub_00048B15's own entry (0x00048B15) and a write
watchpoint on 0x4AB668 in the SAME boot, and reports whichever fires first
(and, if the entry fires first, keeps watching until the write happens too,
to see how many instructions/calls separate them within the same function).

Run xemu with ``-s -S`` first, then invoke this script.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time

ENTRY = 0x00048B15
DEVICE_ADDR = 0x004AB668


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
    parser.add_argument("--timeout", type=float, default=240.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    started = time.monotonic()
    try:
        r1 = packet(sock, f"Z0,{ENTRY:x},1")
        print(f"arm entry bp on 0x{ENTRY:08X}: {r1}", flush=True)
        r2 = packet(sock, f"Z2,{DEVICE_ADDR:x},4")
        print(f"arm write watch on 0x{DEVICE_ADDR:08X}: {r2}", flush=True)

        sock.settimeout(args.timeout)
        entry_hit_time = None
        device_hit_time = None
        deadline = time.monotonic() + args.timeout
        hits = 0
        while time.monotonic() < deadline and hits < 10:
            send_continue(sock)
            stop = wait_stop(sock)
            t = time.monotonic() - started
            r = regs(sock)
            pc = r["eip"]
            device_val = read_u32(sock, DEVICE_ADDR) or 0
            if pc == ENTRY:
                entry_hit_time = t
                print(f"[ENTRY] t={t:.3f}s sub_00048B15 entered, device_ptr=0x{device_val:08X}",
                      flush=True)
                packet(sock, f"z0,{ENTRY:x},1")  # one-shot, don't re-arm
            else:
                device_hit_time = t
                print(f"[DEVICE-WRITE] t={t:.3f}s eip=0x{pc:08X} device_ptr=0x{device_val:08X}",
                      flush=True)
                if device_val != 0:
                    break
            hits += 1
        if entry_hit_time is not None and device_hit_time is not None:
            order = "ENTRY first" if entry_hit_time < device_hit_time else "DEVICE-WRITE first"
            print(f"ORDERING: {order} (entry={entry_hit_time:.3f}s, device_write={device_hit_time:.3f}s)",
                  flush=True)
        elif entry_hit_time is not None:
            print("Only sub_00048B15 entry observed within timeout; device write did not fire.",
                  flush=True)
        elif device_hit_time is not None:
            print("Only the device write observed within timeout; sub_00048B15 entry did not fire "
                  "(it may have already run before the breakpoint was armed, or the write happened "
                  "on a wholly separate path).", flush=True)
    finally:
        for cmd in (f"z0,{ENTRY:x},1", f"z2,{DEVICE_ADDR:x},4"):
            try:
                packet(sock, cmd)
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
