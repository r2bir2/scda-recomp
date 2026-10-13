"""2026-09-06 (continued 65): does real hardware preserve ebx/esi/edi across
sub_00071B49's own full body (entry 0x71B49 through return 0x71DA8), and
does its internal MEM32(0x5B57FC)-based virtual dispatch (via sub_00045ADE,
guest 0x71C65) resolve to a self-consistent, cleanly-returning function --
or does the exact same shift-by-one-slot corruption scda_recomp shows also
happen on real hardware?

Breaks at entry (0x71B49, captures ebx/esi/edi and the caller-supplied
scratch-object pointer at [ebp+0x20]), the icall site (0x71C56, captures
the resolved vtable/target the same way scda_recomp's probe did), the
mid-function restore point (0x71CE2 before the pop-triple, 0x71CEC after),
and the true return (0x71DA8) -- then the immediate post-return point in
the caller so the OUTER ebx/esi/edi (what sub_00048F1D actually sees) can
be compared against the entry values captured here.
"""
from __future__ import annotations

import socket
import struct
import time

ENTRY = 0x00071B49
ICALLSITE = 0x00071C56
PRE_POP = 0x00071CE2
POST_POP = 0x00071CEC
RETURN_PT = 0x00071DA8
POINTS = (ENTRY, ICALLSITE, PRE_POP, POST_POP, RETURN_PT)


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
    address &= 0xFFFFFFFF
    result = packet(sock, f"m{address:x},4")
    if result.startswith("E") or len(result) < 8:
        return None
    return struct.unpack("<I", bytes.fromhex(result))[0]


def step_past(sock, point):
    packet(sock, f"z0,{point:x},1")
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(10)
    wait_stop(sock)
    packet(sock, f"Z0,{point:x},1")


def interrupt_and_resync(sock, timeout=10):
    sock.sendall(b"\x03")
    sock.settimeout(timeout)
    wait_stop(sock)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    for point in POINTS:
        packet(sock, f"Z0,{point:x},1")
    print("armed", [hex(p) for p in POINTS], flush=True)

    start = time.time()
    entry_ebx = entry_esi = entry_edi = None
    continue_pending = False
    hits = 0

    while time.time() - start < 300 and hits < 60:
        if not continue_pending:
            sock.sendall(b"$c#63")
            sock.recv(1)
            continue_pending = True
        try:
            sock.settimeout(min(60, max(1, 300 - (time.time() - start))))
            wait_stop(sock)
            continue_pending = False
        except socket.timeout:
            print(f"no hit (t={time.time()-start:.1f}s)", flush=True)
            continue

        r = regs(sock)
        pc = r["eip"]
        hits += 1

        if pc == ENTRY:
            entry_ebx, entry_esi, entry_edi = r["ebx"], r["esi"], r["edi"]
            scratch_obj = u32(sock, r["ebp"] + 0x20)
            print(f"HIT#{hits} t={time.time()-start:.1f}s ENTRY "
                  f"ebx={r['ebx']:08X} esi={r['esi']:08X} edi={r['edi']:08X} "
                  f"scratch_obj([ebp+0x20])={scratch_obj:08X} esp={r['esp']:08X}",
                  flush=True)
        elif pc == ICALLSITE:
            obj = u32(sock, 0x005B57FC)
            vtbl = u32(sock, obj) if obj else 0
            target = u32(sock, (vtbl or 0) + 0x10) if vtbl else 0
            print(f"HIT#{hits} t={time.time()-start:.1f}s ICALLSITE "
                  f"obj(0x5B57FC)={obj:08X} vtable={vtbl:08X} "
                  f"target(+0x10)={target:08X}", flush=True)
        elif pc in (PRE_POP, POST_POP):
            label = "PRE_POP" if pc == PRE_POP else "POST_POP"
            print(f"HIT#{hits} t={time.time()-start:.1f}s {label} "
                  f"ebx={r['ebx']:08X} esi={r['esi']:08X} edi={r['edi']:08X} "
                  f"esp={r['esp']:08X}", flush=True)
        elif pc == RETURN_PT:
            match = (entry_ebx == r["ebx"] and entry_esi == r["esi"] and
                     entry_edi == r["edi"])
            print(f"HIT#{hits} t={time.time()-start:.1f}s RETURN "
                  f"ebx={r['ebx']:08X}(entry={entry_ebx:08X}) "
                  f"esi={r['esi']:08X}(entry={entry_esi:08X}) "
                  f"edi={r['edi']:08X}(entry={entry_edi:08X}) "
                  f"MATCH={match}", flush=True)

        step_past(sock, pc)

    try:
        if continue_pending:
            interrupt_and_resync(sock)
        for point in POINTS:
            packet(sock, f"z0,{point:x},1")
    except socket.timeout:
        print("cleanup timed out", flush=True)

    print(f"done hits={hits}", flush=True)


if __name__ == "__main__":
    main()
