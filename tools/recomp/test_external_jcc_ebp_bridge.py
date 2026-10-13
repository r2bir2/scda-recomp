"""Regression checks for EBP propagation across external conditional jumps.

Run: py -3 tools/recomp/test_external_jcc_ebp_bridge.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import _emit_cond_goto  # noqa: E402


class _FragmentLifter:
    # New upstream field (not present when this test was first written):
    # _emit_cond_goto checks it to route calls through recomp_lookup_manual
    # overrides. Empty here -- no target in these tests has one.
    manual_functions = set()

    @staticmethod
    def _is_external_target(target):
        return target == 0x2000

    @staticmethod
    def _call_target_name(target):
        return f"sub_{target:08X}"


def test_external_jcc_bridges_ebp():
    result = _emit_cond_goto("eax == 0", "je", "equal", 0x2000,
                             _FragmentLifter())
    assert "g_seh_ebp = ebp; sub_00002000(); return;" in result
    print("ok  external_jcc_bridges_ebp")


def test_internal_jcc_remains_goto():
    result = _emit_cond_goto("eax == 0", "je", "equal", 0x1000,
                             _FragmentLifter())
    assert result == "if (eax == 0) goto loc_00001000; /* je: equal */"
    print("ok  internal_jcc_remains_goto")


if __name__ == "__main__":
    test_external_jcc_bridges_ebp()
    test_internal_jcc_remains_goto()
