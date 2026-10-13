"""Regression check for Seed 29's file-manager constructor boundary.

Run after regeneration:
  py -3 projects/scda/test_seed29_file_manager_constructor.py
"""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SEEDS = ROOT / "projects" / "scda" / "function_seeds.json"
GEN = ROOT / "tools" / "recomp" / "output" / "scda_seeded29_filemanager" / "gen"


def main() -> None:
    seeds = json.loads(SEEDS.read_text(encoding="utf-8"))
    assert any(item["start"] == "0x0004608C" for item in seeds), (
        "Seed 29 must retain the 0x0004608C constructor boundary"
    )

    sources = list(GEN.glob("recomp_*.c"))
    assert sources, "Seed-29 generated sources are missing"
    generated = "\n".join(path.read_text(encoding="utf-8") for path in sources)
    assert "void sub_0004608C(void)" in generated
    assert "sub_0004608C(); return; /* tail jmp 0x0004608C */" in generated

    unresolved = (GEN / "recomp_stubs_unresolved.c").read_text(encoding="utf-8")
    assert "void sub_0004608C(void)" not in unresolved
    print("ok  Seed-29 file-manager constructor is generated, not unresolved")


if __name__ == "__main__":
    main()
