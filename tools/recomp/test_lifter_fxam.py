"""x87 fxam and fcmovcc must be lifted, not dropped (docs Update 406).

SCDA's CRT fmod (0x368712 -> _trandisp2 at 0x36D173) classifies both
operands with `fxam; fnstsw [ebp-0xa0]` and dispatches on C3/C2/C0 through
an xlatb table. fxam was lifted to a comment, so fnstsw reported the last
compare: fmod took an arbitrary path, and the Perlin noise sub_000833C7
built on it drove Sam's upper-arm bone controller in per-frame jumps.
"""

import pathlib
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _reg(name):
    return Operand(type="reg", reg=name)


def _insn(addr, mnemonic, ops, op_str=""):
    i = Instruction(addr, 2, mnemonic, op_str, "")
    i.operands = ops
    return i


def _lift(*insns):
    stmts, _ = lift_basic_block(Lifter(), BasicBlock(start=insns[0].address,
                                                      instructions=list(insns)))
    return "\n".join(stmts)


class FxamTest(unittest.TestCase):
    def test_fxam_sets_raw_condition_codes(self):
        code = _lift(_insn(0x36D139, "fxam", []))
        self.assertIn("g_fp_cc = recomp_fxam(fp_top());", code)
        self.assertIn("g_fp_cmp = RECOMP_FP_CMP_RAW;", code)
        self.assertNotIn("FPU: fxam", code)

    def test_fnstsw_reports_raw_codes_after_fxam(self):
        mem = Operand(type="mem", mem_size=2, mem_base="ebp", mem_disp=-0xA0)
        code = _lift(_insn(0x36D139, "fxam", []),
                     _insn(0x36D142, "fnstsw", [mem], "word ptr [ebp - 0xa0]"))
        self.assertIn("g_fp_cmp == RECOMP_FP_CMP_RAW ? (uint32_t)g_fp_cc", code)

    def test_fcmovnbe_after_fcomi_moves_on_above(self):
        code = _lift(_insn(0x3969BE, "fcomi", [_reg("st(1)")], "st(1)"),
                     _insn(0x3969C2, "fcmovnbe", [_reg("st(0)"), _reg("st(1)")],
                           "st(0), st(1)"))
        self.assertIn("RECOMP_FCMP(fp_top(), fp_st1())", code)
        self.assertIn("if ((g_fp_cmp == 1)", code)
        self.assertIn(") fp_top() = fp_st1(); /* fcmovnbe */", code)
        self.assertNotIn("FPU: fcmovnbe", code)

    def test_runtime_defines_fxam_classifier(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        text = (root / "templates" / "runtime" / "recomp_types.h").read_text(
            encoding="utf-8")
        self.assertIn("static inline uint16_t recomp_fxam(double v)", text)
        self.assertIn("#define RECOMP_FP_CMP_RAW 3", text)
        kernel = (root / "src" / "kernel" / "xbox_memory_layout.c").read_text(
            encoding="utf-8")
        self.assertIn("RECOMP_TLS uint16_t g_fp_cc", kernel)


if __name__ == "__main__":
    unittest.main()
