"""Broaden the Update 214 byte search across the entire extracted disc
image (not just cozumel-named files), since the in-memory 'reader' string
may be an unrelated FName table entry rather than the actual file being
read. Uses a short, safe 16-byte anchor to tolerate any transcription
slip, and reports every file (and offset) where it appears."""
import os

needle = bytes([0x7e,0xc4,0x67,0x1c,0xf9,0x89,0xfc,0x4c,
                0xc6,0xa3,0xbc,0x06,0x26,0x05,0xf0,0x0a])

roots = [
    "build/scda_upgrade/bin/RelWithDebInfo/Default",
    ".scratch_debug/umd_fresh_extract",
]

hits = 0
for root in roots:
    if not os.path.isdir(root):
        print(f"(root not found: {root})")
        continue
    for dirpath, _, filenames in os.walk(root):
        for fn in filenames:
            path = os.path.join(dirpath, fn)
            try:
                sz = os.path.getsize(path)
            except OSError:
                continue
            if sz < 16 or sz > 400 * 1024 * 1024:
                continue
            try:
                with open(path, "rb") as f:
                    data = f.read()
            except OSError:
                continue
            idx = data.find(needle)
            if idx >= 0:
                hits += 1
                print(f"FOUND in {path} at offset 0x{idx:X} ({idx}), file size {sz}")

print(f"total hits: {hits}")
