"""Combined, single-run diagnostic: reproduce the overlap check AND the
candidate lookup for sub_00040260 in the exact same detector instance, to
rule out any run-to-run nondeterminism explaining the earlier contradiction
(functions had an 0x40260 entry, but _candidates.get(0x40260) was None)."""
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

TARGET = 0x00040260
print(f"0x{TARGET:08X} in detector._candidates: {TARGET in detector._candidates}")
if TARGET in detector._candidates:
    print(f"  -> {detector._candidates[TARGET]}")
print(f"0x{TARGET:08X} in detector._alias_entries: {TARGET in detector._alias_entries}")
if TARGET in detector._alias_entries:
    print(f"  -> alias, ends at 0x{detector._alias_entries[TARGET]:08X}")
print(f"0x{TARGET:08X} in detector.functions: {TARGET in detector.functions}")
if TARGET in detector.functions:
    fn = detector.functions[TARGET]
    print(f"  -> Function(start=0x{fn.start:08X}, end=0x{fn.end:08X}, "
          f"method={fn.detection_method}, confidence={fn.confidence})")

# Also re-run the overlap scan here, in the same process, to see if
# sub_00040260 shows up as an overlap in THIS run too.
fns = sorted(detector.functions.values(), key=lambda f: f.start)
overlaps = [(a, b) for a, b in zip(fns, fns[1:]) if b.start < a.end]
print(f"\ntotal overlapping adjacent pairs (this run): {len(overlaps)}")
for a, b in overlaps:
    if a.start == 0x0004023A or b.start == 0x00040260:
        print(f"  {a.name} 0x{a.start:08X}-0x{a.end:08X}  vs  "
              f"{b.name} 0x{b.start:08X}-0x{b.end:08X}")
