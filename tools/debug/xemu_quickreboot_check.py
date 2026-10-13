"""Watch for a SECOND occurrence of the early-boot marker
(sub_000366A2 entry, known to fire reliably once per boot at
t~1.3-1.4s) to directly prove or disprove that real hardware performs
a quick-reboot (HalReturnToFirmware routine=2 relaunching the same
XBE) rather than truly exiting, at the point scda_recomp currently
calls ExitProcess."""
from __future__ import annotations
import socket
import struct
import time

MARKER = 0x000366A2


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


def cont_and_wait(sock, timeout):
    sock.sendall(b"$c#63")
    sock.recv(1)
    sock.settimeout(timeout)
    wait_stop(sock)
    return get_regs(sock)


def main():
    sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
    sock.settimeout(10)
    packet(sock, f"Z0,{MARKER:x},1")
    print(f"armed 0x{MARKER:08X}, watching for up to 3 hits over ~4 minutes", flush=True)

    t0 = time.time()
    for i in range(3):
        try:
            regs = cont_and_wait(sock, 240)
        except socket.timeout:
            print(f"no hit #{i+1} within 240s -- stopping (had {i} hits)", flush=True)
            break
        print(f"HIT #{i+1} at t={time.time()-t0:.1f}s eip=0x{regs['eip']:08X} "
              f"ecx=0x{regs['ecx']:08X}", flush=True)
        unstick(sock, MARKER)
    print("done", flush=True)


if __name__ == "__main__":
    main()
