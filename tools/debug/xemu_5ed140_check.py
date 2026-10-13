"""Check real hardware's value for MEM32(0x5ED140)/MEM32(0x5ED144) --
and which branch gets taken -- at the two decision points in
sub_00048F1D (0x00048F7E: cmp [0x5ED144],ebx / je 0x49060; and
0x00049066: cmp [0x5ED140],ebx / je 0x49336) that determine whether
scda_recomp proceeds toward its eventual HalReturnToFirmware exit."""
from __future__ import annotations
import socket
import struct
import time

CHECK1 = 0x00048F7E   # cmp [0x5ED144], ebx  (before je 0x49060)
CHECK2 = 0x00049066   # already past the je; use 0x00048F86 instead (the je itself)
JE1 = 0x00048F86       # je 0x49060
JE2 = 0x00049066        # je 0x49336


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


def get_regs(sock):
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def cont_and_wait(sock, timeout=120):
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)
    return get_regs(sock)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    packet(sock, f"Z0,{JE1:x},1")
    print(f"arm 0x{JE1:08X}", flush=True)
    t0 = time.time()
    regs = cont_and_wait(sock, 180)
    print(f"HIT 1 at t={time.time()-t0:.1f}s eip=0x{regs['eip']:08X}", flush=True)
    v144 = read_u32(sock, 0x5ED144)
    v140 = read_u32(sock, 0x5ED140)
    print(f"MEM32(0x5ED144)=0x{v144:08X}  MEM32(0x5ED140)=0x{v140:08X}  ebx={regs['ebx']:08X}", flush=True)
    print("-> je 0x49060 will be TAKEN" if v144 == regs["ebx"] else "-> je 0x49060 NOT taken (falls to 0x48F8C)", flush=True)

    packet(sock, f"z0,{JE1:x},1")
    if v144 == regs["ebx"]:
        # branch taken -- follow to the second check
        packet(sock, f"Z0,{JE2:x},1")
        print(f"arm 0x{JE2:08X}", flush=True)
        regs2 = cont_and_wait(sock, 60)
        print(f"HIT 2 at eip=0x{regs2['eip']:08X}", flush=True)
        v140b = read_u32(sock, 0x5ED140)
        print(f"MEM32(0x5ED140)=0x{v140b:08X} ebx={regs2['ebx']:08X}", flush=True)
        print("-> je 0x49336 will be TAKEN (exit-ish path)" if v140b == regs2["ebx"] else "-> je 0x49336 NOT taken (continues into 0x4906C onward)", flush=True)
        packet(sock, f"z0,{JE2:x},1")


if __name__ == "__main__":
    main()
