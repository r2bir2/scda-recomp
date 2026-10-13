"""Follow-up: sub_0004023A is detected as ending at 0x00040295 (91 bytes),
but the code immediately after it (no prologue of its own -- it uses
esi/edi/ebp as already-established by an enclosing frame) is clearly a
direct, coherent continuation doing the exact same path-building pattern,
running at least to a "jne 0x406fc" branch. This disassembles further,
including the 0x406fc target, to find where the function's REAL "ret"
terminator actually is.
"""
import sys

sys.path.insert(0, "tools/disasm")
from loader import load_image
import capstone

img = load_image("build/scda_upgrade/bin/RelWithDebInfo/default.xbe")

start = 0x00040295
length = 0x500
raw = img.read_bytes_at_va(start, length)

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
last_ret = None
for insn in md.disasm(raw, start):
    print(f"0x{insn.address:08x}: {insn.mnemonic} {insn.op_str}")
    if insn.mnemonic == "ret":
        last_ret = insn.address
        print(f"  ^^^ ret at 0x{insn.address:08x}")
