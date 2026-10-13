"""Forward rep movs/stosb must keep element-sized accesses for device memory.

SCDA's DirectSound copies GP DSP memory out of the APU window with
`rep movsd` (sub_004D213E). Lifted to a host memcpy, that became a 32-byte
AVX load the APU trap handler cannot decode, and a `rep stosb` clear there
became misaligned 4-byte stores (docs Update 345). The forward forms now go
through runtime helpers that fall back to per-element accesses for guest
addresses in the device apertures.
"""

import pathlib
import re
import unittest

from .disasm import Instruction
from .lifter import Lifter

_RUNTIME = (pathlib.Path(__file__).resolve().parents[2]
            / "templates" / "runtime" / "recomp_types.h")


def _lift(m):
    return "\n".join(Lifter()._lift_rep_string(Instruction(0x1000, 2, m, "", ""), m))


class RepDeviceTest(unittest.TestCase):
    def test_movs_use_device_aware_helper(self):
        for m, size in (("rep movsb", 1), ("rep movsw", 2), ("rep movsd", 4)):
            code = _lift(m)
            self.assertIn(f"recomp_rep_movs_fwd(edi, esi, ecx, {size})", code, m)
            self.assertNotIn("memcpy(", code, m)

    def test_stosb_uses_device_aware_helper(self):
        code = _lift("rep stosb")
        self.assertIn("recomp_rep_stosb_fwd(edi, (uint8_t)eax, ecx)", code)
        self.assertNotIn("memset(", code)

    def test_runtime_defines_helpers_with_element_path(self):
        h = _RUNTIME.read_text(encoding="utf-8")
        self.assertRegex(h, r"static inline void recomp_rep_movs_fwd\(")
        self.assertRegex(h, r"static inline void recomp_rep_stosb_fwd\(")
        self.assertRegex(h, r"#define RECOMP_IS_DEVICE_VA\(a\) \(\(uint32_t\)\(a\) >= 0xFD000000u\)")


if __name__ == "__main__":
    unittest.main()
