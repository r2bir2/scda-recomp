"""rdtsc must produce edx:eax from the runtime's guest TSC.

It used to lift to a TODO comment. RAD's timer in Bink (SCDA sub_004BC570,
QueryPerformanceCounter = sub_00371B1F: `rdtsc; mov [ecx],eax;
mov [ecx+4],edx`) then read stale registers, and its 5-minute sound resync
fired at random during logos.bik (docs Update 344).
"""

import pathlib
import re
import unittest

from .disasm import Instruction
from .lifter import Lifter

_RUNTIME = (pathlib.Path(__file__).resolve().parents[2]
            / "templates" / "runtime" / "recomp_types.h")


class RdtscTest(unittest.TestCase):
    def test_rdtsc_writes_edx_eax(self):
        insn = Instruction(0x371B23, 2, "rdtsc", "", "0f31")
        code = "\n".join(Lifter().lift_instruction(insn))
        self.assertIn("recomp_rdtsc()", code)
        self.assertIn("eax = (uint32_t)_tsc;", code)
        self.assertIn("edx = (uint32_t)(_tsc >> 32);", code)
        self.assertNotIn("TODO", code)

    def test_runtime_declares_recomp_rdtsc(self):
        header = _RUNTIME.read_text(encoding="utf-8")
        self.assertTrue(re.search(r"uint64_t\s+recomp_rdtsc\s*\(\s*void\s*\)\s*;",
                                  header))


if __name__ == "__main__":
    unittest.main()
