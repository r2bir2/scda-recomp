"""Jump tables indexed downwards from their displacement (docs Update 333).

MSVC's memcpy copies its trailing bytes backwards with

    neg ecx
    jmp dword ptr [ecx*4 + TrailDownVecEnd]

so the reachable entries sit *below* the displacement. SCDA's memcpy
(0x00367270) dispatches at 0x00367436 through 0x00367508 with seven entries at
0x003674EC..0x00367504; only reading upwards cut the function short and turned
its tail copy and epilogue into an unresolved tail jump.

    py -3 -m unittest tools.disasm.test_backward_jump_table
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402
from capstone.x86 import X86_REG_ECX, X86_REG_EDX  # noqa: E402

from tools.disasm.engine import DisasmEngine, Instruction  # noqa: E402

LO, HI = 0x00367000, 0x00368000
DISP = 0x00367508
ENTRIES = [0x0036750C, 0x00367514, 0x0036751C, 0x00367524,
           0x0036752C, 0x00367534, 0x0036753C]          # 0x3674EC..0x367504
INDEX0 = 0x0036754F                                     # [DISP]


class _Section:
    virtual_addr = LO
    virtual_size = HI - LO
    executable = True


class _Image:
    def __init__(self, words):
        self.words = words

    def get_section_at_va(self, va):
        return _Section() if LO <= va < HI else None

    def read_u32_at_va(self, va):
        return self.words.get(va, 0x90909090)


def _engine(prev_mnemonic="neg", prev_op="ecx", index_reg=X86_REG_ECX):
    words = {DISP - 4 * (len(ENTRIES) - i): t for i, t in enumerate(ENTRIES)}
    words[DISP] = INDEX0
    words[DISP + 4] = 0x1C8E448B                         # code again
    eng = DisasmEngine.__new__(DisasmEngine)
    eng.image = _Image(words)
    eng._cs = Cs(CS_ARCH_X86, CS_MODE_32)
    eng._sorted_addrs = None
    eng.jump_tables = {}
    eng.backward_jump_tables = {}
    eng._jt_candidates = {DISP}
    eng._jt_sites = {DISP: [(0x00367436, index_reg)]}
    eng.instructions = {
        0x00367434: Instruction(0x00367434, 2, prev_mnemonic, prev_op, "f7d9"),
        0x00367436: Instruction(0x00367436, 7, "jmp",
                                "dword ptr [ecx*4 + 0x367508]", "ff248d08753600"),
    }
    eng.decoded_at = []
    eng.decode_at = lambda addr, max_insns=4096: eng.decoded_at.append(addr) or 0
    eng.get_instructions_in_range = lambda a, b: [
        i for i in eng.instructions.values() if a <= i.address < b]
    return eng


class BackwardJumpTableTest(unittest.TestCase):
    def test_memcpy_trail_down_table_is_found(self):
        eng = _engine()
        self.assertTrue(eng._resync_backward_table(DISP, 1, LO, HI, 3, 512))
        start = DISP - 4 * len(ENTRIES)
        self.assertEqual(eng.backward_jump_tables[DISP], (start, DISP + 4))
        self.assertEqual(eng.jump_tables[start], DISP + 4)
        self.assertEqual(eng.jump_table_entries(DISP), ENTRIES + [INDEX0])
        self.assertEqual(eng.decoded_at, [DISP + 4])

    def test_without_neg_the_table_is_left_alone(self):
        eng = _engine(prev_mnemonic="mov", prev_op="ecx, eax")
        self.assertFalse(eng._resync_backward_table(DISP, 1, LO, HI, 3, 512))
        self.assertEqual(eng.jump_tables, {})

    def test_neg_of_another_register_does_not_count(self):
        eng = _engine(prev_op="edx", index_reg=X86_REG_ECX)
        self.assertFalse(eng._resync_backward_table(DISP, 1, LO, HI, 3, 512))

    def test_neg_of_the_index_register_counts(self):
        eng = _engine(prev_op="edx", index_reg=X86_REG_EDX)
        self.assertTrue(eng._resync_backward_table(DISP, 1, LO, HI, 3, 512))


if __name__ == "__main__":
    unittest.main()
