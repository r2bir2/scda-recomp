"""2026-09-08 (continued 72): trace sub_000443D0's own execution for the
ebp-56 object's invocation, since every external input to it (this
pointer, nested vtable, resolved icall target, 0x5B57F0, nested field+8)
is already proven byte-identical between xemu and scda_recomp
(continued 71).

sub_000443D0 does: (1) a sub_0006787C substring search whose result
sets a found/not-found flag `edi`, (2) a nested icall via
MEM32(MEM32(esi+4)) 's own vtable slot+8 (a THIRD object, distinct from
`esi` itself and from the outer object passed into it), gated by that
found/not-found flag and MEM32(0x5ED140)/MEM32(0x5ED144).

Breaks at:
  0x000443D0 (ENTRY)      -- this (=esi after mov)
  0x000443E4 (SEARCH-RET) -- raw eax from sub_0006787C, before the
                              double-negate that produces `edi`
  0x00044408 (PRE-ICALL)  -- object2=MEM32(esi+4), vtable2=MEM32(object2),
                              icall_target=MEM32(vtable2+8), plus the
                              three pushed args
  0x00044419 (POST-ICALL) -- eax = the nested icall's own return value

Only the invocation on the ebp-56 ("broken") object is of interest;
identify it at ENTRY by this==nested1 whose own vtable (MEM32(this))
is 0x004F61E0 (as opposed to the healthy object's chain).
"""
from __future__ import annotations

import socket
import struct
import time

ENTRY = 0x000443D0
SEARCH_RET = 0x000443E4
PRE_ICALL = 0x00044408
POST_ICALL = 0x00044419
POINTS = (ENTRY, SEARCH_RET, PRE_ICALL, POST_ICALL)


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

    while time.time() - start < 180 and hits < 20:
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
            this_vtbl = u32(sock, this_obj)
            print(f"HIT#{hits} t={time.time()-start:.1f}s ENTRY this=0x{this_obj:08X} "
                  f"this_vtbl=0x{this_vtbl:08X}", flush=True)
        elif pc == SEARCH_RET:
            print(f"HIT#{hits} t={time.time()-start:.1f}s SEARCH-RET raw_eax=0x{r['eax']:08X}",
                  flush=True)
        elif pc == PRE_ICALL:
            esi = r["esi"]
            ebp = r["ebp"]
            object2 = u32(sock, esi + 4)
            vtbl2 = u32(sock, object2) if object2 else 0
            icall_target = u32(sock, vtbl2 + 8) if vtbl2 else 0
            arg10 = u32(sock, ebp + 0x10)
            argC = u32(sock, ebp + 0xC)
            arg8 = u32(sock, ebp + 8)
            print(f"HIT#{hits} t={time.time()-start:.1f}s PRE-ICALL esi=0x{esi:08X} "
                  f"object2=0x{object2:08X} vtbl2=0x{vtbl2:08X} icall_target=0x{icall_target:08X} "
                  f"arg[ebp+10]=0x{arg10:08X} arg[ebp+C]=0x{argC:08X} arg[ebp+8]=0x{arg8:08X}",
                  flush=True)
        elif pc == POST_ICALL:
            print(f"HIT#{hits} t={time.time()-start:.1f}s POST-ICALL eax(ret)=0x{r['eax']:08X} "
                  f"edi=0x{r['edi']:08X} ebx=0x{r['ebx']:08X}", flush=True)

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
