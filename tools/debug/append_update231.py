path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 231 (2026-09-22): traced the bad 0x00390037 value one call level further back -- it is a pass-through of an inherited `ebx`, not something sub_00062237's own caller computes fresh; the producer is narrowed to inside sub_00048B15's own body but not yet pinpointed

Direct continuation of Update 230, per the same standing instruction (find
the first wrong value, no shortcuts). This entry does not change the
conclusion of Update 230 (still a genuine, confirmed divergence, still
unrelated to the Update 229 contig-alias fix) -- it narrows where the bad
value comes from.

### The external caller of sub_00062237, and its own triple-push

One-shot instrumentation (temporary, gitignored `recomp_0002.c`, reverted
and rebuilt clean afterward -- confirmed via `git status`) at
`sub_00062237`'s own entry, reading the raw stack before its prologue
executes, caught all 5 hits with `incoming_ebx_param == 0x00390037` at the
**identical caller return address `0x0004A31D`**, every time. That call
site, inside `sub_0004934E` (`recomp_0002.c:19467-19472`), is:

```c
ecx = MEM32(0x6012D4);
ecx = MEM32(ecx + eax * 4);   /* ecx (the new "this"/edi) = a real array lookup */
PUSH32(esp, ebx);
PUSH32(esp, ebx);
PUSH32(esp, ebx);              /* all three arguments are the SAME ebx */
call sub_00062237
```

This is byte-faithful to the original x86 (`push ebx` three times is
exactly what the disassembly says) -- not a translation bug at this call
site. `ecx` (the tree's root/"this") is a legitimate array-indexed lookup,
unrelated to the bad value. The bad value is purely `ebx`, inherited
unchanged from whatever `sub_0004934E` itself was called with.

### ebx is a pass-through all the way up through two more functions

Static check: neither `sub_0004934E` nor `sub_00048F1D` (searched their
full bodies for any `ebx = ...` assignment) ever write to `ebx` -- exactly
matching real x86 cdecl callee-saved semantics for a function that simply
doesn't use that register. If the original x86 for these two functions
also never touches `ebx` (consistent with clean recompilation, and no
other bug pattern seen in either function during this check), `ebx`
reaching `sub_0004934E` must be **whatever their common caller passed
in/left there**.

Followed the chain up: `sub_0004934E` is reached from `sub_00048B15` via a
**tail call** (`recomp_0002.c:15529`, `sub_0004934E(); return; /* tail jmp
0x0004934E */`) -- not a normal `RECOMP_ABI_CALL`, which is why an earlier
grep for direct callers came up empty and had to be corrected. Static
search of `sub_00048B15`'s entire ~550-line body finds exactly **one**
`ebx` assignment: `ebx = 0` right after its stack-probe prologue
(`recomp_0002.c:15014`), and nothing else reassigns it before the tail
call at line 15529.

### The open contradiction

Taken at face value, this says `ebx` should be `0` by the time
`sub_0004934E` (and therefore `sub_00062237`) is reached -- not
`0x00390037`. Since the live diagnostic proves it is in fact
`0x00390037`, one of the following is true, and has not yet been
distinguished:

1. **A register-preservation bug in one of `sub_00048B15`'s ~25 direct
   callees** (listed this entry: `sub_00035F65`, `sub_00036116`,
   `sub_000370F1`, `sub_000371A6`, `sub_0003E9D9`, `sub_0003EAE3`,
   `sub_00042082`, `sub_00043455`, `sub_00044C15`, `sub_00045584`,
   `sub_00045647`, `sub_00048856`, `sub_00067886`, `sub_000679C1`,
   `sub_00070854`, `sub_00070B06`, `sub_00070BDA`, `sub_00070CC6`,
   `sub_000715C7`, `sub_00071B49`, `sub_001CAD08`, `sub_001CB603`,
   `sub_001D47CF`, `sub_001D589D`, `sub_0026AF03`) -- if the original x86
   treats `ebx` as callee-saved there and this recompilation's translation
   of one of them fails to restore it correctly, `ebx` would silently pick
   up whatever unrelated value that callee left behind.
2. **A different code path is actually taken** than the simple linear
   read of `sub_00048B15` suggests -- e.g. a branch this static read
   didn't account for that skips the `ebx = 0` line entirely, leaving
   `ebx` as whatever `sub_00048B15`'s own caller passed, pushing the
   search one level higher still.

### Not yet done

Distinguishing between these two requires either bisecting live (checking
`ebx`'s value at successive points inside `sub_00048B15`, narrowing to
the exact callee or branch responsible -- the same one-shot-instrumentation
technique used throughout this investigation) or a byte-exact register-
preservation audit of the listed callees against the original x86. Neither
done this entry; flagged as the next concrete step. Update 230's
conclusion stands unchanged: this is confirmed via xemu ground truth to be
a genuine bug, not a test-environment artifact, and is unrelated to the
Update 229 fix (which remains closed).

No stubs, no fake signals, no clamps. All instrumentation this entry was
temporary, in the gitignored `recomp_0002.c`, fully reverted, and the
clean baseline rebuilt before this entry was written; `git status`/`git
diff` show no tracked changes.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
