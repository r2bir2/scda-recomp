"""Ground-truth check for continued 55's open question: does real hardware
ever take scda_recomp's "second" sub_004A12B0 call path at all -- the one
reached via sub_0007188A -> sub_0010D5C1 -> sub_001E460F -> sub_0003EDCC ->
sub_00077B61 -> ... -> sub_0049CE00 -> sub_004A1740 -> sub_004A1600 ->
sub_004A12B0 -- rather than staying on the continued-45-fixed main path?

sub_0010D5C1 is the first function in this chain not shared with the
already-understood main path, so breaking at its entry is a cheap way to
check whether real hardware ever branches here at all, and if so, from
where (return address) and how often.
"""
from __future__ import annotations

import socket
import struct
import time

ENTRY = 0x0010D5C1


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
    print(f"armed sub_0010D5C1 entry 0x{ENTRY:08X}", flush=True)
    start = time.time()
    hits = 0
    while time.time() - start < 600 and hits < 50:
        try:
            continue_to_stop(sock, min(60, 601 - (time.time() - start)))
        except socket.timeout:
            print(f"timeout at t={time.time()-start:.1f}s, hits={hits} -- "
                  "real hardware never reaches sub_0010D5C1 within the window "
                  "(so far)", flush=True)
            break
        r = regs(sock)
        hits += 1
        ret_addr = mem_u32(sock, r["esp"])
        device_ptr = mem_u32(sock, 0x4AB668)
        print(
            f"HIT #{hits} t={time.time()-start:.1f}s ret=0x{ret_addr:08X} "
            f"ecx(this)=0x{r['ecx']:08X} eax=0x{r['eax']:08X} "
            f"MEM32(0x4AB668)=0x{device_ptr:08X}",
            flush=True,
        )
        packet(sock, f"z0,{ENTRY:x},1")
        step_once(sock, 10)
        packet(sock, f"Z0,{ENTRY:x},1")
    packet(sock, f"z0,{ENTRY:x},1")
    print(f"done hits={hits}", flush=True)


if __name__ == "__main__":
    main()
