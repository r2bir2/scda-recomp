"""Regression checks for x86 FS-segment operand lifting."""

from tools.recomp.disasm import Disassembler
from tools.recomp.lifter import _fmt_mem_read, _fmt_mem_write


def test_fs_operand_is_preserved_and_lifted_separately():
    insn = Disassembler().disassemble_function(
        bytes.fromhex("64a12800000064890d04000000"), 0x1000, 0x100d)
    read_op = insn[0].operands[1]
    write_op = insn[1].operands[0]
    assert read_op.mem_segment == "fs"
    assert _fmt_mem_read(read_op) == "xbox_fs_read32(0x28)"
    assert write_op.mem_segment == "fs"
    assert _fmt_mem_write(write_op, "ecx") == "xbox_fs_write32(4, ecx);"


if __name__ == "__main__":
    test_fs_operand_is_preserved_and_lifted_separately()
    print("ok fs segment lifting")
