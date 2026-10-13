"""Ground-truth check, take 2: watch the two LMaps format-string constants
directly instead of guessing an intermediate helper's calling convention.

Static disassembly of the retail XBE confirmed two format strings:
  0x4F758C  "LMaps\\%s\\common.lin"      (named map/level)
  0x4F75B4  "LMaps" + "\\common.lin"     (flat concatenation, no subfolder)

xemu_commonlin_caller_trace.py guessed sub_0003EDCC's own entry argument was
a name pointer and got zero matches in 141,703 hits -- the assumption about
its calling convention was wrong. Rather than guess again, put an access
(read-or-write) watchpoint directly on the first few bytes of each format
string. Whichever code actually uses a string as a sprintf/wcscpy source has
to read these exact bytes, so this is calling-convention-agnostic: it will
catch the real caller no matter how it passes its arguments.
"""
from __future__ import annotations

import socket
import struct
import time

TARGETS = {
    0x004F758C: "LMaps_pct_s_common (named-map form)",
    0x004F75B4: "LMaps_flat (no-map/menu fallback form)",
}


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


def send_continue(sock):
    sock.sendall(b"$c#63")
    sock.recv(1)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    for addr, label in TARGETS.items():
        r = packet(sock, f"Z4,{addr:x},4")
        print(f"arm access-watchpoint on 0x{addr:08X} ({label}): {r}", flush=True)

    t0 = time.time()
    hits = 0
    continue_pending = False
    while time.time() - t0 < 1200 and hits < 30:
        if not continue_pending:
            send_continue(sock)
            continue_pending = True
        try:
            sock.settimeout(90)
            wait_stop(sock)
            continue_pending = False
            regs = get_regs(sock)
        except socket.timeout:
            # The target is still running -- do NOT send another 'c' on top
            # of the pending one (that desyncs the protocol). Just keep
            # waiting on the same still-open continue.
            print(f"no hit in the last 90s (had {hits} so far, "
                  f"{time.time()-t0:.0f}s elapsed) -- still waiting", flush=True)
            continue
        hits += 1
        t = time.time() - t0
        # Report a stack window too, since the watchpoint stop's EIP is
        # inside whichever instruction touched the byte -- often close to
        # the calling context already (e.g. a rep movs / lods loop body).
        print(
            f"HIT#{hits} t={t:.2f}s eip=0x{regs['eip']:08X} "
            f"eax=0x{regs['eax']:08X} ecx=0x{regs['ecx']:08X} "
            f"edx=0x{regs['edx']:08X} ebx=0x{regs['ebx']:08X} "
            f"esi=0x{regs['esi']:08X} edi=0x{regs['edi']:08X} "
            f"esp=0x{regs['esp']:08X}",
            flush=True,
        )
        ret_guess = read_u32(sock, regs["esp"])
        print(f"  [esp+0] (possible return addr) = 0x{ret_guess if ret_guess else 0:08X}", flush=True)

    try:
        if continue_pending:
            # Interrupt the still-running target before trying to send any
            # more packets -- a 'z' removal request while a 'c' is pending
            # is exactly the unclean-disconnect shape that leaves xemu's
            # gdbstub refusing new connections until it's relaunched.
            sock.sendall(b"\x03")
            sock.settimeout(10)
            wait_stop(sock)
        for addr in TARGETS:
            packet(sock, f"z4,{addr:x},4")
    except socket.timeout:
        print("cleanup timed out (target may still be running) -- "
              "not fatal, just means the watchpoints were left armed",
              flush=True)
    print(f"done hits={hits}", flush=True)


if __name__ == "__main__":
    main()
