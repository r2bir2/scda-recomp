"""Follow-up to xemu_4ab668_watchpoint.py: same write-watchpoint on
MEM32(0x004AB668), but once hit, dump a wide stack window (return
address should sit 12 bytes above ESP at the observed EIP=0x0049D259,
since 3 pushes -- esi, edi, ecx -- happened since sub_0049D210's own
entry with no prologue) to identify the real caller."""
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
    while hits < 5:
        try:
            regs = cont_and_wait(sock, 60)
        except socket.timeout:
            print(f"no (more) hit within 60s (had {hits})", flush=True)
            break
        hits += 1
        t = time.time() - t0
        cur = read_u32(sock, TARGET)
        print(f"HIT#{hits} t={t:.2f}s eip=0x{regs['eip']:08X} "
              f"value: 0x{prev if prev else 0:08X}->0x{cur if cur else 0:08X} "
              f"esp=0x{regs['esp']:08X}", flush=True)
        if cur and cur != 0 and prev == 0:
            esp = regs["esp"]
            print("Stack window around ESP:", flush=True)
            for i in range(0, 64, 4):
                v = read_u32(sock, esp + i)
                print(f"  [esp+0x{i:02x}] = 0x{v if v else 0:08X}", flush=True)
            print("STOPPING after finding the transition.", flush=True)
            break
        prev = cur

    print("done", flush=True)


if __name__ == "__main__":
    main()
