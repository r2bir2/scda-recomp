"""Ground-truth check: what does real hardware's kernel return for
ExQueryNonVolatileSetting(ValueIndex=0x103, ...), the call at the very
start of the sub_001CC602/sub_00046172/"True"-comparison chain that
ultimately (on scda_recomp) triggers the unnecessary registry teardown?

Arms a breakpoint at guest PC 0x00371D02 -- the instruction right after
sub_00371CE7's call into the kernel thunk (sub_00059F00 -> ordinal 24)
returns, where eax = the raw NTSTATUS from ExQueryNonVolatileSetting.
Reads eax (status) and the output buffer that was passed (readable via
esp-relative offsets is fragile across the call boundary; instead we
read the two known-fixed local slots, ebp-4/ebp-8, using the same
ebp-reconstruction trick as the earlier sub_001D589D/sub_001D5A5E
check: ebp = entry_esp - 4 - <frame size>). Simpler and robust here:
just read eax (the STATUS) and, one instruction later at 0x00371D06,
eax again holds the actual return value of sub_00371CE7 itself
(ZX8(MEM8(ebp-3))) -- arm both PCs.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


TARGET_PC = 0x00371D02  # right after the kernel call returns: eax = NTSTATUS
TARGET_PC2 = 0x00371D06  # sub_00371CE7's own return value path: eax = ZX8(MEM8(ebp-3))


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


def get_regs(sock: socket.socket) -> dict:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def halt_and_drain(sock: socket.socket) -> None:
    sock.sendall(b"\x03")
    time.sleep(0.3)
    try:
        wait_stop(sock)
    except socket.timeout:
        pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=600.0)
    parser.add_argument("--free-run", type=float, default=0.2)
    parser.add_argument("--arm-timeout", type=float, default=1.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    t0 = time.time()
    sample = 0
    seen_targets = set()
    try:
        while time.time() - t0 < args.duration:
            halt_and_drain(sock)
            packet(sock, f"Z0,{TARGET_PC:x},1")
            packet(sock, f"Z0,{TARGET_PC2:x},1")
            sock.sendall(b"$c#63")
            sock.recv(4)
            sock.settimeout(args.arm_timeout)
            hit = False
            try:
                wait_stop(sock)
                hit = True
            except socket.timeout:
                pass
            sock.settimeout(10)
            if hit:
                sample += 1
                r = get_regs(sock)
                t = time.time() - t0
                pc = r["eip"]
                label = "STATUS" if pc == TARGET_PC else ("RETVAL" if pc == TARGET_PC2 else f"OTHER(0x{pc:08X})")
                print(f"SAMPLE#{sample} t={t:.2f}s [{label}] pc=0x{pc:08X} eax=0x{r['eax']:08X} "
                      f"ecx=0x{r['ecx']:08X} esp=0x{r['esp']:08X}", flush=True)
                seen_targets.add(pc)
                if TARGET_PC in seen_targets and TARGET_PC2 in seen_targets:
                    print("both checkpoints observed at least once -- stopping", flush=True)
                    break
            halt_and_drain(sock)
            packet(sock, f"z0,{TARGET_PC:x},1")
            packet(sock, f"z0,{TARGET_PC2:x},1")
            sock.sendall(b"$c#63")
            sock.recv(4)
            time.sleep(args.free_run)
        else:
            print(f"duration exhausted ({sample} samples total)", flush=True)
    finally:
        try:
            halt_and_drain(sock)
            packet(sock, f"z0,{TARGET_PC:x},1")
            packet(sock, f"z0,{TARGET_PC2:x},1")
            sock.sendall(b"$c#63")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
