"""Root-cause tool: instruments FunctionDetector._build_functions to print
exactly what next_func/candidate clamps sub_0004023A's end at 0x00040295,
and which pass (by detection_method) is responsible for that candidate.

Read-only: runs the disasm pipeline in memory, writes nothing to disk,
touches no generated/build output.
"""
import sys

sys.path.insert(0, "tools")
from disasm.loader import load_image
from disasm.engine import DisasmEngine
from disasm.functions import FunctionDetector
from disasm.xrefs import build_xrefs
from disasm.labels import LabelManager, populate_kernel_labels, populate_entry_point, extract_strings

TARGET = 0x0004023A

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

# Monkeypatch _build_functions to log next_func for our target, and which
# pass most recently added the candidate that becomes it.
orig_build = FunctionDetector._build_functions
call_count = [0]

def traced_build(self, sections):
    call_count[0] += 1
    n = call_count[0]
    result = orig_build(self, sections)
    fn = self.functions.get(TARGET)
    if fn:
        print(f"[build_functions call #{n}] sub_{TARGET:08X} -> "
              f"start=0x{fn.start:08X} end=0x{fn.end:08X} "
              f"method={fn.detection_method} confidence={fn.confidence}")
        # Find the candidate immediately after TARGET in sorted candidates
        sorted_c = sorted(a for a in self._candidates if a in self.engine.instructions)
        if TARGET in sorted_c:
            idx = sorted_c.index(TARGET)
            if idx + 1 < len(sorted_c):
                nxt = sorted_c[idx + 1]
                conf, method = self._candidates[nxt]
                print(f"    next candidate after target: 0x{nxt:08X} "
                      f"(confidence={conf}, method={method})")
    return result

FunctionDetector._build_functions = traced_build

detector.detect_all(sections)

fn = detector.functions.get(TARGET)
print()
print(f"FINAL: sub_{TARGET:08X} -> end=0x{fn.end:08X}" if fn else "FINAL: not found")
