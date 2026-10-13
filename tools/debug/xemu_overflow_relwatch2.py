"""2026-09-16 (Update 158, corrected): the first version of this check
mistakenly reused the SIZE value (edi at VA 0x771F9, which holds the
requested byte count, not a pointer) as if it were the buffer's base
address. Fixed: anchor on the size-matching allocation request first
(proving we're at the right event), then scoped-arm a breakpoint at
sub_004BF700's entry (0x004BF700) to catch the *next* invocation of
the fill routine and read its real buffer base from EAX (per the
recomp disassembly: `edi = eax;` is the very first thing the function
does with its incoming buffer argument).

Then arms a write watchpoint at (that base + 0x10144) -- the exact
relative offset recomp's own fill loop was proven (Update 155) to
write past, overflowing block 8 into block 9 -- and checks whether
real hardware's identical fill loop ever reaches that same relative
offset.
"""
from __future__ import annotations

import socket
import struct

SIZE_ANCHOR = 0x000771F9
FILL_ENTRY = 0x004BF700
REL_OFFSET = 0x10144
MAX_TRIES = 200


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


def cont(sock):
    sock.sendall(b"$c#63")
    sock.recv(1)
    wait_stop(sock)


def step_once(sock):
    sock.sendall(b"$s#73")
    sock.recv(1)
    wait_stop(sock)


def cont_over_bp(sock, bp_addr):
    packet(sock, f"z0,{bp_addr:x},1")
    step_once(sock)
    packet(sock, f"Z0,{bp_addr:x},1")
    cont(sock)


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


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(30)

    # Step 1: anchor on the size-matching allocation request.
    packet(sock, f"Z0,{SIZE_ANCHOR:x},1")
    print(f"armed size-anchor 0x{SIZE_ANCHOR:08X}, waiting for edi==0xAD000", flush=True)
    sock.settimeout(600)
    found = False
    for i in range(MAX_TRIES):
        if i == 0:
            cont(sock)
        else:
            cont_over_bp(sock, SIZE_ANCHOR)
        r = regs(sock)
        if r["edi"] == 0xAD000:
            found = True
            print(f"size anchor confirmed at try {i}", flush=True)
            break
    packet(sock, f"z0,{SIZE_ANCHOR:x},1")
    if not found:
        print("never saw the 0xAD000 request -- aborting", flush=True)
        return

    # Step 2: scoped-arm the fill-routine entry, take its first hit
    # after the anchor (guaranteed ordering: the anchor breakpoint is
    # now cleared, so nothing else can trigger before this).
    packet(sock, f"Z0,{FILL_ENTRY:x},1")
    print(f"armed fill-entry 0x{FILL_ENTRY:08X}, taking next hit", flush=True)
    cont(sock)
    r = regs(sock)
    base = r["eax"]
    packet(sock, f"z0,{FILL_ENTRY:x},1")
    print(f"fill routine entered with eax (buffer base) = 0x{base:08X}", flush=True)

    if base < 0x10000 or base > 0x0FFFFFFF:
        print("base looks implausible for a heap pointer -- aborting", flush=True)
        return

    # Step 3: arm the write watchpoint at the same relative offset
    # recomp's overflow was proven at, and see if it ever fires.
    watch = base + REL_OFFSET
    before = u32(sock, watch)
    print(f"watch address = base+0x{REL_OFFSET:X} = 0x{watch:08X}, value before = "
          f"{'0x%08X' % before if before is not None else '<unreadable>'}", flush=True)

    packet(sock, f"Z2,{watch:x},4")
    print("armed write watchpoint, continuing (single wait, up to 900s)...", flush=True)

    fired = False
    sock.settimeout(900)
    try:
        cont(sock)
        r = regs(sock)
        val = u32(sock, watch)
        print(f"stop: eip=0x{r['eip']:08X} value="
              f"{'0x%08X' % val if val is not None else '<unreadable>'}", flush=True)
        if val is not None and before is not None and val != before:
            print("VALUE CHANGED -- real hardware DOES write past this offset too", flush=True)
            fired = True
    except socket.timeout:
        print("no stop within 900s", flush=True)

    if not fired:
        print("watchpoint never fired -- real hardware does NOT write past this offset "
              "(within the observed window)", flush=True)

    packet(sock, f"z2,{watch:x},4")
    print("done", flush=True)


if __name__ == "__main__":
    main()
