"""MOVSX must sign-extend every 16-bit general-purpose register.

SCDA's Bink IDCT uses ``movsx ebx, bp`` at 0x004C3C88.  A negative
coefficient (-1) previously became +65535, corrupting decoded blocks.
"""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def lift_movsx(dst, src):
    insn = Instruction(0x004C3C88, 3, "movsx", f"{dst}, {src}", "0fbfdd")
    insn.operands = [Operand(type="reg", reg=dst), Operand(type="reg", reg=src)]
    return "\n".join(Lifter().lift_instruction(insn))


class MovsxRegisterTest(unittest.TestCase):
    def test_bp_sign_extends(self):
        self.assertIn("ebx = SX16(LO16(ebp));", lift_movsx("ebx", "bp"))

    def test_sp_sign_extends(self):
        self.assertIn("eax = SX16(LO16(esp));", lift_movsx("eax", "sp"))

    def test_existing_si_sign_extension(self):
        self.assertIn("ecx = SX16(LO16(esi));", lift_movsx("ecx", "si"))


if __name__ == "__main__":
    unittest.main()
