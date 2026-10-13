"""2026-09-08 (continued 71): compare sub_00045D0B's icall2 inputs
(MEM32(0x5B57F0), MEM32(esi+8), the nested stream object's own +8
field) between real hardware and scda_recomp, for the specific
invocation on the ebp-56 object (identifiable by nested1_vtbl ==
0x004F61E0, as opposed to the healthy 0x5B5800 object's invocation,
nested1_vtbl == 0x004F6420).

Breaks at guest 0x00045D0B (entry: this, nested1, nested1_vtbl,
nested1_field4) and 0x00045D24 (right before icall2's own code reads
its args: MEM32(0x5B57F0) as both hex and text, nested1_field8,
esi_field8). Captures every hit (there are exactly two per boot, one
per object) so both can be compared against scda_recomp's own log.
"""
from __future__ import annotations

import socket
import struct
import time

ENTRY = 0x00045D0B
ICALL1_RESULT = 0x00045D24
POINTS = (ENTRY, ICALL1_RESULT)


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


def read_wstring(sock, address, maxlen=40):
    out = []
    for i in range(maxlen):
        response = packet(sock, f"m{(address + i*2) & 0xFFFFFFFF:x},2")
        if response.startswith("E") or len(response) < 4:
            break
        wc = struct.unpack("<H", bytes.fromhex(response))[0]
        if wc == 0:
            break
        out.append(chr(wc) if 32 <= wc < 127 else f"\\x{wc:04x}")
    return "".join(out)


def step_past(sock, addr):
    packet(sock, f"z0,{addr:x},1")
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(10)
    wait_stop(sock)
    packet(sock, f"Z0,{addr:x},1")


def interrupt_and_resync(sock, timeout=10):
    sock.sendall(b"\x03")
    sock.settimeout(timeout)
    wait_stop(sock)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    for p in POINTS:
        packet(sock, f"Z0,{p:x},1")
    print(f"armed {[hex(p) for p in POINTS]}", flush=True)

    start = time.time()
    hits = 0
    continue_pending = False

    while time.time() - start < 180 and hits < 10:
        if not continue_pending:
            sock.sendall(b"$c#63")
            sock.recv(1)
            continue_pending = True
        try:
            sock.settimeout(60)
            wait_stop(sock)
            continue_pending = False
        except socket.timeout:
            print(f"no hit (t={time.time()-start:.1f}s)", flush=True)
            continue

        r = regs(sock)
        pc = r["eip"]
        hits += 1

        if pc == ENTRY:
            this_obj = r["ecx"]
            nested1 = u32(sock, this_obj + 4)
            nested1_vtbl = u32(sock, nested1) if nested1 else 0
            nested1_field4 = u32(sock, nested1 + 4) if nested1 else 0
            arg = u32(sock, r["esp"] + 4)
            print(f"HIT#{hits} t={time.time()-start:.1f}s ENTRY this=0x{this_obj:08X} "
                  f"nested1=0x{nested1:08X} nested1_vtbl=0x{nested1_vtbl:08X} "
                  f"nested1_field4=0x{nested1_field4:08X} arg=0x{arg:08X}", flush=True)
        elif pc == ICALL1_RESULT:
            esi = r["esi"]
            nested1 = u32(sock, esi + 4)
            g_5b57f0 = read_wstring(sock, 0x005B57F0)
            g_5b57f0_raw = u32(sock, 0x005B57F0)
            nested1_field8 = u32(sock, nested1 + 8) if nested1 else 0
            esi_field8 = u32(sock, esi + 8)
            print(f"HIT#{hits} t={time.time()-start:.1f}s ICALL1-RESULT esi(this)=0x{esi:08X} "
                  f"eax(icall1_ret)=0x{r['eax']:08X} 5B57F0_raw=0x{g_5b57f0_raw:08X} "
                  f"5B57F0_text=\"{g_5b57f0}\" nested1_field8=0x{nested1_field8:08X} "
                  f"esi_field8=0x{esi_field8:08X}", flush=True)

        step_past(sock, pc)

    try:
        if continue_pending:
            interrupt_and_resync(sock)
        for p in POINTS:
            packet(sock, f"z0,{p:x},1")
    except socket.timeout:
        print("cleanup timed out", flush=True)

    print(f"done hits={hits}", flush=True)


if __name__ == "__main__":
    main()
