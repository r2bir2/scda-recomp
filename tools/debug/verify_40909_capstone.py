"""Update 213: byte-exact verification of sub_00040821's guest 0x40909-0x40992
block (the loc_00040909/loc_00040942 buffer-compaction memcpy region newly
identified as the exact spot where the ring-buffer subobject's header fields
get zeroed), against the real XBE bytes, before concluding this is an
original-game bug rather than a recompiler mistranslation."""
import sys

sys.path.insert(0, "tools/disasm")
from loader import load_image
import capstone

img = load_image("build/scda_upgrade/bin/RelWithDebInfo/default.xbe")

start = 0x00040909
length = 0x90
raw = img.read_bytes_at_va(start, length)

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
for insn in md.disasm(raw, start):
    print(f"0x{insn.address:08x}: {insn.mnemonic} {insn.op_str}")
