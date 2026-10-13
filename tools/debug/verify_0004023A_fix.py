"""Verification (not regeneration): confirm the current tools/disasm code
correctly detects sub_0004023A's real extent, does not create a new
alias/duplicate explosion, and does not swallow/overlap neighboring
functions. Read-only: writes no output files.

Checklist (per user instruction, 2026-09-09):
  1. cache-version gate            -> covered separately by test_cache_version.py
  2. detection actually re-runs    -> this script never touches the stale
                                        tools/disasm/output/scda/functions.json;
                                        it runs FunctionDetector fresh in-memory
  3. sub_0004023A ends at 0x40766
  4. 0x00040350 lies inside it
  5. 0x0004034B (-> sub_000511E7) lies inside it
  6. "end" field semantics: confirm it is EXCLUSIVE (one past the last
     decoded byte), not the start of the last instruction
  7. neighboring functions not swallowed/overlapped
  8. largest alias/duplicate cluster stays small (2482x/867x-class bug closed)
  9. total function count before/after, classify the delta
"""
import sys

sys.path.insert(0, "tools")
from disasm.loader import load_image
from disasm.engine import DisasmEngine
from disasm.functions import FunctionDetector
from disasm.xrefs import build_xrefs
from disasm.labels import LabelManager, populate_kernel_labels, populate_entry_point

TARGET = 0x0004023A
CALL_SITE_TO_511E7 = 0x0004034B   # "call 0x511e7"
RETURN_FROM_511E7 = 0x00040350    # return address right after that call

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

fns = sorted(detector.functions.values(), key=lambda f: f.start)
by_start = {f.start: f for f in fns}

print("=" * 70)
print("Item 3/4/5: sub_0004023A boundary and containment")
target_fn = by_start.get(TARGET)
if target_fn is None:
    print(f"  FAIL: 0x{TARGET:08X} is not a detected function start at all")
else:
    print(f"  start=0x{target_fn.start:08X} end=0x{target_fn.end:08X} "
          f"size={target_fn.end - target_fn.start} "
          f"method={target_fn.detection_method}")
    ok_end = target_fn.end == 0x00040766
    ok_350 = target_fn.start <= RETURN_FROM_511E7 < target_fn.end
    ok_34b = target_fn.start <= CALL_SITE_TO_511E7 < target_fn.end
    print(f"  end == 0x40766:            {'PASS' if ok_end else 'FAIL'}")
    print(f"  0x40350 inside [start,end): {'PASS' if ok_350 else 'FAIL'}")
    print(f"  0x4034B inside [start,end): {'PASS' if ok_34b else 'FAIL'}")

print()
print("Item 6: 'end' field semantics (exclusive, one-past-last-byte)")
# The real terminator is "ret 0xC" (opcode C2 0C 00, 3 bytes) at 0x40763.
ret_insn = engine.get_instruction(0x00040763)
if ret_insn is not None:
    print(f"  instruction at 0x40763: mnemonic={ret_insn.mnemonic} "
          f"size={ret_insn.size} end_address=0x{ret_insn.end_address:08X}")
    print(f"  function.end (0x{target_fn.end:08X}) == "
          f"ret's end_address (0x{ret_insn.end_address:08X}): "
          f"{'PASS -- end is exclusive/one-past-last-byte' if target_fn.end == ret_insn.end_address else 'MISMATCH'}")
else:
    print("  could not decode instruction at 0x40763 for cross-check")
# Cross-check against the next real function's own start -- for adjacent
# functions, end == next.start is the expected exclusive convention.
next_fn = min((f for f in fns if f.start > TARGET), key=lambda f: f.start, default=None)
if next_fn:
    print(f"  next function starts at 0x{next_fn.start:08X}; "
          f"matches this function's end: "
          f"{'PASS' if next_fn.start == target_fn.end else 'note: gap or non-adjacent, not necessarily a problem'}")

print()
print("Item 7: neighboring functions not swallowed/overlapped")
idx = fns.index(target_fn)
prev_fn = fns[idx - 1] if idx > 0 else None
next_fn2 = fns[idx + 1] if idx + 1 < len(fns) else None
if prev_fn:
    print(f"  prev: 0x{prev_fn.start:08X}-0x{prev_fn.end:08X} ({prev_fn.name})  "
          f"overlap-with-target: {'YES -- FAIL' if prev_fn.end > target_fn.start else 'no'}")
if next_fn2:
    print(f"  next: 0x{next_fn2.start:08X}-0x{next_fn2.end:08X} ({next_fn2.name})  "
          f"overlap-with-target: {'YES -- FAIL' if next_fn2.start < target_fn.end else 'no'}")
# General overlap scan across the whole function list (cheap, and directly
# answers "are neighbors elsewhere swallowed/overlapped" beyond just this
# one function's immediate neighbors).
overlaps = []
for a, b in zip(fns, fns[1:]):
    if b.start < a.end:
        overlaps.append((a, b))
print(f"  total overlapping adjacent-pairs across ALL functions: {len(overlaps)}")
for a, b in overlaps[:10]:
    print(f"    OVERLAP: {a.name} 0x{a.start:08X}-0x{a.end:08X}  vs  "
          f"{b.name} 0x{b.start:08X}-0x{b.end:08X}")

print()
print("Item 8: largest alias/duplicate cluster (2482x/867x-class check)")
from collections import Counter
cluster_sizes = Counter(detector._alias_entries.values())
if cluster_sizes:
    biggest_end, biggest_count = cluster_sizes.most_common(1)[0]
    print(f"  total alias entries: {len(detector._alias_entries)}")
    print(f"  largest cluster: {biggest_count} aliases all ending at "
          f"0x{biggest_end:08X}  "
          f"{'FAIL -- looks like the 2482x/867x bug reopened' if biggest_count > 50 else 'OK'}")
else:
    print("  no alias entries at all")

print()
print("Item 9: total function count")
print(f"  functions detected (fresh, current code): {len(detector.functions)}")
print("  (stale tools/disasm/output/scda/functions.json recorded 20383 as of "
      "2026-08-24, per its own summary.json -- see narrative for how to "
      "classify the delta once this number is known)")
