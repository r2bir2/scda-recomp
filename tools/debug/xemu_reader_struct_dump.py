"""Dump the reader/renderer object's own fields (0x00..0x80) at the
point sub_0005FBDA is entered for the call from sub_00045E42, on real
hardware. Continued 43 found real hardware reads the CORRECT UMOD
"FILES" compact-index bytes (0x5B,0x0C at DynamicXbox.umd's info_off,
confirmed by direct file parsing) while scda_recomp reads 0x5C,0x6E
from an unidentified location -- this dumps the object layout to find
whatever field holds its read position/file identity, for comparison
against the same dump on scda_recomp.

Run xemu with ``-s -S`` first, then invoke this script.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


ENTRY_FBDA = 0x0005FBDA
WANTED_RETURN = 0x00045E7B


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


def read_mem(sock: socket.socket, address: int, length: int) -> bytes | None:
    response = packet(sock, f"m{address:x},{length:x}")
    if response.startswith("E") or len(response) != length * 2:
        return None
    return bytes.fromhex(response)


def read_u32(sock: socket.socket, address: int) -> int | None:
    data = read_mem(sock, address, 4)
    return struct.unpack("<I", data)[0] if data else None


def regs(sock: socket.socket) -> dict[str, int]:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def bp_set(sock: socket.socket, addr: int) -> None:
    r = packet(sock, f"Z0,{addr:x},1")
    if r != "OK":
        raise RuntimeError(f"arm failed at 0x{addr:08X}: {r}")


def bp_clear(sock: socket.socket, addr: int) -> None:
    try:
        packet(sock, f"z0,{addr:x},1")
    except OSError:
        pass


def dump_object(sock: socket.socket, base: int, size: int = 0x90) -> None:
    data = read_mem(sock, base, size)
    if not data:
        print(f"  <unreadable at 0x{base:08X}>")
        return
    for off in range(0, size, 16):
        chunk = data[off:off + 16]
        words = struct.unpack("<" + "I" * (len(chunk) // 4), chunk)
        words_str = " ".join(f"{w:08X}" for w in words)
        print(f"  +0x{off:02X}: {words_str}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    try:
        bp_set(sock, ENTRY_FBDA)
        sock.settimeout(args.timeout)
        send_continue(sock)
        deadline = time.monotonic() + args.timeout
        hits = 0
        matched = 0
        while time.monotonic() < deadline and hits < 500 and matched < 1:
            stop = wait_stop(sock)
            r = regs(sock)
            pc = r["eip"]
            if pc == ENTRY_FBDA:
                ret_addr = read_u32(sock, r["esp"]) or 0
                if ret_addr == WANTED_RETURN:
                    reader = read_u32(sock, r["esp"] + 4) or 0
                    print(f"[MATCHED] reader object = 0x{reader:08X}")
                    dump_object(sock, reader)
                    matched += 1
                bp_clear(sock, ENTRY_FBDA)
                packet(sock, "s")
                bp_set(sock, ENTRY_FBDA)
                send_continue(sock)
                hits += 1
                continue
            else:
                print(f"[unexpected] pc=0x{pc:08X} stop={stop}", flush=True)
            hits += 1
            send_continue(sock)
        print(f"done: {hits} hits, matched={matched}", flush=True)
    finally:
        bp_clear(sock, ENTRY_FBDA)
        try:
            packet(sock, "D")
        except OSError:
            pass
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
