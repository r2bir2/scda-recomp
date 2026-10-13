"""Disassemble sub_0004023A from its real start (0x0004023A) through past
the currently-detected end (0x00040295), to see exactly what instruction
sequence triggers the (wrong) boundary."""
import sys

sys.path.insert(0, "tools/disasm")
from loader import load_image
import capstone

img = load_image("build/scda_upgrade/bin/RelWithDebInfo/default.xbe")

start = 0x0004023A
length = 0x100
raw = img.read_bytes_at_va(start, length)

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True
for insn in md.disasm(raw, start):
    marker = "  <<<< CURRENTLY DETECTED END" if insn.address == 0x00040295 else ""
    print(f"0x{insn.address:08x}: {insn.mnemonic} {insn.op_str}{marker}")
