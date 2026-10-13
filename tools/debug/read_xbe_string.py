"""One-off: read the null-terminated ASCII string at a given guest VA in
default.xbe, used to identify what the 0x4F781C lookup-key constant
(pushed before the sub_001D4A5A call in sub_00048F1D) actually says.
"""
import sys

sys.path.insert(0, "tools/disasm")
from loader import load_image

img = load_image("build/scda_upgrade/bin/RelWithDebInfo/default.xbe")

for addr_str in sys.argv[1:]:
    addr = int(addr_str, 16)
    raw = img.read_bytes_at_va(addr, 64)
    end = raw.find(b"\x00")
    text = raw[:end if end >= 0 else len(raw)]
    print(f"0x{addr:08X}: {text!r}")
