"""2026-09-07 (continued 69): find the exact write/call that first
populates the ebp-56 object's table fields ([obj+0x20]/[obj+0x24]/
[obj+0x2C]) on real hardware, between its construction (first
sub_00045647 call inside sub_00048B15, guest 0x48C8F) and
sub_00071B49's own consumption of it via MEM32(0x5B57FC) (guest
0x71C56).

Step 1: break at 0x48C8F (right before `ecx = ebp - 56; call
sub_00045647`), read EBP, compute the object's address as ebp-56.
Step 2: arm GDB hardware write-watchpoints (Z2) on obj+0x20 and
obj+0x2C.
Step 3: continue, logging every hit (guest PC, old/new value, full
register state, return address on stack, and a short EBP-chain walk)
until either watchpoint fires, or a stop-marker breakpoint at 0x71C56
(sub_00071B49's own consumption point) is reached first (meaning
nothing ever wrote it).
"""
from __future__ import annotations

import socket
import struct
import time

CONSTRUCT_SITE = 0x00048C8F
STOP_MARKER = 0x00071C56


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


def regs(sock):
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def u32(sock, address):
    if address is None:
        return None
    address &= 0xFFFFFFFF
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) < 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def step_past_bp(sock, addr):
    packet(sock, f"z0,{addr:x},1")
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(10)
    wait_stop(sock)
    packet(sock, f"Z0,{addr:x},1")


def ebp_chain(sock, ebp, depth=5):
    frames = []
    cur = ebp
    for _ in range(depth):
        if cur is None or cur == 0 or cur > 0xFFFFFFFF:
            break
        saved_ebp = u32(sock, cur)
        ret = u32(sock, cur + 4)
        if ret is None:
            break
        frames.append((cur, saved_ebp, ret))
        if saved_ebp is None or saved_ebp <= cur:
            break
        cur = saved_ebp
    return frames


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    # Step 1: break at construction site, compute object address.
    packet(sock, f"Z0,{CONSTRUCT_SITE:x},1")
    print(f"armed construction-site breakpoint 0x{CONSTRUCT_SITE:08X}", flush=True)
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(120)
    wait_stop(sock)
    r = regs(sock)
    obj = (r["ebp"] - 56) & 0xFFFFFFFF
    print(f"HIT construction-site: ebp=0x{r['ebp']:08X} -> object=0x{obj:08X}", flush=True)
    packet(sock, f"z0,{CONSTRUCT_SITE:x},1")

    target20 = obj + 0x20
    target24 = obj + 0x24
    target2c = obj + 0x2C
    print(f"initial field20=0x{u32(sock, target20):08X} "
          f"field24=0x{u32(sock, target24):08X} "
          f"field2c=0x{u32(sock, target2c):08X}", flush=True)

    # Step 2: arm hardware write-watchpoints on the table fields.
    r1 = packet(sock, f"Z2,{target20:x},4")
    r2 = packet(sock, f"Z2,{target2c:x},4")
    print(f"armed write-watch +0x20: {r1}, +0x2c: {r2}", flush=True)

    # Stop marker: sub_00071B49's own consumption point.
    packet(sock, f"Z0,{STOP_MARKER:x},1")
    print(f"armed stop-marker 0x{STOP_MARKER:08X}", flush=True)

    start = time.time()
    hit_count = 0
    while time.time() - start < 180 and hit_count < 10:
        sock.sendall(b"$c#63")
        sock.recv(1)
        try:
            sock.settimeout(60)
            wait_stop(sock)
        except socket.timeout:
            print(f"no hit (t={time.time()-start:.1f}s)", flush=True)
            continue

        r = regs(sock)
        hit_count += 1
        pc = r["eip"]
        if pc == STOP_MARKER:
            print(f"HIT#{hit_count} t={time.time()-start:.1f}s STOP-MARKER REACHED "
                  f"(sub_00071B49 consumption) -- field20=0x{u32(sock, target20):08X} "
                  f"field24=0x{u32(sock, target24):08X} field2c=0x{u32(sock, target2c):08X}",
                  flush=True)
            break

        ret = u32(sock, r["esp"])
        frames = ebp_chain(sock, r["ebp"])
        chain_str = " <- ".join(f"ret=0x{ret_:08X}" for (_, _, ret_) in frames)
        print(f"HIT#{hit_count} t={time.time()-start:.1f}s WRITE at pc=0x{pc:08X} "
              f"field20=0x{u32(sock, target20):08X} field2c=0x{u32(sock, target2c):08X} "
              f"eax=0x{r['eax']:08X} ecx=0x{r['ecx']:08X} edx=0x{r['edx']:08X} "
              f"ebx=0x{r['ebx']:08X} esi=0x{r['esi']:08X} edi=0x{r['edi']:08X} "
              f"esp=0x{r['esp']:08X} ebp=0x{r['ebp']:08X} immediate_ret=0x{ret if ret else 0:08X} "
              f"chain=[{chain_str}]", flush=True)

        # Step past whatever we hit (could be the same instruction re-hitting).
        packet(sock, f"z2,{target20:x},4")
        packet(sock, f"z2,{target2c:x},4")
        step_past_bp_like = True
        sock.sendall(b"$s#73")
        sock.recv(1)
        sock.settimeout(10)
        try:
            wait_stop(sock)
        except socket.timeout:
            pass
        packet(sock, f"Z2,{target20:x},4")
        packet(sock, f"Z2,{target2c:x},4")

    try:
        packet(sock, f"z2,{target20:x},4")
        packet(sock, f"z2,{target2c:x},4")
        packet(sock, f"z0,{STOP_MARKER:x},1")
    except socket.timeout:
        print("cleanup timed out", flush=True)

    print(f"done hits={hit_count}", flush=True)


if __name__ == "__main__":
    main()
