path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 218 (2026-09-21): Live capture REFUTES Update 217's own unified hypothesis -- the outer loop's counter is already zero at the moment of corruption, meaning this is the loop's normal, expected final iteration, not an extra one; also discovered genuine run-to-run nondeterminism in which stream trips the pre-existing sub_0003FCA5 freeze first

**Task followed:** Update 217 proposed a specific, falsifiable hypothesis
(the outer loop in `sub_000430A5` calls `sub_00040821` one extra,
unnecessary time after a short read because its own byte counter has not
yet reached zero) and named the exact next test: capture
`MEM32(object_this + 0x40B0 - 8)` (the outer loop's own remaining-byte
counter, reachable from inside `sub_00040821` since `ebp_outer = this +
0x40B0`) at the moment of the known corrupting compaction. Implemented this
as a new, minimal, one-shot capture (`SCDA-DIAG-217` marker, gated on
`vtable==0x4F6940 && remaining_space>0x4000`, guarded to fire at most once)
directly in `recomp_0001.c` at `loc_00040942` (both textual occurrences,
`sub_00040821` and its duplicate `sub_0004086E`), rebuilt, ran.

**Result -- the hypothesis is REFUTED:**

```
SCDA-DIAG217 this=01135928 vtable=004F6940 bufsize=00024000 cursor=00004001
             remaining_space=0001FFFF outer_ebp=011399D8 outer_remaining=00000000
```

`outer_remaining=00000000`. The outer loop's own counter is *already zero*
at the exact moment the dangerous compaction fires. Given the loop
structure found in Update 217 (`ebx = min(remaining, 0x1000); remaining -=
ebx; call sub_00040821(...); call sub_000517A4(...); if (remaining != 0)
goto loop;`), a counter of exactly zero *entering* the call to
`sub_00040821` means this is not an extra, erroneous iteration -- it is the
loop's normal, final iteration, consuming the last (possibly partial)
`<=0x1000`-byte unit before the loop's own `!= 0` check would exit it. The
loop cannot skip this call; real hardware must make the exact same final
call, since the code is byte-identical. This rules out "the outer loop
asks for one call too many" as an explanation on its own.

**Refined mechanistic understanding.** `cursor=00004001` is itself highly
suspicious: it is exactly what the already-known buggy formula from Update
215 (`MEM32(ebx+0x4060) = 0x4000 - MEM32(ebp-12)`) would produce if the
"stale, pre-read remaining space" input were `0xFFFFFFFF` (-1), since
`0x4000 - (-1) = 0x4001` under 32-bit wraparound. This means the garbage
cursor value was almost certainly written by the *previous* invocation's
own execution of the exact defect Update 215 already found and byte-verified
against the original XBE -- and that garbage cursor is what then makes
`remaining_space = bufsize - cursor = 0x24000 - 0x4001 = 0x1FFFF` compute
as a huge, dangerous number on this, the very next visit to the compaction
path. Under this reading, **the cursor-formula defect (Update 215,
explanation 3) is independently sufficient to explain the corruption on
this exact, ordinary final loop iteration -- no terminator/tag question and
no outer-loop-iteration-count discrepancy (Update 217's own hypothesis) are
needed as additional mechanisms.** The remaining open question narrows
further, cleanly, to: why does the *previous* invocation's short read
produce a "stale" input of exactly -1 (or something that maps to a
dangerous cursor) on recomp, while the analogous moment on real hardware
apparently does not.

**Attempted a direct follow-up capture, could not complete it --
discovered a new, important environmental fact instead.** To verify the
"stale value = -1" arithmetic directly, added a second one-shot capture
(`SCDA-DIAG-218`) at `loc_00040992` (the line that writes the cursor),
logging every update for the specific object address `0x01135928` seen
above. Two things went wrong in sequence, and untangling them produced a
useful new finding:
1. First suspected the new capture itself caused a hang (two long,
   silent, 100%-CPU-pegged runs in a row with the new code present, after
   two earlier runs with only the first capture had returned quickly).
   Reverted the second capture and reran to isolate it.
2. The *same* silent, CPU-pegged behavviour then reappeared with only the
   already-proven first capture in place, ruling out the second capture as
   the cause. A live, non-invasive snapshot (`cdb -pv -p <pid> -c "r eip;
   kb 3; q"`, the exact safe method Update 210 already established) on a
   run that had been silently pegging one core for 64+ minutes showed the
   process was **not stuck or slow -- it had already reached the same
   pre-existing, long-documented terminal freeze from Update 210**
   (`jmp $` self-loop at `sub_0003FCA5+0x17b0`, reached this time via a
   *different* call into `sub_00040821+0x11cd`, i.e. a different streamed
   resource than the `common.lin` one this round's instrumentation is
   watching). Update 210 itself already saw the identical symptom once
   (36 minutes of silence before an equivalent snapshot found the same
   frozen state) and characterized it as normal variance, not a hang.

**New finding: which stream trips the pre-existing `sub_0003FCA5` freeze
first is not deterministic run-to-run**, even though the guest addresses
and computed values *within* a given stream's own processing are highly
reproducible (the exact same `this=0x01135928` recurred across multiple,
independently-launched runs this session). This is consistent with
`sub_000516B7`'s underlying disc I/O being asynchronous (`STATUS_PENDING`
/ callback-completion, per Update 213's own reading) -- real OS-level I/O
completion ordering can vary run to run even for single-threaded-looking
guest code, so whichever unrelated stream happens to reach the fatal
`MEM32(0x5E6E0C)` callback path first can differ between otherwise
identical launches. Two of four total runs this round reached the
`common.lin` diagnostic point productively; the other two happened to
freeze on a different stream first and never reached it. This is a useful,
durable environmental fact for future debugging of this whole area: a
single non-responsive run is not evidence of a regression by itself, and a
live `cdb -pv` snapshot (never a kill) is the correct, low-cost way to
distinguish "still progressing" from "already at the known terminal
freeze" before deciding whether to wait longer or restart.

**Housekeeping:** both one-shot captures (`SCDA-DIAG-217`, the never-
completed `SCDA-DIAG-218`, and the `#include <stdio.h>` they needed) have
been fully removed from `recomp_0001.c` again, verified via grep (zero
remaining `SCDA-DIAG` references) and a clean rebuild.

**No fix implemented.** The root cause is now understood one level more
precisely than Update 217 left it: the cursor-formula defect from Update
215 is very likely sufficient by itself to explain the corruption on this
stream's own final loop iteration, without needing an additional
outer-loop-iteration-count mismatch. What remains open is *why* the
previous invocation's short read produces a dangerous stale input on
recomp specifically -- this has not yet been captured directly (the
`SCDA-DIAG-218` attempt to do so was blocked by the freeze-ordering
nondeterminism described above, not disproven).

**Next test:** retry the `SCDA-DIAG-218`-style capture (full sequence of
cursor-formula updates for the specific object, not just the final one),
accepting that it may take 2-4 attempts due to the newly-understood
freeze-ordering nondeterminism; use a live `cdb -pv` snapshot rather than a
fixed wall-clock timeout to distinguish "still progressing toward
`common.lin`" from "already frozen on an unrelated stream" without killing
a run that might still be about to succeed.

**Model tier:** STRONG remains appropriate -- this is a bounded, well-
specified capture task with an understood, characterized source of run
variance, not open-ended search.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
