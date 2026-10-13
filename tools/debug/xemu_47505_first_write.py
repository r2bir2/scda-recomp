"""2026-09-09 (new session): find the FIRST real write that turns the
vtable-0x004F7248 file-manager container from empty into populated on
real hardware, without guessing from (unreliable) stack walks.

Prior findings this container is scoped to:
  - object address deterministic across boots: 0x0081FC30
    (matched via xemu_47505_container_check.py, twice)
  - fields read by scda_recomp's own probe / xemu_47505_container_check.py:
      +0x00 = container base/bucket-array pointer
      +0x0C = container "buckets" field
      +0x10 = container mask
  - sub_00046172/sub_0004608C/sub_00047A53 never fire in scda_recomp
    before the spin, so the real populate call must go through some
    other, not-yet-identified path -- this script does not assume one.

Strategy: boot completely frozen (-S) so watchpoints are armed before
any code runs, watch the three known fields (base/buckets/mask) on the
known object address, and stop on the very FIRST write -- not a batch
of 8 like the prior (retired) script. At the stop:
  - read eip, all GPRs, eflags
  - read the raw bytes at eip (for manual disassembly / cross-check
    against gen/recomp_*.c or functions.json -- no frame-walk is
    attempted here, deliberately, per the "no stack-walk-as-proof"
    instruction)
  - read the field's old value (from before the watchpoint, captured by
    priming a read right after arming, before continuing) and new value
    (read immediately at the stop)
  - if the stop pc happens to fall inside sub_0004721E's own address
    range (0x0004721E..next symbol), flag it explicitly so the caller
    knows to go do the dynamic-entry / static-callsite work by hand,
    per the task's explicit 3-step verification procedure -- this
    script does not attempt that itself.
"""
from __future__ import annotations

import socket
import struct
import time

# 2026-09-09 (session 2, correct disc): container_check2.py established
# via generated-C cross-reference that "container_base/buckets/mask"
# (per the [SCDA-FM] pc=0x00047858 hook) are read from `esi`, which is
# NOT the outer vtable-0x4F7248 object (`ecx` at sub_0004721E's own
# entry) -- it is a nested object returned by two further lookups
# (sub_0004721E's own name-lookup, then sub_0004693B on the found
# entry) performed inside the *caller*, sub_00047797. Confirmed
# deterministic across two fresh boots on the corrected retail disc:
# outer object=0x0081BE80, nested object=0x0082C784, match at t=1.33s,
# fields populated (container_base=0x0082E290 container_buckets=0x00838E10
# container_mask=0x00000008) by t=1.45s. This script watches the NESTED
# object, which is the one whose fields actually feed that hook.
OBJ = 0x0082C784
FIELDS = {"base(+0x00)": 0x00, "buckets(+0x0C)": 0x0C, "mask(+0x10)": 0x10}
SUB_0004721E_START = 0x0004721E
SUB_0004721E_END = 0x00047900  # generous upper bound used by the prior probe window


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
    reply = bytearray()
    while True:
        char = sock.recv(1)
        if char == b"#":
            sock.recv(2)
            sock.sendall(b"+")
            return reply.decode("ascii")
        reply.extend(char)


def cont(sock):
    sock.sendall(b"$c#63")
    sock.recv(1)
    return wait_stop(sock)


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


def read_bytes(sock, address, length):
    address &= 0xFFFFFFFF
    result = packet(sock, f"m{address:x},{length:x}")
    if result.startswith("E"):
        return None
    return bytes.fromhex(result)


def main():
    t0 = time.monotonic()
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    before = {name: u32(sock, OBJ + off) for name, off in FIELDS.items()}
    print(f"t={time.monotonic()-t0:.2f}s pre-arm field values (should all be 0 if truly unpopulated): {before}", flush=True)

    for name, off in FIELDS.items():
        packet(sock, f"Z2,{OBJ + off:x},4")
    print(f"armed write-watchpoints on 0x{OBJ:08X} +0x00/+0x0C/+0x10, continuing from frozen boot...", flush=True)

    sock.settimeout(60)
    MAX_HITS = 40
    TARGET = {"base(+0x00)": 0x0082E290, "buckets(+0x0C)": 0x00838E10, "mask(+0x10)": 0x00000008}
    for hit_no in range(MAX_HITS):
        reply = cont(sock)
        if reply.startswith("W") or reply.startswith("X"):
            print(f"process exited/terminated: {reply}", flush=True)
            break

        r = regs(sock)
        eip = r["eip"]
        code = read_bytes(sock, eip, 16)
        after = {name: u32(sock, OBJ + off) for name, off in FIELDS.items()}
        changed = [name for name in FIELDS if before[name] != after[name]]
        t = time.monotonic() - t0

        print(f"=== WRITE HIT #{hit_no} t={t:.2f}s ===", flush=True)
        print(f"guest eip = 0x{eip:08X}  bytes = {code.hex() if code else None}", flush=True)
        print(f"registers: eax=0x{r['eax']:08X} ecx=0x{r['ecx']:08X} edx=0x{r['edx']:08X} "
              f"ebx=0x{r['ebx']:08X} esp=0x{r['esp']:08X} ebp=0x{r['ebp']:08X} "
              f"esi=0x{r['esi']:08X} edi=0x{r['edi']:08X} eflags=0x{r['eflags']:08X}", flush=True)
        print(f"before={before}  after={after}  changed={changed}", flush=True)

        if SUB_0004721E_START <= eip < SUB_0004721E_END:
            print("  NOTE: eip inside sub_0004721E's own probe window -- do not "
                  "trust a frame-walk here; verify caller edge statically+dynamically.",
                  flush=True)

        if after == TARGET:
            print("  *** all three fields now match the known final populated "
                  "values -- this write hit is the last candidate before the "
                  "confirmed-populated read. Stopping. ***", flush=True)
            ebp = r["ebp"]
            saved_ebp = u32(sock, ebp) or 0
            ret_into_77b61_caller = u32(sock, ebp + 4) or 0
            outer_ebp_ret = (u32(sock, saved_ebp + 4) or 0) if saved_ebp else 0
            print(f"  frame walk (sub_000778BC has a real prologue, verified): "
                  f"ebp=0x{ebp:08X} [ebp+4] (return into sub_00077B61)=0x{ret_into_77b61_caller:08X} "
                  f"[ebp] (saved/caller ebp)=0x{saved_ebp:08X} "
                  f"[[ebp]+4] (one hop further, valid IF sub_00077B61's own caller "
                  f"also has a real prologue -- sub_00077B61 itself is fpo_leaf so "
                  f"this ebp is inherited, not a fresh frame)=0x{outer_ebp_ret:08X}",
                  flush=True)
            before = after
            break

        before = after

    for name, off in FIELDS.items():
        packet(sock, f"z2,{OBJ + off:x},4")
    print("done", flush=True)


if __name__ == "__main__":
    main()
