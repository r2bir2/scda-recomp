"""Re-sign Xbox save files for another set of kernel signing keys.

Xbox titles sign save files with XCalculateSignature and append the 20-byte
result (SCDA: sub_0003F514, docs Update 453):

    roamable:      HMAC-SHA1(SignatureKey, data)
    non-roamable:  HMAC-SHA1(HDKey, HMAC-SHA1(SignatureKey, data))

SignatureKey is the title's key the kernel derives when it loads the XBE
(export XboxSignatureKey), HDKey is the console's EEPROM key (XboxHDKey).
A save moved to a console, emulator or recomp build with other keys fails
the title's check. Re-signing needs no secret of the source: strip the
trailer, sign again with the target keys. The source keys are only used to
recognise which files carry a signature at all.

Key files are 32 bytes: SignatureKey, then HDKey (tools/debug/
xemu_read_kernel_keys.py writes one). "zero" means all-zero keys, the
recomp's keys when no key file is loaded. Key files are console/Microsoft
data -- keep them local, never commit them.

Usage:
  py -3 tools/xbox_resign_saves.py <save dir> --from zero|<keys.bin>
                                   --to zero|<keys.bin> [--dry-run]
Only files whose trailer verifies under --from are rewritten.
"""
import argparse
import hashlib
import hmac
import os
import sys


def load_keys(spec):
    if spec == "zero":
        return bytes(16), bytes(16)
    data = open(spec, "rb").read()
    if len(data) != 32:
        sys.exit(f"{spec}: expected 32 bytes, got {len(data)}")
    return data[:16], data[16:]


def sign(keys, data, roamable):
    sig = hmac.new(keys[0], data, hashlib.sha1).digest()
    return sig if roamable else hmac.new(keys[1], sig, hashlib.sha1).digest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("savedir")
    ap.add_argument("--from", dest="src", required=True)
    ap.add_argument("--to", dest="dst", required=True)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    src, dst = load_keys(a.src), load_keys(a.dst)
    counts = {"resigned": 0, "already": 0, "unsigned": 0}
    for root, _, files in os.walk(a.savedir):
        for name in sorted(files):
            path = os.path.join(root, name)
            d = open(path, "rb").read()
            if len(d) <= 20:
                counts["unsigned"] += 1
                continue
            data, trailer = d[:-20], d[-20:]
            kind = None
            for roam in (False, True):
                if sign(dst, data, roam) == trailer:
                    kind = "already"
                    break
                if sign(src, data, roam) == trailer:
                    kind = ("resign", roam)
                    break
            rel = os.path.relpath(path, a.savedir)
            if kind is None:
                counts["unsigned"] += 1
                print(f"  skip (no matching signature)  {rel}")
            elif kind == "already":
                counts["already"] += 1
            else:
                roam = kind[1]
                if not a.dry_run:
                    with open(path, "wb") as f:
                        f.write(data + sign(dst, data, roam))
                counts["resigned"] += 1
                print(f"  {'would re-sign' if a.dry_run else 're-signed'} "
                      f"({'roamable' if roam else 'non-roamable'})  {rel}")
    print(f"done: {counts['resigned']} re-signed, {counts['already']} already valid "
          f"for target, {counts['unsigned']} without a recognised signature")


if __name__ == "__main__":
    main()
