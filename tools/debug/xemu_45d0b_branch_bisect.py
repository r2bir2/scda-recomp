"""Check the branch-decision inputs for sub_00045D0B/sub_00045E42's
internal conditionals against real hardware, to find where scda_recomp's
translation of this call chain first disagrees (continued 40's next step).

Breaks at each decision point and reads exactly the register/memory value
the original x86 `cmp`/`test` at that address consumes, so the branch
outcome can be inferred directly without needing to catch both target
labels separately.

Run xemu with ``-s -S`` first, then invoke this script.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


POINTS = {
    0x00045D36: "sub_00045D0B: cmp status144(0x5ED144), 0",
    0x00045D6F: "sub_00045D0B: test eax,eax (sub_0003ED6E retval)",
    0x00045D86: "sub_00045D0B: test eax,eax (icall via 0x5B57FC+0x54 retval)",
    0x00045E6B: "sub_00045E42: cmp [ebx+0x18], 0",
    0x00045E87: "sub_00045E42: cmp [ebp+8], 0 (loop count)",
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
        while time.monotonic() < deadline and hits < 25:
            stop = wait_stop(sock)
            r = regs(sock)
            pc = r["eip"]
            name = POINTS.get(pc, "unexpected")
            if pc == 0x00045D36:
                v = read_u32(sock, 0x005ED144) or 0
                print(f"[{name}] status144=0x{v:08X} -> branch={'0x45D8E (skip)' if v != 0 else '0x45D44 (fallthrough)'}",
                      flush=True)
            elif pc == 0x00045D6F:
                print(f"[{name}] eax=0x{r['eax']:08X} -> branch={'0x45D8E (skip)' if r['eax'] == 0 else '0x45D73 (fallthrough)'}",
                      flush=True)
            elif pc == 0x00045D86:
                print(f"[{name}] eax=0x{r['eax']:08X} -> branch={'0x45D91 (skip)' if r['eax'] != 0 else '0x45D8E (fallthrough)'}",
                      flush=True)
            elif pc == 0x00045E6B:
                v = read_u32(sock, r["ebx"] + 0x18) or 0
                print(f"[{name}] ebx=0x{r['ebx']:08X} [ebx+0x18]=0x{v:08X} -> branch={'0x45ECA (skip loop)' if v == 0 else '0x45E71 (loop)'}",
                      flush=True)
            elif pc == 0x00045E87:
                v = read_u32(sock, r["ebp"] + 8) or 0
                print(f"[{name}] [ebp+8]=0x{v:08X} (signed={struct.unpack('<i', struct.pack('<I', v))[0]}) "
                      f"-> branch={'0x45EFB (skip loop)' if struct.unpack('<i', struct.pack('<I', v))[0] <= 0 else '0x45E91 (loop)'}",
                      flush=True)
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
