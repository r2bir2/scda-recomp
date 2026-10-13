"""Callers of a wrapped function must reach the wrapper, not the body.

recomp_manual.c can wrap a generated function: it defines sub_X itself and
calls the generated body, which the generator then emits as sub_X_gen. The
rename went through func_db, so every direct call site also named
sub_X_gen and the wrapper never ran (docs Update 349: the SCDA fade-manager
checks around sub_00112276 stayed silent while the crash went through
sub_00112276_gen).
"""

import unittest

from .lifter import Lifter


class WrapCallTargetTest(unittest.TestCase):
    def test_direct_call_to_wrapped_function_names_the_wrapper(self):
        lifter = Lifter()
        lifter.func_db = {0x112276: {"name": "sub_00112276_gen"}}
        self.assertEqual(lifter._call_target_name(0x112276), "sub_00112276")

    def test_other_renames_are_kept(self):
        lifter = Lifter()
        lifter.func_db = {0x8DB80: {"name": "cseries__sub_0008DB80"}}
        self.assertEqual(lifter._call_target_name(0x8DB80), "cseries__sub_0008DB80")


if __name__ == "__main__":
    unittest.main()
