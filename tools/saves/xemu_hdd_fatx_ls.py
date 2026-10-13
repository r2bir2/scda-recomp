"""Update 436: read-only listing of a xemu qcow2 HDD image's FATX cache
partitions (X/Y/Z = partitions 3/4/5), to see which files the real title
logic put into its utility cache (e.g. SCDA's mission music).

Usage: py -3 xemu_hdd_fatx_ls.py <image.qcow2> [path-filter] [--extract <dir>]
Opens the image read-only; supports qcow2 v2/v3 incl. compressed clusters.
Update 453: --extract copies every matching file to <dir>/P<n>/<path>
(e.g. SCDA saves under UDATA/5553005e for signature checks).
"""
import os
import struct
import zlib
import sys

PARTS = {1: (0xABE80000, 0x1312D6000), 3: (0x00080000, 0x2EE00000), 4: (0x2EE80000, 0x2EE00000),
         5: (0x5DC80000, 0x2EE00000)}


class Qcow2:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(72)
        assert h[:4] == b"QFI\xfb", "not qcow2"
        self.cbits = struct.unpack(">I", h[20:24])[0]
        self.csize = 1 << self.cbits
        l1_size = struct.unpack(">I", h[36:40])[0]
        l1_off = struct.unpack(">Q", h[40:48])[0]
        self.f.seek(l1_off)
        self.l1 = struct.unpack(f">{l1_size}Q", self.f.read(8 * l1_size))
        self.l2bits = self.cbits - 3
        self.l2cache = {}

    def _cluster(self, off):
        l1i = off >> (self.cbits + self.l2bits)
        l2i = (off >> self.cbits) & ((1 << self.l2bits) - 1)
        if l1i >= len(self.l1):
            return None
        l2off = self.l1[l1i] & 0x00FFFFFFFFFFFE00
        if not l2off:
            return None
        if l2off not in self.l2cache:
            self.f.seek(l2off)
            self.l2cache[l2off] = struct.unpack(f">{1 << self.l2bits}Q",
                                                self.f.read(self.csize))
        e = self.l2cache[l2off][l2i]
        if e & (1 << 62):
            # Compressed: raw deflate at host offset, size in 512-byte
            # sectors (qcow2 spec).
            x = 62 - (self.cbits - 8)
            hoff = e & ((1 << x) - 1)
            nsec = ((e >> x) & ((1 << (self.cbits - 8)) - 1)) + 1
            self.f.seek(hoff)
            raw = self.f.read(nsec * 512 - (hoff & 511))
            return ("z", zlib.decompressobj(-15).decompress(raw)[:self.csize])
        return (e & 0x00FFFFFFFFFFFE00) or None

    def read(self, off, n):
        out = bytearray()
        while n > 0:
            c = self._cluster(off)
            within = off & (self.csize - 1)
            take = min(n, self.csize - within)
            if c is None:
                out += bytes(take)
            elif isinstance(c, tuple):
                out += c[1][within:within + take].ljust(take, b"\0")
            else:
                self.f.seek(c + within)
                out += self.f.read(take)
            off += take
            n -= take
        return bytes(out)


def list_part(q, num, flt, extract=None):
    base, size = PARTS[num]
    sb = q.read(base, 16)
    if sb[:4] != b"FATX":
        print(f"partition {num}: no FATX ({sb[:4]!r})")
        return
    spc, root = struct.unpack_from("<II", sb, 8)
    csize = spc * 512
    nclusters = size // csize
    fat_bytes = nclusters * (2 if nclusters < 0xFFF0 else 4)
    fat_bytes = (fat_bytes + 4095) & ~4095
    data = base + 0x1000 + fat_bytes
    wide = nclusters >= 0xFFF0
    fat = q.read(base + 0x1000, fat_bytes)

    def chain(c):
        seen = 0
        while seen < 100000:
            yield c
            seen += 1
            n = struct.unpack_from("<I" if wide else "<H", fat,
                                   c * (4 if wide else 2))[0]
            if n == 0 or n >= (0xFFFFFFF0 if wide else 0xFFF0):
                return
            c = n

    def walk(c, prefix):
        for cl in chain(c):
            blob = q.read(data + (cl - 1) * csize, csize)
            for o in range(0, csize, 64):
                nl = blob[o]
                if nl in (0x00, 0xFF):
                    return
                if nl == 0xE5 or nl > 42:
                    continue
                attr = blob[o + 1]
                name = blob[o + 2:o + 2 + nl].decode("latin-1")
                first, fsize = struct.unpack_from("<II", blob, o + 44)
                path = prefix + "\\" + name
                if attr & 0x10:
                    walk(first, path)
                elif not flt or flt.lower() in path.lower():
                    print(f"P{num} {fsize:>10} {path}")
                    if extract:
                        out = bytearray()
                        for fc in chain(first):
                            if len(out) >= fsize:
                                break
                            out += q.read(data + (fc - 1) * csize, csize)
                        dst = os.path.join(extract, f"P{num}", *path.strip("\\").split("\\"))
                        os.makedirs(os.path.dirname(dst), exist_ok=True)
                        with open(dst, "wb") as f:
                            f.write(bytes(out[:fsize]))

    print(f"partition {num}: FATX cluster={csize} root={root}")
    walk(root, "")


if __name__ == "__main__":
    args = sys.argv[1:]
    ext = None
    if "--extract" in args:
        i = args.index("--extract")
        ext = args[i + 1]
        del args[i:i + 2]
    q = Qcow2(args[0])
    for p in (1, 3, 4, 5):
        list_part(q, p, args[1] if len(args) > 1 else "", ext)
