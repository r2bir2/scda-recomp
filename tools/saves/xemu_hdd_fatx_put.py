"""Overwrite existing files inside a xemu qcow2 HDD image's FATX partition
(docs Update 497) -- e.g. to copy an SCDA save from the recomp's UDATA into
xemu for a ground-truth check.

Usage:
  py -3 xemu_hdd_fatx_put.py <image.qcow2> <partition> <fatx\\path> <local file>
                             [<fatx\\path> <local file> ...]

Deliberately limited, so it cannot damage an image it does not understand:
  - the target file must already exist (no directory or FAT changes);
  - the new data must fit in the file's existing cluster chain;
  - qcow2 clusters must be plain or unallocated (never compressed). An
    unallocated cluster -- a FATX file preallocated but never written, as
    SCDA's save slots are -- gets a new host cluster at the end of the
    file, its refcount set to 1 and its L2 entry pointed at it (qcow2 v3,
    an existing L2 table and refcount block are required; otherwise the
    tool refuses).
All checks run before the first byte is written. Only the directory entry's
size field changes besides the data. Always work on a copy of the image.
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from xemu_hdd_fatx_ls import PARTS, Qcow2  # noqa: E402


class Fatx:
    def __init__(self, q, num):
        self.q = q
        base, size = PARTS[num]
        sb = q.read(base, 16)
        assert sb[:4] == b"FATX", f"partition {num}: no FATX"
        spc, self.root = struct.unpack_from("<II", sb, 8)
        self.csize = spc * 512
        n = size // self.csize
        self.wide = n >= 0xFFF0
        fat_bytes = (n * (4 if self.wide else 2) + 4095) & ~4095
        self.data = base + 0x1000 + fat_bytes
        self.fat = q.read(base + 0x1000, fat_bytes)

    def chain(self, c):
        out = []
        while len(out) < 1000000:
            out.append(c)
            n = struct.unpack_from("<I" if self.wide else "<H", self.fat,
                                   c * (4 if self.wide else 2))[0]
            if n == 0 or n >= (0xFFFFFFF0 if self.wide else 0xFFF0):
                return out
            c = n
        raise RuntimeError("FAT chain loop")

    def cl_off(self, c):
        return self.data + (c - 1) * self.csize

    def find(self, path):
        """-> (first cluster, size, image offset of the dirent's size field)"""
        cur = self.root
        parts = [p for p in path.split("\\") if p]
        for depth, name in enumerate(parts):
            hit = None
            for cl in self.chain(cur):
                blob = self.q.read(self.cl_off(cl), self.csize)
                for o in range(0, self.csize, 64):
                    nl = blob[o]
                    if nl in (0x00, 0xFF):
                        break
                    if nl == 0xE5 or nl > 42:
                        continue
                    if blob[o + 2:o + 2 + nl].decode("latin-1").lower() == name.lower():
                        first, fsize = struct.unpack_from("<II", blob, o + 44)
                        hit = (blob[o + 1], first, fsize, self.cl_off(cl) + o + 48)
                        break
                if hit:
                    break
            if not hit:
                raise FileNotFoundError(path)
            attr, first, fsize, size_off = hit
            if depth < len(parts) - 1:
                assert attr & 0x10, f"{name} is not a directory"
                cur = first
            else:
                assert not attr & 0x10, f"{name} is a directory"
                return first, fsize, size_off


def host_off(q, off):
    c = q._cluster(off)
    if c is None or isinstance(c, tuple):
        return None
    return c + (off & (q.csize - 1))


class Alloc:
    """Host-cluster allocation in a qcow2 v3 image (see module docstring)."""

    def __init__(self, path, q):
        self.q = q
        with open(path, "rb") as f:
            h = f.read(104)
            f.seek(0, 2)
            self.end = f.tell()
        assert struct.unpack(">I", h[4:8])[0] >= 3, "qcow2 v2 not supported"
        self.rt_off = struct.unpack(">Q", h[48:56])[0]
        rt_clusters = struct.unpack(">I", h[56:60])[0]
        self.rbits = 1 << struct.unpack(">I", h[96:100])[0]
        assert self.rbits == 16, "only 16-bit refcounts supported"
        q.f.seek(self.rt_off)
        n = rt_clusters * q.csize // 8
        self.rt = struct.unpack(f">{n}Q", q.f.read(n * 8))
        self.per_block = q.csize * 8 // self.rbits
        self.end = (self.end + q.csize - 1) & ~(q.csize - 1)
        self.planned = {}            # virtual cluster offset -> host offset

    def l2_entry_off(self, voff):
        q = self.q
        l1i = voff >> (q.cbits + q.l2bits)
        l2i = (voff >> q.cbits) & ((1 << q.l2bits) - 1)
        if l1i >= len(q.l1):
            return None
        l2off = q.l1[l1i] & 0x00FFFFFFFFFFFE00
        return l2off + 8 * l2i if l2off else None

    def refcount_off(self, host):
        ci = host >> self.q.cbits
        bi, ei = divmod(ci, self.per_block)
        if bi >= len(self.rt) or not self.rt[bi]:
            return None
        return (self.rt[bi] & ~0x1FF) + ei * 2

    def plan(self, voff):
        """Host offset for virtual cluster voff, allocating if needed."""
        vc = voff & ~(self.q.csize - 1)
        if vc in self.planned:
            return self.planned[vc]
        c = self.q._cluster(vc)
        if isinstance(c, tuple):
            raise RuntimeError(f"compressed cluster at {vc:#x}")
        if c is None:
            if self.l2_entry_off(vc) is None:
                raise RuntimeError(f"no L2 table for {vc:#x}")
            host = self.end
            if self.refcount_off(host) is None:
                raise RuntimeError(f"no refcount block for host {host:#x}")
            self.end += self.q.csize
            c = ("new", host)
        self.planned[vc] = c
        return c


def plan_writes(q, fx, path, data, alloc):
    first, old_size, size_off = fx.find(path)
    chain = fx.chain(first)
    need = (len(data) + fx.csize - 1) // fx.csize
    if need > len(chain):
        raise RuntimeError(f"{path}: needs {need} clusters, chain has {len(chain)}")
    writes = []
    pos = 0
    for cl in chain[:need]:
        img = fx.cl_off(cl)
        piece = data[pos:pos + fx.csize]
        off = 0
        while off < len(piece):
            take = min(len(piece) - off, q.csize - ((img + off) & (q.csize - 1)))
            alloc.plan(img + off)
            writes.append((img + off, piece[off:off + take]))
            off += take
        pos += len(piece)
    if host_off(q, size_off) is None:
        raise RuntimeError(f"{path}: directory entry not in a plain cluster")
    writes.append((size_off, struct.pack("<I", len(data))))
    return writes, old_size


def main(argv):
    if len(argv) < 5 or (len(argv) - 3) % 2:
        print(__doc__)
        return 2
    image, num = argv[1], int(argv[2])
    pairs = list(zip(argv[3::2], argv[4::2]))
    q = Qcow2(image)
    fx = Fatx(q, num)
    alloc = Alloc(image, q)
    all_writes = []
    for fpath, local in pairs:
        data = open(local, "rb").read()
        w, old = plan_writes(q, fx, fpath, data, alloc)
        print(f"plan {fpath}: {old} -> {len(data)} bytes, {len(w)} writes")
        all_writes += w
    new = {vc: c[1] for vc, c in alloc.planned.items()
           if isinstance(c, tuple) and c[0] == "new"}
    print(f"allocating {len(new)} new qcow2 clusters")
    q.f.close()
    with open(image, "r+b") as f:
        for vc, host in sorted(new.items(), key=lambda kv: kv[1]):
            f.seek(host)                       # zero-filled new cluster
            f.write(bytes(q.csize))
            f.seek(alloc.refcount_off(host))
            f.write(struct.pack(">H", 1))
            f.seek(alloc.l2_entry_off(vc))
            f.write(struct.pack(">Q", host | (1 << 63)))
        f.flush()
        for voff, b in all_writes:
            vc = voff & ~(q.csize - 1)
            c = alloc.planned.get(vc)
            host = new[vc] if vc in new else (c if c is not None
                                              else q._cluster(vc))
            f.seek(host + (voff - vc))
            f.write(b)
    q = Qcow2(image)
    fx = Fatx(q, num)
    for fpath, local in pairs:
        data = open(local, "rb").read()
        first, fsize, _ = fx.find(fpath)
        got = bytearray()
        for cl in fx.chain(first):
            if len(got) >= fsize:
                break
            got += q.read(fx.cl_off(cl), fx.csize)
        ok = fsize == len(data) and bytes(got[:fsize]) == data
        print(f"verify {fpath}: {'OK' if ok else 'MISMATCH'}")
        if not ok:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
