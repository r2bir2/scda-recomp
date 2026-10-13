"""Full bin[6] write history from process boot up to sub_001D589D entry,
via xemu's GDB stub write watchpoint (Z2). Mirrors the CDB hardware-watch
technique already used on the scda_recomp side
(cdb_bin6_history.txt / bin6_history_*.txt), for a direct, event-by-event
comparison.

Run xemu with ``-s -S`` first, then invoke this script; it resumes
execution itself. For every write to bin[6]'s head slot (guest
0x0064D2B4) it records the new value and the guest return address (the
call site one frame up, read from [esp] at the watchpoint stop -- QEMU's
gdbstub reports the watchpoint hit with PC already past the faulting
instruction, so [esp] here is the *caller's* frame, giving us the
guest PC immediately after the store completed). Stops automatically
once execution reaches sub_001D589D (0x001D589D), matching the same
breakpoint used for the "before the dump" chain-length snapshot.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


BIN6_ADDR = 0x0064D2B4
STOP_PC = 0x001D589D


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
    parser.add_argument("--timeout", type=float, default=1200.0)
    parser.add_argument("--max-hits", type=int, default=2000)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(args.timeout)
    try:
        watch_result = packet(sock, f"Z2,{BIN6_ADDR:x},4")
        bp_result = packet(sock, f"Z0,{STOP_PC:x},1")
        print(f"watch set: {watch_result}; stop-bp set: {bp_result}")
        if watch_result != "OK":
            raise RuntimeError(f"could not set write watchpoint: {watch_result}")
        send_continue(sock)
        hits = 0
        while hits < args.max_hits:
            stop = wait_stop(sock)
            r = regs(sock)
            pc = r["eip"]
            if pc == STOP_PC:
                print(f"REACHED sub_001D589D after {hits} bin6 writes")
                break
            val = read_u32(sock, BIN6_ADDR)
            ret = read_u32(sock, r["esp"])
            hits += 1
            print(f"#{hits} val={val if val is not None else -1:08X} pc={pc:08X} ret={ret if ret is not None else -1:08X}")
            send_continue(sock)
    finally:
        try:
            sock.sendall(b"\x03")
            time.sleep(0.3)
            try:
                wait_stop(sock)
            except socket.timeout:
                pass
            packet(sock, f"z2,{BIN6_ADDR:x},4")
            packet(sock, f"z0,{STOP_PC:x},1")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
