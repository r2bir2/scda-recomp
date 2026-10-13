"""An immediate that points at a flag-reading instruction is not a function
start (docs Update 470).

SCDA's deleting destructor sub_00081D7F ends in `test byte [esp+8], 1` /
`je 0x81d9e` (skip the free). A constant 0x00081D8C elsewhere made the `je` a
function of its own; the lifter has no flags at a function start, took the
branch as "not equal", and the destructor freed its object on every call.

    py -3 -m unittest tools.disasm.test_imm_flag_entry
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import Instruction  # noqa: E402
from tools.disasm.functions import FunctionDetector, reads_flags_first  # noqa: E402


class _Engine:
    def __init__(self, insns):
        self.instructions = {i.address: i for i in insns}


def _detector(insns):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    return det


TEST = Instruction(0x00081D87, 5, "test", "byte ptr [esp + 8], 1", "f644240801")
JE = Instruction(0x00081D8C, 2, "je", "0x81d9e", "7410")
PUSH = Instruction(0x00081D98, 2, "push", "0", "6a00")


class ImmFlagEntryTest(unittest.TestCase):
    def test_je_after_test_is_rejected(self):
        self.assertTrue(_detector([TEST, JE, PUSH])._opens_on_flag_read(0x00081D8C))

    def test_plain_instruction_is_kept(self):
        self.assertFalse(_detector([TEST, JE, PUSH])._opens_on_flag_read(0x00081D98))

    def test_undecoded_address_is_kept(self):
        self.assertFalse(_detector([])._opens_on_flag_read(0x00081D8C))

    # Update 471: a jcc into another function's interior becomes an alias.
    def test_jcc_into_other_body_becomes_alias(self):
        from tools.disasm.functions import Function
        je = Instruction(0x81D8C, 2, "je", "0x81d9e", "7410")
        je.is_cond_jump, je.jump_target = True, 0x81D9E
        mov = Instruction(0x81D9E, 2, "mov", "eax, esi", "8bc6")
        det = _detector([je, mov])
        det.functions = {0x81D98: Function(0x81D98, 0x81DA4, "sub_00081D98")}
        det._alias_entries = {0x81D7F: 0x81D98}   # the destructor itself
        det._pass_cond_branch_into_bodies()
        self.assertEqual(det._alias_entries.get(0x81D9E), 0x81DA4)

    def test_jcc_inside_own_body_is_left_alone(self):
        from tools.disasm.functions import Function
        je = Instruction(0x1004, 2, "je", "0x1008", "7402")
        je.is_cond_jump, je.jump_target = True, 0x1008
        det = _detector([je, Instruction(0x1008, 1, "ret", "", "c3")])
        det.functions = {0x1000: Function(0x1000, 0x1010, "sub_00001000")}
        det._alias_entries = {}
        det._pass_cond_branch_into_bodies()
        self.assertEqual(det._alias_entries, {})

    def test_flag_readers(self):
        for m in ("je", "jne", "jl", "jbe", "sete", "setne", "cmovz", "fcmovb",
                  "adc", "sbb", "rcl", "lahf", "pushfd"):
            self.assertTrue(reads_flags_first(m), m)
        for m in ("jmp", "jecxz", "jcxz", "push", "mov", "test", "cmp", "call"):
            self.assertFalse(reads_flags_first(m), m)


if __name__ == "__main__":
    unittest.main()
