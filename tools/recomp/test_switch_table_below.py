import struct
import unittest
from unittest import mock

from .disasm import Operand
from .lifter import Lifter

BASE = 0x00367000
FUNC_START, FUNC_END = 0x00367270, 0x003675AD
DISP = 0x00367508
BELOW = [0x0036750C, 0x00367514, 0x0036751C, 0x00367524,
         0x0036752C, 0x00367534, 0x0036753C]            # 0x3674EC..0x367504
INDEX0 = 0x0036754F                                     # [DISP]


def _image(words):
    data = bytearray(b"\x90" * 0x1000)
    for va, val in words.items():
        struct.pack_into("<I", data, va - BASE, val)
    return bytes(data)


def _lifter(words):
    lifter = Lifter()
    lifter.xbe_data = _image(words)
    lifter.func_start, lifter.func_end = FUNC_START, FUNC_END
    lifter.jump_table_targets = {}
    return lifter


def _offset(va):
    return va - BASE if BASE <= va < BASE + 0x1000 else None


def _is_code(va):
    return 0x00011000 <= va < 0x00465AA8


def _jmp_operand(disp):
    return [Operand(type="mem", mem_index="ecx", mem_scale=4, mem_disp=disp,
                    mem_size=4)]


@mock.patch("tools.recomp.lifter.va_to_file_offset", _offset)
@mock.patch("tools.recomp.lifter.is_code_address", _is_code)
class SwitchTableBelowTest(unittest.TestCase):
    """`neg ecx; jmp [ecx*4 + TrailDownVecEnd]` -- MSVC memcpy's tail copy
    indexes its table downwards (SCDA 0x00367436, docs Update 333)."""

    def _words(self):
        words = {DISP - 4 * (len(BELOW) - i): t for i, t in enumerate(BELOW)}
        words[DISP] = INDEX0
        words[DISP + 4] = 0x1C8E448B          # code bytes after the slot
        return words

    def test_downward_table_becomes_a_switch(self):
        lifter = _lifter(self._words())
        self.assertEqual(lifter._analyze_switch_table(_jmp_operand(DISP)),
                         BELOW + [INDEX0])

    def test_forward_table_gains_no_arms_from_below(self):
        # An ordinary forward table: the dwords below it must be ignored even
        # when they happen to look like in-function addresses.
        words = self._words()
        fwd = 0x00367558
        for i, t in enumerate([0x00367568, 0x00367570, 0x00367580, 0x00367594]):
            words[fwd + 4 * i] = t
        words[fwd + 16] = 0x5E08458B
        lifter = _lifter(words)
        self.assertEqual(lifter._analyze_switch_table(_jmp_operand(fwd)),
                         [0x00367568, 0x00367570, 0x00367580, 0x00367594])

    def test_single_entry_below_is_not_a_switch(self):
        words = {DISP - 4: 0x0036750C, DISP: 0x1C8E448B}
        lifter = _lifter(words)
        self.assertEqual(lifter._analyze_switch_table(_jmp_operand(DISP)), [])

    def test_table_entirely_below_displacement_becomes_a_switch(self):
        # SCDA's forward small-copy path uses `sub ecx, 4` and then indexes
        # with ecx=-4..-1.  The displacement (0x3673CC in the real image) is
        # already code, while all four table slots live below it.  An empty
        # upward read must not prevent the downward-table fallback.
        words = {
            DISP - 16: 0x003673CC,
            DISP - 12: 0x003673D4,
            DISP - 8: 0x003673E0,
            DISP - 4: 0x003673F4,
            DISP: 0x1C8E448B,
        }
        lifter = _lifter(words)
        self.assertEqual(lifter._analyze_switch_table(_jmp_operand(DISP)),
                         [0x003673CC, 0x003673D4,
                          0x003673E0, 0x003673F4])

    def test_forward_table_with_unreachable_index_zero_becomes_a_switch(self):
        # SCDA memcpy proves dst is unaligned before `eax = dst & 3`, so eax
        # is 1..3.  The nominal index-zero slot is padding/garbage and the
        # three real entries begin one dword after the displacement.
        words = {
            DISP: 0x90003675,
            DISP + 4: 0x0036746C,
            DISP + 8: 0x00367490,
            DISP + 12: 0x003674B8,
            DISP + 16: 0x2303468A,
        }
        lifter = _lifter(words)
        self.assertEqual(lifter._analyze_switch_table(_jmp_operand(DISP)),
                         [0x0036746C, 0x00367490, 0x003674B8])


if __name__ == "__main__":
    unittest.main()
