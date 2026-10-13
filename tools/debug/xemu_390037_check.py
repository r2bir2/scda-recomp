"""Ground-truth check for the Update 230 investigation: does real hardware
(via xemu) ever see a value other than the static image constant 0x81660000
at guest [0x00390037+0x38] by the time sub_00062237's tree-walk reaches that
exact node (ebx==0x00390037) and is about to call sub_00060B2E(ebx, cursor)?

Breakpoint at 0x000622D1 (the "push ebx" right before the call to
sub_00060B2E, inside sub_00062237) is guest-address-identical on both
platforms since it is the same XBE. Loops past every hit whose ebx does not
match the specific node identified in the recomp run (0x00390037).
"""
from __future__ import annotations

import socket
import struct

BP = 0x000622D1
TARGET_EBX = 0x00390037


def packet(sock, body):
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


def wait_stop(sock):
    first = sock.recv(1)
    while first != b"$":
        first = sock.recv(1)
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return


def cont(sock):
    sock.sendall(b"$c#63")
    sock.recv(1)
    wait_stop(sock)


def cont_over_bp(sock, bp_addr):
    packet(sock, f"z0,{bp_addr:x},1")
    sock.sendall(b"$s#73")
    sock.recv(1)
    wait_stop(sock)
    packet(sock, f"Z0,{bp_addr:x},1")
    cont(sock)


def regs(sock):
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def u32(sock, address):
    address &= 0xFFFFFFFF
    result = packet(sock, f"m{address:x},4")
    if result.startswith("E") or len(result) < 8:
        return None
    return struct.unpack("<I", bytes.fromhex(result))[0]


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=30)
    sock.settimeout(30)
    packet(sock, f"Z0,{BP:x},1")
    print(f"armed 0x{BP:08X}, filtering ebx==0x{TARGET_EBX:08X}", flush=True)

    sock.settimeout(900)
    found = False
    seen_ebx = {}
    for attempt in range(20000):
        if attempt == 0:
            cont(sock)
        else:
            cont_over_bp(sock, BP)
        r = regs(sock)
        ebx = r["ebx"]
        seen_ebx[ebx] = seen_ebx.get(ebx, 0) + 1
        field38 = u32(sock, ebx + 0x38)
        field38_s = f"0x{field38:08X}" if field38 is not None else "(unreadable)"
        print(f"  hit {attempt}: ebx=0x{ebx:08X} [ebx+0x38]={field38_s}", flush=True)
        if ebx == TARGET_EBX:
            edi = r["edi"]
            esp = r["esp"]
            print(f"MATCH at attempt {attempt}: ebx=0x{ebx:08X} [ebx+0x38]={field38_s} "
                  f"edi(orig this)=0x{edi:08X} esp=0x{esp:08X} eip=0x{r['eip']:08X}",
                  flush=True)
            found = True
            break

    packet(sock, f"z0,{BP:x},1")
    if not found:
        print(f"never matched ebx==0x{TARGET_EBX:08X} after all attempts", flush=True)
        print("distinct ebx values seen (top 20 by count):", flush=True)
        for val, cnt in sorted(seen_ebx.items(), key=lambda kv: -kv[1])[:20]:
            print(f"  0x{val:08X}: {cnt}", flush=True)
        return

    print("done", flush=True)


if __name__ == "__main__":
    main()
