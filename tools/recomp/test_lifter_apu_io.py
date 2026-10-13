"""Plain loads/stores in APU driver functions use the direct IOR/IOW path.

Every APU register access used to fault into the vectored handler; SCDA's
DirectSound voice update (sub_004D4D14: `mov [ebx], edx` over 0xFE820400..
0xFE82043C, plus constant-address stores) cost ~22,000 exceptions per second
on the game thread (docs Update 643). The translator flags functions that
name an APU register by constant (lifter.io_function); only those take the
checked accessors, and stack slots never do.
"""

import pathlib
import unittest

from .disasm import Disassembler
from .lifter import Lifter

_RUNTIME = (pathlib.Path(__file__).resolve().parents[2]
            / "templates" / "runtime" / "recomp_types.h")


def _lift(code_hex, io_function=True):
    code = bytes.fromhex(code_hex)
    insn, = Disassembler().disassemble_function(code, 0x4D4E1E,
                                                0x4D4E1E + len(code))
    lifter = Lifter()
    lifter.io_function = io_function
    return "\n".join(lifter.lift_instruction(insn))


class ApuIoTest(unittest.TestCase):
    def test_store_through_register_pointer(self):
        # mov [ebx], edx
        self.assertEqual(_lift("8913"), "IOW32(ebx, edx);")

    def test_constant_load(self):
        # mov ecx, [0xFE820010]
        self.assertIn("ecx = IOR32(", _lift("8b0d100082fe"))

    def test_constant_store_imm(self):
        # mov dword ptr [0xFE8202FC], 1
        code = _lift("c705fc0282fe01000000")
        self.assertTrue(code.startswith("IOW32("), code)
        self.assertIn(", 1);", code)

    def test_byte_load_into_low_register(self):
        # mov al, [esi+0x10]
        self.assertIn("IOR8(esi + 0x10)", _lift("8a4610"))

    def test_stack_slots_stay_plain(self):
        # mov eax, [ebp-8] / mov [esp+4], eax
        self.assertNotIn("IOR", _lift("8b45f8"))
        self.assertNotIn("IOW", _lift("89442404"))

    def test_other_functions_untouched(self):
        self.assertEqual(_lift("8913", io_function=False), "MEM32(ebx) = edx;")

    def test_runtime_defines_accessors(self):
        header = _RUNTIME.read_text(encoding="utf-8")
        for name in ("IOR8", "IOR16", "IOR32", "IOW8", "IOW16", "IOW32"):
            self.assertIn(f"#define {name}(", header)
        self.assertIn("recomp_io_write(uint32_t va", header)

    def test_read_modify_and_test_forms(self):
        # test dword ptr [eax+0x100410], 0x10000 (D3D kick spin, NV2A PFB)
        self.assertIn("IOR32(eax + 0x100410)", _lift("f7801004100000000100"))
        # or dword ptr [esi+8], 0x2000: read and write both checked
        code = _lift("814e0800200000")
        self.assertIn("IOW32(esi + 8, ", code)
        self.assertIn("IOR32(esi + 8)", code)


if __name__ == "__main__":
    unittest.main()
