"""Root-cause the NEW overlap discovered during verification:
sub_00040260 (falls inside sub_0004023A's now-correctly-detected
0x0004023A-0x00040766 body) gets registered as its own separate function.
Traces which pass/method added it as a candidate, and why."""
import sys

sys.path.insert(0, "tools")
from disasm.loader import load_image
from disasm.engine import DisasmEngine
from disasm.functions import FunctionDetector
from disasm.xrefs import build_xrefs
from disasm.labels import LabelManager, populate_kernel_labels, populate_entry_point

TARGET = 0x00040260

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

orig_add = FunctionDetector._add_candidate

def traced_add(self, addr, confidence, method):
    if addr == TARGET:
        print(f"  _add_candidate(0x{addr:08X}, confidence={confidence}, method={method!r})")
    return orig_add(self, addr, confidence, method)

FunctionDetector._add_candidate = traced_add

detector.detect_all(sections)

print()
conf, method = detector._candidates.get(TARGET, (None, None))
print(f"final candidate record for 0x{TARGET:08X}: confidence={conf}, method={method}")
fn = detector.functions.get(TARGET)
if fn:
    print(f"final function: 0x{fn.start:08X}-0x{fn.end:08X}")

print()
print("Instruction at 0x40260 (per engine):")
insn = engine.get_instruction(TARGET)
if insn:
    print(f"  mnemonic={insn.mnemonic} op_str={getattr(insn,'op_str','?')} "
          f"size={insn.size}")
