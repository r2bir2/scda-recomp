"""Ground-truth check: does real hardware also ask for LMaps\\menu\\common.lin?

scda_recomp fails to open DataXb\\LMaps\\menu\\common.lin under every device/
path mapping tried, falls through to a genuine disc-error trap in
sub_0003FCA5, and never appears to retry the plain DataXb\\LMaps\\common.lin
path the user confirms is where this file actually lives on the retail disc.

The retail XBE (verified via static disassembly) builds this path from one of
two format strings: "LMaps\\%s\\common.lin" (a named map/level) or a flat
"LMaps" + "\\common.lin" concatenation (no subfolder, the no-map/menu case).
Something decides which one to use. sub_0003EDCC (0x3EDCC) is a generic
"open resource by name" helper on the path that led to every one of
scda_recomp's own common.lin lookups (confirmed via a kernel-bridge caller
trace this session) -- its own first argument (at [esp+4] on entry) is a
pointer to the requested name/path string. Breaking there and filtering by
substring answers directly: does real hardware ever pass "menu\\common.lin"
through this same helper, and if so, does a later call also pass the plain
"LMaps\\common.lin" (proving a normal probe-then-fallback), or never (proving
our recompiled code invents the "menu" segment where retail code would not)?
"""
from __future__ import annotations

import socket
import struct
import time

ENTRY = 0x0003EDCC


def packet(sock: socket.socket, body: str) -> str:
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


def wait_stop(sock: socket.socket) -> None:
    first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return


def continue_to_stop(sock: socket.socket, timeout: float) -> None:
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)


def step_once(sock: socket.socket, timeout: float) -> None:
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)


def regs(sock: socket.socket) -> dict[str, int]:
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def mem_u32(sock: socket.socket, address: int) -> int:
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) < 8:
        return 0
    return struct.unpack("<I", bytes.fromhex(response))[0]


def cstring(sock: socket.socket, address: int, maxlen: int = 200) -> bytes:
    if address == 0:
        return b""
    response = packet(sock, f"m{address:x},{maxlen:x}")
    raw = b"" if response.startswith("E") else bytes.fromhex(response)
    return raw.split(b"\0", 1)[0]


def main() -> None:
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{ENTRY:x},1")
    print(f"armed sub_0003EDCC entry 0x{ENTRY:08X}", flush=True)
    start = time.time()
    hits = 0
    logged = 0
    seen_menu = False
    seen_plain = False
    while time.time() - start < 600 and hits < 200000:
        try:
            continue_to_stop(sock, min(60, 601 - (time.time() - start)))
        except socket.timeout:
            print("timeout waiting for a hit", flush=True)
            break
        r = regs(sock)
        hits += 1
        # entry: [esp] = return addr, [esp+4] = arg0 (name/path pointer)
        arg0_ptr = mem_u32(sock, r["esp"] + 4)
        name = cstring(sock, arg0_ptr)
        low = name.lower()
        if b"common.lin" in low:
            logged += 1
            is_menu = b"menu" in low
            if is_menu:
                seen_menu = True
            else:
                seen_plain = True
            print(
                f"HIT #{logged} t={time.time()-start:.1f}s hits={hits} "
                f"name={name.decode('latin1','replace')!r} "
                f"caller=0x{mem_u32(sock, r['esp']):08X} esp=0x{r['esp']:08X} "
                f"eax=0x{r['eax']:08X} ecx=0x{r['ecx']:08X} edx=0x{r['edx']:08X} "
                f"ebx=0x{r['ebx']:08X} esi=0x{r['esi']:08X} edi=0x{r['edi']:08X}",
                flush=True,
            )
            if logged >= 40:
                break
        # Remove/step/re-arm: EIP sits on the breakpoint instruction after a
        # stop, and a plain continue would re-trap the same PC forever.
        packet(sock, f"z0,{ENTRY:x},1")
        step_once(sock, 10)
        packet(sock, f"Z0,{ENTRY:x},1")
    packet(sock, f"z0,{ENTRY:x},1")
    print(
        f"done hits={hits} common.lin_hits={logged} "
        f"saw_menu_variant={seen_menu} saw_plain_variant={seen_plain}",
        flush=True,
    )


if __name__ == "__main__":
    main()
