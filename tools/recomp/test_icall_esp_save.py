"""Regression checks for indirect-call ESP recovery.

Run: py -3 tools/recomp/test_icall_esp_save.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.translator import _fixup_icall_esp_save  # noqa: E402


ICALL = (
    "    g_icall_source = 0x00001020u; PUSH32(esp, 0); "
    "RECOMP_ICALL_SAFE(MEM32(eax + 4), _icall_esp);"
)


def _save_index(lines):
    return next(i for i, line in enumerate(lines)
                if "uint32_t _icall_esp = g_esp" in line)


def test_ebp_frame_is_not_discarded():
    lines = [
        "loc_00001000: ;",
        "    RECOMP_TRACE_PC(0x00001000u);",
        "    PUSH32(esp, ebp);",
        "    ebp = esp;",
        "    esp = esp - 0x40;",
        "    PUSH32(esp, ebx);",
        "    PUSH32(esp, esi);",
        "    PUSH32(esp, edi);",
        "    esi = 0;",
        "    PUSH32(esp, esi);",
        "    PUSH32(esp, MEM32(ebp + 8));",
        ICALL,
        "    POP32(esp, edi);",
        "    POP32(esp, esi);",
        "    POP32(esp, ebx);",
        "    POP32(esp, ebp);",
    ]
    fixed = _fixup_icall_esp_save(lines)
    # The save must land right after "esi = 0;" (that reassignment is not an
    # esp touch, so its exact position relative to the save has no effect on
    # g_esp correctness) and before the argument run's own re-push of esi
    # (now holding 0) -- not the earlier, textually-identical prologue save
    # of the same register a few lines up, which .index() would find first.
    assert fixed.index("    esi = 0;") + 1 == _save_index(fixed), "\n".join(fixed)
    push_esi_indices = [i for i, l in enumerate(fixed) if l.strip() == "PUSH32(esp, esi);"]
    assert len(push_esi_indices) == 2, "\n".join(fixed)
    assert _save_index(fixed) < push_esi_indices[1], "\n".join(fixed)
    print("ok  ebp_frame_is_not_discarded")


def test_fpo_saved_register_is_not_an_argument():
    lines = [
        "loc_00002000: ;",
        "    RECOMP_TRACE_PC(0x00002000u);",
        "    PUSH32(esp, esi);",
        "    esi = ecx;",
        "    edx = MEM32(esi + 8);",
        "    PUSH32(esp, edx);",
        ICALL,
        "    POP32(esp, esi);",
    ]
    fixed = _fixup_icall_esp_save(lines)
    # The save must land after the genuine esi save and its dependent
    # computation (neither touches esp, so their exact position relative to
    # the save has no effect on g_esp correctness) and before the actual
    # argument push.
    assert fixed.index("    PUSH32(esp, esi);") < _save_index(fixed), "\n".join(fixed)
    assert fixed.index("    edx = MEM32(esi + 8);") < _save_index(fixed), "\n".join(fixed)
    assert _save_index(fixed) < fixed.index("    PUSH32(esp, edx);"), "\n".join(fixed)
    print("ok  fpo_saved_register_is_not_an_argument")


def test_fpo_save_after_vtable_fetch_setup():
    # sub_0026940F: two pure setup lines (a vtable fetch) precede the FPO
    # saves, and esi/edi are each reused as a repeated argument in nine
    # later icalls in the same function -- pushed far more than popped, so
    # the push/pop count heuristic also can't recognize them as saves.
    lines = [
        "loc_0026940F: ;",
        "    RECOMP_TRACE_PC(0x0026940Fu);",
        "    ecx = MEM32(0x5EAE74);",
        "    eax = MEM32(ecx);",
        "    PUSH32(esp, esi);",
        "    PUSH32(esp, edi);",
        "    edi = 0;",
        "    PUSH32(esp, edi);",
        "    PUSH32(esp, 0x564868);",
        "    esi = 0x564860;",
        "    PUSH32(esp, esi);",
        ICALL,
        "    PUSH32(esp, edi);",
        "    PUSH32(esp, 0x564838);",
        "    PUSH32(esp, esi);",
        ICALL,
        "    POP32(esp, edi);",
        "    POP32(esp, esi);",
    ]
    fixed = _fixup_icall_esp_save(lines)
    # The first save must land after both setup lines and both FPO saves,
    # not before them -- a save captured any earlier would let a failed
    # lookup's rewind discard the esi/edi preservation itself.
    first_save_idx = _save_index(fixed)
    assert fixed.index("    ecx = MEM32(0x5EAE74);") < first_save_idx, "\n".join(fixed)
    fpo_push_indices = [i for i, l in enumerate(fixed) if l.strip() in
                         ("PUSH32(esp, esi);", "PUSH32(esp, edi);")]
    # Three PUSH32(esp, edi) / PUSH32(esp, esi) lines precede the save: the
    # two FPO saves, plus "PUSH32(esp, edi);" is textually identical whether
    # it's a save or (two lines later, once edi = 0) an argument -- only
    # position distinguishes them. All three lines at or before the save must
    # be the two FPO saves, i.e. the save is the third such line, not earlier.
    assert fpo_push_indices[:2] == [i for i in fpo_push_indices if i < first_save_idx], \
        "\n".join(fixed)
    assert first_save_idx == fpo_push_indices[1] + 1, "\n".join(fixed)
    print("ok  fpo_save_after_vtable_fetch_setup")


def test_later_block_still_saves_before_arguments():
    lines = [
        "loc_00003000: ;",
        "    RECOMP_TRACE_PC(0x00003000u);",
        "    eax = 1;",
        "loc_00003010: ;",
        "    RECOMP_TRACE_PC(0x00003010u);",
        "    PUSH32(esp, eax);",
        ICALL,
    ]
    fixed = _fixup_icall_esp_save(lines)
    assert _save_index(fixed) == fixed.index("    PUSH32(esp, eax);") - 1
    print("ok  later_block_still_saves_before_arguments")


if __name__ == "__main__":
    test_ebp_frame_is_not_discarded()
    test_fpo_saved_register_is_not_an_argument()
    test_fpo_save_after_vtable_fetch_setup()
    test_later_block_still_saves_before_arguments()
