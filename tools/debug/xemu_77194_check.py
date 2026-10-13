"""Ground-truth check: does real hardware's sub_00077194 (the function whose
own call to sub_0049CE00 at guest 0x000774CF is scda_recomp's "second"
sub_004A12B0 call path) ever reach that call at all, or does some condition
it evaluates differently make it skip that call on real hardware?

Breaks at sub_00077194's own entry to see how often/when it runs, then at
the call site itself (0x000774CF) to see if real hardware ever executes it.

2026-09-06 fix: the first version of this script called continue_to_stop()
(send 'c', block for a stop) as one unit and, on a timeout, went straight
to sending 'z' (breakpoint-clear) packets -- with the target still mid-'c',
that is exactly this project's known "unclean disconnect" shape that can
leave xemu's gdbstub refusing new connections until it's relaunched (see
xemu_lmaps_fmt_watch.py's own note on this). Fixed by tracking whether a
'c' is still outstanding and, if so, sending an interrupt (\\x03) and
waiting for the resulting stop *before* touching breakpoints again, on
every timeout path (the per-iteration one and the final cleanup).
"""
from __future__ import annotations

import socket
import struct
import time

ENTRY_FUNC = 0x00077194
ENTRY_CALLSITE = 0x000774CF


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
    """Stop a still-running (mid-'c') target cleanly before sending anything
    else. Must be called whenever a continue's stop-reply was never
    consumed -- a timeout on wait_stop(), or the loop simply ending while a
    continue is outstanding."""
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
    packet(sock, f"Z0,{ENTRY_CALLSITE:x},1")
    print(f"armed sub_00077194 entry 0x{ENTRY_FUNC:08X} and call site "
          f"0x{ENTRY_CALLSITE:08X}", flush=True)
    start = time.time()
    hits = 0
    func_hits = 0
    callsite_hits = 0
    continue_pending = False
    timed_out = False

    while time.time() - start < 600 and hits < 200:
        if not continue_pending:
            send_continue(sock)
            continue_pending = True
        try:
            sock.settimeout(min(60, max(1, 601 - (time.time() - start))))
            wait_stop(sock)
            continue_pending = False
        except socket.timeout:
            print(f"no hit in the last interval (t={time.time()-start:.1f}s, "
                  f"func_hits={func_hits} callsite_hits={callsite_hits}) -- "
                  "still waiting on the same outstanding continue",
                  flush=True)
            continue

        r = regs(sock)
        hits += 1
        eip = r["eip"]
        which = "FUNC-ENTRY" if eip == ENTRY_FUNC else "CALLSITE"
        if which == "FUNC-ENTRY":
            func_hits += 1
        else:
            callsite_hits += 1
        ret_addr = mem_u32(sock, r["esp"])
        device_ptr = mem_u32(sock, 0x4AB668)
        if hits <= 10 or which == "CALLSITE":
            print(
                f"HIT #{hits} [{which}] t={time.time()-start:.1f}s "
                f"ret=0x{ret_addr:08X} ecx(this)=0x{r['ecx']:08X} "
                f"MEM32(0x4AB668)=0x{device_ptr:08X}",
                flush=True,
            )
        # Remove/step/re-arm: EIP sits on the breakpoint after a stop, and a
        # plain continue would re-trap the same PC forever.
        packet(sock, f"z0,{eip:x},1")
        step_once(sock, 10)
        packet(sock, f"Z0,{eip:x},1")
    else:
        timed_out = time.time() - start >= 600

    try:
        if continue_pending:
            # The loop ended (window/hit-cap reached) while a 'c' was still
            # outstanding -- interrupt it before touching breakpoints, the
            # exact fix for the bug this entry closes out.
            interrupt_and_resync(sock)
        packet(sock, f"z0,{ENTRY_FUNC:x},1")
        packet(sock, f"z0,{ENTRY_CALLSITE:x},1")
    except socket.timeout:
        print("cleanup timed out (target may still be running) -- "
              "not fatal, just means the breakpoints were left armed",
              flush=True)

    if timed_out:
        print(f"window elapsed at t={time.time()-start:.1f}s with "
              f"func_hits={func_hits} callsite_hits={callsite_hits}",
              flush=True)
    print(f"done total_hits={hits} func_entry_hits={func_hits} "
          f"callsite_hits={callsite_hits}", flush=True)


if __name__ == "__main__":
    main()
