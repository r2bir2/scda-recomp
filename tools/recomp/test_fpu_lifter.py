"""Regression checks for x87 stack-depth semantics in the C lifter.

Run: py -3 tools/recomp/test_fpu_lifter.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import Lifter  # noqa: E402


class _Op:
    def __init__(self, kind, *, mem_size=0, reg=""):
        self.type = kind
        self.mem_size = mem_size
        self.reg = reg
        self.mem_base = None
        self.mem_index = None
        self.mem_scale = 1
        self.mem_disp = 0


class _Insn:
    op_str = ""


def _lift(mnemonic, operands):
    # _lift_fpu only uses instance state through helper methods already present
    # on the class, so avoid a full disassembler/database setup in this unit test.
    return Lifter._lift_fpu(object.__new__(Lifter), _Insn(), mnemonic, operands)[0]


def test_fst_does_not_pop_and_fstp_does():
    mem = _Op("mem", mem_size=8)
    assert "fp_pop" not in _lift("fst", [mem])
    assert "fp_pop" in _lift("fstp", [mem])
    print("ok  fst stack depth")


def test_nonp_arithmetic_does_not_pop():
    mem = _Op("mem", mem_size=8)
    assert "fp_pop" not in _lift("fmul", [mem])
    assert "fp_top() = fp_top() * MEMD(" in _lift("fmul", [mem])
    assert "fp_pop" in _lift("fmulp", [_Op("reg", reg="st(1)")])
    print("ok  arithmetic stack depth")


def test_fcomp_and_fistp_pop_only_when_required():
    mem = _Op("mem", mem_size=8)
    assert "fp_pop" not in _lift("fcom", [mem])
    assert "fp_pop" in _lift("fcomp", [mem])
    assert "fp_pop" in _lift("fistp", [mem])
    print("ok  compare/integer-store stack depth")


if __name__ == "__main__":
    test_fst_does_not_pop_and_fstp_does()
    test_nonp_arithmetic_does_not_pop()
    test_fcomp_and_fistp_pop_only_when_required()
