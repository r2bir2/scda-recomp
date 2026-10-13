"""Ground-truth capture for the sub_00047150/sub_00045FB0/sub_00045F56
registry-teardown chain (the exact-1-chunk bin[6] investigation).

Watches three addresses:
  - 0x00047150 (sub_00047150 entry): outer registry loop. ecx=registry
    array object. MEM32(ecx)=array base, MEM32(ecx+4)=entry count N.
  - 0x00045FB0 (sub_00045FB0 entry): per-registry-entry hashtable
    teardown wrapper. ecx=hashtable object. MEM32(ecx)=bucket array,
    MEM32(ecx+4)=bucket count.
  - 0x00045F56 (sub_00045F56 entry): the real destroy-N-elements loop.
    ecx=bucket-array base, [esp+4]=start index, [esp+8]=count.

Uses the throttled arm/free-run/re-arm pattern already proven necessary
for this guest address range earlier this session (continuing
immediately after every hit risks getting stuck in a frozen, non-
progressing BIOS-era artifact at these low addresses). Meant to be
started BEFORE the user resumes/advances xemu past the intro gate, so
no further interaction is needed afterward -- runs unattended.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


BREAKPOINTS = {
    0x00047150: "47150-REGISTRY",
    0x00045FB0: "45FB0-HASHTABLE",
    0x00045F56: "45F56-DESTROY-LOOP",
}


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


def read_u32(sock: socket.socket, address: int) -> int | None:
    if address == 0:
        return None
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


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


def arm_all(sock: socket.socket) -> None:
    for addr in BREAKPOINTS:
        packet(sock, f"Z0,{addr:x},1")


def disarm_all(sock: socket.socket) -> None:
    for addr in BREAKPOINTS:
        packet(sock, f"z0,{addr:x},1")


def report_hit(sock: socket.socket, r: dict, t: float, sample: int) -> None:
    pc = r["eip"]
    label = BREAKPOINTS.get(pc, f"UNKNOWN(0x{pc:08X})")
    if pc == 0x00047150:
        ecx = r["ecx"]
        base = read_u32(sock, ecx)
        count = read_u32(sock, ecx + 4)
        print(f"SAMPLE#{sample} t={t:.2f}s [{label}] registry=0x{ecx:08X} "
              f"array_base=0x{base if base is not None else -1:08X} "
              f"entry_count={count}", flush=True)
    elif pc == 0x00045FB0:
        ecx = r["ecx"]
        bucket_array = read_u32(sock, ecx)
        bucket_count = read_u32(sock, ecx + 4)
        print(f"SAMPLE#{sample} t={t:.2f}s [{label}] hashtable=0x{ecx:08X} "
              f"bucket_array=0x{bucket_array if bucket_array is not None else -1:08X} "
              f"bucket_count={bucket_count}", flush=True)
    elif pc == 0x00045F56:
        ecx = r["ecx"]
        start = read_u32(sock, r["esp"] + 4)
        count = read_u32(sock, r["esp"] + 8)
        print(f"SAMPLE#{sample} t={t:.2f}s [{label}] array_base=0x{ecx:08X} "
              f"start={start} count={count}", flush=True)
    else:
        print(f"SAMPLE#{sample} t={t:.2f}s [{label}] ecx=0x{r['ecx']:08X} esp=0x{r['esp']:08X}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=1800.0)
    parser.add_argument("--free-run", type=float, default=0.2)
    parser.add_argument("--arm-timeout", type=float, default=1.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    t0 = time.time()
    sample = 0
    try:
        while time.time() - t0 < args.duration:
            halt_and_drain(sock)
            arm_all(sock)
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
                report_hit(sock, r, t, sample)
            halt_and_drain(sock)
            disarm_all(sock)
            sock.sendall(b"$c#63")
            sock.recv(4)
            time.sleep(args.free_run)
        print(f"duration exhausted ({sample} samples total)", flush=True)
    finally:
        try:
            halt_and_drain(sock)
            disarm_all(sock)
            sock.sendall(b"$c#63")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
