"""Redo the overlap check, but distinguish alias-derived stubs (intentional,
by-design duplication for shared-tail code -- see _pass_tail_jump_targets's
own docstring) from genuine, unexplained real-function collisions."""
import sys

sys.path.insert(0, "tools")
from disasm.loader import load_image
from disasm.engine import DisasmEngine
from disasm.functions import FunctionDetector
from disasm.xrefs import build_xrefs
from disasm.labels import LabelManager, populate_kernel_labels, populate_entry_point

image = load_image("build/scda_upgrade/bin/RelWithDebInfo/default.xbe")
engine = DisasmEngine(image)
sections = image.get_code_sections()
for sec in sections:
    engine.linear_sweep(sec)
engine.resync_jump_tables()

xrefs = build_xrefs(engine, image)
labels = LabelManager()
populate_kernel_labels(labels, image)
populate_entry_point(labels, image)

detector = FunctionDetector(engine, image, xrefs, labels)
detector.detect_all(sections)

ALIAS_METHODS = {"tail_jump_alias", "cond_branch_orphan_alias",
                 "imm_ref_alias", "data_ptr_alias", "seed_alias"}
# (names guessed; print the actual set of methods seen among alias entries
# below so we don't have to guess right.)
alias_method_names = {f.detection_method for f in detector.functions.values()
                       if f.start in detector._alias_entries}
print("detection_method values seen for alias-derived functions:",
      alias_method_names)

fns = sorted(detector.functions.values(), key=lambda f: f.start)
overlaps = [(a, b) for a, b in zip(fns, fns[1:]) if b.start < a.end]
print(f"total overlapping adjacent pairs: {len(overlaps)}")

real_overlaps = [(a, b) for a, b in overlaps
                  if a.start not in detector._alias_entries
                  and b.start not in detector._alias_entries]
print(f"overlaps where NEITHER side is an alias-derived stub: {len(real_overlaps)}")
for a, b in real_overlaps[:20]:
    print(f"  REAL OVERLAP: {a.name} 0x{a.start:08X}-0x{a.end:08X} "
          f"(method={a.detection_method})  vs  "
          f"{b.name} 0x{b.start:08X}-0x{b.end:08X} (method={b.detection_method})")

alias_only_overlaps = len(overlaps) - len(real_overlaps)
print(f"overlaps where at least one side IS an alias-derived stub "
      f"(expected/by-design shared-tail pattern): {alias_only_overlaps}")
