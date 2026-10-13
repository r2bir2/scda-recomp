"""Read, on real hardware, the file_obj value (this->+0x3c ring-buffer
sub-object, then that->+0x3c) for the confirmed lipsyncxbox.umd-class
object (this=0x00868CC0, vtable 0x4F6F08, per Update 183), so a
follow-up refill trace can filter sub_000516B7 hits down to only this
object's own reads instead of mixing in unrelated streams (Update 184).

Run xemu with ``-s -S`` first, then invoke this script.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time

READER_4438C = 0x0004438C
EXPECT_THIS = 0x00868CC0


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    started = time.monotonic()
    try:
        r = packet(sock, f"Z0,{READER_4438C:x},1")
        print(f"arm bp 0x{READER_4438C:08X}: {r}", flush=True)
        sock.settimeout(args.timeout)
        deadline = time.monotonic() + args.timeout
        found = False
        while time.monotonic() < deadline:
            send_continue(sock)
            wait_stop(sock)
            t = time.monotonic() - started
            r = regs(sock)
            this = r["ecx"]
            if this == EXPECT_THIS:
                vtable = read_u32(sock, this) or 0
                subobj = read_u32(sock, this + 0x3C) or 0
                subobj_vtable = read_u32(sock, subobj) or 0
                file_obj = read_u32(sock, subobj + 0x3C) or 0
                bytes_consumed = read_u32(sock, this + 0x44) or 0
                avail = read_u32(sock, subobj + 0x50) or 0
                print(f"t={t:.3f}s this=0x{this:08X} vtable=0x{vtable:08X} "
                      f"subobj=0x{subobj:08X} subobj_vtable=0x{subobj_vtable:08X} "
                      f"file_obj(subobj+0x3c)=0x{file_obj:08X} "
                      f"this+0x44(consumed)=0x{bytes_consumed:08X} "
                      f"subobj+0x50(avail)=0x{avail:08X}", flush=True)
                found = True
                break
            packet(sock, f"z0,{READER_4438C:x},1")
            packet(sock, "s")
            packet(sock, f"Z0,{READER_4438C:x},1")
        if not found:
            print("Timed out without seeing the expected this pointer.", flush=True)
    finally:
        try:
            packet(sock, f"z0,{READER_4438C:x},1")
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
