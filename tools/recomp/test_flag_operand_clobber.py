"""Flag conditions must survive a flag-preserving write to their operands.

SCDA's DirectSound EP-module lookup sub_004D8E47 (docs Update 344):

    0x4D8E5F: dec eax
    0x4D8E60: mov eax, dword ptr [ebp + 0xc]   ; mov leaves EFLAGS alone
    0x4D8E63: je  0x4d8e6e                     ; ZF of the dec

The je used to be lowered to `(eax == 0)` after the mov had already
replaced eax, so case 5 never matched and EP module 6 got length 0.
"""

import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _reg(name):
    return Operand(type="reg", reg=name)


def _mem(base, disp, size=4):
    return Operand(type="mem", mem_base=base, mem_disp=disp, mem_size=size)


def _insn(addr, mnemonic, ops, target=None):
    i = Instruction(addr, 2, mnemonic, "", "")
    i.operands = ops
    i.jump_target = target
    return i


def _lift(insns, flag_state=None):
    stmts, state = lift_basic_block(
        Lifter(), BasicBlock(start=insns[0].address, instructions=insns),
        flag_state=flag_state)
    return "\n".join(stmts), state


class FlagOperandClobberTest(unittest.TestCase):
    def test_dec_mov_je_reads_saved_decrement(self):
        code, _ = _lift([
            _insn(0x4D8E5F, "dec", [_reg("eax")]),
            _insn(0x4D8E60, "mov", [_reg("eax"), _mem("ebp", 0xC)]),
            _insn(0x4D8E63, "je", [Operand(type="imm", imm=0x4D8E6E)],
                  target=0x4D8E6E),
        ])
        save = code.index("_fsv0 = eax;")
        load = code.index("eax = MEM32(ebp + 0xC);")
        self.assertLess(save, load, code)
        self.assertIn("(_fsv0 == 0)", code)
        self.assertNotIn("if ((eax == 0))", code)

    def test_untouched_operands_are_not_copied(self):
        code, _ = _lift([
            _insn(0x1000, "dec", [_reg("eax")]),
            _insn(0x1001, "mov", [_reg("ecx"), _mem("ebp", 0xC)]),
            _insn(0x1004, "je", [Operand(type="imm", imm=0x2000)],
                  target=0x2000),
        ])
        self.assertNotIn("_fsv", code)
        self.assertIn("(eax == 0)", code)

    def test_sub_register_write_clobbers_family(self):
        # `add ecx, edx ; mov cl, 5 ; jb` -- jb after add re-reads both
        # operands (CF via _cf here, ja also reads the result).
        code, _ = _lift([
            _insn(0x1000, "and", [_reg("ecx"), _reg("edx")]),
            _insn(0x1002, "mov", [_reg("cl"), Operand(type="imm", imm=5)]),
            _insn(0x1004, "jne", [Operand(type="imm", imm=0x2000)],
                  target=0x2000),
        ])
        self.assertIn("_fsv0 = ecx;", code)
        self.assertIn("(_fsv0 != 0)", code)

    def test_byte_width_is_kept(self):
        # js after a byte op tests bit 7 of the byte, also once saved.
        code, _ = _lift([
            _insn(0x1000, "dec", [_reg("al")]),
            _insn(0x1002, "movzx", [_reg("eax"), _mem("esi", 0, size=1)]),
            _insn(0x1005, "je", [Operand(type="imm", imm=0x2000)],
                  target=0x2000),
        ])
        self.assertIn("_fsv0b = LO8(eax);", code)
        self.assertIn("(_fsv0b == 0)", code)

    def test_memory_operand_saved_before_store(self):
        code, _ = _lift([
            _insn(0x1000, "dec", [_mem("esi", 8)]),
            _insn(0x1003, "mov", [_mem("edi", 0), _reg("eax")]),
            _insn(0x1005, "jne", [Operand(type="imm", imm=0x2000)],
                  target=0x2000),
        ])
        self.assertLess(code.index("_fsv0 = MEM32(esi + 8);"),
                        code.index("MEM32(edi) = eax;"), code)
        self.assertIn("(_fsv0 != 0)", code)

    def test_saved_state_flows_to_successor(self):
        _, state = _lift([
            _insn(0x1000, "dec", [_reg("eax")]),
            _insn(0x1001, "pop", [_reg("eax")]),
        ])
        code, _ = _lift([_insn(0x1002, "jne", [Operand(type="imm", imm=0x2000)],
                               target=0x2000)], flag_state=state)
        self.assertIn("(_fsv0 != 0)", code)

    def test_no_copy_when_flags_are_redefined_first(self):
        # Frame setup: the sub's flags die at the test. No copy may appear --
        # translator._fixup_icall_esp_save matches these prologue lines.
        code, _ = _lift([
            _insn(0x1000, "sub", [_reg("esp"), Operand(type="imm", imm=0x18)]),
            _insn(0x1003, "push", [_reg("ebx")]),
            _insn(0x1004, "push", [_reg("esi")]),
            _insn(0x1005, "test", [_reg("ecx"), _reg("ecx")]),
            _insn(0x1007, "jne", [Operand(type="imm", imm=0x2000)],
                  target=0x2000),
        ])
        self.assertNotIn("_fsv", code)

    def test_call_ends_the_lookahead(self):
        # The callee leaves its own flags; nothing after it reads the dec's.
        code, _ = _lift([
            _insn(0x1000, "dec", [_reg("eax")]),
            _insn(0x1001, "push", [_reg("eax")]),
            _insn(0x1002, "call", [Operand(type="imm", imm=0x3000)]),
            _insn(0x1007, "jne", [Operand(type="imm", imm=0x2000)],
                  target=0x2000),
        ])
        self.assertNotIn("_fsv", code)

    def test_cmp_uses_its_own_snapshot(self):
        code, _ = _lift([
            _insn(0x1000, "cmp", [_reg("eax"), _reg("ecx")]),
            _insn(0x1002, "mov", [_reg("eax"), _mem("ebp", 0xC)]),
            _insn(0x1005, "je", [Operand(type="imm", imm=0x2000)],
                  target=0x2000),
        ])
        self.assertNotIn("_fsv", code)
        self.assertIn("CMP_EQ(_fa, _fb)", code)


if __name__ == "__main__":
    unittest.main()
