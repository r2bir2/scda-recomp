"""lahf must load AH from the flags of the preceding compare.

SCDA's viewport tick sub_001C97B3 (docs Update 347):

    0x1C97C2: ucomiss xmm0, dword ptr [0x5a6ed4]   ; DeltaTime vs -1.0f
    0x1C97C9: lahf
    0x1C97CA: test ah, 0x44                        ; ZF | PF
    0x1C97CD: jnp 0x1c97e0                         ; equal -> skip ReadInput

lahf lifted to a comment, so ah was whatever eax held, the branch always
skipped ReadInput and the title never saw a button (stuck at "Press START").
The idiom is MSVC's NaN-safe float (in)equality, ~170 sites in SCDA.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _reg(name):
    return Operand(type="reg", reg=name)


def _insn(addr, mnemonic, ops, target=None):
    i = Instruction(addr, 2, mnemonic, "", "")
    i.operands = ops
    i.jump_target = target
    return i


def _lift(insns):
    stmts, _ = lift_basic_block(
        Lifter(), BasicBlock(start=insns[0].address, instructions=insns))
    return "\n".join(stmts)


class LahfTest(unittest.TestCase):
    def _viewport_tick(self, setter="ucomiss"):
        return _lift([
            _insn(0x1C97C2, setter, [
                _reg("xmm0"),
                Operand(type="mem", mem_disp=0x5A6ED4, mem_size=4)]),
            _insn(0x1C97C9, "lahf", []),
            _insn(0x1C97CA, "test", [_reg("ah"), Operand(type="imm", imm=0x44)]),
            _insn(0x1C97CD, "jnp", [Operand(type="imm", imm=0x1C97E0)], target=0x1C97E0),
        ])

    def test_ucomiss_lahf_writes_ah_from_the_compare(self):
        code = self._viewport_tick()
        self.assertIn("eax = (eax & 0xFFFF00FFu) |", code)
        self.assertIn("lahf <- flags of ucomiss", code)
        # both operands of the compare, NaN -> ZF|PF|CF, equal -> ZF
        self.assertIn("xmm0.f[0]", code)
        self.assertIn("MEMF(", code)
        self.assertIn("0x45u", code)
        self.assertIn("0x40u", code)
        self.assertNotIn("lahf - load AH", code)
        # ...and the parity test after it reads that ah
        lahf = code.index("lahf <- flags")
        test = code.index("HI8(eax)")
        self.assertLess(lahf, test, code)
        self.assertIn("RECOMP_PARITY8", code)

    def test_ucomisd_reads_doubles(self):
        code = self._viewport_tick("ucomisd")
        self.assertIn("xmm0.d[0]", code)
        self.assertIn("MEMD(", code)

    def test_cmp_lahf_uses_the_snapshot(self):
        code = _lift([
            _insn(0x1000, "cmp", [_reg("eax"), _reg("ecx")]),
            _insn(0x1002, "lahf", []),
        ])
        self.assertIn("lahf <- flags of cmp", code)
        self.assertIn("(_fa - _fb)", code)
        self.assertIn("(_fa < _fb)", code)

    def test_unknown_flags_say_so(self):
        code = _lift([_insn(0x1000, "lahf", [])])
        self.assertIn("not modelled", code)
        self.assertNotIn("eax = (eax & 0xFFFF00FFu)", code)


if __name__ == "__main__":
    unittest.main()
