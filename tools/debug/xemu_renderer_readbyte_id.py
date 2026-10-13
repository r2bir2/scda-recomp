"""Identify the renderer object obtained in sub_00045D0B (via
MEM32(0x5B57FC)+0x54) and its own vtable slot 1 -- the "read byte"
method sub_0005FBDA calls repeatedly to decode the LEB128 count that
continued 41/42 traced the wrong sub_004A12B0 loop trip count to.

Checkpoints:
  1. 0x00045D86 (right after the icall that returns the renderer
     object into eax): capture the renderer pointer, its vtable, and
     vtable slot 1 (the presumed "read byte" target).
  2. sub_0005FBDA's five per-byte icall sites (0x5FC38, 0x5FC68,
     0x5FC99, 0x5FCC6, 0x5FCE0): capture the resolved target (should
     match slot 1 above), the buffer argument address, and -- after
     stepping over the call -- the byte that ended up there.

Run xemu with ``-s -S`` first, then invoke this script.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


ENTRY_RENDERER = 0x00045D86
# call-site -> (label, matching post-call return address, buffer address expression)
BYTE_SITES = {
    0x0005FC38: ("byte#1", 0x0005FC3B),
    0x0005FC68: ("byte#2", 0x0005FC6B),
    0x0005FC99: ("byte#3", 0x0005FC9C),
    0x0005FCC6: ("byte#4", 0x0005FCC9),
    0x0005FCE0: ("byte#5", 0x0005FCE3),
}
RETURN_SITES = {ret: (call, label) for call, (label, ret) in BYTE_SITES.items()}
POINTS = {ENTRY_RENDERER: "sub_00045D0B: renderer obtained"}
POINTS.update({call: label for call, (label, _ret) in BYTE_SITES.items()})
POINTS.update({ret: f"{label}-return" for _call, (label, ret) in BYTE_SITES.items()})


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


def read_u8(sock: socket.socket, address: int) -> int | None:
    response = packet(sock, f"m{address:x},1")
    if response.startswith("E") or len(response) != 2:
        return None
    return int(response, 16)


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
    renderer_ptr = None
    renderer_vtbl = None
    readbyte_target = None
    pending_buf_addr: dict[int, int] = {}
    try:
        for address in active:
            result = packet(sock, f"Z0,{address:x},1")
            if result != "OK":
                raise RuntimeError(f"could not set {address:08X}: {result}")
        sock.settimeout(args.timeout)
        send_continue(sock)
        deadline = time.monotonic() + args.timeout
        hits = 0
        while time.monotonic() < deadline and hits < 40:
            stop = wait_stop(sock)
            r = regs(sock)
            pc = r["eip"]
            name = POINTS.get(pc, "unexpected")
            if pc == ENTRY_RENDERER:
                renderer_ptr = r["eax"]
                renderer_vtbl = read_u32(sock, renderer_ptr) or 0
                readbyte_target = read_u32(sock, renderer_vtbl + 4) or 0
                print(f"[{name}] renderer=0x{renderer_ptr:08X} vtbl=0x{renderer_vtbl:08X} "
                      f"slot1(readbyte)=0x{readbyte_target:08X}", flush=True)
            elif pc in BYTE_SITES:
                # At the exact `call dword ptr [eax+4]` instruction: ecx already
                # holds "this" (mov ecx,edi ran), eax already holds the vtable
                # (mov eax,[edi] ran). Both pushes (len=1, &local_byte) are
                # already on the stack (call hasn't pushed its own return yet).
                this_ptr = r["ecx"]
                vtbl = r["eax"]
                target = read_u32(sock, vtbl + 4) or 0
                buf_addr = read_u32(sock, r["esp"]) or 0  # top of stack = &local_byte just pushed
                pending_buf_addr[pc] = buf_addr
                print(f"[{name}] this=0x{this_ptr:08X} vtbl=0x{vtbl:08X} target=0x{target:08X} "
                      f"buf_addr=0x{buf_addr:08X}", flush=True)
            elif pc in RETURN_SITES:
                call_pc, label = RETURN_SITES[pc]
                buf_addr = pending_buf_addr.get(call_pc)
                byte_val = read_u8(sock, buf_addr) if buf_addr else None
                print(f"[{label}-return] eax(retval)=0x{r['eax']:08X} "
                      f"buf_byte=0x{byte_val if byte_val is not None else -1:02X}", flush=True)
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
