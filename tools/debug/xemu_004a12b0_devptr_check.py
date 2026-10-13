"""Ground-truth check for the sub_004A12B0 spin's second occurrence.

scda_recomp hangs a second time at sub_004A12B0+0x1d0 (test [eax+0x100410],
0x10000; jne), reached this time via a different call chain than the one
fixed in continued 45 (through bridge_PsCreateSystemThreadEx / a
thread-creation callback rather than the main config-parsing path). The
loop's guard is:

    eax = MEM32(MEM32(0x4AB668) + 0x1C28)
    test [eax + 0x100410], 0x10000

On real hardware this either (a) never reaches sub_004A12B0 from this call
path at all (something upstream gates/skips it, matching the general shape
of continued 45's fix), or (b) reaches it with MEM32(0x4AB668) already
holding a real, non-zero D3D device pointer (meaning device creation has
already happened earlier on real hardware than it has in scda_recomp at
this point).

Breaks at sub_004A12B0 entry (0x004A12B0) and, on every hit, logs the
return address (to identify the call path) and MEM32(0x4AB668).
"""
from __future__ import annotations

import socket
import struct
import time

ENTRY = 0x004A12B0


def packet(sock: socket.socket, body: str) -> str:
    raw = body.encode("ascii")
    sock.sendall(b"$" + raw + b"#" + f"{sum(raw) & 0xff:02x}".encode("ascii"))
    first = sock.recv(1)
    if first == b"+":
        first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    result = bytearray()
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return result.decode("ascii")
        result.extend(char)


def wait_stop(sock: socket.socket) -> None:
    first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return


def continue_to_stop(sock: socket.socket, timeout: float) -> None:
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)


def step_once(sock: socket.socket, timeout: float) -> None:
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)


def regs(sock: socket.socket) -> dict[str, int]:
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def mem_u32(sock: socket.socket, address: int) -> int:
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) < 8:
        return 0
    return struct.unpack("<I", bytes.fromhex(response))[0]


def main() -> None:
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{ENTRY:x},1")
    print(f"armed sub_004A12B0 entry 0x{ENTRY:08X}", flush=True)
    start = time.time()
    hits = 0
    while time.time() - start < 900 and hits < 500:
        try:
            continue_to_stop(sock, min(60, 901 - (time.time() - start)))
        except socket.timeout:
            print("timeout waiting for a hit -- real hardware never reaches "
                  "sub_004A12B0 within the window", flush=True)
            break
        r = regs(sock)
        hits += 1
        ret_addr = mem_u32(sock, r["esp"])
        device_ptr = mem_u32(sock, 0x4AB668)
        struct_field = mem_u32(sock, device_ptr + 0x1C28) if device_ptr else 0
        print(
            f"HIT #{hits} t={time.time()-start:.1f}s ret=0x{ret_addr:08X} "
            f"ecx(this)=0x{r['ecx']:08X} MEM32(0x4AB668)=0x{device_ptr:08X} "
            f"struct+0x1C28=0x{struct_field:08X}",
            flush=True,
        )
        packet(sock, f"z0,{ENTRY:x},1")
        step_once(sock, 10)
        packet(sock, f"Z0,{ENTRY:x},1")
    packet(sock, f"z0,{ENTRY:x},1")
    print(f"done hits={hits}", flush=True)


if __name__ == "__main__":
    main()
