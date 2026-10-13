"""On a fresh cold boot, watch: (1) sub_00055666's entry (0x55666), reading
0x5B1D90/0x5B1D94 live at that exact moment; (2) which of its two call
sites actually fires -- the callback dispatch at 0x556A4 (calls whatever
is stored at 0x5B1D94, i.e. sub_0047D997) or the default path's call to
sub_00055380 at 0x55691."""
from __future__ import annotations
import socket
import struct
import time

ENTRY = 0x00055666
CALLBACK_CALL = 0x000556A4   # call dword ptr [ecx] -- the registered-callback path
DEFAULT_CALL = 0x00055691    # call sub_00055380 -- the default/no-callback path
MARKER = 0x000366A2          # early-boot sanity marker, fires ~t=1.3s if xemu is actually running guest code


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


def get_regs(sock):
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def read_mem(sock, addr, length):
    response = packet(sock, f"m{addr:x},{length:x}")
    return bytes.fromhex(response)


def read_u32(sock, addr):
    return struct.unpack("<I", read_mem(sock, addr, 4))[0]


def cont_and_wait(sock, timeout):
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)
    return get_regs(sock)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    sites = {ENTRY: "ENTRY", CALLBACK_CALL: "CALLBACK_CALL(0x556A4)", DEFAULT_CALL: "DEFAULT_CALL(0x55691)", MARKER: "MARKER(sanity)"}
    for addr in sites:
        packet(sock, f"Z0,{addr:x},1")
    print(f"armed {[hex(a) for a in sites]}, watching for up to 4 minutes", flush=True)

    t0 = time.time()
    deadline = t0 + 240
    hits = 0

    while time.time() < deadline:
        remaining = deadline - time.time()
        try:
            regs = cont_and_wait(sock, max(1, remaining))
        except socket.timeout:
            print(f"no more hits -- stopping (hits={hits})", flush=True)
            break

        eip = regs["eip"]
        t = time.time() - t0
        hits += 1
        if eip == ENTRY:
            v90 = read_u32(sock, 0x5B1D90)
            v94 = read_u32(sock, 0x5B1D94)
            print(f"HIT #{hits} [ENTRY] at t={t:.1f}s 0x5B1D90=0x{v90:08X} 0x5B1D94=0x{v94:08X}", flush=True)
        elif eip == CALLBACK_CALL:
            ecx = regs["ecx"]
            target = read_u32(sock, ecx) if ecx else 0
            print(f"HIT #{hits} [CALLBACK_CALL] at t={t:.1f}s ecx=0x{ecx:08X} target=0x{target:08X}", flush=True)
        elif eip == DEFAULT_CALL:
            print(f"HIT #{hits} [DEFAULT_CALL] at t={t:.1f}s", flush=True)
        elif eip == MARKER:
            print(f"HIT #{hits} [MARKER-sanity] at t={t:.1f}s -- xemu is running guest code", flush=True)
            packet(sock, f"z0,{MARKER:x},1")
        else:
            print(f"HIT #{hits} unexpected eip=0x{eip:08X} at t={t:.1f}s", flush=True)

    print(f"done. total hits={hits}", flush=True)


if __name__ == "__main__":
    main()
