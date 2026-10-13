"""Regression check for Seed 32's file-manager fall-through frame.

Run after regeneration:
  py -3 projects/scda/test_seed32_filemanager_frame.py
"""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
EXCLUDES = ROOT / "projects" / "scda" / "function_seed_excludes.json"
GEN = ROOT / "tools" / "recomp" / "output" / "scda_seeded32_filemanager_frame" / "gen"


def main() -> None:
    excluded = {item["start"] for item in json.loads(EXCLUDES.read_text(encoding="utf-8"))}
    assert {"0x0004655D", "0x00046683", "0x000466A0"} <= excluded

    generated = "\n".join(path.read_text(encoding="utf-8") for path in GEN.glob("recomp_*.c"))
    assert "void sub_000464EC(void)" in generated
    assert "loc_00046877:" in generated
    assert "if (TEST_Z(eax, eax)) goto loc_00046877;" in generated
    for false_start in ("0004655D", "00046683", "000466A0"):
        assert f"void sub_{false_start}(void)" not in generated
    print("ok  Seed-32 retains the file-manager fall-through and its EBP frame")


if __name__ == "__main__":
    main()
