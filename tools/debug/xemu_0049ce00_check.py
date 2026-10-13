"""Ground-truth check for continued 55's open question: on real hardware,
who calls sub_0049CE00 (the direct ancestor of sub_004A1740 -> sub_004A1600
-> sub_004A12B0 in scda_recomp's "second" call path), and is the D3D device
already created (MEM32(0x4AB668) != 0) whenever it is?
"""
from __future__ import annotations

import socket
import struct
import time

ENTRY = 0x0049CE00


def packet(sock, body):
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


def wait_stop(sock):
    first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return


def continue_to_stop(sock, timeout):
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)


def step_once(sock, timeout):
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)


def regs(sock):
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def mem_u32(sock, address):
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) < 8:
        return 0
    return struct.unpack("<I", bytes.fromhex(response))[0]


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{ENTRY:x},1")
    print(f"armed sub_0049CE00 entry 0x{ENTRY:08X}", flush=True)
    start = time.time()
    hits = 0
    seen_rets = {}
    while time.time() - start < 900 and hits < 300:
        try:
            continue_to_stop(sock, min(60, 901 - (time.time() - start)))
        except socket.timeout:
            print(f"timeout at t={time.time()-start:.1f}s, hits={hits}", flush=True)
            break
        r = regs(sock)
        hits += 1
        ret_addr = mem_u32(sock, r["esp"])
        device_ptr = mem_u32(sock, 0x4AB668)
        first_seen = ret_addr not in seen_rets
        seen_rets[ret_addr] = seen_rets.get(ret_addr, 0) + 1
        if first_seen or hits <= 5 or device_ptr == 0:
            print(
                f"HIT #{hits} t={time.time()-start:.1f}s ret=0x{ret_addr:08X} "
                f"(seen {seen_rets[ret_addr]}x) ecx(this)=0x{r['ecx']:08X} "
                f"MEM32(0x4AB668)=0x{device_ptr:08X}",
                flush=True,
            )
        packet(sock, f"z0,{ENTRY:x},1")
        step_once(sock, 10)
        packet(sock, f"Z0,{ENTRY:x},1")
    packet(sock, f"z0,{ENTRY:x},1")
    print(f"done hits={hits} distinct_returns={len(seen_rets)}", flush=True)
    for ra, n in sorted(seen_rets.items(), key=lambda kv: -kv[1]):
        print(f"  ret=0x{ra:08X} count={n}", flush=True)


if __name__ == "__main__":
    main()
