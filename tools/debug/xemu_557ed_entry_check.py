"""Does real hardware ever CALL sub_000557ED at all during boot, and if so,
does sub_000556A8's return value (eax) come back negative (taking the jl
error path, never reaching HalReturnToFirmware) rather than >=0 (the path
scda_recomp incorrectly takes)?

Breakpoints:
  0x000557ED - function entry (proves the function is even called)
  0x00055806 - right after `call sub_000556A8`, eax = sub_000556A8's
               return value, BEFORE the `test eax,eax` / `jl` executes
"""
from __future__ import annotations
import socket
import struct
import time

ENTRY = 0x000557ED
POST_CALL = 0x00055806


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
    packet(sock, f"Z0,{ENTRY:x},1")
    packet(sock, f"Z0,{POST_CALL:x},1")
    print(f"armed entry 0x{ENTRY:08X} and post-call 0x{POST_CALL:08X}, "
          f"watching for up to 6 minutes", flush=True)

    t0 = time.time()
    deadline = t0 + 360
    entry_hits = 0
    post_call_hits = 0

    while time.time() < deadline:
        remaining = deadline - time.time()
        try:
            regs = cont_and_wait(sock, max(1, remaining))
        except socket.timeout:
            print(f"no more hits within remaining window -- stopping "
                  f"(entry_hits={entry_hits} post_call_hits={post_call_hits})",
                  flush=True)
            break

        eip = regs["eip"]
        t = time.time() - t0
        if eip == ENTRY:
            entry_hits += 1
            a1 = read_u32(sock, regs["esp"] + 4)
            a2 = read_u32(sock, regs["esp"] + 8)
            a3 = read_u32(sock, regs["esp"] + 12)
            a4 = read_u32(sock, regs["esp"] + 16)
            a5 = read_u32(sock, regs["esp"] + 20)
            print(f"ENTRY HIT #{entry_hits} at t={t:.1f}s eip=0x{eip:08X} "
                  f"esp=0x{regs['esp']:08X} args=[0x{a1:08X},0x{a2:08X},"
                  f"0x{a3:08X},0x{a4:08X},0x{a5:08X}]", flush=True)
        elif eip == POST_CALL:
            post_call_hits += 1
            eax = regs["eax"]
            signed = eax if eax < 0x80000000 else eax - 0x100000000
            print(f"POST-CALL HIT #{post_call_hits} at t={t:.1f}s eip=0x{eip:08X} "
                  f"eax(sub_000556A8 retval)=0x{eax:08X} ({signed}) "
                  f"{'NEGATIVE -> error path (skips reboot)' if signed < 0 else 'NON-NEGATIVE -> reboot path taken'}",
                  flush=True)
        else:
            print(f"unexpected stop at eip=0x{eip:08X} t={t:.1f}s", flush=True)

    print(f"done. entry_hits={entry_hits} post_call_hits={post_call_hits}", flush=True)


if __name__ == "__main__":
    main()
