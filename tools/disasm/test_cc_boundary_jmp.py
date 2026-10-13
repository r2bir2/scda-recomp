"""int3 padding after an unconditional jmp is a function boundary too
(docs Update 336).

SCDA 0x0033DA60:
    sub ecx, 0xC ; jmp 0x33DA80 ; int3 x8
    sub ecx, 0x10; jmp 0x33DA80 ; int3 x8      <- adjustor thunks
    0x0033DA80: push esi ...                   <- the method they forward to

    py -3 -m unittest tools.disasm.test_cc_boundary_jmp
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402

VA = 0x0033DA60
CODE = (bytes.fromhex("83e90c" "e918000000") + b"\xcc" * 8 +
        bytes.fromhex("83e910" "e908000000") + b"\xcc" * 8 +
        bytes.fromhex("568bf1" "c3"))
METHOD = VA + CODE.index(bytes.fromhex("568bf1"))


class _Section:
    virtual_addr = VA
    virtual_size = len(CODE)
    executable = True


class _Image:
    base_address = 0x00010000
    image_size = 0x01000000

    def get_section_data(self, section):
        return CODE


def _detector():
    eng = DisasmEngine.__new__(DisasmEngine)
    eng.image = _Image()
    eng._cs = Cs(CS_ARCH_X86, CS_MODE_32)
    eng._cs.detail = True
    eng.instructions = {}
    eng._jt_candidates = set()
    eng._jt_sites = {}
    eng._sorted_addrs = None
    for ins in eng._cs.disasm(CODE, VA):
        if ins.mnemonic == "int3":
            continue
        eng.instructions[ins.address] = eng._classify_instruction(ins)
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = eng
    det.image = eng.image
    det._candidates = {}
    added = []
    det._add_candidate = lambda addr, conf, method: added.append((addr, method))
    return det, added


class CcBoundaryAfterJmpTest(unittest.TestCase):
    def test_method_behind_thunk_padding_is_a_candidate(self):
        self.assertEqual(METHOD, 0x0033DA80)
        det, added = _detector()
        det._pass_cc_boundaries(_Section())
        self.assertIn((METHOD, "cc_boundary"), added)
        # the second thunk follows padding after the first thunk's jmp
        self.assertIn((0x0033DA70, "cc_boundary"), added)


if __name__ == "__main__":
    unittest.main()
