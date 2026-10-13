import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block
from .translator import _flag_states_agree


def _reg(name):
    return Operand(type="reg", reg=name)


def _mem(base, disp, size=4):
    return Operand(type="mem", mem_base=base, mem_disp=disp, mem_size=size)


class FlagMergeTest(unittest.TestCase):
    """A jcc at a join point whose predecessors all end in a cmp.

    SCDA's linker VerifyImport (0x7EB3E..0x7EB4D) is exactly this shape:

        0x7EB3E: cmp dword ptr [ebx + 8], edi ; jmp 0x7eb4d
        0x7EB4B: cmp ecx, edi
        0x7EB4D: je 0x7eb60

    Both predecessors snapshot their compare into _fa/_fb, so the join may
    use either state. Treating the differing operands as a conflict lowered
    the je to the never-assigned _flags -- the "export found" branch was
    never taken and every import lookup failed.
    """

    def test_cmps_with_different_operands_agree(self):
        a = ("cmp", [_mem("ebx", 8), _reg("edi")])
        b = ("cmp", [_reg("ecx"), _reg("edi")])
        self.assertTrue(_flag_states_agree(a, b))

    def test_tests_with_different_operands_agree(self):
        a = ("test", [_reg("eax"), _reg("eax")])
        b = ("test", [_mem("esi", 0x10), _reg("ecx")])
        self.assertTrue(_flag_states_agree(a, b))

    def test_cmp_and_test_do_not_agree(self):
        # Same snapshot variables, different meaning: ZF of a - b vs a & b.
        a = ("cmp", [_reg("ecx"), _reg("edi")])
        b = ("test", [_reg("ecx"), _reg("edi")])
        self.assertFalse(_flag_states_agree(a, b))

    def test_different_widths_do_not_agree(self):
        # js/jns read the sign at the compare's width.
        a = ("cmp", [_mem("esi", 0, size=1), Operand(type="imm", imm=0)])
        b = ("cmp", [_reg("ecx"), _reg("edi")])
        self.assertFalse(_flag_states_agree(a, b))

    def test_other_setters_still_need_an_exact_match(self):
        # add/sub/... are re-read from their operands, so those must match.
        a = ("sub", [_reg("eax"), _reg("ecx")])
        b = ("sub", [_reg("eax"), _reg("edx")])
        self.assertFalse(_flag_states_agree(a, b))
        self.assertTrue(_flag_states_agree(a, ("sub", [_reg("eax"), _reg("ecx")])))

    def test_unknown_state_never_agrees(self):
        self.assertFalse(_flag_states_agree(None, ("cmp", [_reg("ecx"), _reg("edi")])))
        self.assertFalse(_flag_states_agree(None, None))

    def test_join_je_reads_the_snapshot(self):
        je = Instruction(0x7EB4D, 2, "je", "0x7eb60", "7411")
        je.operands = [Operand(type="imm", imm=0x7EB60)]
        je.jump_target = 0x7EB60
        incoming = ("cmp", [_mem("ebx", 8), _reg("edi")])
        lifted, _ = lift_basic_block(
            Lifter(), BasicBlock(start=0x7EB4D, instructions=[je]),
            flag_state=incoming)
        generated = "\n".join(lifted)
        self.assertIn("CMP_EQ(_fa, _fb)", generated)
        self.assertNotIn("_flags", generated)


if __name__ == "__main__":
    unittest.main()
