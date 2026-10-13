"""Regression checks for NEG's carry flag used by SBB boolean idioms."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Operand
from tools.recomp.lifter import Lifter


def test_neg_defines_carry_for_sbb():
    lifter = Lifter()
    # _lift_neg only materializes CF when the block-level analysis proved a
    # later ADC/SBB consumes it.  This is the same context supplied by
    # lift_basic_block for the real `neg; sbb reg,reg` idiom.
    neg = lifter._lift_neg(None, [Operand(type="reg", reg="eax")],
                            preserve_carry=True)
    sbb = lifter._lift_sbb(None, [Operand(type="reg", reg="eax"),
                                  Operand(type="reg", reg="eax")])
    assert "_cf = (_neg_operand != 0)" in neg[0]
    assert "_neg_operand" in neg[0]
    assert "_cf ? 0xFFFFFFFF : 0" in sbb[0]
    print("ok  NEG defines CF for SBB self boolean idiom")


if __name__ == "__main__":
    test_neg_defines_carry_for_sbb()
