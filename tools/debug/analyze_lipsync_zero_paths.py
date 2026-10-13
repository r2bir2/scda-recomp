"""Reconstruct direct-call paths from the live Lipsync entry to selected zero fills.

Read-only diagnostic over generated SCDA C.  It does not alter generated code,
the build, or game data.  Direct calls only are intentional: indirect calls
cannot be attributed statically without a concrete runtime target.
"""
from __future__ import annotations

import re
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / "tools" / "recomp" / "output" / "scda_v060" / "gen"
START = "00039566"
TARGETS = {
    "004B8950", "000677E4", "004B9000", "004BABE0", "004BBAF0",
    "0049D210", "004BCE10", "00253F0D", "002547A3", "004A2220",
    "004A26F0", "004C3040", "00055936",
}

FUNC = re.compile(r"^void sub_([0-9A-F]{8})\(void\)\s*$", re.M)
CALL = re.compile(r"RECOMP_ABI_CALL\(0x([0-9A-F]{8})u, sub_([0-9A-F]{8})\)")


def main() -> None:
    graph: dict[str, set[str]] = {}
    for path in GEN.glob("recomp_*.c"):
        text = path.read_text(encoding="utf-8", errors="replace")
        starts = list(FUNC.finditer(text))
        for index, match in enumerate(starts):
            end = starts[index + 1].start() if index + 1 < len(starts) else len(text)
            node = match.group(1)
            calls = {callee for _, callee in CALL.findall(text[match.start():end])}
            graph.setdefault(node, set()).update(calls)

    parent: dict[str, str | None] = {START: None}
    depth: dict[str, int] = {START: 0}
    queue: deque[str] = deque([START])
    while queue:
        node = queue.popleft()
        if depth[node] >= 4:
            continue
        for callee in sorted(graph.get(node, ())):
            if callee not in parent:
                parent[callee] = node
                depth[callee] = depth[node] + 1
                queue.append(callee)

    print(f"START sub_{START}; direct-call nodes through depth 4: {len(parent)}")
    for target in sorted(TARGETS):
        if target not in parent:
            print(f"sub_{target}: NOT REACHED")
            continue
        path = []
        cur: str | None = target
        while cur is not None:
            path.append(f"sub_{cur}")
            cur = parent[cur]
        print(f"sub_{target}: depth {depth[target]} :: " + " -> ".join(reversed(path)))


if __name__ == "__main__":
    main()
