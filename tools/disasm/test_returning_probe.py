"""probes_as_returning_body and MSVC's null-checked delete wrapper.

    mov ecx, [esp+4]      8B 4C 24 04
    test ecx, ecx         85 C9
    je L                  74 05
    jmp <dtor>            E9 <rel32>      ; tail call, leaves the window
L:  ret                   C3

The ret is reachable through the je, but the probe stopped at the tail jmp and
rejected the whole body. SCDA takes these wrappers by address only
(0x001E098E, 0x001DFD9D, 0x001F9EF4 -- data_imm xrefs), so none of them became
functions and every callback through one was skipped at run time (docs
Update 326). The bytes below are 0x001E098E verbatim.

    py -3 -m unittest tools.disasm.test_returning_probe
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402

from tools.disasm.engine import DisasmEngine  # noqa: E402


class _Section:
    def __init__(self, va, size):
        self.virtual_addr = va
        self.virtual_size = size
        self.executable = True


class _Image:
    def __init__(self, va, data):
        self.va = va
        self.data = data
        self.section = _Section(va, len(data))

    def get_section_at_va(self, addr):
        if self.va <= addr < self.va + len(self.data):
            return self.section
        return None

    def read_bytes_at_va(self, addr, n):
        off = addr - self.va
        if off < 0 or off >= len(self.data):
            return b""
        return self.data[off:off + n]


VA = 0x001E098E


def _engine(data):
    eng = DisasmEngine.__new__(DisasmEngine)
    eng.image = _Image(VA, data)
    eng._cs = Cs(CS_ARCH_X86, CS_MODE_32)
    eng._cs.detail = True
    return eng


DELETE_WRAPPER = bytes.fromhex("8b4c240485c97405e9fbfeffffc3")
PAD = b"\xcc" * 16


class ReturningProbeTest(unittest.TestCase):
    def test_null_checked_delete_wrapper_is_a_returning_body(self):
        eng = _engine(DELETE_WRAPPER + PAD)
        self.assertTrue(eng.probes_as_returning_body(VA))

    def test_plain_tail_jump_is_still_rejected(self):
        # mov ecx, [esp+4]; jmp <dtor>; ret -- nothing branches past the jmp
        eng = _engine(bytes.fromhex("8b4c2404e9fbfeffffc3") + PAD)
        self.assertFalse(eng.probes_as_returning_body(VA))

    def test_branch_that_does_not_pass_the_jmp_is_not_enough(self):
        # test ecx, ecx; je +0 (lands before the jmp); jmp <dtor>; ret
        eng = _engine(bytes.fromhex("85c97400e9fbfeffffc3") + PAD)
        self.assertFalse(eng.probes_as_returning_body(VA))

    def test_backward_conditional_branch_does_not_count(self):
        # test ecx, ecx; je <before start>; jmp <dtor>; ret
        eng = _engine(bytes.fromhex("85c974f0e9fbfeffffc3") + PAD)
        self.assertFalse(eng.probes_as_returning_body(VA))


class FramePrologueTest(unittest.TestCase):
    """probes_as_frame_prologue (docs Update 329)."""

    def test_offset_frame_prologue(self):
        # SCDA 0x00077BBB: push ebp; lea ebp, [esp-0x74]; sub esp, 0x264
        eng = _engine(bytes.fromhex("558d6c248c81ec64020000") + PAD)
        self.assertTrue(eng.probes_as_frame_prologue(VA))

    def test_classic_frame_prologue(self):
        eng = _engine(bytes.fromhex("558bec") + PAD)   # push ebp; mov ebp, esp
        self.assertTrue(eng.probes_as_frame_prologue(VA))

    def test_push_ebp_alone_is_not_a_frame(self):
        # push ebp; mov eax, [esp+8] -- saves ebp as a scratch register
        eng = _engine(bytes.fromhex("558b442408") + PAD)
        self.assertFalse(eng.probes_as_frame_prologue(VA))

    def test_lea_into_another_register_is_not_a_frame(self):
        # push ebp; lea ecx, [esp-0x74]
        eng = _engine(bytes.fromhex("558d4c248c") + PAD)
        self.assertFalse(eng.probes_as_frame_prologue(VA))


if __name__ == "__main__":
    unittest.main()
