"""One-off: dump raw bytes and (if capstone is available) disassemble the
gap right after sub_0004023A (0x00040295) through past 0x00040400 -- this
is where the guest EBP-chain walk on xemu found a real caller
(0x00040350) that our own functions.json has NO detected function for,
right in a gap after sub_0004023A (whose own only caller is [] --
i.e. reached exclusively through an indirect call this pipeline never
resolved to a function).
"""
import sys

sys.path.insert(0, "tools/disasm")
from loader import load_image

img = load_image("build/scda_upgrade/bin/RelWithDebInfo/default.xbe")

start = 0x00040295
length = 0x180
raw = img.read_bytes_at_va(start, length)
print(f"raw bytes {start:#010x}..{start+length:#010x}:")
for i in range(0, length, 16):
    chunk = raw[i:i+16]
    hexs = " ".join(f"{b:02x}" for b in chunk)
    print(f"{start+i:#010x}: {hexs}")

try:
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    print("\ndisassembly:")
    for insn in md.disasm(raw, start):
        print(f"0x{insn.address:08x}: {insn.mnemonic} {insn.op_str}")
except ImportError:
    print("\n(capstone not installed -- raw bytes only)")
