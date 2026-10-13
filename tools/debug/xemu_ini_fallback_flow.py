"""Capture the original-Xbox control flow after the verified optional INI opens.

The recomp probe established the three exact return PCs of the default/T:/
fallback attempts.  These addresses belong to sub_00040FAC.  The final
fallback PC is shared by unrelated callers, so this script first filters for
the real sub_0004721E virtual-dispatch caller (return 0x47456), then records
only that invocation's immediate outer branch/return path.
"""
from __future__ import annotations

import socket
import struct
import time

POINTS = (0x0004108A, 0x0004119F, 0x000412B4, 0x000412E3, 0x000412F4)
OUTER_POINTS = (0x00047456, 0x00047458, 0x0004749A, 0x000474AC,
                0x000474AE, 0x000474FC)


def packet(sock: socket.socket, body: str) -> str:
    raw = body.encode("ascii")
    sock.sendall(b"$" + raw + b"#" + f"{sum(raw) & 0xff:02x}".encode("ascii"))
    first = sock.recv(1)
    if first == b"+": first = sock.recv(1)
    while first != b"$": first = sock.recv(1)
    result = bytearray()
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2); sock.sendall(b"+")
            return result.decode("ascii")
        result.extend(char)


def wait_stop(sock: socket.socket) -> None:
    first = sock.recv(1)
    while first != b"$": first = sock.recv(1)
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2); sock.sendall(b"+"); return


def regs(sock: socket.socket) -> dict[str, int]:
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def u32(sock: socket.socket, address: int) -> int | None:
    response = packet(sock, f"m{address:x},4")
    return None if response.startswith("E") else struct.unpack("<I", bytes.fromhex(response))[0]


def stack_words(sock: socket.socket, esp: int, count: int = 5) -> str:
    values = [u32(sock, (esp + index * 4) & 0xffffffff) for index in range(count)]
    return "[" + ", ".join("%08X" % value if value is not None else "????????"
                           for value in values) + "]"


def step_past_breakpoint(sock: socket.socket, point: int) -> None:
    """Avoid immediately re-hitting the same software breakpoint."""
    packet(sock, f"z0,{point:x},1")
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(10)
    wait_stop(sock)
    packet(sock, f"Z0,{point:x},1")


def main() -> None:
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    for point in POINTS: packet(sock, f"Z0,{point:x},1")
    print("armed " + ", ".join(f"0x{x:08X}" for x in POINTS), flush=True)
    start = time.time()
    outer_active = False
    for hit in range(60):
        try:
            sock.sendall(b"$c#63"); sock.recv(1); sock.settimeout(90); wait_stop(sock)
        except socket.timeout:
            print("timeout", flush=True); break
        r = regs(sock)
        local_flags = u32(sock, (r["ebp"] - 4) & 0xffffffff)
        caller_return = u32(sock, (r["ebp"] + 4) & 0xffffffff)
        if not outer_active:
            print(
                f"INI#{hit+1} t={time.time()-start:.1f}s pc=0x{r['eip']:08X} "
                f"eax=0x{r['eax']:08X} ecx=0x{r['ecx']:08X} edx=0x{r['edx']:08X} "
                f"ebx=0x{r['ebx']:08X} esi=0x{r['esi']:08X} edi=0x{r['edi']:08X} "
                f"esp=0x{r['esp']:08X} ebp=0x{r['ebp']:08X} "
                f"caller_return={('0x%08X' % caller_return) if caller_return is not None else 'NA'} "
                f"flags_local={('0x%08X' % local_flags) if local_flags is not None else 'NA'} "
                f"stack={stack_words(sock, r['esp'])}", flush=True)
            if r["eip"] == 0x000412F4 and caller_return == 0x00047456:
                outer_active = True
                for point in OUTER_POINTS:
                    packet(sock, f"Z0,{point:x},1")
                print("SYNC: final optional-INI fallback is returning to 0x00047456; "
                      "outer sub_0004721E trace armed", flush=True)
        else:
            slot_5b5800 = u32(sock, 0x005B5800)
            slot_5b57fc = u32(sock, 0x005B57FC)
            print(
                f"OUTER#{hit+1} t={time.time()-start:.1f}s pc=0x{r['eip']:08X} "
                f"eax=0x{r['eax']:08X} ecx=0x{r['ecx']:08X} edx=0x{r['edx']:08X} "
                f"ebx=0x{r['ebx']:08X} esi=0x{r['esi']:08X} edi=0x{r['edi']:08X} "
                f"esp=0x{r['esp']:08X} ebp=0x{r['ebp']:08X} eflags=0x{r['eflags']:08X} "
                f"slot5B5800={('0x%08X' % slot_5b5800) if slot_5b5800 is not None else 'NA'} "
                f"slot5B57FC={('0x%08X' % slot_5b57fc) if slot_5b57fc is not None else 'NA'} "
                f"stack={stack_words(sock, r['esp'])}", flush=True)
            if r["eip"] == 0x000474FC:
                print("DONE: traced the synchronized sub_0004721E return", flush=True)
                break
        step_past_breakpoint(sock, r["eip"])
    for point in POINTS + OUTER_POINTS: packet(sock, f"z0,{point:x},1")


if __name__ == "__main__": main()
