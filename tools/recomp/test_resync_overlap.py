import unittest

from .disasm import Disassembler

# A jump-table byte (0x0C) followed by memcpy's backward tail-copy case
# (SCDA 0x0036750C):
#     mov eax, [esi+ecx*4+0x1C]    8B 44 8E 1C
#     mov [edi+ecx*4+0x1C], eax    89 44 8F 1C
#     ret                          C3
# Decoded linearly from the table byte the stream is out of phase and yields
# `inc esp` at +2 and +6 -- both *inside* real instructions.
RAW = bytes.fromhex("0c" "8b448e1c" "89448f1c" "c3")
START = 0x1000


class ResyncOverlapTest(unittest.TestCase):
    """docs Update 333: a resync must drop stale instructions that start
    inside the re-decoded ones, not only the one straddling the resync point."""

    def test_linear_decode_is_out_of_phase(self):
        addrs = {i.address for i in Disassembler().disassemble_function(
            RAW, START, START + len(RAW))}
        self.assertIn(START + 6, addrs)

    def test_resync_drops_phantom_inc_esp(self):
        insns = Disassembler().disassemble_function(
            RAW, START, START + len(RAW), resync={START + 1})
        by_addr = {i.address: i for i in insns}
        self.assertNotIn(START + 2, by_addr)
        self.assertNotIn(START + 6, by_addr)
        self.assertEqual(by_addr[START + 1].mnemonic, "mov")
        self.assertEqual(by_addr[START + 5].mnemonic, "mov")
        self.assertEqual(by_addr[START + 9].mnemonic, "ret")
        # no two kept instructions overlap
        ordered = sorted(i.address for i in insns if i.address >= START + 1)
        for a, b in zip(ordered, ordered[1:]):
            self.assertLessEqual(a + by_addr[a].size, b)


if __name__ == "__main__":
    unittest.main()
