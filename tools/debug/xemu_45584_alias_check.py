"""Settle continued 41's open question: is sub_00045584's 'this' (ecx,
observed numerically as ebp-220 relative to sub_00048B15's frame) really
the same memory as the 0x5B5800 transient object's +0x20 field, and if
so, does sub_00045584's call chain (-> sub_00044AEE -> tail-jmp
sub_0024A50B) actually initialize it?

Three checkpoints in one boot:
  1. sub_00045584 entry (0x00045584): dump [ecx+0]/[ecx+4]/[ecx+8] BEFORE
     the call chain runs (ecx is 'this', passed unchanged from the
     caller's `ecx = ebp + -220`).
  2. The return site in sub_00048B15 (0x00048CE1): dump the SAME three
     words at the literal address captured at checkpoint 1, to see
     whether the call chain wrote anything there.
  3. Just before the icall that actually decodes the count
     (0x00048CF8): dump MEM32(0x5B5800)+0x20/+0x24/+0x28 directly, to
     confirm (or refute) that this is numerically the same address as
     checkpoint 1's `ecx`.

Run xemu with ``-s -S`` first, then invoke this script.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


ENTRY_45584 = 0x00045584
RETURN_48CE1 = 0x00048CE1
ICALL_48CF8 = 0x00048CF8
POINTS = {ENTRY_45584: "sub_00045584-entry", RETURN_48CE1: "back-in-sub_00048B15",
          ICALL_48CF8: "loc_00048CF8-icall-site"}


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


def dump3(sock: socket.socket, base: int) -> str:
    a = read_u32(sock, base) or 0
    b = read_u32(sock, base + 4) or 0
    c = read_u32(sock, base + 8) or 0
    return f"[+0]=0x{a:08X} [+4]=0x{b:08X} [+8]=0x{c:08X}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    active = list(POINTS)
    captured_base = None
    try:
        for address in active:
            result = packet(sock, f"Z0,{address:x},1")
            if result != "OK":
                raise RuntimeError(f"could not set {address:08X}: {result}")
        sock.settimeout(args.timeout)
        send_continue(sock)
        deadline = time.monotonic() + args.timeout
        hits = 0
        while time.monotonic() < deadline and hits < 10:
            stop = wait_stop(sock)
            r = regs(sock)
            pc = r["eip"]
            name = POINTS.get(pc, "unexpected")
            if pc == ENTRY_45584:
                captured_base = r["ecx"]
                print(f"[{name}] ecx(this)=0x{captured_base:08X} {dump3(sock, captured_base)}",
                      flush=True)
            elif pc == RETURN_48CE1:
                if captured_base is not None:
                    print(f"[{name}] same-addr {dump3(sock, captured_base)}", flush=True)
                else:
                    print(f"[{name}] (entry checkpoint not seen yet)", flush=True)
            elif pc == ICALL_48CF8:
                obj = read_u32(sock, 0x005B5800) or 0
                print(f"[{name}] MEM32(0x5B5800)=0x{obj:08X} "
                      f"obj+0x20.. {dump3(sock, obj + 0x20) if obj else '<null>'}",
                      flush=True)
                if captured_base is not None:
                    print(f"    (captured_base == obj+0x20? "
                          f"{'YES' if captured_base == obj + 0x20 else 'NO'})", flush=True)
            else:
                print(f"[unexpected-stop] pc=0x{pc:08X} stop={stop}", flush=True)
            hits += 1
            if pc in POINTS:
                packet(sock, f"z0,{pc:x},1")
                packet(sock, "s")
                packet(sock, f"Z0,{pc:x},1")
            send_continue(sock)
        print(f"done: {hits} hits", flush=True)
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
