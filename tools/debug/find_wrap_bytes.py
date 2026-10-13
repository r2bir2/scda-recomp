"""Update 214: empirically locate which real disc file (and at what byte
offset) the captured ring-buffer bytes at the moment of the underflowing
memcpy actually came from, by searching for the exact captured byte
sequence across every candidate x06_cozumel_a data file. This sidesteps
the ambiguity in the decoded in-memory "reader" filename string (which may
be an internal/original-PC engine name, not the literal file on this
disc image)."""
import glob

needle = bytes([
    0x7e,0xc4,0x67,0x1c,0xf9,0x89,0xfc,0x4c,
    0xc6,0xa3,0xbc,0x06,0x26,0x05,0xf0,0x0a,
    0xfe,0x81,0x37,0xf0,0x11,0x3e,0xc3,0x27,
    0xf8,0x02,0x5f,0x39,0x2f,0x00,0x1f,0xe2,
    0x43,0x7c,0x49,0x33,0xd2,0x92,0xb4,0x20,
    0x7e,0xc4,0x9f,0xb4,0x22,0xad,0x49,0x1b,
    0x12,0x40,0x02,0x49,0x3b,0xd2,0x96,0x04,
    0x11,0x2d,0xe7,0xbe,0xe4,0x04,0xca,0xaf,
    0xe4,0x2a,0x3e,0xd7,0xc8,0x75,0x72,0x9e,
    0xa4,0x92,0x5f,0xc8,0x05,0x72,0x91,0xa4,
    0x91,0x74,0x62,0x24,0x02,0xb9,0x44,0x2e,
    0x93,0x2d,0x64,0x1b,0xd9,0x4a,
])
# Shorter, more robust anchor in case of any transcription slip in the tail.
needle_short = needle[:24]

roots = [
    "build/scda_upgrade/bin/RelWithDebInfo/Default/DataXb/LMaps/x06_cozumel_a",
    ".scratch_debug/umd_fresh_extract/DataXb/Maps",
    ".scratch_debug/umd_fresh_extract/DataXb/LMaps",
]

candidates = []
for root in roots:
    candidates += glob.glob(root + "/**/*", recursive=True)
candidates += glob.glob("build/scda_upgrade/bin/RelWithDebInfo/Default/DataXb/LMaps/x06_cozumel_a/*")

seen = set()
for path in candidates:
    if path in seen:
        continue
    seen.add(path)
    try:
        with open(path, "rb") as f:
            data = f.read()
    except (IsADirectoryError, PermissionError, FileNotFoundError):
        continue
    idx = data.find(needle_short)
    if idx >= 0:
        full_idx = data.find(needle)
        print(f"FOUND in {path}: short-needle at offset 0x{idx:X} ({idx}), "
              f"full-needle at offset {full_idx if full_idx>=0 else 'NOT MATCHED (tail differs)'}")
        # Also show what's at idx-24 (the run of zero bytes we expect to precede it)
        prefix = data[max(0, idx-24):idx]
        print(f"  24 bytes before match: {prefix.hex()}")
    else:
        print(f"no match in {path} (size {len(data)})")
