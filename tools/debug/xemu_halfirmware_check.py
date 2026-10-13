"""Ground-truth check for the sub_000557ED -> HalReturnToFirmware(2) exit
path that scda_recomp currently hits (routine=2, LaunchDataPage type=-1,
plain self-relaunch, no custom payload).

Question this answers: does REAL hardware (via xemu) ever call
HalReturnToFirmware at this exact site (0x00055823, inside sub_000557ED)
during a normal boot? If yes -- what does real hardware do immediately
after, in guest terms (does control ever return / does the early-boot
marker refire / does execution simply continue past this point)?

Strategy:
  1. Arm a breakpoint on the call site itself (0x00055823).
  2. Arm a breakpoint on the early-boot marker (sub_000366A2, the same
     one xemu_quickreboot_check.py used, known to fire once at
     t~1.3-1.4s on a fresh cold boot).
  3. Run for up to N minutes. Report every hit of either breakpoint with
     a timestamp and register snapshot (esp/eax for the call site --
     eax should equal the routine arg's push, but the pushed literal 2
     is on the stack, not in a register, so read [esp] AFTER the push
     but BEFORE the call executes -- we stop before the call, so [esp]
     at that point is [routine, retaddr, ...]).
  4. If the call-site breakpoint fires, single-step through the call and
     keep tracing eip for a short window afterward to see where real
     hardware actually goes (does it come back / does the marker refire
     / does eip end up somewhere in kernel HAL code and never return).
"""
from __future__ import annotations
import socket
import struct
import time

CALL_SITE = 0x00055823   # call dword ptr [0x4f5268] inside sub_000557ED (routine=2, scda_recomp's path)
CALL_SITE_B = 0x000554E8 # call dword ptr [0x4f5268] inside the fs:[0x20]+0x250-callback sibling (routine=2)
CALL_SITE_C = 0x00057A50 # call dword ptr [0x4f5268] inside sub_000579F2, gated by *0x10118+8==0xfffe0000 (routine=4)
MARKER = 0x000366A2      # early-boot marker, fires ~once at t~1.3-1.4s


def packet(sock, body):
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


def wait_stop(sock):
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


def get_regs(sock):
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def read_mem(sock, addr, length):
    response = packet(sock, f"m{addr:x},{length:x}")
    return bytes.fromhex(response)


def read_u32(sock, addr):
    return struct.unpack("<I", read_mem(sock, addr, 4))[0]


def cont_and_wait(sock, timeout):
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)
    return get_regs(sock)


def step_and_wait(sock, timeout):
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)
    return get_regs(sock)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    all_sites = {CALL_SITE: "A(0x55823,routine2,scda_recomp-path)",
                 CALL_SITE_B: "B(0x554E8,routine2,TLScallback-sibling)",
                 CALL_SITE_C: "C(0x57A50,routine4,fatal-flag-gated)"}
    for addr in all_sites:
        packet(sock, f"Z0,{addr:x},1")
    packet(sock, f"Z0,{MARKER:x},1")
    print(f"armed call sites {[hex(a) for a in all_sites]} and marker 0x{MARKER:08X}, "
          f"watching for up to 6 minutes", flush=True)

    t0 = time.time()
    deadline = t0 + 360
    call_site_hits = 0
    marker_hits = 0

    while time.time() < deadline:
        remaining = deadline - time.time()
        try:
            regs = cont_and_wait(sock, max(1, remaining))
        except socket.timeout:
            print(f"no more hits within remaining window -- stopping "
                  f"(call_site_hits={call_site_hits} marker_hits={marker_hits})",
                  flush=True)
            break

        eip = regs["eip"]
        t = time.time() - t0
        if eip in all_sites:
            call_site_hits += 1
            routine = read_u32(sock, regs["esp"])
            print(f"CALL-SITE HIT #{call_site_hits} [{all_sites[eip]}] at t={t:.1f}s "
                  f"eip=0x{eip:08X} esp=0x{regs['esp']:08X} "
                  f"routine_arg(esp+0)=0x{routine:08X} "
                  f"eax=0x{regs['eax']:08X} ecx=0x{regs['ecx']:08X}", flush=True)

            # single-step through the call itself and trace the next
            # ~40 instructions to see where real hardware actually goes.
            packet(sock, f"z0,{eip:x},1")
            for i in range(40):
                try:
                    step_regs = step_and_wait(sock, 10)
                except socket.timeout:
                    print(f"  step {i}: timeout (stuck?)", flush=True)
                    break
                print(f"  step {i}: eip=0x{step_regs['eip']:08X} "
                      f"esp=0x{step_regs['esp']:08X}", flush=True)
            packet(sock, f"Z0,{eip:x},1")
            print("  (resuming normal watch after trace)", flush=True)
        elif eip == MARKER:
            marker_hits += 1
            print(f"MARKER HIT #{marker_hits} (early-boot re-entry) at "
                  f"t={t:.1f}s eip=0x{eip:08X} ecx=0x{regs['ecx']:08X}", flush=True)
            packet(sock, f"z0,{MARKER:x},1")
            sock.sendall(b"$s#73")
            sock.recv(1)
            sock.settimeout(10)
            try:
                wait_stop(sock)
            except socket.timeout:
                pass
            packet(sock, f"Z0,{MARKER:x},1")
        else:
            print(f"unexpected stop at eip=0x{eip:08X} t={t:.1f}s", flush=True)

    print(f"done. call_site_hits={call_site_hits} marker_hits={marker_hits}", flush=True)


if __name__ == "__main__":
    main()
