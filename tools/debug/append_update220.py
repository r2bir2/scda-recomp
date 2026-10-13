path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 220 (2026-09-21): Chain directly confirmed with exact numbers -- CORRECTS the stale==-1 guess to stale==7 (a legitimate value); true defect isolated to a hardcoded 0x4000 assumption instead of the already-available real transfer count; minimal register-liveness-safe fix implemented and verified to change runtime behavior (process now progresses past the old freeze into new territory)

**Task followed:** per explicit instruction, confirm the chain
`stale_value -> cursor formula -> remaining_space -> memcpy underflow`
directly, using minimal one-shot instrumentation with a host-side success
marker, fresh processes per attempt, max ~3 minute bounded waits, a single
`cdb` snapshot instead of long silent waits, and immediate restart if a run
is found already stuck in the known unrelated `sub_0003FCA5` freeze on a
different stream. Once confirmed, trace `stale_value`'s origin or determine
the correct fix; no clamp on cursor/remaining_space until this is understood.

**Orchestration built and used exactly as specified**
(`tools/debug/diag219_orchestrate.sh`, kept as a reusable tool): launches a
fresh `scda_recomp.exe`, polls for a marker file every few seconds, takes
one `cdb -pv -p <pid> -c "r eip; kb 3; q"` snapshot after 180s if the marker
hasn't appeared, kills and retries immediately if that snapshot shows the
known `sub_0003FCA5` freeze, otherwise gives it one more slice. This
directly avoids repeating the long, blind waits from Update 218.

**First two gate attempts missed the target (both cheaply diagnosed, not
symptoms of a real problem):**
1. A first one-shot capture gated on the same condition already proven at
   the *corrupting* call site (`remaining_space>0x4000`) but placed at the
   cursor-formula site itself fired in 34s -- but on the *first, completely
   ordinary* cycle for this object, since `remaining_space>0x4000` turns out
   to be true on almost every normal cycle for this buffer shape
   (`bufsize=0x24000`, formula output always in `[0,0x4000]`), not a sign of
   anything wrong.
2. Tightening the gate to `stale>0x4000` (expecting a wrapped/negative-
   looking sentinel) never fired across 8 fresh attempts in a row -- all 8
   independently landed in the pre-existing, already-documented
   `sub_0003FCA5` freeze on a *different* stream before ever reaching this
   one. Diagnosed via the orchestration's own `cdb` snapshots (each showed
   `sub_0003FCA5+0x17b0`'s `jmp $`, reached through a *different*
   `sub_00040821` call site than the one this instrumentation was watching)
   -- confirms Update 218's own finding about freeze-order nondeterminism,
   not a flaw in the new instrumentation.

**Final, successful design: reuse the already-proven trigger, not a new
guess.** Rather than gate a *new* condition at the cursor-formula site,
added a small, cheap, unconditional ring buffer (4 entries) recording every
cursor-formula update for this object (no I/O on the hot path), and dumped
its contents only when `sub_00040821`'s already-validated dangerous-
compaction condition (the exact one Updates 217/218 relied on) fires. This
reliably reproduced in 36 seconds on the very first attempt -- twice.

**First successful capture (ring recorded only one of the formula's two
textual sites) showed a gap:** the ring's most recent entry
(`cursor_after=0x21DE`) did not match the corrupting call's own reading
(`cursor=0x4001`) -- a missing link. Root cause: Update 215 already noted
the formula exists at **two** separate textual locations
(`loc_000408F0`, following call site 1, and `loc_00040992`, following call
site 2); the first capture only instrumented the second. Added the same
cheap recorder to `loc_000408F0` too, reran -- **gapless result:**

```
SCDA-DIAG220 CORRUPTING-CALL this=01135928 bufsize=00024000 cursor=00004001 remaining_space=0001FFFF ring_count=4
SCDA-DIAG220 ring[3] this=01135928 stale=00000007 cursor_before=00023FF9 cursor_after=00003FF9
```

`cursor_after=0x3FF9` plus the two 4-byte record-header field reads at
`loc_00040909` (each advancing the cursor by 4, +8 total) lands exactly on
`0x3FF9 + 8 = 0x4001` -- matching the corrupting call's own reading
bit-for-bit. **The chain is now fully, numerically confirmed with no gaps:**
`stale=7 -> cursor=0x4000-7=0x3FF9 -> +8 (header parse) -> cursor=0x4001 ->
remaining_space=bufsize-cursor=0x1FFFF -> underflowing memcpy destination.`

**This corrects the standing `stale==-1` hypothesis from Update 217/218.**
`stale=7` is not a wrapped/garbage sentinel -- it is a small, entirely
plausible "bytes remaining before the ring buffer's physical end" value.
The actual defect is not in `stale` itself, but in how the formula uses it:
`new_cursor = 0x4000 - stale` is only correct if the read that just
completed transferred *exactly* 0x4000 bytes (the formula's implicit,
hardcoded assumption -- matching Update 215's own description, "never uses
the actual transferred count"). Critically, **the real transferred byte
count is already loaded into `eax` two lines earlier in the exact same
block** (`eax = MEM32(ebp+-4); MEM32(ebx+0x4064) += eax;`, used to update an
accumulator) -- the very next lines then needlessly discard it
(`eax = 0x4000;` at `loc_00040992`, or `edx = 0x4000; eax = edx;` at
`loc_000408F0`) before computing the new cursor from the wrong, fixed
constant instead.

**Fix implemented (not a clamp -- a correction using data already at
hand):** removed exactly the one instruction at each site that discards the
already-correct transferred count, so `eax` (already holding
`MEM32(ebp+-4)`) flows directly into `eax = eax - MEM32(ebp+-12)`.
One subtlety required care: at `loc_000408F0`, `edx = 0x4000` is *also*
needed, unchanged, further down by the compaction's own destination formula
(`edi = edi + edx` inside `loc_00040942`) -- that assignment was
deliberately left untouched; only the subsequent `eax = edx` copy (which
served no purpose other than discarding the transferred count) was removed.
At `loc_00040992` there is no such second use, so the corresponding
`eax = 0x4000` line was simply removed outright. Applied identically to
both textual occurrences of each site (the `sub_00040821`/`sub_0004086E`
duplication already established this session).

**Verified the fix changes runtime behavior, not just the formula in
isolation.** Rebuilt clean, ran a fresh instance with the same bounded-wait
discipline: after ~4 minutes the process was found (via `cdb` snapshot, not
a kill) executing in `sub_004A12B0`/`sub_004A1600`/`sub_004A1740` -- three
addresses never seen anywhere in this session's entire investigation --
with CPU usage around 16% (a real, low-overhead poll/wait loop, not the
~100%-pegged hard `jmp $` spin the old freeze always showed) and a stable,
unchanging call stack across a further 2-minute recheck. **The process no
longer reaches the old corruption/freeze point at all** -- it has genuinely
progressed further than any run has in this entire investigation arc, and
is now blocked on a new, later, previously-unreachable synchronization
point. This is the next blocker referred to in the standing instruction
sequence ("...weiter bis zum naechsten Blocker"), not yet investigated.

**Critical caveat, not yet resolved: this fix currently has no permanent
home.** `recomp_0001.c` is gitignored, generated output; a full pipeline
regenerate from the original XBE (the "vollstaendige Recomp-Pipeline" step
in the standing instruction sequence) would silently reproduce the
original, byte-identical-to-the-XBE buggy instructions and erase this fix
with no warning, since the lifter has no per-function override/patch
mechanism found in this codebase (checked `tools/recomp/config.py`,
`__main__.py`, and for any existing address-keyed override list -- none
found for this or any other address). **Do not run a full pipeline
regenerate until this fix has a durable location** (a lifter-level patch
list keyed by address, a post-generation patch-application step, or moving
this one function to the hand-maintained `recomp_manual.c` as a full
replacement) -- this is now the top-priority follow-up, ahead of
investigating the new `sub_004A12B0` blocker.

**Housekeeping:** all `SCDA-DIAG-219`/`SCDA-DIAG-220` diagnostic code has
been fully removed from `recomp_0001.c` again (verified via grep), leaving
only the fix itself (tagged `SCDA-FIX-215`, four occurrences: two sites x
two duplicated functions).

**Model tier:** STRONG remains appropriate for the immediate next step
(finding a durable-patch mechanism is a bounded infrastructure task); the
`sub_004A12B0` blocker itself is unassessed and may need its own tier
judgment once looked at.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
