"""Break at sub_002E75CE's own entry (0x002E75CE) on real hardware.
It has a normal prologue (push ebp; mov ebp, esp per functions.json),
so at entry -- before any of its own instructions run -- [esp+0] is
reliably the genuine return address of whoever called it."""
from __future__ import annotations
import socket
import struct

TARGET = 0x002E75CE


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


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    r1 = packet(sock, f"Z0,{TARGET:x},1")
    print(f"arm breakpoint on 0x{TARGET:08X}: {r1}", flush=True)

    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(120)
    wait_stop(sock)
    regs = get_regs(sock)
    print(f"HIT eip=0x{regs['eip']:08X} esp=0x{regs['esp']:08X} "
          f"ecx(this)=0x{regs['ecx']:08X}", flush=True)

    ret = read_u32(sock, regs["esp"])
    print(f"Return address (real caller) = 0x{ret:08X}" if ret else "no return addr", flush=True)

    # dump a bit more of the stack for context
    for i in range(0, 32, 4):
        v = read_u32(sock, regs["esp"] + i)
        print(f"  [esp+0x{i:02x}] = 0x{v if v else 0:08X}", flush=True)

    print("done", flush=True)


if __name__ == "__main__":
    main()
