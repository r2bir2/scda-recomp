"""A loop head must inherit flags from its back edge too.

SCDA zlib pqdownheap, sub_0004ED7F (docs Update 355):

    0x0004ED9C: cmp ecx, edx
    0x0004ED9E: jg 0x4EE20
    0x0004EDA0: push ebx
    0x0004EDA1: push esi
    0x0004EDA2: jge 0x4EDD9          ; loop head: entry + back edge
    ...
    0x0004EE1A: cmp ecx, edx
    0x0004EE1C: jle 0x4EDA2          ; back edge

The address-order walk had not lifted the back edge when it reached
0x4EDA2, so the jge lowered to the never-assigned _flags (always false) and
zlib compared past the heap end.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block
from .translator import _solve_block_flag_states


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(v):
    return Operand(type="imm", imm=v)


def _insn(addr, mnemonic, ops, target=None):
    i = Instruction(addr, 2, mnemonic, "", "")
    i.operands = ops
    i.jump_target = target
    return i


def _pqdownheap_blocks():
    b_entry = BasicBlock(start=0x4ED9C, instructions=[
        _insn(0x4ED9C, "cmp", [_reg("ecx"), _reg("edx")]),
        _insn(0x4ED9E, "jg", [_imm(0x4EE20)], 0x4EE20)])
    b_push = BasicBlock(start=0x4EDA0, instructions=[
        _insn(0x4EDA0, "push", [_reg("ebx")]),
        _insn(0x4EDA1, "push", [_reg("esi")])])
    b_head = BasicBlock(start=0x4EDA2, instructions=[
        _insn(0x4EDA2, "jge", [_imm(0x4EDD9)], 0x4EDD9)])
    b_body = BasicBlock(start=0x4EDA4, instructions=[
        _insn(0x4EDA4, "inc", [_reg("ecx")])])
    b_tail = BasicBlock(start=0x4EDD9, instructions=[
        _insn(0x4EE14, "shl", [_reg("ecx"), _imm(1)]),
        _insn(0x4EE1A, "cmp", [_reg("ecx"), _reg("edx")]),
        _insn(0x4EE1C, "jle", [_imm(0x4EDA2)], 0x4EDA2)])
    b_exit = BasicBlock(start=0x4EE20, instructions=[
        _insn(0x4EE20, "ret", [])])
    blocks = [b_entry, b_push, b_head, b_body, b_tail, b_exit]
    preds = {
        0x4ED9C: set(),
        0x4EDA0: {0x4ED9C},
        0x4EDA2: {0x4EDA0, 0x4EDD9},
        0x4EDA4: {0x4EDA2},
        0x4EDD9: {0x4EDA2, 0x4EDA4},
        0x4EE20: {0x4ED9C, 0x4EDD9},
    }
    return blocks, preds


class LoopFlagFixpointTest(unittest.TestCase):
    def test_loop_head_gets_cmp_state(self):
        blocks, preds = _pqdownheap_blocks()
        lifter = Lifter()
        states = _solve_block_flag_states(
            blocks, preds, 0x4ED9C,
            lambda bb, st: lift_basic_block(lifter, bb, flag_state=st)[1])
        self.assertIsNotNone(states)
        self.assertEqual(states[0x4EDA2][0], "cmp")
        stmts, _ = lift_basic_block(lifter, blocks[2],
                                    flag_state=states[0x4EDA2])
        code = "\n".join(stmts)
        self.assertNotIn("_flags", code)
        self.assertIn("CMP_GE", code)

    def test_disagreeing_back_edge_still_unknown(self):
        blocks, preds = _pqdownheap_blocks()
        # The back edge now ends in a flag setter of a different kind that
        # shares no condition with cmp's jge: the head must stay unknown.
        blocks[4].instructions[1] = _insn(0x4EE1A, "shl", [_reg("ecx"), _imm(1)])
        lifter = Lifter()
        states = _solve_block_flag_states(
            blocks, preds, 0x4ED9C,
            lambda bb, st: lift_basic_block(lifter, bb, flag_state=st)[1])
        self.assertIsNotNone(states)
        self.assertIsNone(states[0x4EDA2])

    def test_entry_block_has_no_state(self):
        blocks, preds = _pqdownheap_blocks()
        lifter = Lifter()
        states = _solve_block_flag_states(
            blocks, preds, 0x4ED9C,
            lambda bb, st: lift_basic_block(lifter, bb, flag_state=st)[1])
        self.assertIsNone(states[0x4ED9C])


if __name__ == "__main__":
    unittest.main()
