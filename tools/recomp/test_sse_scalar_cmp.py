"""SSE scalar compares must produce the low-lane mask.

SCDA appSqrt, sub_00067AB9 (docs Update 355):

    rsqrtss xmm0, xmm1 ; Newton step ...  xmm3 = sqrt(x) (NaN for x == 0)
    xorps   xmm4, xmm4 (via [ebp-4] = 0)
    cmpneqss xmm4, xmm1              ; mask = (0 != x)
    andps   xmm3, xmm4               ; sqrt(0) -> 0

cmpneqss was emitted as a TODO comment, the mask stayed 0 and every square
root in the game returned 0.
"""

import math
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import (Lifter, lift_basic_block, _scalar_cmp_predicate,
                     _SCALAR_CMP_COND)


def _reg(name):
    return Operand(type="reg", reg=name)


def _insn(addr, mnemonic, ops):
    i = Instruction(addr, 4, mnemonic, "", "")
    i.operands = ops
    return i


def _lift(insn):
    stmts, _ = lift_basic_block(Lifter(), BasicBlock(start=insn.address,
                                                      instructions=[insn]))
    return "\n".join(stmts)


def _py(cond, a, b):
    expr = (cond.format(a="a", b="b").replace("!(", "not (")
            .replace("||", " or ").replace("&&", " and "))
    return eval(expr, {}, {"a": a, "b": b})


class ScalarCompareTest(unittest.TestCase):
    def test_cmpneqss_writes_low_lane_mask(self):
        code = _lift(_insn(0x67B00, "cmpneqss", [_reg("xmm4"), _reg("xmm1")]))
        self.assertNotIn("TODO", code)
        self.assertIn("xmm4.u[0] =", code)
        self.assertIn("0xFFFFFFFFu", code)
        self.assertIn("xmm1.f[0]", code)

    def test_cmpss_imm_form_and_double(self):
        self.assertEqual(_scalar_cmp_predicate(
            "cmpss", [_reg("xmm0"), _reg("xmm1"), Operand(type="imm", imm=1)]),
            ("lt", 4))
        self.assertEqual(_scalar_cmp_predicate(
            "cmpnlesd", [_reg("xmm0"), _reg("xmm1")]), ("nle", 8))
        code = _lift(_insn(0x1000, "cmpeqsd", [_reg("xmm2"), _reg("xmm3")]))
        self.assertIn("xmm2.q[0] =", code)
        self.assertIn("xmm3.d[0]", code)

    def test_string_cmpsd_is_not_sse(self):
        mem = Operand(type="mem", mem_size=4)
        self.assertIsNone(_scalar_cmp_predicate("cmpsd", [mem, mem]))

    def test_packed_sqrt_family_uses_all_lanes(self):
        for m, helper in (("sqrtps", "XMM_SQRT"), ("rsqrtps", "XMM_RSQRT"),
                          ("rcpps", "XMM_RCP")):
            code = _lift(_insn(0x2000, m, [_reg("xmm0"), _reg("xmm1")]))
            self.assertIn(f"xmm0 = {helper}(xmm1);", code)
            self.assertNotIn("low lane", code)

    def test_conversions_honour_rounding_mode(self):
        eax = _reg("eax")
        code = _lift(_insn(0x3000, "cvtss2si", [eax, _reg("xmm0")]))
        self.assertIn("recomp_cvtss2si(xmm0.f[0])", code)
        self.assertNotIn("(int32_t)xmm0", code)
        code = _lift(_insn(0x3000, "cvttss2si", [eax, _reg("xmm0")]))
        self.assertIn("recomp_cvttss2si(xmm0.f[0])", code)
        code = _lift(_insn(0x3000, "cvtsd2si", [eax, _reg("xmm1")]))
        self.assertIn("recomp_cvtsd2si(xmm1.d[0])", code.replace("xmm1.f[0]", "xmm1.d[0]"))

    def test_mxcsr_load_store(self):
        mem = Operand(type="mem", mem_size=4)
        self.assertIn("recomp_ldmxcsr(MEM32(",
                      _lift(_insn(0x3100, "ldmxcsr", [mem])))
        self.assertIn("= recomp_stmxcsr();",
                      _lift(_insn(0x3100, "stmxcsr", [mem])))

    def test_runtime_defines_rounding_helpers(self):
        import pathlib
        text = (pathlib.Path(__file__).resolve().parents[2] / "templates"
                / "runtime" / "recomp_types.h").read_text(encoding="utf-8")
        for name in ("recomp_x87_round", "recomp_cvtss2si", "recomp_cvttss2si",
                     "recomp_cvtsd2si", "recomp_cvttsd2si", "recomp_ldmxcsr",
                     "recomp_stmxcsr", "XMM_SQRT", "XMM_RSQRT", "XMM_RCP"):
            self.assertIn(name, text)

    def test_rotates_are_width_aware_and_rcr_uses_carry(self):
        one = Operand(type="imm", imm=1)
        lifter = Lifter()
        lifter.needs_cf = True
        stmts, _ = lift_basic_block(lifter, BasicBlock(start=0x4000, instructions=[
            _insn(0x4000, "rcr", [_reg("eax"), one])]))
        self.assertIn("recomp_rcr(eax, (uint32_t)(1), 32, &_cf)", "\n".join(stmts))
        code = _lift(_insn(0x4000, "rol", [_reg("al"), one]))
        self.assertIn("recomp_rol(LO8(eax), (uint32_t)(1), 8)", code)

    def test_nan_semantics(self):
        nan = math.nan
        expect = {  # (1,2), (2,2), (nan,1)
            "eq": (False, True, False), "lt": (True, False, False),
            "le": (True, True, False), "unord": (False, False, True),
            "neq": (True, False, True), "nlt": (False, True, True),
            "nle": (False, False, True), "ord": (True, True, False),
        }
        for pred, (x, y, z) in expect.items():
            cond = _SCALAR_CMP_COND[pred]
            self.assertEqual(_py(cond, 1.0, 2.0), x, pred)
            self.assertEqual(_py(cond, 2.0, 2.0), y, pred)
            self.assertEqual(_py(cond, nan, 1.0), z, pred)


if __name__ == "__main__":
    unittest.main()
