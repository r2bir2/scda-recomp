"""Extended full ordinal-indexed capture of every sub_001E460F (the
single generic realloc/resize call site all bin[6]-relevant activity in
this window flows through -- confirmed by reading its own generated
code: it computes new_size=count*stride, calls the allocator's vtable+4
"resize" method, which may internally free the old block AND/OR
allocate a new one) between marker A (sub_000366A2 entry) and marker B
(sub_001D589D entry).

For every hit, records:
  - sequential event index
  - guest PC (entry address, 0x001E460F)
  - return address / caller (MEM32(esp) at entry -- reliable here since
    the trace fires before this cdecl function's own prologue pushes
    anything, so esp still points straight at the return address)
  - container/chunk pointer (ecx at entry == esi once loaded)
  - old capacity (MEM32(ecx+8)), requested new count (MEM32(esp+8)),
    element stride (inferred as new_size/new_count when non-zero)
  - requested new_size in bytes (the actual value passed to the
    allocator's resize method)
  - bin6_len immediately before the call and immediately after it
    returns (captured via a second breakpoint at the function's own
    exit label, 0x001E4631) -- the delta tells us push (+1), pop (-1),
    or no bin6 effect (0) for this specific call, net of whatever the
    opaque C++ allocator vtable method does internally.
  - returned chunk pointer (eax at exit)
"""
from __future__ import annotations
import socket
import struct
import time

MARKER_A = 0x000366A2
MARKER_B = 0x001D589D
ENTRY = 0x001E460F
EXIT = 0x001E4631
BIN6_ADDR = 0x0064D2B4
FAST_PATH_UNTIL = 615  # detailed capture starts here (last run's first
                        # divergence was at n=625/632 of ~650)


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


def read_u32(sock, address):
    response = packet(sock, f"m{address:x},4")
    if response.startswith("E") or len(response) != 8:
        return None
    return struct.unpack("<I", bytes.fromhex(response))[0]


def bin6_chain_len(sock):
    cur = read_u32(sock, BIN6_ADDR)
    n = 0
    while cur and n < 64:
        n += 1
        cur = read_u32(sock, cur + 8)
    return n


def get_regs(sock):
    response = packet(sock, "g")
    data = bytes.fromhex(response)
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "eip", "eflags")
    return {name: struct.unpack_from("<I", data, index * 4)[0] for index, name in enumerate(names)}


def unstick(sock, addr):
    packet(sock, f"z0,{addr:x},1")
    sock.sendall(b"$s#73")
    sock.recv(1)
    sock.settimeout(10)
    try:
        wait_stop(sock)
    except socket.timeout:
        pass
    packet(sock, f"Z0,{addr:x},1")


def cont_and_wait(sock, timeout=60):
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)
    return get_regs(sock)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)

    packet(sock, f"Z0,{MARKER_A:x},1")
    regs = cont_and_wait(sock, 120)
    assert regs["eip"] == MARKER_A
    print("MARKER A HIT", flush=True)
    packet(sock, f"z0,{MARKER_A:x},1")

    packet(sock, f"Z0,{ENTRY:x},1")
    packet(sock, f"Z0,{MARKER_B:x},1")
    print("armed entry(0x1E460F) + marker B", flush=True)

    out = open("C:/VScode/XboxRecomp/build/scda_upgrade/xemu_vecfree_ordinal2.log", "w")
    n = 0
    t0 = time.time()
    while True:
        try:
            regs = cont_and_wait(sock, 60)
        except socket.timeout:
            print(f"no hit within 60s (had {n}) -- stopping", flush=True)
            break
        eip = regs["eip"]
        if eip == MARKER_B:
            bl = bin6_chain_len(sock)
            line = f"MARKER B HIT t={time.time()-t0:.1f}s bin6_len={bl} after {n} sub_001E460F events"
            print(line, flush=True)
            out.write(line + "\n")
            break
        if eip != ENTRY:
            continue
        n += 1

        # Cheap fast-path for the bulk of the window (already proven
        # identical/harmless up to n~=630 last session) -- only switch to
        # the expensive dual-breakpoint (entry+exit) detailed capture for
        # the final stretch where the actual divergence was localized,
        # to stay within a practical wall-clock budget.
        if n < FAST_PATH_UNTIL:
            unstick(sock, eip)
            continue

        ecx = regs["ecx"]
        esp = regs["esp"]
        ret_addr = read_u32(sock, esp) or 0
        # NOTE on offsets: the trace fires *before* this function's own
        # "PUSH32(esp, esi)" (its first statement), so the stack argument
        # the source code later reads as "MEM32(esp+8)" (after that one
        # push has shifted esp down by 4) is at esp+4 relative to THIS
        # captured (pre-push) esp -- matches the established convention
        # already used by the existing [SCDA-VECTOR-FREE] hook.
        elem_field = read_u32(sock, ecx + 8) or 0
        new_count_arg = read_u32(sock, esp + 4) or 0
        new_size_computed = elem_field * new_count_arg
        bin6_before = bin6_chain_len(sock)

        # Disarm entry, arm exit, single-step off entry, continue to exit.
        packet(sock, f"z0,{ENTRY:x},1")
        packet(sock, f"Z0,{EXIT:x},1")
        sock.sendall(b"$s#73")
        sock.recv(1)
        sock.settimeout(10)
        try:
            wait_stop(sock)
        except socket.timeout:
            pass
        exit_regs = cont_and_wait(sock, 30)
        returned_ptr = None
        bin6_after = None
        if exit_regs["eip"] == EXIT:
            returned_ptr = exit_regs["eax"]
            bin6_after = bin6_chain_len(sock)
            packet(sock, f"z0,{EXIT:x},1")
            sock.sendall(b"$s#73")
            sock.recv(1)
            sock.settimeout(10)
            try:
                wait_stop(sock)
            except socket.timeout:
                pass
            packet(sock, f"Z0,{EXIT:x},1")
        packet(sock, f"Z0,{ENTRY:x},1")

        delta = (bin6_after - bin6_before) if (bin6_after is not None) else None
        line = (f"n={n} pc=0x{ENTRY:08X} ret=0x{ret_addr:08X} container=0x{ecx:08X} "
                f"elem_field={elem_field} new_count_arg={new_count_arg} new_size={new_size_computed} "
                f"returned_ptr=0x{returned_ptr if returned_ptr else 0:08X} "
                f"bin6_before={bin6_before} bin6_after={bin6_after} delta={delta}")
        out.write(line + "\n")
        out.flush()
        if delta:
            print(line, flush=True)

    out.close()
    print("done, full log at build/scda_upgrade/xemu_vecfree_ordinal2.log", flush=True)


if __name__ == "__main__":
    main()
