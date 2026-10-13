"""Semantic-window comparison capture: arm marker A (sub_000366A2 entry,
0x000366A2) and marker B (sub_001D589D entry, 0x001D589D), plus the four
teardown-chain addresses (sub_00047A76, sub_00047150, sub_00045FB0,
sub_00045F56). Record every hit of the four chain addresses strictly
between the FIRST marker-A hit and the FIRST marker-B hit, with the same
fields as scda_recomp's new [SCDA-CHAIN] hooks, plus a bin[6] walk at
every event and at marker B itself (for the already-established 40 vs 39
comparison). Uses the established disarm-on-hit / single-step unstick
pattern throughout to avoid the recurring "frozen continue" GDB artifact
at these specific addresses."""
from __future__ import annotations

import socket
import struct
import time

MARKER_A = 0x000366A2
MARKER_B = 0x001D589D
CHAIN = {
    0x00047A76: "DTOR",
    0x00047150: "OUTER",
    0x00045FB0: "INNER",
    0x00045F56: "DESTROYN",
}
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
    if address == 0:
        return None
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def get_regs(sock):
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def bin6_chain_len(sock):
    cur = read_u32(sock, BIN6_ADDR)
    n = 0
    while cur and n < 64:
        n += 1
        cur = read_u32(sock, cur + 8)
    return n


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

    for addr in [MARKER_A]:
        r = packet(sock, f"Z0,{addr:x},1")
        print(f"arm marker A 0x{addr:08X}: {r}", flush=True)

    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(120)
    wait_stop(sock)
    regs = get_regs(sock)
    print(f"MARKER A HIT eip=0x{regs['eip']:08X} ret=0x{read_u32(sock, regs['esp']) or 0:08X} "
          f"bin6_len={bin6_chain_len(sock)}", flush=True)
    packet(sock, f"z0,{MARKER_A:x},1")

    # Now arm marker B + all 4 chain addresses.
    for addr in list(CHAIN.keys()) + [MARKER_B]:
        r = packet(sock, f"Z0,{addr:x},1")
        print(f"arm 0x{addr:08X}: {r}", flush=True)

    seq = 0
    t0 = time.time()
    while True:
        sock.sendall(b"$c#63")
        sock.recv(1)
        sock.settimeout(60)
        try:
            wait_stop(sock)
        except socket.timeout:
            print(f"no hit within 60s (had {seq} chain events) -- stopping", flush=True)
            break
        regs = get_regs(sock)
        eip = regs["eip"]
        if eip == MARKER_B:
            print(f"MARKER B HIT t={time.time()-t0:.1f}s bin6_len={bin6_chain_len(sock)} "
                  f"after {seq} chain events -- STOPPING", flush=True)
            break
        if eip not in CHAIN:
            print(f"unexpected stop at 0x{eip:08X}, ignoring and continuing", flush=True)
            continue
        seq += 1
        label = CHAIN[eip]
        ecx = regs["ecx"]
        esp = regs["esp"]
        ret = read_u32(sock, esp) or 0
        bl = bin6_chain_len(sock)
        if label == "DTOR":
            vtable = read_u32(sock, ecx) or 0
            print(f"E{seq} {label} pc=0x{eip:08X} ret=0x{ret:08X} this=0x{ecx:08X} "
                  f"vtable=0x{vtable:08X} bin6_len={bl}", flush=True)
        elif label == "OUTER":
            ea = read_u32(sock, ecx) or 0
            ec = read_u32(sock, ecx + 4) or 0
            print(f"E{seq} {label} pc=0x{eip:08X} ret=0x{ret:08X} this=0x{ecx:08X} "
                  f"entry_array=0x{ea:08X} entry_count={ec} bin6_len={bl}", flush=True)
        elif label == "INNER":
            ba = read_u32(sock, ecx) or 0
            bc = read_u32(sock, ecx + 4) or 0
            print(f"E{seq} {label} pc=0x{eip:08X} ret=0x{ret:08X} container=0x{ecx:08X} "
                  f"bucket_array=0x{ba:08X} bucket_count={bc} bin6_len={bl}", flush=True)
        elif label == "DESTROYN":
            start = read_u32(sock, esp + 4) or 0
            count = read_u32(sock, esp + 8) or 0
            print(f"E{seq} {label} pc=0x{eip:08X} ret=0x{ret:08X} this=0x{ecx:08X} "
                  f"start={start} count={count} bin6_len={bl}", flush=True)
        unstick(sock, eip)


if __name__ == "__main__":
    main()
