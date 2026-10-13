path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 232 (2026-09-22): ROOT CAUSE FOUND AND FIXED for the menu tree node pointer bug (Updates 230-231) -- a code-generator bug in `_fixup_icall_esp_save`'s FPO-prologue detection, not a data/pointer problem at all

Direct continuation of Update 231's live bisection plan, per the same
standing instruction (narrow the exact call boundary where ebx flips from
0 to the observed 0x00390037, live-checkpoint by live-checkpoint, no
static-only guessing once a live bisection is possible).

### Bisection: from sub_00048B15 down into sub_0026940F

One-shot instrumentation (temporary, in the gitignored generated `.c`
files, fully reverted by a full pipeline regeneration afterward) placed
sequential checkpoints along the exact call chain identified in Update
231, narrowing in each round:

1. `sub_00048B15`'s own body (CP0-CP6): `ebx` stays `0` at every point
   checked, including the tail-call boundary into `sub_0004934E` --
   confirmed clean, consistent with Update 231's static reading.
2. Realized `sub_0004934E` has **two independent entry paths** into the
   array-loop call site that reaches `sub_00062237`: the tail-call from
   `sub_00048B15` (rare -- observed exactly once per process run, a
   one-time init path), and a `fallthrough`/tail-call chain through
   `sub_00048F1D` -> `sub_000491BA` -> `sub_000492FA` -> `sub_00049336`
   that is in fact the game's own per-frame update path (this function
   family has no real call/return boundary in the original binary --
   `sub_00048B15` through `sub_0004934E` are one contiguous procedure,
   split into separate C functions purely for translation-unit size).
   `ebx` was `0` at the entry of every one of these, confirmed live.
3. Bisected `sub_0004934E`'s own ~3000-line body (quarter/half/three-
   quarter checkpoints, then progressively finer) and isolated the flip
   to between two adjacent checkpoints at addresses `0x00049FC7` (still
   `0`) and `0x0004A0E9` (already `0x00390037`) -- a ~190-line window.
4. Within that window, per-callee checkpoints isolated the flip further:
   `ebx` is still `0` after `sub_004528E7` returns, and already
   `0x00390037` after `sub_001D8842` returns. `sub_001D8842` is the
   direct culprit call.

### Inside sub_001D8842: a genuine 8-byte stack imbalance, empirically measured

Instrumented `sub_001D8842`'s own prologue and epilogue to record `esp`
and the raw stack contents at its saved-register slots. Prologue:
`esp=0x00F7B710`, slot values `edi=0, esi=0x00F7B798, ebx=0` (correct).
Epilogue: `esp=0x00F7B718` -- **8 bytes higher than the prologue's esp**,
a directly measured, unambiguous stack imbalance, not a guess. Bisecting
with quarter-point and then finer esp-delta checkpoints (all reading
`esp - <prologue esp>`, expected `0` throughout a balanced function)
narrowed the introduction of this `+8` to a single call: `sub_0026940F`.
Checkpoints immediately before and after that specific call (in
`sub_001D8842`) confirmed the entire `+8` appears inside `sub_0026940F`
itself -- not before or after it.

### Inside sub_0026940F: byte-exact disassembly comparison, then a second empirical bisection

Disassembled `sub_0026940F` (301 bytes, `capstone`, byte-exact against
`default.xbe`) end to end and compared every instruction against the
generated C -- structurally identical, including the closing `pop ecx;
pop edi; mov [...],eax; pop esi; ret` (matching `POP32(ecx); POP32(edi);
...; POP32(esi); esp+=4; return;` exactly). The function issues 10
identical-shaped indirect calls (`ecx = MEM32(g); eax = MEM32(ecx); ...;
call [eax+0x14]`) against the same object, 9 of them followed by
`push eax; mov ecx,<const>; call sub_000D28EC` and the 10th by
`push eax; call sub_000678A4` instead. A first pass suspected
`sub_000678A4` (which tail-jumps twice, through `sub_00368B0E` into
`sub_00368AAF`, a small integer-parsing routine) or `sub_000D28EC`
(called 9x) -- both were instrumented directly (entry/exit `esp`,
per-call counters) and both proved **perfectly balanced on every single
call**, including all 9 real invocations of `sub_000D28EC` from this
exact loop (matched by their distinctive `ecx` constants, isolated out of
11154 total calls to that shared, widely-reused helper across the whole
run). That left only the 10 indirect calls themselves; printing each
one's resolved target confirmed **all 10 read the identical vtable slot,
same object, same value (`0x000460C5`)** -- ruling out "different callee
behaves differently" and pointing at the resolution mechanism itself.

### The actual root cause: an unregistered indirect-call target correctly triggers RECOMP_ICALL_SAFE's fallback, but the fallback's saved rewind-point was captured too early -- by the code GENERATOR, not by any hand-written code

`0x000460C5` is not a registered function entry in this recompilation --
it falls in the middle of `sub_000460BB`'s own address range (a
different, separately named function starting a few bytes earlier), so
`recomp_lookup()` cannot find it. Directly probing this from generated
code (`recomp_lookup_manual` / `recomp_lookup` / `recomp_lookup_kernel`,
all returning null) confirmed it: `RECOMP_ICALL_SAFE` takes its designed
safety fallback for an unresolved-but-code-range target -- `g_esp =
saved_esp; eax = 0;` -- for every one of the 10 calls, every run. This
fallback is correct in general; `saved_esp` is exactly the `_icall_esp`
value the code generator inserts via `_fixup_icall_esp_save()` in
`tools/recomp/translator.py`, meant to be captured immediately before
the call's own argument pushes so a failed lookup can cleanly unwind
*only* those arguments.

For 9 of the 10 calls in this function, that is exactly what happens.
For the *first* call specifically, `_fixup_icall_esp_save()` places the
save one step too early -- before the function's own one-time
`PUSH32(esp, esi); PUSH32(esp, edi);` register-preservation pair, not
after it. Root-caused in `_fixup_icall_esp_save()` itself (verified by
reading, not guessing): the function locates a leading FPO-style
register-save run by scanning forward from the very first line after the
entry label's `RECOMP_TRACE_PC()`, requiring a `PUSH32(esp, ebx|esi|edi)`
line *immediately* there. `sub_0026940F`'s body has two ordinary setup
lines first (`ecx = MEM32(g); eax = MEM32(ecx);`, the vtable fetch shared
by all 10 calls) before the two saves -- so the scan sees a non-push line
first and stops immediately, leaving `prologue_end_idx` at 0 and the
icall-save backward-scan free to walk past the saves. The function's
*other* safeguard -- treating a push as a "save" whenever it is pushed no
more than it is popped -- also misses this specific case, because both
`esi` and `edi` are separately reused as a repeated constant argument
value across the other 9 calls in the same function: each is pushed 11
times total and popped only once (the real epilogue restore), so by
count alone they look exactly like arguments, not saves. Confirmed via
the disassembly that these two conditions (leading non-push setup lines,
plus a callee-saved register reused many times over as a literal
argument later in the same function) can and do occur together in real,
unremarkable game code -- this is not a hypothetical edge case.

With `_icall_esp` captured before the `esi`/`edi` saves instead of after
them, the fallback's `g_esp = saved_esp` on the very first call's
resolution failure permanently discards those two pushes -- 8 bytes --
from `sub_0026940F`'s idea of its own stack depth for the rest of its
execution. Every subsequent push/pop in the function (all 9 remaining
calls, all individually correct in isolation) now runs 8 bytes off from
where the function's own epilogue expects, and the epilogue's `pop edi;
pop esi;` at the very end reads two stack slots that are no longer the
ones it saved -- garbage is loaded into what should be the restored
`edi`/`esi`, propagated back into the caller `sub_001D8842`, whose own
epilogue's `pop ebx` (at an offset now also permanently displaced by the
same 8 bytes) is where `ebx = 0x00390037` was actually manufactured, many
calls before it ever reaches `sub_00062237`.

### The fix

`tools/recomp/translator.py`, `_fixup_icall_esp_save()`: added a skip
loop before the existing FPO-save-detection loop, advancing past leading
lines that are pure register/memory-load setup with no stack or
control-flow effect (no `esp`, `PUSH32`, `POP32`, `RECOMP_`, `goto`,
`return`, `if (`, or a label), stopping the moment it finds either a
`PUSH32(esp, ebx|esi|edi)` (hands off to the existing detection loop
unchanged) or anything else (falls back to today's behavior, unchanged).
This is strictly additive: it only widens what counts as "the start of
the FPO prologue," it does not change how that prologue is validated
once found (the existing push/pop-count matching still applies).

Added a fourth regression test to `tools/recomp/test_icall_esp_save.py`,
`test_fpo_save_after_vtable_fetch_setup`, modeled directly on
`sub_0026940F`'s real structure (two setup lines, two FPO saves, ten
calls with two of the pushed registers reused as repeated arguments) --
verifies the `_icall_esp` save lands after both the setup lines and both
FPO saves, not before either. All four tests in the file pass
(`py -3 tools/recomp/test_icall_esp_save.py`).

### Verified: full pipeline regeneration, clean rebuild, and a live retest

Regenerated the entire SCDA output via the canonical recomp-only command
(existing `scda_v060` disassembly, `--exclude-manual
projects/scda/src/recomp_manual.c`) -- 28159/28162 functions, 0 failed,
identical scale to the pre-fix generation. Confirmed in the fresh
`sub_0026940F` that `_icall_esp = g_esp;` now lands directly after
`PUSH32(esp, esi); PUSH32(esp, edi);` and before the first call-specific
argument push, for all 10 indirect-call sites. Full clean rebuild
succeeded (no new warnings beyond pre-existing, unrelated
`recomp_manual.c` unreferenced-label notices).

Ran the rebuilt `scda_recomp.exe` for ~90 seconds (179,000+ kernel
calls). Results:
- `0x00390037` does not appear anywhere in `scda_runtime.log` -- zero
  occurrences, down from the previous steady-state recurrence every
  frame.
- The specific null-vtable-call signature this whole investigation
  traced (`[ICALL] ... target=0x00000000 ... source=0x0005F701`) is
  gone; the only remaining `source=0x0005F701` hits (10 over the full
  run) now show real, non-null target addresses and legitimate
  heap-range `ecx`/`edi` values -- a different, much lower-severity
  "unresolved indirect-call target" case (same general RECOMP_ICALL_SAFE
  fallback, correctly triggered on some other, still-unidentified vtable
  slot; not investigated further this entry since it does not corrupt
  any preserved register the way this one did) rather than the corrupted
  garbage-pointer pattern this investigation targeted.
- The run reaches a **new, different steady state** (a repeating kernel
  call cycling through a small set of ordinals/slots) -- a distinct,
  not-yet-investigated blocker, flagged as the next item.

### Not this entry

The new post-fix steady state (kernel-call cycle across ordinals
99/234/246/277/294) is unexplored -- flagged as the next concrete blocker
per the standing instruction to continue autonomously once a relevant
result is documented. The 10 remaining benign-looking unresolved-icall
hits at `source=0x0005F701` (different target, non-null) are noted but
not chased -- no evidence they discard a preserved register the way the
`0x0026940F` case did, and the standing instruction for this bisection
was specifically the `0x00390037` corruption, now closed.

No stubs, no fake signals, no clamps, and no hand-edit to any generated
`.c` file survives -- the fix lives entirely in the code generator
(`tools/recomp/translator.py`) plus its test suite, exercised by a full,
real pipeline regeneration, not a patched-around symptom.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
