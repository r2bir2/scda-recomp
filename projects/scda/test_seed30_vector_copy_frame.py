"""Regression check for Seed 30's vector-copy caller frame reconstruction.

Run after regeneration:
  py -3 projects/scda/test_seed30_vector_copy_frame.py
"""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXCLUDES = ROOT / "projects" / "scda" / "function_seed_excludes.json"
GEN = ROOT / "tools" / "recomp" / "output" / "scda_seeded30_vectorframe" / "gen"


def main() -> None:
    excludes = json.loads(EXCLUDES.read_text(encoding="utf-8"))
    starts = {item["start"] for item in excludes}
    assert {"0x00036138", "0x00036172"} <= starts

    sources = list(GEN.glob("recomp_*.c"))
    assert sources, "Seed-30 generated sources are missing"
    generated = "\n".join(path.read_text(encoding="utf-8") for path in sources)
    assert "void sub_00036135(void)" in generated
    # 0x36138 is now ordinary fallthrough after the real prologue, so it has
    # no mandatory C label of its own.  Its first instruction must remain in
    # sub_00036135 immediately after `ebp = esp`.
    assert "ebp = esp;\n    PUSH32(esp, ebx);\n    PUSH32(esp, esi);\n    esi = MEM32(ebp + 8);" in generated
    assert "loc_00036172:" in generated
    assert "void sub_00036138(void)" not in generated
    assert "void sub_00036172(void)" not in generated
    print("ok  Seed-30 keeps the vector-copy caller in one EBP frame")


if __name__ == "__main__":
    main()
