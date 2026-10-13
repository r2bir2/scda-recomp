"""Full ordinal-indexed capture of every sub_0001E460F (actual free
call) hit between marker A (sub_000366A2 entry) and marker B
(sub_001D589D entry) on real hardware, with bin6_len_before at each --
directly comparable, event-for-event, against scda_recomp's
n_bin6.txt / [SCDA-VECTOR-FREE] n=... log from the same window, since
ordinal position has already been proven a reliable, platform-
independent synchronization key for this whole investigation."""
from __future__ import annotations
import socket
import struct
import time

MARKER_A = 0x000366A2
MARKER_B = 0x001D589D
TARGET = 0x001E460F
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


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    r = packet(sock, f"Z0,{MARKER_A:x},1")
    print(f"arm marker A: {r}", flush=True)
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(120)
    wait_stop(sock)
    print("MARKER A HIT", flush=True)
    packet(sock, f"z0,{MARKER_A:x},1")

    packet(sock, f"Z0,{TARGET:x},1")
    packet(sock, f"Z0,{MARKER_B:x},1")
    print("armed target + marker B", flush=True)

    out = open("C:/VScode/XboxRecomp/build/scda_upgrade/xemu_vecfree_ordinal.log", "w")
    n = 0
    prev_bin6 = None
    t0 = time.time()
    while True:
        sock.sendall(b"$c#63")
        sock.recv(1)
        sock.settimeout(60)
        try:
            wait_stop(sock)
        except socket.timeout:
            print(f"no hit within 60s (had {n}) -- stopping", flush=True)
            break
        regs = get_regs(sock)
        eip = regs["eip"]
        if eip == MARKER_B:
            bl = bin6_chain_len(sock)
            line = f"MARKER B HIT t={time.time()-t0:.1f}s bin6_len={bl} after {n} vecfree events"
            print(line, flush=True)
            out.write(line + "\n")
            break
        if eip != TARGET:
            continue
        n += 1
        ecx = regs["ecx"]
        esp = regs["esp"]
        capacity = read_u32(sock, ecx + 8) or 0
        stride_arg = read_u32(sock, esp + 4) or 0
        bl = bin6_chain_len(sock)
        line = (f"n={n} ecx=0x{ecx:08X} capacity={capacity} stride_arg={stride_arg} "
                f"byte_count={capacity*stride_arg} bin6_len_before={bl}")
        out.write(line + "\n")
        if prev_bin6 is None or bl != prev_bin6:
            print(line, flush=True)
        prev_bin6 = bl
        unstick(sock, eip)
    out.close()
    print("done, full log at build/scda_upgrade/xemu_vecfree_ordinal.log", flush=True)


if __name__ == "__main__":
    main()
