"""Update 453: read the kernel's save-signing keys from a running xemu.

Xbox titles sign save files with XCalculateSignature: HMAC-SHA1 keyed from
XboxSignatureKey (kernel data export 325, the same on every retail kernel)
for roamable data and from XboxHDKey (export 323, per console) for
non-roamable data. The recomp's kernel exports zeros for both, so saves
from a real console or xemu fail signature checks and vice versa.

This reads both 16-byte keys out of the user's own kernel running in xemu
(gdbstub, `xemu.exe -s`): PE header of the kernel image at 0x80010000 ->
export directory -> AddressOfFunctions[ordinal - Base] -> 16 bytes.
The keys are written to a local binary file (32 bytes: SignatureKey then
HDKey). Never commit that file -- the keys are Microsoft/console data.

Usage: py -3 xemu_read_kernel_keys.py <out.bin>
"""
import socket
import struct
import sys
import time

_argv, sys.argv = sys.argv, sys.argv[:1]
sys.path.insert(0, __file__.rsplit("\\", 1)[0])
import xemu_anim_channels as x  # noqa: E402
sys.argv = _argv

KERNEL_BASE = 0x80010000
ORD_HD_KEY = 323
ORD_SIGNATURE_KEY = 325


def pause(sock):
    for _ in range(20):
        sock.sendall(b"\x03")
        time.sleep(0.12)
        x.drain(sock)
        try:
            x.packet(sock, "?")
            x.drain(sock)
            return
        except (socket.timeout, TimeoutError):
            time.sleep(0.5)
    raise TimeoutError("xemu gdbstub does not answer the break-in")


def export_va(sock, ordinal):
    mz = x.read(sock, KERNEL_BASE, 0x40)
    if mz[:2] != b"MZ":
        raise RuntimeError(f"no MZ header at 0x{KERNEL_BASE:08X}: {mz[:4]!r}")
    pe = KERNEL_BASE + struct.unpack_from("<I", mz, 0x3C)[0]
    hdr = x.read(sock, pe, 0x100)
    if hdr[:4] != b"PE\0\0":
        raise RuntimeError("no PE header")
    exp_rva = struct.unpack_from("<I", hdr, 0x18 + 0x60)[0]
    exp = x.read(sock, KERNEL_BASE + exp_rva, 0x28)
    base, nfuncs = struct.unpack_from("<II", exp, 0x10)
    funcs = struct.unpack_from("<I", exp, 0x1C)[0]
    idx = ordinal - base
    if not 0 <= idx < nfuncs:
        raise RuntimeError(f"ordinal {ordinal} outside export table")
    rva = x.u32(x.read(sock, KERNEL_BASE + funcs + 4 * idx, 4), 0)
    return KERNEL_BASE + rva


out = sys.argv[1]
sock = socket.create_connection(("127.0.0.1", 1234), timeout=10)
try:
    pause(sock)
    keys = {}
    for name, ordn in (("XboxSignatureKey", ORD_SIGNATURE_KEY), ("XboxHDKey", ORD_HD_KEY)):
        va = export_va(sock, ordn)
        keys[name] = x.read(sock, va, 16)
        nonzero = any(keys[name])
        print(f"{name} (ordinal {ordn}) at 0x{va:08X}: 16 bytes, nonzero={nonzero}")
    with open(out, "wb") as f:
        f.write(keys["XboxSignatureKey"] + keys["XboxHDKey"])
    print(f"written {out} (32 bytes; do not commit)")
finally:
    sock.sendall(b"$c#63")
    sock.close()
