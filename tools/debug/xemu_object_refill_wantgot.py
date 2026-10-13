"""Trace want/got for exactly ONE stream object's disc refills on real
hardware: the lipsyncxbox.umd-class object identified in Update 183
(this=0x00868CC0, vtable 0x4F6F08) whose ring-buffer file handle is
file_obj=0x18 (read directly in xemu_object_fileobj.py).

Breaks at sub_000516B7 entry (the disc-I/O issuing function), filters to
calls where arg1 (file_obj, at [esp+4]) equals TARGET_FILE_OBJ, and for
each matching call arms a temporary breakpoint at whichever of the two
known return sites in sub_00040821 (0x00040992 or 0x000408E1, per
Update 179's traced call sites) fires next, then reads the actual bytes
transferred from the out-parameter pointer (arg4, [esp+0x10] at entry)
to compute got vs want.

Run xemu with ``-s -S`` first, then invoke this script.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time

REFILL_516B7 = 0x000516B7
RETURN_A = 0x00040983  # second sub_00040821 call site (loc_00040909 branch)
RETURN_B = 0x000408E1  # first sub_00040821 call site (loc_0004087B branch)
TARGET_FILE_OBJ = 0x18


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


def regs(sock: socket.socket) -> dict[str, int]:
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


def step_over(sock: socket.socket, pc: int) -> None:
    packet(sock, f"z0,{pc:x},1")
    packet(sock, "s")
    packet(sock, f"Z0,{pc:x},1")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=600.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    started = time.monotonic()
    total_refills = 0
    matched_refills = 0
    short_reads = 0
    try:
        r = packet(sock, f"Z0,{REFILL_516B7:x},1")
        print(f"arm bp 0x{REFILL_516B7:08X}: {r}", flush=True)

        sock.settimeout(args.timeout)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            send_continue(sock)
            wait_stop(sock)
            t = time.monotonic() - started
            r = regs(sock)
            pc = r["eip"]
            if pc != REFILL_516B7:
                print(f"[UNEXPECTED] t={t:.3f}s eip=0x{pc:08X}", flush=True)
                # Defensive: clear any stray leftover breakpoint at this PC
                # (should not normally happen with the fix above, but avoid
                # an infinite stall if it does) and step past it.
                for addr in (RETURN_A, RETURN_B):
                    try:
                        packet(sock, f"z0,{addr:x},1")
                    except OSError:
                        pass
                packet(sock, "s")
                continue
            total_refills += 1
            file_obj = read_u32(sock, r["esp"] + 4) or 0
            dest = read_u32(sock, r["esp"] + 8) or 0
            size = read_u32(sock, r["esp"] + 0xC) or 0
            # arg4 ([esp+0x10]) is a POINTER to the caller's local that will
            # receive the actual bytes-transferred count once the call
            # returns -- read the pointer value now, dereference it later.
            bytes_read_out_ptr = read_u32(sock, r["esp"] + 0x10) or 0

            if file_obj != TARGET_FILE_OBJ:
                step_over(sock, pc)
                continue

            matched_refills += 1
            # Step over this call's own entry breakpoint, arm both known
            # return sites, run until one fires.
            step_over(sock, pc)
            packet(sock, f"Z0,{RETURN_A:x},1")
            packet(sock, f"Z0,{RETURN_B:x},1")
            send_continue(sock)
            wait_stop(sock)
            r2 = regs(sock)
            ret_pc = r2["eip"]
            got = read_u32(sock, bytes_read_out_ptr) or 0
            status = "SHORT-READ" if got < size else "full"
            if got < size:
                short_reads += 1
            print(f"[MATCH] #{matched_refills} t={t:.3f}s dest=0x{dest:08X} "
                  f"want=0x{size:08X} got=0x{got:08X} ret=0x{ret_pc:08X} {status}",
                  flush=True)
            # Fully remove both return breakpoints (they are shared by every
            # OTHER stream's sub_00040821 calls too) and single-step past
            # the current one WITHOUT re-arming -- re-arming here was the
            # bug that flooded [UNEXPECTED] hits on unrelated streams.
            packet(sock, f"z0,{RETURN_A:x},1")
            packet(sock, f"z0,{RETURN_B:x},1")
            if ret_pc in (RETURN_A, RETURN_B):
                packet(sock, "s")
        print(f"SUMMARY: total_516B7_hits={total_refills} matched(file_obj=0x18)="
              f"{matched_refills} short_reads={short_reads} elapsed="
              f"{time.monotonic()-started:.1f}s", flush=True)
    finally:
        for addr in (REFILL_516B7, RETURN_A, RETURN_B):
            try:
                packet(sock, f"z0,{addr:x},1")
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
