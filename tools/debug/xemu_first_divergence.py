"""Targeted capture of the exact call to sub_00036116 (generic vector
clear+deallocate) for container 0xD00AD444 -- identified as event n=631
in xemu_vecfree_ordinal2.py's capture, the FIRST bin[6]-changing
operation after the last point proven identical to scda_recomp (delta
+1/push). Reads sub_00036116's own caller (one level up from
sub_001E460F), plus the vector's size/capacity fields *before* they get
zeroed by sub_00036116's own prologue."""
from __future__ import annotations
import socket
import struct
import time

MARKER_A = 0x000366A2
ENTRY_36116 = 0x00036116
TARGET_CONTAINER = 0x0081BE98


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


def read_u32(sock, address):
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def read_bytes(sock, address, length):
    response = packet(sock, f"m{address:x},{length:x}")
    if response.startswith("E"):
        return b""
    return bytes.fromhex(response)


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


def cont_and_wait(sock, timeout=60):
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)
    return get_regs(sock)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    packet(sock, f"Z0,{MARKER_A:x},1")
    regs = cont_and_wait(sock, 120)
    assert regs["eip"] == MARKER_A
    print("MARKER A HIT", flush=True)
    packet(sock, f"z0,{MARKER_A:x},1")

    packet(sock, f"Z0,{ENTRY_36116:x},1")
    print(f"armed sub_00036116 entry (0x{ENTRY_36116:08X})", flush=True)

    hits = 0
    t0 = time.time()
    while True:
        try:
            regs = cont_and_wait(sock, 60)
        except socket.timeout:
            print(f"no more hits within 60s (had {hits}) -- stopping", flush=True)
            break
        hits += 1
        ecx = regs["ecx"]
        if ecx != TARGET_CONTAINER:
            unstick(sock, ENTRY_36116)
            continue
        esp = regs["esp"]
        caller = read_u32(sock, esp) or 0
        size_before = read_u32(sock, ecx + 4) or 0
        capacity_before = read_u32(sock, ecx + 8) or 0
        data_ptr = read_u32(sock, ecx) or 0
        print(f"FOUND at hits={hits} t={time.time()-t0:.1f}s ecx=0x{ecx:08X} "
              f"caller=0x{caller:08X} data=0x{data_ptr:08X} "
              f"size_before={size_before} capacity_before={capacity_before}", flush=True)
        # Read the raw guest bytes at the caller's call-site (16 bytes
        # before/around it) for the surrounding instructions.
        context = read_bytes(sock, caller - 16, 32)
        print(f"raw bytes at caller-16..caller+16: {context.hex()}", flush=True)
        break

    print("done", flush=True)


if __name__ == "__main__":
    main()
