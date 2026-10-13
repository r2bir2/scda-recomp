"""Cold-boot xemu register samples across sub_00071B49's pre-INI calls."""
from __future__ import annotations

import socket
import struct
import time

POINTS = (0x00071B8C, 0x00071B97, 0x00071BA6, 0x00071BD7,
          0x00071BDC, 0x0003EDBF, 0x0003EDC8, 0x00077B38,
          0x000767ED, 0x0007680D, 0x00076814, 0x00077B3D,
          0x00077B44, 0x000774E6, 0x000778B9, 0x00077B4E,
          0x00077B5D, 0x00071BE8, 0x00071BEF, 0x00071BFE,
          0x00071C09, 0x00071C3B, 0x00071C56, 0x00071C65)


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
    result = packet(sock, f"m{address:x},4")
    return None if result.startswith("E") else struct.unpack("<I", bytes.fromhex(result))[0]


def step_past_breakpoint(sock: socket.socket, point: int) -> None:
    packet(sock, f"z0,{point:x},1")
    sock.sendall(b"$s#73"); sock.recv(1)
    sock.settimeout(10); wait_stop(sock)
    packet(sock, f"Z0,{point:x},1")


def main() -> None:
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    for point in POINTS: packet(sock, f"Z0,{point:x},1")
    start = time.time()
    for count in range(40):
        sock.sendall(b"$c#63"); sock.recv(1); sock.settimeout(90); wait_stop(sock)
        r = regs(sock); pc = r["eip"]
        status = u32(sock, 0x005ED140)
        target = u32(sock, r["eax"]) if pc in (0x00071BDC, 0x00071C65) else None
        print(f"HIT#{count+1} t={time.time()-start:.1f}s pc=0x{pc:08X} "
              f"eax={r['eax']:08X} ecx={r['ecx']:08X} edx={r['edx']:08X} "
              f"ebx={r['ebx']:08X} esi={r['esi']:08X} edi={r['edi']:08X} "
              f"esp={r['esp']:08X} ebp={r['ebp']:08X} status140="
              f"{status:08X} target={target:08X}" if status is not None and target is not None
              else f"{status:08X}" if status is not None else "NA", flush=True)
        step_past_breakpoint(sock, pc)
        if pc == 0x00071C65:
            break
    for point in POINTS: packet(sock, f"z0,{point:x},1")


if __name__ == "__main__": main()
