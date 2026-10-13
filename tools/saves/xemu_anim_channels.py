"""Update 394: sample Sam's animation-channel state in ORIGINAL xemu.

Recomp (Update 393) shows the 61-bone skeletal instance's channels 2/3
(TArray at [inst+0x98], stride 0x80) flipping every frame between flag
0x80 / tween 0 and flag 0x480 / tween 1/30, plus [[inst+0xB8]+4] flipping
1.0 <-> 0.6667. This script breaks at the same guest function sub_0021AF22
(ecx = instance), keeps only the 61-bone instance, and logs the same fields
at each hit, so original and recomp can be compared directly.
Read-only: software breakpoint, memory reads, then breakpoint removed.
"""
from __future__ import annotations

import socket
import struct
import sys
import time

TARGET = 0x0021AF22
HITS_WANTED = int(sys.argv[1]) if len(sys.argv) > 1 else 80
WANT_BONES = tuple(int(v) for v in (sys.argv[2] if len(sys.argv) > 2 else "61").split(","))
DEADLINE = time.time() + 480


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


def read(sock, address, size):
    response = packet(sock, f"m{address:x},{size:x}")
    if response.startswith("E") or len(response) != size * 2:
        return None
    return bytes.fromhex(response)


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def f32(b, o):
    return struct.unpack_from("<f", b, o)[0]


def regs(sock):
    data = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip")
    return {n: struct.unpack_from("<I", data, i * 4)[0] for i, n in enumerate(names)}


def drain(sock):
    sock.settimeout(0.3)
    try:
        while sock.recv(4096):
            pass
    except OSError:
        pass
    sock.settimeout(10)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.sendall(b"\x03")
    time.sleep(0.2)
    drain(sock)
    print("stop:", packet(sock, "?"), flush=True)
    drain(sock)
    print("arm:", packet(sock, f"Z0,{TARGET:x},1"), flush=True)
    got = 0
    hits = 0
    while got < HITS_WANTED and time.time() < DEADLINE:
        sock.sendall(b"$c#63")
        sock.settimeout(60)
        wait_stop(sock)
        sock.settimeout(10)
        hits += 1
        inst = regs(sock)["ecx"]
        head = read(sock, inst, 0x230)
        if not head or u32(head, 0x174) not in WANT_BONES:
            continue
        got += 1
        ch = read(sock, u32(head, 0x98), 0x200) or b"\0" * 0x200
        b8 = read(sock, u32(head, 0xB8), 0x18) or b"\0" * 0x18
        parts = []
        for c in range(4):
            o = c * 0x80
            parts.append(f"ch{c} name={u32(ch, o):08X} f2={f32(ch, o + 8):.4g} "
                         f"f4={f32(ch, o + 0x10):.4g} tw={f32(ch, o + 0x14):.4g} "
                         f"flags={u32(ch, o + 0x28):08X}")
        print(f"[XEMU-ANIM] hit={hits} n={got} inst=0x{inst:08X} bones={u32(head, 0x174)} vt=0x{u32(head, 0):08X} "
              f"i208={u32(head, 0x208):X} i218={u32(head, 0x218):X} "
              f"b8={f32(b8, 4):.4g},{f32(b8, 0x14):.4g} | " + " | ".join(parts),
              flush=True)
    print("disarm:", packet(sock, f"z0,{TARGET:x},1"), flush=True)
    sock.sendall(b"$c#63")
    print(f"done hits={hits} sam={got}", flush=True)
    sock.close()


if __name__ == "__main__":
    main()
