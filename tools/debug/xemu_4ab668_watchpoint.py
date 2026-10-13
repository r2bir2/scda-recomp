"""Real-hardware ground truth: place a GDB hardware write-watchpoint
(Z2 packet) on MEM32(0x004AB668) (the global D3D device-struct pointer
sub_0049CE00/sub_004A12B0 read as null in scda_recomp) as early in boot
as possible, and record every write with full context: guest PC,
previous/new value, registers, ESP/EBP, return address on the guest
stack. Modeled directly on tools/debug/xemu_6513a8_watchpoint.py, which
used this same technique successfully for 0x6513A4/0x6513A8."""
from __future__ import annotations
import socket
import struct
import time

TARGET = 0x004AB668


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


def read_bytes(sock, address, length):
    response = packet(sock, f"m{address:x},{length:x}")
    if response.startswith("E"):
        return b""
    return bytes.fromhex(response)


def cont_and_wait(sock, timeout):
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)
    return get_regs(sock)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    r1 = packet(sock, f"Z2,{TARGET:x},4")
    print(f"arm write-watchpoint on 0x{TARGET:08X}: {r1}", flush=True)

    prev = read_u32(sock, TARGET)
    print(f"initial: MEM32(0x{TARGET:08X})=0x{prev if prev else 0:08X}", flush=True)

    t0 = time.time()
    hits = 0
    max_hits = 20
    while hits < max_hits:
        try:
            regs = cont_and_wait(sock, 180)
        except socket.timeout:
            print(f"no (more) watchpoint hit within 180s (had {hits})", flush=True)
            break
        hits += 1
        t = time.time() - t0
        cur = read_u32(sock, TARGET)
        ret = read_u32(sock, regs["esp"])
        code = read_bytes(sock, regs["eip"], 16)
        print(f"HIT#{hits} t={t:.2f}s eip=0x{regs['eip']:08X} "
              f"value: 0x{prev if prev else 0:08X}->0x{cur if cur else 0:08X} "
              f"eax=0x{regs['eax']:08X} ecx=0x{regs['ecx']:08X} edx=0x{regs['edx']:08X} "
              f"ebx=0x{regs['ebx']:08X} esi=0x{regs['esi']:08X} edi=0x{regs['edi']:08X} "
              f"esp=0x{regs['esp']:08X} ebp=0x{regs['ebp']:08X} "
              f"ret=0x{ret if ret else 0:08X} bytes_at_eip={code.hex()}", flush=True)
        prev = cur
        if cur and cur != 0:
            print("*** TARGET STATE REACHED (non-null pointer written) -- STOPPING ***", flush=True)
            break

    print("done", flush=True)


if __name__ == "__main__":
    main()
