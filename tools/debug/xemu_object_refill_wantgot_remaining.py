"""Update 215 follow-up: extends xemu_object_refill_wantgot.py (Update 185)
with exactly one more measurement -- at the SAME sub_000516B7 entry
breakpoint, for the SAME file_obj=0x18 filter, additionally read `ebx`
(the ring-buffer subobject "this", still valid at entry before this
function's own prologue repurposes it) and its own +0x4058 (buffer_size)
and +0x4060 (read/write cursor, BEFORE this call's read) fields, to
compute remaining_to_buffer_end = buffer_size - cursor on REAL hardware,
for the exact hit that produces the want=0x20000 got=0x15F71 short read
Update 185 already found.

recomp's own captured value for this same quantity (Updates 213/214) is 7.
This script exists to answer: is xemu's value also 7 (meaning the two
environments reach this call in the identical bookkeeping state, and the
divergence -- real hardware not crashing afterward -- lies elsewhere), or
is it different (most likely 0, which would make the known-buggy cursor
formula 0x4000-remaining degenerate to exactly 0x4000, landing on the
genuine fresh-data boundary by chance rather than by any real fix)?

Run xemu with -s -S first, then invoke this script.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time

REFILL_516B7 = 0x000516B7
RETURN_A = 0x00040983  # second sub_00040821 call site (loc_00040909 branch)
RETURN_B = 0x000408E1  # first sub_00040821 call site (loc_0004087B branch) -- the one that matters here
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
            bytes_read_out_ptr = read_u32(sock, r["esp"] + 0x10) or 0

            if file_obj != TARGET_FILE_OBJ:
                step_over(sock, pc)
                continue

            matched_refills += 1
            # NEW for Update 215: ebx at this exact entry point is still the
            # ring-buffer subobject "this" (confirmed on recomp: its own
            # prologue only repurposes ebx as scratch a few instructions
            # in). Read its buffer_size/cursor fields BEFORE stepping past
            # this breakpoint, i.e. exactly the state the buggy formula
            # will use for THIS call.
            this_obj = r["ebx"]
            buffer_size = read_u32(sock, this_obj + 0x4058) or 0
            cursor_before = read_u32(sock, this_obj + 0x4060) or 0
            remaining_to_buffer_end = (buffer_size - cursor_before) & 0xFFFFFFFF

            step_over(sock, pc)
            packet(sock, f"Z0,{RETURN_A:x},1")
            packet(sock, f"Z0,{RETURN_B:x},1")
            send_continue(sock)
            wait_stop(sock)
            r2 = regs(sock)
            ret_pc = r2["eip"]
            got = read_u32(sock, bytes_read_out_ptr) or 0
            status = "SHORT-READ" if got < size else "full"
            print(f"[MATCH] #{matched_refills} t={t:.3f}s this=0x{this_obj:08X} "
                  f"buffer_size=0x{buffer_size:08X} cursor_before=0x{cursor_before:08X} "
                  f"remaining_to_buffer_end=0x{remaining_to_buffer_end:08X} "
                  f"dest=0x{dest:08X} want=0x{size:08X} got=0x{got:08X} "
                  f"ret=0x{ret_pc:08X} {status}", flush=True)
            if status == "SHORT-READ":
                print(f"*** SHORT READ FOUND -- remaining_to_buffer_end=0x{remaining_to_buffer_end:08X} "
                      f"(recomp's own captured value for the equivalent call was 0x7 -- "
                      f"Update 213/214) ***", flush=True)
            packet(sock, f"z0,{RETURN_A:x},1")
            packet(sock, f"z0,{RETURN_B:x},1")
            if ret_pc in (RETURN_A, RETURN_B):
                packet(sock, "s")
        print(f"SUMMARY: total_516B7_hits={total_refills} matched(file_obj=0x18)="
              f"{matched_refills} elapsed={time.monotonic()-started:.1f}s", flush=True)
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
