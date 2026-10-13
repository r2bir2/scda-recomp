"""Ground-truth check for continued 62's precise question: at guest
0x0004749A (sub_0004721E's fallback "get name" call on MEM32(0x5B57FC),
taken when MEM32(0x5B5800)'s own get-name attempt is null/fails), does
real hardware ever see an empty result the way scda_recomp does right
before the Partition5 misopen -- or does the upstream branch (object #1's
own state/call) never even reach this fallback with the same shape?

Breaks at 0x0004749A (log obj1, obj2, obj2's vtable/target) and at
0x000474AC (log the return value and the resulting buffer content) for
every hit, not just oversized/suspicious ones -- this branch is not
expected to be rare (continued 60 showed the surrounding resource loop
runs constantly for legitimate small files), so the point is to see the
*shape* of every hit, not filter for something unusual.
"""
from __future__ import annotations

import socket
import struct
import time

CALLSITE = 0x0004749A
RESULT = 0x000474AC
BUF_OFFSET = 0xFFFFFDE0  # ebp - 544, as a 32-bit two's-complement offset added to ebp


def packet(sock, body):
    raw = body.encode("ascii")
    sock.sendall(b"$" + raw + b"#" + f"{sum(raw) & 0xff:02x}".encode("ascii"))
    first = sock.recv(1)
    if first == b"+":
        first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    result = bytearray()
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return result.decode("ascii")
        result.extend(char)


def wait_stop(sock):
    first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return


def send_continue(sock):
    sock.sendall(b"$c#63")
    sock.recv(1)


def step_once(sock, timeout):
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)


def interrupt_and_resync(sock, timeout=10):
    sock.sendall(b"\x03")
    sock.settimeout(timeout)
    wait_stop(sock)


def regs(sock):
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def mem_u32(sock, address):
    address &= 0xFFFFFFFF
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) < 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def read_cstring(sock, address, maxlen=48):
    out = []
    for i in range(maxlen):
        response = packet(sock, f"m{(address + i) & 0xFFFFFFFF:x},1")
        if response.startswith("E") or len(response) < 2:
            break
        b = bytes.fromhex(response)[0]
        if b == 0:
            break
        out.append(chr(b) if 32 <= b < 127 else f"\\x{b:02x}")
    return "".join(out)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{CALLSITE:x},1")
    packet(sock, f"Z0,{RESULT:x},1")
    print(f"armed 0x{CALLSITE:08X} and 0x{RESULT:08X}", flush=True)
    start = time.time()
    hits = 0
    continue_pending = False
    buf_addr_by_call = {}

    while time.time() - start < 900 and hits < 60:
        if not continue_pending:
            send_continue(sock)
            continue_pending = True
        try:
            sock.settimeout(min(60, max(1, 901 - (time.time() - start))))
            wait_stop(sock)
            continue_pending = False
        except socket.timeout:
            print(f"no hit (t={time.time()-start:.1f}s, hits={hits})", flush=True)
            continue

        r = regs(sock)
        hits += 1
        eip = r["eip"]
        if eip == CALLSITE:
            obj1 = mem_u32(sock, 0x005B5800)
            obj2 = mem_u32(sock, 0x005B57FC)
            vtable2 = mem_u32(sock, obj2) if obj2 else 0
            target2 = mem_u32(sock, (vtable2 or 0) + 0x10) if vtable2 else 0
            buf_addr = (r["ebp"] - 544) & 0xFFFFFFFF
            buf_addr_by_call[hits] = buf_addr
            print(
                f"HIT #{hits} [CALLSITE] t={time.time()-start:.1f}s "
                f"obj1=0x{obj1:08X} obj2=0x{obj2:08X} vtable2=0x{vtable2:08X} "
                f"target2=0x{target2:08X} buf=0x{buf_addr:08X}",
                flush=True,
            )
        else:
            # find the matching callsite's buffer address (should be the
            # immediately preceding CALLSITE hit for this same call)
            buf_addr = (r["ebp"] - 544) & 0xFFFFFFFF
            name = read_cstring(sock, buf_addr)
            print(
                f"HIT #{hits} [RESULT] t={time.time()-start:.1f}s "
                f"retval=0x{r['eax']:08X} ({r['eax'] if r['eax'] < 0x80000000 else r['eax']-0x100000000}) "
                f"name=\"{name}\"",
                flush=True,
            )
        packet(sock, f"z0,{eip:x},1")
        step_once(sock, 10)
        packet(sock, f"Z0,{eip:x},1")

    try:
        if continue_pending:
            interrupt_and_resync(sock)
        packet(sock, f"z0,{CALLSITE:x},1")
        packet(sock, f"z0,{RESULT:x},1")
    except socket.timeout:
        print("cleanup timed out", flush=True)

    print(f"done hits={hits}", flush=True)


if __name__ == "__main__":
    main()
