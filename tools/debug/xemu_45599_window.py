"""Capture every sub_00045599 hit between marker A (sub_000366A2 entry)
and marker B (sub_001D589D entry) on real hardware, grouped by return
address, to compare against scda_recomp's window_45599_recomp.log
summary (15x ret=0x4665A count=0, 15x ret=0x45F42 counts summing to 132).
Assumes xemu is already paused past marker A (this session's earlier
capture already consumed marker A and left marker B armed alongside the
four chain breakpoints) -- so this script just adds 0x00045599 to the
armed set and continues from wherever xemu currently is, stopping at
marker B."""
from __future__ import annotations
import socket
import struct
import time

MARKER_B = 0x001D589D
TARGET = 0x00045599


def packet(sock, body):
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


def wait_stop(sock):
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


BIN6_ADDR = 0x0064D2B4


def read_u32(sock, address):
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def bin6_chain_len(sock):
    cur = read_u32(sock, BIN6_ADDR)
    n = 0
    while cur and n < 64:
        n += 1
        cur = read_u32(sock, cur + 8)
    return n


def get_regs(sock):
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def unstick(sock, addr):
    packet(sock, f"z0,{addr:x},1")
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(10)
    try:
        wait_stop(sock)
    except socket.timeout:
        pass
    packet(sock, f"Z0,{addr:x},1")


MARKER_A = 0x000366A2


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    r = packet(sock, f"Z0,{MARKER_A:x},1")
    print(f"arm marker A 0x{MARKER_A:08X}: {r}", flush=True)
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(120)
    wait_stop(sock)
    print("MARKER A HIT", flush=True)
    packet(sock, f"z0,{MARKER_A:x},1")

    r = packet(sock, f"Z0,{TARGET:x},1")
    print(f"arm 0x{TARGET:08X}: {r}", flush=True)
    r = packet(sock, f"Z0,{MARKER_B:x},1")
    print(f"arm marker B 0x{MARKER_B:08X}: {r}", flush=True)

    hits = []
    t0 = time.time()
    while True:
        sock.sendall(b"$c#63")
        sock.recv(1)
        sock.settimeout(60)
        try:
            wait_stop(sock)
        except socket.timeout:
            print(f"no hit within 60s (had {len(hits)}) -- stopping", flush=True)
            break
        regs = get_regs(sock)
        eip = regs["eip"]
        if eip == MARKER_B:
            print(f"MARKER B HIT t={time.time()-t0:.1f}s bin6_len={bin6_chain_len(sock)} "
                  f"after {len(hits)} sub_00045599 hits -- STOPPING", flush=True)
            break
        if eip != TARGET:
            print(f"unexpected stop 0x{eip:08X}, continuing", flush=True)
            continue
        ecx = regs["ecx"]
        esp = regs["esp"]
        ret = read_u32(sock, esp) or 0
        data = read_u32(sock, ecx) or 0
        count = read_u32(sock, ecx + 4) or 0
        capacity = read_u32(sock, ecx + 8) or 0
        bl = bin6_chain_len(sock)
        hits.append((ret, count, capacity))
        print(f"#{len(hits)} ret=0x{ret:08X} container=0x{ecx:08X} data=0x{data:08X} "
              f"count={count} capacity={capacity} bin6_len_before={bl}", flush=True)
        unstick(sock, eip)

    from collections import defaultdict
    byret = defaultdict(list)
    for ret, count, cap in hits:
        byret[ret].append(count)
    print("=== SUMMARY ===")
    for ret, counts in byret.items():
        print(f"ret=0x{ret:08X} n_calls={len(counts)} sum_count={sum(counts)} counts={counts}")


if __name__ == "__main__":
    main()
