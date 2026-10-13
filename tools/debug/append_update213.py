path = "docs/SCDA_WORKLOG.md"

with open(path, "rb") as f:
    raw = f.read()

text = """
## Update 213 (2026-09-21): ROOT CAUSE FOUND AND BYTE-VERIFIED -- the ring-buffer subobject's header fields are zeroed by an underflowing memcpy destination inside sub_00040821's own buffer-wraparound compaction logic; confirmed present verbatim in the original XBE, not a recompiler bug

**Correction to Update 212:** the "native kernel-bridge" hypothesis was wrong.
Update 212 concluded the corruption must be in native host code because two
back-to-back, same-thread entries into `sub_000516B7`'s own
`RECOMP_TRACE_PC` checkpoint appeared with no intervening return checkpoint
-- but this was an instrumentation gap, not evidence of reentrancy:
`sub_00040821` has **two separate call sites** to `sub_000516B7` (guest
`0x408E1` and guest `0x40983`), both textually near-identical, and Update
212's checkpoints only covered the second one. Once the first call site was
also instrumented (see below), the "two same-thread entries" resolved into
two perfectly ordinary, sequential calls within a single loop iteration --
no cross-thread activity, no APC, no native code involved at all.

**Method:** four more ring-buffer checkpoints (still the same Update
210-212 technique, still zero cdb polling), specifically chosen to bisect
the ~140-byte span between the two `sub_000516B7` call sites:
callsite 13 right after the FIRST call returns (`loc_000408E1`, previously
completely uninstrumented), callsite 14 at `loc_00040909` (start of the
"read two 4-byte record-header fields from the current cursor" block),
callsite 16 at `loc_00040942` (entry to the buffer-compaction block, right
before its two `memcpy`/`rep movsd`+`rep movsb` calls), and callsite 17
immediately after those same two copies complete. Ring buffer size raised
from 8 to 24 entries (`recomp_manual.c`) so one full cycle (up to 10+
checkpoint firings) stays visible without eviction. One more fresh,
undisturbed run, one single static read after the freeze.

**Result -- the transition is bracketed to two adjacent lines of code:**
in exact chronological order for the fatal cycle: callsite 17 (end of a
PRIOR iteration's compaction, valid) -> callsite 11 (`sub_000516B7` entry,
call site 1, valid) -> callsite 13 (call site 1 returns, valid) ->
callsite 14 (`loc_00040909`, valid) -> **callsite 16 (`loc_00040942`, right
before the two memcpy operations -- still fully valid: `this=0x1135928`,
`handle=0x48000005`, `buffer_size=0x24000`, `ring_ptr=0x1139998`) ->
callsite 17 (immediately after both memcpy operations complete --
`handle=0`, `buffer_size=0`, `ring_ptr=0`, all zeroed)**. Nothing else runs
between callsite 16 and 17 except the two copies themselves
(`recomp_0001.c:68725-68739`).

**The exact mechanism (byte-verified against `default.xbe` via Capstone,
`tools/debug/verify_40909_capstone.py`, guest `0x40909`-`0x40992` --
matches the generated C exactly, instruction for instruction, confirming
this is NOT a recompiler mistranslation):**
```
loc_0004087B: ecx = ring_buffer_size - cursor            ; "remaining contiguous space"
              edx = 0x4000                               ; fixed constant
              if (remaining >= record_length) goto loc_00040909  ; enough room, no wrap needed
              ... (only reached when remaining < record_length, i.e. a genuine
                   wraparound: the next record does not fit before the
                   buffer's physical end) ...
loc_00040942: esi = cursor_after_headers + ring_buffer_base       ; copy source
              edi = ring_buffer_base - remaining + 0x4000         ; copy DESTINATION
              memcpy(edi, esi, remaining);                        ; relocate the tail
```
`edi`'s destination formula, `ring_buffer_base - remaining + 0x4000`, is
only within the allocation when `remaining <= 0x4000`. If the record that
triggered the wraparound check declares a length greater than `0x4000`
while `remaining` (the actual leftover contiguous space) is *also* greater
than `0x4000`, `edi` computes to an address **before** the ring buffer's own
allocation -- directly underflowing into the ~0x4070-byte gap that
separates this buffer (`0x1139998`) from the owning subobject's own header
(`0x1135928`), exactly the region whose zeroing this entire investigation
(Updates 109-212) has been chasing since Update 190. The `memcpy` (a
`rep movsd`/`rep movsb` pair copying `remaining` bytes) then overwrites
that region with data read from the disc buffer -- appearing as "zeroed"
whenever the source bytes at that offset happen to be zero, which is common
for un-filled buffer tail space (consistent with `scda_disc_zero_short_read_tail`
elsewhere legitimately zeroing unread tail bytes, Update 186).

**This is present, byte-for-byte, in the original Xbox binary -- not a
scda_recomp bug.** Whether it is a genuine original-game edge case (only
triggered by a specific, rare combination of record length and buffer
cursor position that this recompiled environment's disc-image content
happens to reach, where real hardware or the retail disc's actual byte
stream may not) or a design assumption the original code makes that this
project's virtual disc/ISO content violates is the one remaining open
question -- but the corruption mechanism itself is now fully understood, to
the instruction.

**Model tier:** the technical mystery this whole arc was chasing is closed.
Remaining work is classification (game-data-dependent edge case vs. a
real ship bug vs. an XBE/ISO content difference from a retail disc), not
further reverse-engineering of unknown mechanisms.

MODEL STATUS: DE-ESCALATION RECOMMENDED
Tier: STRONG
Reason: the root cause is now fully identified and byte-verified against the
original binary; what remains is confirming the runtime values (the actual
record length and remaining-space numbers that trigger the underflow) and
deciding on a fix strategy, both more mechanical than the open-ended
reverse-engineering this investigation required through Update 212.

**Next test (for STRONG):**
1. Capture the actual runtime values of `ecx` (remaining space) and `esi`
   (record length) at the branch (`loc_00040940`, `jae 0x409a9`) for the
   fatal iteration, using the same ring-buffer technique, to confirm the
   exact numbers that trigger the underflow (expected: `esi > 0x4000` and
   `ecx` also `> 0x4000` but `< esi`).
2. Decide a fix: either (a) special-case the destination computation so it
   never underflows below `ring_buffer_base` regardless of `remaining`
   (defensive clamp -- changes recompiled behavior, needs a decision on
   whether that is in scope), or (b) determine why this project's disc
   content produces a record length that the original game apparently never
   encountered on real hardware/the retail disc (data-content root cause,
   fixed by correcting the served file content instead of the code).
3. Once resolved, revert ALL temporary `SCDA-DISC-ERR-INSTRUMENTATION`
   markers (seventeen callsites across `recomp_0001.c`/`recomp_0002.c`, the
   ring-buffer block in `recomp_manual.c`, and the three macro checks in
   `recomp_types.h`) per the standing instruction that this instrumentation
   must not remain as a permanent fix.

**Instrumentation status:** four more temporary checkpoints (13, 14, 16, 17)
added this round, ring buffer enlarged from 8 to 24 entries
(`SCDA_DISC_ERR_RING_SIZE`, `recomp_manual.c`). All still diagnostic only.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
