"""Real-hardware ground truth: place a GDB hardware write-watchpoint
(Z2 packet) on MEM32(0x006513A8) (the count field for the array-bounds
check in sub_00048F1D) as early in boot as possible, and record every
write with full context: guest PC, previous/new value, registers,
ESP/EBP, return address on the guest stack. Also separately captures
MEM32(0x006513A4) (base) and MEM32(0x006513C8) (index) at each hit for
correlation, per the user's explicit request to treat base+count as one
state. Falls back to a write-watchpoint on 0x6513A4 too if the count
watchpoint alone doesn't fire, since they likely get written by the
same routine and one might trip first depending on instruction order."""
from __future__ import annotations
import socket
import struct
import time

TARGET_COUNT = 0x006513A8
TARGET_BASE = 0x006513A4
TARGET_INDEX = 0x006513C8


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

    # Z2 = write watchpoint, length 4 bytes.
    r1 = packet(sock, f"Z2,{TARGET_COUNT:x},4")
    print(f"arm write-watchpoint on count 0x{TARGET_COUNT:08X}: {r1}", flush=True)
    r2 = packet(sock, f"Z2,{TARGET_BASE:x},4")
    print(f"arm write-watchpoint on base 0x{TARGET_BASE:08X}: {r2}", flush=True)

    prev_count = read_u32(sock, TARGET_COUNT)
    prev_base = read_u32(sock, TARGET_BASE)
    print(f"initial: base=0x{prev_base:08X} count={prev_count}", flush=True)

    t0 = time.time()
    hits = 0
    max_hits = 20
    while hits < max_hits:
        try:
            regs = cont_and_wait(sock, 120)
        except socket.timeout:
            print(f"no (more) watchpoint hit within 120s (had {hits})", flush=True)
            break
        hits += 1
        t = time.time() - t0
        cur_count = read_u32(sock, TARGET_COUNT)
        cur_base = read_u32(sock, TARGET_BASE)
        cur_index = read_u32(sock, TARGET_INDEX)
        ret = read_u32(sock, regs["esp"])
        code = read_bytes(sock, regs["eip"], 16)
        print(f"HIT#{hits} t={t:.2f}s eip=0x{regs['eip']:08X} "
              f"base: 0x{prev_base:08X}->0x{cur_base:08X} "
              f"count: {prev_count}->{cur_count} index=0x{cur_index if cur_index else 0:08X} "
              f"eax=0x{regs['eax']:08X} ecx=0x{regs['ecx']:08X} edx=0x{regs['edx']:08X} "
              f"ebx=0x{regs['ebx']:08X} esi=0x{regs['esi']:08X} edi=0x{regs['edi']:08X} "
              f"esp=0x{regs['esp']:08X} ebp=0x{regs['ebp']:08X} "
              f"ret=0x{ret if ret else 0:08X} bytes_at_eip={code.hex()}", flush=True)
        prev_count = cur_count
        prev_base = cur_base
        if cur_count == 32 and cur_base != 0:
            print("*** TARGET STATE REACHED (base!=0, count=32) -- STOPPING ***", flush=True)
            break

    print("done", flush=True)


if __name__ == "__main__":
    main()
