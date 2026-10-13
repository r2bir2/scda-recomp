"""Capture bin[6] (48-byte smallbin) allocator state around the
sub_001E460F vector-destruction sequence, via xemu's GDB stub.

Run xemu with ``-s -S`` first (paused at boot), then invoke this script;
it resumes execution itself. Read-only: only places temporary software
breakpoints and reads guest registers/memory, all removed before exit.

At every hit of sub_001E460F (0x001E460F, the vector-free helper) it
records: the vector object (ecx), its data/size/capacity fields, and a
full walk of bin[6]'s free chain (head at guest 0x0064D29C + 6*4 =
0x0064D2B4, following the +8 "next" link) up to a safety cap, flagging
any link that does not look like a plausible heap pointer.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


BIN6_ADDR = 0x0064D2B4
CHAIN_CAP = 40
# Xbox user-mode code+data range (docs/technical/xemu-debugging.md); a
# bin[6] link outside a much narrower "looks like a real chunk" heuristic
# (below the loaded image's low code pages) is almost certainly corrupt.
HEAP_FLOOR = 0x00400000
HEAP_CEIL = 0x03FFFFFF


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


def read_u32(sock: socket.socket, address: int) -> int | None:
    if address == 0:
        return None
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def regs(sock: socket.socket) -> dict[str, int]:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def walk_bin6(sock: socket.socket) -> tuple[list[int], str]:
    """Walk bin[6]'s free chain. Returns (chain, status) where status is
    'empty', 'ok', 'cycle', 'unreadable', or 'invalid-range'."""
    chain: list[int] = []
    head = read_u32(sock, BIN6_ADDR)
    if head is None:
        return chain, "unreadable-head"
    if head == 0:
        return chain, "empty"
    cur = head
    seen = set()
    for _ in range(CHAIN_CAP):
        if cur in seen:
            chain.append(cur)
            return chain, "cycle"
        seen.add(cur)
        chain.append(cur)
        if not (HEAP_FLOOR <= cur <= HEAP_CEIL):
            return chain, "invalid-range"
        nxt = read_u32(sock, cur + 8)
        if nxt is None:
            return chain, "unreadable-link"
        if nxt == 0:
            return chain, "ok"
        cur = nxt
    return chain, "cap-reached"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--max-hits", type=int, default=400)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    bp_addr = 0x001E460F
    try:
        # Halt before touching breakpoints -- the GDB stub times out if the
        # guest is still running (docs/technical/xemu-debugging.md).
        sock.sendall(b"\x03")
        time.sleep(0.3)
        try:
            wait_stop(sock)
        except socket.timeout:
            pass
        result = packet(sock, f"Z0,{bp_addr:x},1")
        if result != "OK":
            raise RuntimeError(f"could not set breakpoint: {result}")
        sock.settimeout(args.timeout)
        send_continue(sock)
        deadline = time.monotonic() + args.timeout
        hits = 0
        while time.monotonic() < deadline and hits < args.max_hits:
            stop = wait_stop(sock)
            r = regs(sock)
            pc = r["eip"]
            if pc != bp_addr:
                print(f"[XEMU-BIN6] unexpected stop pc={pc:08X} stop={stop}")
                break
            vector = r["ecx"]
            data = read_u32(sock, vector)
            size = read_u32(sock, vector + 4)
            capacity = read_u32(sock, vector + 8)
            byte_count_arg = read_u32(sock, r["esp"] + 8)
            ret_addr = read_u32(sock, r["esp"])
            chain, status = walk_bin6(sock)
            hits += 1
            print(
                f"[XEMU-BIN6] #{hits} pc={pc:08X} vector={vector:08X} "
                f"data={data if data is not None else -1:08X} size={size} "
                f"capacity={capacity} arg8={byte_count_arg} "
                f"ret={ret_addr if ret_addr is not None else -1:08X} "
                f"bin6_len={len(chain)} bin6_status={status} "
                f"bin6_head={(chain[0] if chain else 0):08X} "
                f"bin6_chain={','.join(f'{c:08X}' for c in chain[:12])}"
            )
            # QEMU's remote stub leaves execution at the breakpoint
            # instruction; step once with it removed, then restore it.
            packet(sock, f"z0,{pc:x},1")
            packet(sock, "s")
            packet(sock, f"Z0,{pc:x},1")
            send_continue(sock)
    finally:
        try:
            sock.sendall(b"\x03")
            time.sleep(0.3)
            try:
                wait_stop(sock)
            except socket.timeout:
                pass
            packet(sock, f"z0,{bp_addr:x},1")
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
