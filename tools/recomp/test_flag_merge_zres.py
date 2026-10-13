"""A ZF consumer at a join of a cmp and a test must not fall back to _flags.

SCDA's portal walk sub_002AD5AF (docs Update 353):

    0x2AD69F: cmp eax, -1        ; visited-list lookup result
    0x2AD6A2: jmp 0x2ad6bf
    ...
    0x2AD6BD: test al, dl        ; visited-bitmap bit
    0x2AD6BF: setne al           ; join -> "already visited?"

The two predecessors differ in kind and width, so the join used to inherit
no flag state and `setne al` became the never-assigned _flags (always 0):
no portal was ever seen as visited, the walk recursed forever and the host
stack overflowed on the first mission load.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block, _make_condition
from .translator import _merge_flag_states


def _reg(name):
    return Operand(type="reg", reg=name)


def _insn(addr, mnemonic, ops):
    i = Instruction(addr, 2, mnemonic, "", "")
    i.operands = ops
    return i


class ZeroResultMergeTest(unittest.TestCase):
    def test_cmp_and_test_join_as_zres(self):
        a = ("cmp", [_reg("eax"), Operand(type="imm", imm=0xFFFFFFFF)])
        b = ("test", [_reg("al"), _reg("dl")])
        self.assertEqual(_merge_flag_states(a, b), ("zres", []))
        self.assertIsNone(_merge_flag_states(a, ("sub", [_reg("eax"), _reg("ecx")])))
        self.assertIsNone(_merge_flag_states(a, None))

    def test_zres_resolves_zf_only(self):
        self.assertEqual(_make_condition("jne", "zres", [])[0], "(_fzr != 0)")
        self.assertEqual(_make_condition("je", "zres", [])[0], "(_fzr == 0)")
        self.assertIsNone(_make_condition("jl", "zres", []))

    def test_setne_at_the_join_reads_fzr(self):
        stmts, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0x2AD6BF, instructions=[
                _insn(0x2AD6BF, "setne", [_reg("al")])]),
            flag_state=("zres", []))
        code = "\n".join(stmts)
        self.assertIn("_fzr != 0", code)
        self.assertNotIn("_flags", code)

    def test_snapshots_record_the_zero_result(self):
        cmp_code = "\n".join(Lifter()._snapshot_flags(
            None, [_reg("eax"), Operand(type="imm", imm=0xFFFFFFFF)], "cmp"))
        test_code = "\n".join(Lifter()._snapshot_flags(
            None, [_reg("al"), _reg("dl")], "test"))
        self.assertIn("_fzr = (_fa - _fb) &", cmp_code)
        self.assertIn("_fzr = _fa & _fb", test_code)


if __name__ == "__main__":
    unittest.main()
