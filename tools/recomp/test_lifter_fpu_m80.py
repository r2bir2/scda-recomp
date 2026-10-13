"""x87 80-bit memory operands must be lifted as extended precision.

docs Update 459: `fld tbyte ptr [0x5DB9CE]` (the retail CRT's exp limit
32767.5) was lifted as MEMF -- a 32-bit float read of the low mantissa
bytes, i.e. 0.0 -- so every exp()/pow() took the overflow branch, and
`fstp tbyte ptr [...]` fell through to a comment that dropped both the
store and the pop.
"""

import math
import pathlib
import re
import struct
import unittest

from .disasm import BasicBlock, Disassembler
from .lifter import Lifter, lift_basic_block


def _lift_bytes(hexbytes, addr=0x0036D066):
    code = bytes.fromhex(hexbytes)
    insns = Disassembler().disassemble_function(code, addr, addr + len(code))
    stmts, _ = lift_basic_block(Lifter(), BasicBlock(start=addr, instructions=insns))
    return "\n".join(stmts)


def _ld80_model(raw):
    """Python mirror of recomp_ld80 for finite values."""
    mant, se = struct.unpack("<QH", raw)
    exp = se & 0x7FFF
    v = math.ldexp(float(mant), (exp or 1) - 16383 - 63)
    return -v if se & 0x8000 else v


def _st80_model(v):
    """Python mirror of recomp_st80 for finite nonzero values."""
    f, e = math.frexp(abs(v))
    mant = int(math.ldexp(f, 64))
    se = ((e - 1 + 16383) & 0x7FFF) | (0x8000 if v < 0 else 0)
    return struct.pack("<QH", mant, se)


class FpuM80Test(unittest.TestCase):
    def test_fld_m80_uses_extended_load(self):
        code = _lift_bytes("db2dceb95d00")  # fld xword ptr [0x5db9ce]
        self.assertIn("fp_push(recomp_ld80(", code)
        self.assertNotIn("MEMF", code)

    def test_fstp_m80_stores_and_pops(self):
        code = _lift_bytes("dbbd62ffffff")  # fstp xword ptr [ebp - 0x9e]
        self.assertIn("recomp_st80(", code)
        self.assertIn("fp_pop();", code)

    def test_crt_constants_decode(self):
        # The two CRT constants from the XBE: exp limit and +inf.
        self.assertEqual(_ld80_model(bytes.fromhex("000000000000ffff0d40")), 32767.5)
        self.assertEqual(_ld80_model(bytes.fromhex("0000000000000080ff3f")), 1.0)

    def test_st80_round_trip(self):
        for v in (1.0, -2.5, 32767.5, 1e-300, 3.141592653589793, -1e300):
            self.assertEqual(_ld80_model(_st80_model(v)), v)

    def test_runtime_defines_helpers(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        text = (root / "templates" / "runtime" / "recomp_types.h").read_text(
            encoding="utf-8")
        self.assertRegex(text, re.escape("static inline double recomp_ld80(uint32_t addr)"))
        self.assertRegex(text, re.escape(
            "static inline void recomp_st80(uint32_t addr, double v)"))


if __name__ == "__main__":
    unittest.main()
