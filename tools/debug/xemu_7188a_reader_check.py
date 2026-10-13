"""Ground-truth check for continued 59's open question: does real hardware
ever call sub_0007188A (the generic UTF-16 stream-length-based buffer
allocator scda_recomp reaches with Partition5's raw device handle) at all,
and if so, on what object/length -- to tell apart the two candidate fixes:

  (1) xbox_NtQueryInformationFile answers a device-backed handle's
      FileStandardInformation query differently than real hardware would
      (the query itself is the bug), or
  (2) something upstream in scda_recomp decides to invoke this reader on
      Partition5 at all, a decision real hardware's guest code never makes
      here (an earlier state/logic difference is the bug, and the query
      answer is a correct red herring).

Breaks at sub_0007188A's own entry (0x0007188A) and at the "get length"
call site (0x000718B4, `call dword ptr [eax+0x50]`) to log the resulting
length (edi) and the reader object's own vtable pointer, so an obviously-
huge length (matching Partition5's ~750MB) versus an ordinary small one
(a real config/INI file) tells the two hypotheses apart directly.
"""
from __future__ import annotations

import socket
import struct
import time

ENTRY_FUNC = 0x0007188A
LENGTH_CALLSITE = 0x000718B7  # `mov edi, eax` right after `call [eax+0x50]` -- eax already holds the length result here


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


def send_continue(sock):
    sock.sendall(b"$c#63")
    sock.recv(1)


def step_once(sock, timeout):
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)


def interrupt_and_resync(sock, timeout=10):
    sock.sendall(b"\x03")
    sock.settimeout(timeout)
    wait_stop(sock)


def regs(sock):
    raw = bytes.fromhex(packet(sock, "g"))
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", raw, i * 4)[0] for i, name in enumerate(names)}


def mem_u32(sock, address):
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) < 8:
        return 0
    return struct.unpack("<I", bytes.fromhex(response))[0]


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{ENTRY_FUNC:x},1")
    packet(sock, f"Z0,{LENGTH_CALLSITE:x},1")
    print(f"armed sub_0007188A entry 0x{ENTRY_FUNC:08X} and length-callsite "
          f"0x{LENGTH_CALLSITE:08X}", flush=True)
    start = time.time()
    hits = 0
    func_hits = 0
    len_hits = 0
    continue_pending = False

    while time.time() - start < 900 and hits < 100:
        if not continue_pending:
            send_continue(sock)
            continue_pending = True
        try:
            sock.settimeout(min(60, max(1, 901 - (time.time() - start))))
            wait_stop(sock)
            continue_pending = False
        except socket.timeout:
            print(f"no hit in the last interval (t={time.time()-start:.1f}s, "
                  f"func_hits={func_hits} len_hits={len_hits})", flush=True)
            continue

        r = regs(sock)
        hits += 1
        eip = r["eip"]
        if eip == ENTRY_FUNC:
            func_hits += 1
            reader_obj_ptr = mem_u32(sock, r["esp"] + 0x10)
            reader_vtable = mem_u32(sock, reader_obj_ptr) if reader_obj_ptr else 0
            print(
                f"HIT #{hits} [FUNC-ENTRY] t={time.time()-start:.1f}s "
                f"ret=0x{mem_u32(sock, r['esp']):08X} "
                f"reader_obj=0x{reader_obj_ptr:08X} "
                f"reader_vtable=0x{reader_vtable:08X}",
                flush=True,
            )
        else:
            len_hits += 1
            print(
                f"HIT #{hits} [LENGTH-CALLSITE] t={time.time()-start:.1f}s "
                f"length=0x{r['eax']:08X} ({r['eax']}) esi(reader)=0x{r['esi']:08X}",
                flush=True,
            )
        packet(sock, f"z0,{eip:x},1")
        step_once(sock, 10)
        packet(sock, f"Z0,{eip:x},1")

    try:
        if continue_pending:
            interrupt_and_resync(sock)
        packet(sock, f"z0,{ENTRY_FUNC:x},1")
        packet(sock, f"z0,{LENGTH_CALLSITE:x},1")
    except socket.timeout:
        print("cleanup timed out (target may still be running)", flush=True)

    print(f"done total_hits={hits} func_entry_hits={func_hits} "
          f"len_callsite_hits={len_hits}", flush=True)


if __name__ == "__main__":
    main()
