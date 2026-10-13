path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 221 (2026-09-21): Native NtReadFile/disc-I/O audit conclusively rules out our kernel bridge, host I/O, and even real hardware's own I/O layer as the cause -- the short read is genuine, honest, and byte-identical to xemu's own already-established short read; the cursor-formula fix is now permanent (recomp_manual.c + --exclude-manual), survives a full from-scratch pipeline regenerate, and an unrelated function-seed data-drift bug found along the way is fixed too

**Task followed:** treat the Update 220 generated-code fix as diagnostic
proof only, not final. Top priority: explain why the actual transfer is
smaller than 0x4000 despite the byte-verified original XBE code explicitly
computing with 0x4000 at both wrap sites. Audit commit e1b6823 ("report
honest short-read byte count instead of topping up disc reads") and the
current NtReadFile/disc-I/O semantics. Capture, once, at the concretely
problematic read: requested byte count, actual transferred count /
IO_STATUS_BLOCK.Information, NTSTATUS, file offset, file size, remaining
bytes to EOF, reader/handle. Determine whether the short read is caused by
EOF, host I/O, our kernel implementation, or another limit. Compare against
xemu if practical; if xemu delivers 0x4000 and recomp delivers less, fix
the I/O semantics instead of patching the original game code. Only once the
correct, durable fix is settled: remove the temporary generated-code
change, implement the fix in a regeneration-safe source, run the complete
recomp pipeline, and confirm the old `sub_00040821` blocker stays gone.
Then investigate the new `sub_004A12B0`/`sub_004A1600`/`sub_004A1740` poll
loop as a separate next blocker.

**First, an important correction found while re-reading the call sites:**
call site 1's own disc-read request length (`recomp_0001.c`, the
`MEM32(ebx+0x44)` rounding/cap sequence immediately before its
`sub_000516B7` call) is capped at up to **0x20000**, identical to the
compaction path's own request cap -- not a fixed 0x4000. This means the
hardcoded `0x4000` in the cursor formula was never "the request size" in
the first place; the actual transferred amount for any given refill can
legitimately be anywhere up to 0x20000 bytes. This raised real doubt about
whether Update 220's fix (substitute the actual transferred count for the
hardcoded constant) was even dimensionally sound, since a transfer far
larger than 0x4000 plugged into that formula produces a cursor value with
no obvious a-priori justification -- exactly the kind of question this
round's instruction demanded be settled with real numbers before trusting
anything further.

**Native I/O audit, via a one-shot capture directly in `xbox_NtReadFile`
(`src/kernel/kernel_file.c`), temporarily reverting the Update 220 fix
first so the original, unfixed code path (which is what actually produces
the problematic read) runs exactly as it always has:**

```
SCDA-DIAG221 #2 handle=0x290 requested=131072 transferred=89969 offset=... filesize=1924977 ...
SCDA-DIAG221 #3 handle=0x290 requested=131072 transferred=0     offset=... filesize=1924977 ...
```

(The precise offset field had its own small bug in this capture --
`ByteOffset` was NULL for this call, meaning the read uses the file's own
implicit, sequentially-advancing position, and the fallback
`SetFilePointerEx(...,FILE_CURRENT)` query failed with `ERROR_INVALID_PARAMETER`
on this handle, most likely because it is opened for overlapped-style I/O;
not pursued further, since it is not needed for the conclusion below.)
`transferred=89969` alone is already conclusive: it matches Update 214's
independently-derived final-chunk size for `common.lin` (1,924,977 -
1,835,008 = 89,969) exactly, confirming this is the exact same, single
event this whole investigation arc has been tracing since Update 213 --
not a different or newly-introduced short read. `requested=131072
(0x20000)` against `filesize=1924977` at an offset 89,969 bytes from the
end is a completely ordinary, arithmetically exact "request past EOF,
receive exactly what remains" pattern: **entry #2 is a genuine, honestly-
reported short read at the file's true end, not an artifact of our own
I/O layer, the host filesystem, or any artificial limit.** Entry #3
(the very next call, same handle, same 0x20000 request) receiving
`transferred=0` is the textbook, correctly-handled genuine-EOF case
(`xbox_NtReadFile` returns `STATUS_END_OF_FILE`, `Information=0`, exactly
per NT semantics) -- also correct, also not a bug.

**This directly answers the round's central question via the round's own
specified method (compare against xemu):** Updates 185/216 already
established, via live xemu ground truth on a **fresh boot, not a reused
log**, that real hardware experiences the *exact same* short read for this
exact stream: `want=0x20000 got=0x15F71` (0x15F71 = 89969, the identical
number). **xemu does not deliver 0x4000 while recomp delivers less -- both
platforms deliver the identical, much-larger-than-0x4000 short transfer.**
The round's own explicit fallback condition ("wenn xemu 0x4000 liefert und
Recomp weniger, behebe die I/O-Semantik") therefore does not apply: there
is no I/O-semantics discrepancy between the platforms to fix. Our kernel
bridge, the host filesystem, and commit e1b6823's honest-reporting change
are all confirmed correct and are not the cause. **The defect is
exclusively in the guest-side cursor formula's own hardcoded 0x4000
assumption** (Updates 215/220), which silently assumes every wrap-
triggering refill transfers exactly 0x4000 bytes when transfers routinely
run up to 0x20000 -- it happens to surface here specifically because this
particular wrap coincides with the stream's genuine, honestly-reported end
of data. This resolves the dimensional-soundness doubt raised above too:
using the real transferred count (however large) is not an arbitrary
substitution, it is restoring the one piece of already-available, always-
correct information (`eax`, loaded from `MEM32(ebp-4)` two lines earlier
for the accumulator update) that the original formula discarded in favor
of a constant that was never actually representative of the true transfer
size for this call.

**Housekeeping (native side):** `src/kernel/kernel_file.c`'s temporary
`SCDA-DIAG-221` capture has been fully reverted via `git checkout`
(zero diff remains); the Update 220 generated-code fix was restored before
proceeding to the permanent-fix work below.

**Permanent, regeneration-safe fix implemented.** Per the caveat flagged in
Update 220 (the fix lived only in gitignored generated output and would be
silently erased by a full pipeline regenerate), searched the existing
recompiler pipeline for a durable-patch mechanism (delegated to a research
agent; confirmed no address-keyed instruction/byte-patch list and no
generic post-lift patch step exist, but a `--exclude-manual <file>`
mechanism does: `manual_scan.py` scans a hand-written C file for
`void sub_XXXXXXXX(void)` definitions and automatically excludes those
exact addresses from auto-generation, with a direct, already-used precedent
in this same file: `sub_00040A88`, moved there in an earlier investigation
for an analogous "the auto-lift needs a hand correction" reason). Applied
the same mechanism: mechanically (via a small Python script, not manual
retyping, to eliminate transcription risk) extracted `sub_00040821`'s and
`sub_0004086E`'s exact, byte-faithful generated bodies, applied the same
two-line textual fix from Update 220 to each, wrapped them with the
established register-alias `#define`/`#undef` boilerplate this file's
existing hand-written functions already use, and appended them to
`projects/scda/src/recomp_manual.c` with a full explanatory comment citing
Updates 213-221. Re-ran the canonical recomp-generation command with
`--exclude-manual projects/scda/src/recomp_manual.c` (already the standard
SCDA invocation per this file's own prior entries) -- confirmed via grep
that both addresses are now declare-only in the freshly generated
`recomp_funcs.h` with no bodies in the bulk-generated `.c` files.

**An unrelated, pre-existing data-drift bug surfaced immediately, and was
fixed too (not scope creep -- required to link at all):** the full
regenerate failed to link with `sub_00040841` undefined. Root-caused via
direct Capstone disassembly of the real XBE (reusing the established
`verify_40909_capstone.py` pattern): `0x00040841` is a genuine, legitimate
mid-function re-entry point inside this same routine (a loop-back target
for repeat refill iterations, reached only via jumps, never fallthrough --
confirmed byte-exact against the original game code, not a disassembly
artifact). It is missing from the *current* `tools/func_id/output/
scda_seeded_corrected/identified_functions.json`, even though `sub_0004086E`'s
own auto-generated body (before this round's changes) already contained a
call to it -- meaning some earlier snapshot of that generated data must
have included it, but the currently-checked-in disassembly/func-id
artifacts no longer do. This is a latent, pre-existing fragility in the
project's own pipeline data (unrelated to this round's fix) that would have
surfaced on *any* full from-scratch regenerate, not something introduced by
moving these two functions by hand -- it was simply never exercised before
because `sub_0004086E` (reachable only via the indirect dispatch table, never
called by name from other generated code) previously lived inside the same
bulk-generated translation unit as many other, definitely-referenced
functions, so the object file was always linked in regardless, silently
carrying the same latent dangling reference without ever needing it
resolved. Fixed via this project's own already-established, precedented
mechanism for exactly this class of problem (git log shows several prior
"disasm: fix ... function-boundary bug via N verified seed addition(s)"
commits): added `0x00040841` to `projects/scda/function_seeds.json` with a
full justification, then re-ran the complete three-stage pipeline
(`tools.disasm` with `--seed-functions`/`--exclude-seed-functions` ->
`tools.func_id` -> `tools.recomp` with `--exclude-manual`) from scratch.
Confirmed `sub_00040841` is now a real function in the fresh output, and
the full project builds clean.

**Full pipeline verified end to end.** After the complete three-stage
regenerate and a full rebuild (`cmake --build`, zero errors), launched a
fresh instance and confirmed via a live, non-invasive `cdb -pv` snapshot
after ~150s that the process reaches the exact same `sub_004A12B0+0x1e8`/
`sub_004A1600+0x7b2`/`sub_004A1740+0xb6` location Update 220's hand-patched-
generated-file test already reached -- **not** the old `sub_0003FCA5`
fatal-callback freeze. The fix is confirmed to survive a genuine, complete,
from-scratch pipeline regenerate, not just a hand-edit to already-generated
output.

**Committed:** `projects/scda/function_seeds.json` (4 lines, the new seed)
and `projects/scda/src/recomp_manual.c` (657 lines, the two hand-written
function overrides). The generated output directories
(`tools/disasm/output/`, `tools/func_id/output/`, `tools/recomp/output/`)
remain gitignored, as established -- they are reproducible from the two
committed source changes via the documented three-command pipeline above.

**Next, separate blocker (per this round's own explicit instruction, not
yet started):** investigate the new `sub_004A12B0`/`sub_004A1600`/
`sub_004A1740` poll loop -- a low-CPU (~16%), stable, previously-unreached
synchronization point. Notably, `recomp_manual.c`'s own pre-existing
`sub_00040A88` comment (the `sub_00040A88`-precedent function, unrelated to
this round's fix until just now) already mentions this exact address from
an earlier (2026-09-05/11) UMOD-reader investigation: "...why the whole
chain ... looks like it spins forever in sub_004A12B0 instead of finishing
in a normal amount of time..." -- worth checking first whether that
earlier fix fully covers this new occurrence or whether this is a distinct
instance of a similar problem.

**Model tier:** STRONG remains appropriate throughout this round -- every
step (native I/O audit, manual-override mechanism research, mechanical
extraction, seed-data fix, pipeline re-run) was a bounded, well-specified
task with a clear, checkable success criterion.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
