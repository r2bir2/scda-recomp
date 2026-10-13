"""Single, uniform-methodology full ordinal capture (no fast/slow path
switch, to avoid the counting-boundary inconsistency seen when mixing
capture styles) of every sub_001E460F hit between marker A and marker
B, reading return address + bin6_len at entry for every single event
from n=1 onward. Lighter weight than ordinal2.py (no exit tracking) so
it stays fast throughout."""
from __future__ import annotations
import socket
import struct
import time

MARKER_A = 0x000366A2
MARKER_B = 0x001D589D
ENTRY = 0x001E460F
BIN6_ADDR = 0x0064D2B4


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

    packet(sock, f"Z0,{ENTRY:x},1")
    packet(sock, f"Z0,{MARKER_B:x},1")
    print("armed entry + marker B", flush=True)

    out = open("C:/VScode/XboxRecomp/build/scda_upgrade/xemu_vecfree_ordinal3.log", "w")
    n = 0
    prev_bl = None
    t0 = time.time()
    while True:
        try:
            regs = cont_and_wait(sock, 60)
        except socket.timeout:
            print(f"no hit within 60s (had {n}) -- stopping", flush=True)
            break
        eip = regs["eip"]
        if eip == MARKER_B:
            bl = bin6_chain_len(sock)
            line = f"MARKER B HIT t={time.time()-t0:.1f}s bin6_len={bl} after {n} events"
            print(line, flush=True)
            out.write(line + "\n")
            break
        if eip != ENTRY:
            continue
        n += 1
        ecx = regs["ecx"]
        esp = regs["esp"]
        ret = read_u32(sock, esp) or 0
        bl = bin6_chain_len(sock)
        line = f"n={n} ret=0x{ret:08X} ecx=0x{ecx:08X} bin6_len_before={bl}"
        out.write(line + "\n")
        if prev_bl is None or bl != prev_bl:
            print(line, flush=True)
        prev_bl = bl
        unstick(sock, eip)
    out.close()
    print("done", flush=True)


if __name__ == "__main__":
    main()
