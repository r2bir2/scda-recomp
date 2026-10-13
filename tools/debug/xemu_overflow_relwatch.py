"""2026-09-16 (Update 158): object-identity-correct version of the
overflow write-check. Does NOT reuse a raw address across runs/platforms
(the standing rule for this investigation) -- instead:

1. Break at VA 0x000771F9 (sub_00077194's call forwarding a size into
   NtAllocateVirtualMemory), condition edi==0xAD000, to catch THIS
   run's own live base address for the block-8 fill buffer.
2. Compute the watch address as (this run's own base) + 0x10144 -- the
   SAME relative offset recomp's write-trace showed sub_004BF700's
   fill loop overrunning past on the recomp side (Update 155).
3. Arm a write watchpoint there and continue, to see whether real
   hardware's identical fill loop ever writes past that same relative
   offset.
"""
from __future__ import annotations

import socket
import struct

ENTRY = 0x000771F9
REL_OFFSET = 0x10144  # recomp's overflow write, relative to the buffer base
MAX_TRIES = 200
MAX_CONT_AFTER_ARM = 60


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
    packet(sock, f"Z0,{ENTRY:x},1")
    print(f"armed 0x{ENTRY:08X}, waiting for edi==0xAD000", flush=True)

    sock.settimeout(600)
    base = None
    for i in range(MAX_TRIES):
        if i == 0:
            cont(sock)
        else:
            cont_over_bp(sock, ENTRY)
        r = regs(sock)
        if r["edi"] == 0xAD000:
            base = r["edi"]
            print(f"found target allocation at try {i}: base=0x{base:08X}", flush=True)
            break
    if base is None:
        print("never saw edi==0xAD000 -- aborting", flush=True)
        packet(sock, f"z0,{ENTRY:x},1")
        return

    packet(sock, f"z0,{ENTRY:x},1")
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
