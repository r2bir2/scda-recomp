"""An immediate that lands inside a decoded instruction is a number, not a
function address (docs Update 336).

SCDA's D3D code holds 0x00201E40 as a constant (0x004A8C55); that is two bytes
into `lea ecx, [esi+0x10C]` at 0x00201E3E of the constructor sub_00201DF4.
Accepting it as a function split the constructor and left the object it builds
without a vtable.

    py -3 -m unittest tools.disasm.test_imm_mid_instruction
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import Instruction  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402


class _Engine:
    def __init__(self, insns):
        self.instructions = {i.address: i for i in insns}


def _detector(insns):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    return det


LEA = Instruction(0x00201E3E, 6, "lea", "ecx, [esi + 0x10c]", "8d8e0c010000")
CALL = Instruction(0x00201E44, 5, "call", "0x28f452", "e809d60800")


class ImmMidInstructionTest(unittest.TestCase):
    def test_address_inside_lea_is_rejected(self):
        self.assertTrue(_detector([LEA, CALL])._inside_decoded_instruction(0x00201E40))

    def test_instruction_boundary_is_not_inside(self):
        self.assertFalse(_detector([LEA, CALL])._inside_decoded_instruction(0x00201E44))

    def test_undecoded_region_is_not_inside(self):
        self.assertFalse(_detector([])._inside_decoded_instruction(0x00201E40))


if __name__ == "__main__":
    unittest.main()
