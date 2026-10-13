"""Check, on real hardware, whether the early sub_00048B15 boot-time load
path ever reaches the fatal error screen (sub_0003FCA5) the way scda_recomp
does, and how often the generic stream-reader (sub_0004438C) gets called
in the same early window.

Arms three breakpoints simultaneously in one fresh boot:
  - 0x00048B15 (level/map load entry, confirmed on real HW at t=1.266s
    by xemu_48b15_entry_vs_device.py)
  - 0x0004438C (generic stream-reader vtable target, hit ~787K times in
    scda_recomp within the first ~40s per Update 179-182 tracing)
  - 0x0003FCA5 (the fatal error screen with no return path, Update 180)

Reports each hit with a timestamp; caps sub_0004438C reporting after the
first 20 hits (just to confirm it's really being exercised) then keeps
running silently, counting further hits, until timeout or a 0x3FCA5 hit
(which would be definitive and stops the run immediately).

Run xemu with ``-s -S`` first, then invoke this script.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time

ENTRY_48B15 = 0x00048B15
READER_4438C = 0x0004438C
FATAL_3FCA5 = 0x0003FCA5


def packet(sock: socket.socket, body: str) -> str:
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


def send_continue(sock: socket.socket) -> None:
    body = b"c"
    sock.sendall(b"$" + body + b"#63")
    if sock.recv(1) != b"+":
        raise RuntimeError("GDB stub did not acknowledge continue")


def wait_stop(sock: socket.socket) -> str:
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


def regs(sock: socket.socket) -> dict[str, int]:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    started = time.monotonic()
    hit_48b15 = 0
    hit_4438c = 0
    hit_fatal = 0
    try:
        for addr in (ENTRY_48B15, READER_4438C, FATAL_3FCA5):
            r = packet(sock, f"Z0,{addr:x},1")
            print(f"arm bp 0x{addr:08X}: {r}", flush=True)

        sock.settimeout(args.timeout)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            send_continue(sock)
            stop = wait_stop(sock)
            t = time.monotonic() - started
            r = regs(sock)
            pc = r["eip"]
            if pc == ENTRY_48B15:
                hit_48b15 += 1
                print(f"[48B15] #{hit_48b15} t={t:.3f}s ecx=0x{r['ecx']:08X}", flush=True)
            elif pc == READER_4438C:
                hit_4438c += 1
                if hit_4438c <= 20 or hit_4438c % 5000 == 0:
                    print(f"[4438C] #{hit_4438c} t={t:.3f}s ecx(this)=0x{r['ecx']:08X}",
                          flush=True)
            elif pc == FATAL_3FCA5:
                hit_fatal += 1
                print(f"[FATAL-3FCA5-HIT] #{hit_fatal} t={t:.3f}s -- REAL HARDWARE REACHED "
                      f"THE FATAL SCREEN. ecx=0x{r['ecx']:08X} eax=0x{r['eax']:08X}",
                      flush=True)
                break
            else:
                print(f"[UNEXPECTED] t={t:.3f}s eip=0x{pc:08X} stop={stop}", flush=True)
            # QEMU's remote stub leaves execution AT the software-breakpoint
            # instruction; a plain continue re-hits the same PC forever.
            # Remove it, single-step past it, then restore it before
            # continuing (established fix, see xemu_scda_5b5800_trace.py).
            if pc in (ENTRY_48B15, READER_4438C, FATAL_3FCA5):
                packet(sock, f"z0,{pc:x},1")
                packet(sock, "s")
                packet(sock, f"Z0,{pc:x},1")
        print(f"SUMMARY: 48B15 hits={hit_48b15} 4438C hits={hit_4438c} "
              f"FATAL-3FCA5 hits={hit_fatal} elapsed={time.monotonic()-started:.1f}s",
              flush=True)
    finally:
        for addr in (ENTRY_48B15, READER_4438C, FATAL_3FCA5):
            try:
                packet(sock, f"z0,{addr:x},1")
            except OSError:
                pass
        try:
            packet(sock, "D")
        except OSError:
            pass
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
