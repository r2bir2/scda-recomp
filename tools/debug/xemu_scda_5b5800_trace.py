"""Capture one SCDA producer-to-destructor transaction through xemu GDB.

Run xemu with ``-s -S`` first, then invoke this script.  It is deliberately
read-only: it only places temporary software breakpoints and reads guest
registers/memory.  All breakpoints are removed before detaching.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


POINTS = {
    0x00047488: "payload-before-cleanup",
    0x000464EC: "cleanup-entry",
    0x00046504: "cleanup-after-first-call",
    0x00046549: "cleanup-before-7188A",
    0x00046558: "cleanup-after-7188A",
    0x00046877: "cleanup-before-36116",
    0x0004687F: "cleanup-before-saved-pops",
    0x000474FC: "lookup-return",
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


def send_continue(sock: socket.socket) -> None:
    """Send ``c`` without waiting; its reply is the later stop packet."""
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


def read_u32(sock: socket.socket, address: int) -> int | None:
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def regs(sock: socket.socket) -> dict[str, int]:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def object_words(sock: socket.socket, address: int, count: int = 8) -> str:
    # Xbox kernel mappings (e.g. D0000000 stack/RAM aliases) are also
    # valid guest addresses in xemu, so let the GDB stub decide readability.
    if not address:
        return "<null>"
    response = packet(sock, f"m{address:x},{count * 4:x}")
    if response.startswith("E") or len(response) != count * 8:
        return "<unreadable>"
    values = struct.unpack("<" + "I" * count, bytes.fromhex(response))
    return " ".join(f"{value:08X}" for value in values)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    active = list(POINTS)
    try:
        for address in active:
            result = packet(sock, f"Z0,{address:x},1")
            if result != "OK":
                raise RuntimeError(f"could not set {address:08X}: {result}")
        sock.settimeout(args.timeout)
        send_continue(sock)
        deadline = time.monotonic() + args.timeout
        hits = 0
        while time.monotonic() < deadline:
            # The reply to continue is the later stop packet.
            stop = wait_stop(sock)
            r = regs(sock)
            pc = r["eip"]
            name = POINTS.get(pc, "unexpected-stop")
            slot = read_u32(sock, 0x005B5800) or 0
            print(
                f"[XEMU-5B5800] point={name} stop={stop} "
                + " ".join(f"{key}={r[key]:08X}" for key in ("eip", "eax", "ecx", "edx", "ebx", "esi", "edi", "esp", "ebp"))
                + f" slot={slot:08X} slot_words={object_words(sock, slot)}"
                + f" esp_words={object_words(sock, r['esp'])}"
                + f" eax_words={object_words(sock, r['eax'])}"
                + f" ecx_words={object_words(sock, r['ecx'])}"
            )
            hits += 1
            if pc == 0x000474FC or hits >= 80:
                break
            # QEMU's remote stub leaves execution at the software-breakpoint
            # instruction.  Step once with it removed, then restore it;
            # otherwise a plain continue re-hits the same PC forever.
            if pc in POINTS:
                packet(sock, f"z0,{pc:x},1")
                packet(sock, "s")
                packet(sock, f"Z0,{pc:x},1")
            send_continue(sock)
    finally:
        for address in active:
            try:
                packet(sock, f"z0,{address:x},1")
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
