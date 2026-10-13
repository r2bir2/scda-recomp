"""x87 fxtract must be lifted, not dropped (docs Update 427).

SCDA's fast log2 sub_00397510 is `fld dword [esp+4]; fxtract; faddp st(1);
fld1; fsubp st(1)` = exponent + significand - 1. fxtract was lifted to a
comment, so the function returned x + (whatever was in st1) - 1.
"""

import math
import pathlib
import re
import unittest

from .disasm import BasicBlock, Instruction
from .lifter import Lifter, lift_basic_block


def _lift(mnemonic):
    insn = Instruction(0x00397514, 2, mnemonic, "", "")
    stmts, _ = lift_basic_block(Lifter(), BasicBlock(start=insn.address,
                                                      instructions=[insn]))
    return "\n".join(stmts)


def _fxtract_model(v):
    """Python mirror of recomp_fxtract for the finite, nonzero case."""
    m, e = math.frexp(v)
    return m * 2.0, float(e - 1)


class FxtractTest(unittest.TestCase):
    def test_fxtract_is_lifted(self):
        code = _lift("fxtract")
        self.assertIn("recomp_fxtract(fp_top(), &_xt_sig, &_xt_exp);", code)
        self.assertIn("fp_top() = _xt_exp; fp_push(_xt_sig);", code)
        self.assertNotIn("FPU: fxtract", code)

    def test_fast_log2_identity(self):
        # exponent + significand - 1 must approximate log2 and be exact at
        # powers of two, as sub_00397510 relies on.
        for v, want in ((1.0, 0.0), (2.0, 1.0), (0.5, -1.0), (48000.0, None)):
            sig, exp = _fxtract_model(v)
            self.assertTrue(1.0 <= sig < 2.0)
            approx = exp + sig - 1.0
            if want is not None:
                self.assertEqual(approx, want)
            else:
                self.assertAlmostEqual(approx, math.log2(v), delta=0.09)

    def test_runtime_defines_fxtract(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        text = (root / "templates" / "runtime" / "recomp_types.h").read_text(
            encoding="utf-8")
        self.assertRegex(text, re.escape(
            "static inline void recomp_fxtract(double v, double *sig, "
            "double *exponent)"))


if __name__ == "__main__":
    unittest.main()
