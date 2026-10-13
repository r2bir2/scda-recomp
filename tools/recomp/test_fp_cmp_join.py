"""Float-compare flags must survive block joins and honour NaN.

SCDA sub_0008C1xx (docs Update 354):

    0x0008C215: comiss xmm1, [ebx+0x764]
    0x0008C21A: jbe 0x8c24e
    ...
    0x0008C233: comiss xmm2, xmm1
    0x0008C238: movss [ebx+0x760], xmm1    ; join (label)
    0x0008C240: jbe 0x8c24e

The two comiss predecessors differ in operands, so the join inherited no
flag state and the jbe lowered to the never-assigned _flags (always false).
Every float compare now snapshots into g_fp_cmp and joins of them agree.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block, _make_condition
from .translator import _merge_flag_states


def _reg(name):
    return Operand(type="reg", reg=name)


def _insn(addr, mnemonic, ops, op_str=""):
    i = Instruction(addr, 3, mnemonic, op_str, "")
    i.operands = ops
    return i


class FpCompareJoinTest(unittest.TestCase):
    def test_comiss_snapshots_g_fp_cmp(self):
        stmts, state = lift_basic_block(Lifter(), BasicBlock(
            start=0x8C233, instructions=[
                _insn(0x8C233, "comiss", [_reg("xmm2"), _reg("xmm1")],
                      "xmm2, xmm1")]))
        code = "\n".join(stmts)
        self.assertIn("g_fp_cmp = RECOMP_FCMP(xmm2.f[0], xmm1.f[0])", code)
        self.assertEqual(state[0], "comiss")

    def test_differing_float_compares_merge(self):
        a = ("comiss", [_reg("xmm1"), _reg("xmm3")])
        b = ("comiss", [_reg("xmm2"), _reg("xmm1")])
        c = ("fcomip", [_reg("st(1)")])
        self.assertEqual(_merge_flag_states(a, b), a)
        self.assertEqual(_merge_flag_states(a, c), a)
        self.assertIsNone(_merge_flag_states(a, ("cmp", [_reg("eax"), _reg("ecx")])))

    def test_jbe_at_join_reads_g_fp_cmp(self):
        stmts, _ = lift_basic_block(Lifter(), BasicBlock(
            start=0x8C238, instructions=[
                _insn(0x8C240, "jbe", [Operand(type="imm", imm=0x8C24E)])]),
            flag_state=("comiss", [_reg("xmm2"), _reg("xmm1")]))
        code = "\n".join(stmts)
        self.assertIn("g_fp_cmp != 1", code)
        self.assertNotIn("_flags", code)

    def test_unordered_sets_zf_pf_cf(self):
        cond = lambda j: _make_condition(j, "comiss", [])[0]
        self.assertIn("g_fp_cmp == 2", cond("jb"))
        self.assertIn("g_fp_cmp == 2", cond("je"))
        self.assertIn("g_fp_cmp == 2", cond("jp"))
        self.assertEqual(cond("ja").split(" /*")[0], "(g_fp_cmp == 1)")
        self.assertNotIn("2", cond("jne").split(" /*")[0])

    def test_fcom_invalidates_tracked_float_flags(self):
        stmts, state = lift_basic_block(Lifter(), BasicBlock(
            start=0x1000, instructions=[
                _insn(0x1000, "fcomp", [], "")]),
            flag_state=("comiss", [_reg("xmm0"), _reg("xmm1")]))
        self.assertIsNone(state)


class ResultSetterJoinTest(unittest.TestCase):
    """SCDA 0x0030A4E3: `and ebx, 0x800001FF; jns L` / `dec; or; inc ebx`
    both reach L: `je` there asks whether ebx is zero."""

    def test_and_inc_same_dest_merge(self):
        a = ("and", [_reg("ebx"), Operand(type="imm", imm=0x800001FF)])
        b = ("inc", [_reg("ebx")])
        self.assertEqual(_merge_flag_states(a, b), ("zsres", [_reg("ebx")]))
        self.assertIsNone(_merge_flag_states(a, ("inc", [_reg("ecx")])))
        self.assertIsNone(_merge_flag_states(a, ("shl", [_reg("ebx")])))

    def test_zsres_conditions(self):
        self.assertEqual(_make_condition("je", "zsres", [_reg("ebx")])[0], "(ebx == 0)")
        self.assertEqual(_make_condition("js", "zsres", [_reg("bl")])[0],
                         "((int8_t)LO8(ebx) < 0)")
        self.assertIsNone(_make_condition("jb", "zsres", [_reg("ebx")]))

    def test_narrow_sign_uses_operand_width(self):
        cond = _make_condition("js", "and", [_reg("al"), Operand(type="imm", imm=0x80)])[0]
        self.assertIn("(int8_t)", cond)


if __name__ == "__main__":
    unittest.main()
