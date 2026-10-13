"""Isolate ONLY the sub_0005FBDA call made from sub_00045E42 (return
address 0x00045E7B) -- sub_0005FBDA is called from multiple places
(0x00044A50 among them per functions.json), and the first attempt
(xemu_renderer_readbyte_id.py) mostly caught unrelated calls, whose
small single-byte values (0x23-0x31, ASCII-range) don't match the
363/795/8610 trip counts continued 41 confirmed for THIS call site.

Breaks at sub_0005FBDA's own entry (0x0005FBDA), reads the return
address off the stack, and only arms the five per-byte icall
breakpoints (and reads the buffer bytes at each) when the return
address is 0x00045E7B. Also reads the final decoded value at the
return site itself (0x00045E7B, eax) for a direct cross-check against
continued 41's already-confirmed 363/795/8610.

Run xemu with ``-s -S`` first, then invoke this script.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time


ENTRY_FBDA = 0x0005FBDA
WANTED_RETURN = 0x00045E7B
BYTE_CALL_SITES = [0x0005FC38, 0x0005FC68, 0x0005FC99, 0x0005FCC6, 0x0005FCE0]
BYTE_RETURN_SITES = [0x0005FC3B, 0x0005FC6B, 0x0005FC9C, 0x0005FCC9, 0x0005FCE3]


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


def read_u8(sock: socket.socket, address: int) -> int | None:
    response = packet(sock, f"m{address:x},1")
    if response.startswith("E") or len(response) != 2:
        return None
    return int(response, 16)


def regs(sock: socket.socket) -> dict[str, int]:
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def bp_set(sock: socket.socket, addr: int) -> None:
    r = packet(sock, f"Z0,{addr:x},1")
    if r != "OK":
        raise RuntimeError(f"arm failed at 0x{addr:08X}: {r}")


def bp_clear(sock: socket.socket, addr: int) -> None:
    try:
        packet(sock, f"z0,{addr:x},1")
    except OSError:
        pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=180.0)
    args = parser.parse_args()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    inner_armed = False
    matched_calls = 0
    pending_buf: dict[int, int] = {}
    try:
        bp_set(sock, ENTRY_FBDA)
        bp_set(sock, WANTED_RETURN)
        sock.settimeout(args.timeout)
        send_continue(sock)
        deadline = time.monotonic() + args.timeout
        hits = 0
        while time.monotonic() < deadline and hits < 2000 and matched_calls < 5:
            stop = wait_stop(sock)
            r = regs(sock)
            pc = r["eip"]
            if pc == ENTRY_FBDA:
                ret_addr = read_u32(sock, r["esp"]) or 0
                if ret_addr == WANTED_RETURN and not inner_armed:
                    print(f"[FBDA-entry] MATCHED call from sub_00045E42 "
                          f"(this/edi arg=0x{read_u32(sock, r['esp']+4) or 0:08X})",
                          flush=True)
                    for a in BYTE_CALL_SITES + BYTE_RETURN_SITES:
                        bp_set(sock, a)
                    inner_armed = True
                # (non-matching calls: fall through, do nothing extra)
                # Every hit here needs the remove-step-restore dance or a
                # plain continue re-hits this same PC forever (QEMU leaves
                # EIP at the breakpoint instruction, not past it).
                bp_clear(sock, ENTRY_FBDA)
                packet(sock, "s")
                bp_set(sock, ENTRY_FBDA)
                send_continue(sock)
                hits += 1
                continue
            elif inner_armed and pc in BYTE_CALL_SITES:
                this_ptr = r["ecx"]
                vtbl = r["eax"]
                target = read_u32(sock, vtbl + 4) or 0
                buf_addr = read_u32(sock, r["esp"]) or 0
                pending_buf[pc] = buf_addr
                print(f"  [call 0x{pc:08X}] this=0x{this_ptr:08X} vtbl=0x{vtbl:08X} "
                      f"target=0x{target:08X} buf_addr=0x{buf_addr:08X}", flush=True)
                bp_clear(sock, pc)
                packet(sock, "s")
                bp_set(sock, pc)
                send_continue(sock)
                hits += 1
                continue
            elif inner_armed and pc in BYTE_RETURN_SITES:
                idx = BYTE_RETURN_SITES.index(pc)
                call_pc = BYTE_CALL_SITES[idx]
                buf_addr = pending_buf.get(call_pc)
                byte_val = read_u8(sock, buf_addr) if buf_addr else None
                print(f"  [return 0x{pc:08X}] byte=0x{byte_val if byte_val is not None else -1:02X} "
                      f"eax(retval)=0x{r['eax']:08X}", flush=True)
                bp_clear(sock, pc)
                packet(sock, "s")
                bp_set(sock, pc)
                send_continue(sock)
                hits += 1
                continue
            elif pc == WANTED_RETURN:
                # eax at this point is "this" (sub_0005FBDA returns the reader
                # object, C++ chaining convention) -- the actual decoded value
                # is in sub_00045E42's own [ebp+8] slot (the out-parameter),
                # which 0x45E87 (a few instructions later) checks as the loop
                # count.
                count = read_u32(sock, r["ebp"] + 8) or 0
                print(f"[sub_00045E42 resumes] eax(this,chained)=0x{r['eax']:08X} "
                      f"[ebp+8](decoded count)=0x{count:08X} (signed={struct.unpack('<i', struct.pack('<I', count))[0]})",
                      flush=True)
                if inner_armed:
                    for a in BYTE_CALL_SITES + BYTE_RETURN_SITES:
                        bp_clear(sock, a)
                    inner_armed = False
                matched_calls += 1
                bp_clear(sock, WANTED_RETURN)
                packet(sock, "s")
                bp_set(sock, WANTED_RETURN)
                send_continue(sock)
                hits += 1
                continue
            else:
                print(f"[unexpected] pc=0x{pc:08X} stop={stop}", flush=True)
            hits += 1
            send_continue(sock)
        print(f"done: {hits} hits, {matched_calls} matched sub_00045E42 calls fully traced",
              flush=True)
    finally:
        for a in [ENTRY_FBDA, WANTED_RETURN] + BYTE_CALL_SITES + BYTE_RETURN_SITES:
            bp_clear(sock, a)
        try:
            packet(sock, "D")
        except OSError:
            pass
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
