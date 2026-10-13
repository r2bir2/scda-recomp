"""One-off: given a list of guest addresses (hex), print the function
(name/start/end) that contains each one, per functions.json."""
import json
import sys

with open("tools/disasm/output/scda/functions.json", encoding="utf-8") as f:
    funcs = json.load(f)

targets = [int(a, 16) for a in sys.argv[1:]]

for addr in targets:
    hit = None
    for fn in funcs:
        start = int(fn["start"], 16)
        end = int(fn["end"], 16)
        if start <= addr < end:
            hit = fn
            break
    if hit:
        print(f"0x{addr:08X}: {hit['name']}  [{hit['start']}-{hit['end']}]  "
              f"called_by={hit.get('called_by')}")
    else:
        print(f"0x{addr:08X}: NOT FOUND in any function range")
