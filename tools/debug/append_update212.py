path = "docs/SCDA_WORKLOG.md"

with open(path, "rb") as f:
    raw = f.read()

text = """
## Update 212 (2026-09-21): sub_000516B7 itself is proven clean (object still valid at its own entry); the object flips to zero between two back-to-back, same-thread entries into sub_000516B7 -- pointing at the native kernel-bridge wait, not at any recompiled guest code

**Method:** added two more ring-buffer checkpoints (same diagnostic
technique as Updates 210/211, still zero cdb polling): callsite 11 at the
very first line of `sub_000516B7` itself (stashes `ebx` into a local before
its own prologue repurposes the register as scratch -- confirmed necessary
by reading the prologue: `PUSH32(esp, ebx); ... ebx = 0;` a few lines in),
and callsite 12 immediately after the one call `sub_000516B7` makes to
`sub_00052A85` (the generic kernel-bridge call-and-SetLastError wrapper,
`recomp_0002.c:50908` -- a 23-byte function that itself just does one
indirect call through the table slot `MEM32(0x4F5180)` and relays the result,
i.e. the actual blocking work happens entirely in whatever native host
function that table slot currently resolves to). One more fresh, fully
undisturbed run, one single static read after the freeze, same as before.

**Result:** the trailing 8-entry window, in exact chronological order, was:
1. Two callsite-7 entries (`sub_00040821` entries, both with the object
   fully valid) with no checkpoint recorded in between the first and second
   -- consistent with at least one ordinary `sub_00040821` call that did not
   need to issue a fresh read this cycle.
2. Callsite 11 (`sub_000516B7` entry) -- **object still fully valid**
   (`handle=0x48000005`, `buffer_size=0x24000`, `ring_ptr=0x1139998`),
   proving the corruption has not yet happened when `sub_000516B7` itself is
   entered.
3. **A second callsite-11 entry immediately after the first, same thread ID,
   with NO callsite-9 (the "`sub_000516B7` has returned" checkpoint in its
   caller) in between -- and this second entry already shows the object
   fully zeroed.**
4. Callsite 12 (right after the `sub_00052A85` call inside this second,
   already-corrupted invocation) -- still zero, consistent.
5. Callsite 9 (return to the caller) -- zero.
6. Callsite 10, then callsite 5 (the dispatch itself, return address
   `0x40992` again) -- both zero/consistent with Updates 210/211.

**What this proves:** `sub_000516B7`'s own translated body is innocent --
the object is provably still valid at its own entry (item 2), so nothing in
its prologue or in the direct-value parameters it receives (it is called
with individual scalar values, not the object pointer itself -- confirmed by
re-reading its caller's argument setup in `sub_00040821`, which pushes
`MEM32(ebx+0x3C)`, a computed buffer address, and plain scalars, never `ebx`
itself) can be the writer; it structurally has no reference to this object at
all except by accident. The interesting fact is item 3: a **second entry
into the exact same `RECOMP_TRACE_PC(0x000516B7)` checkpoint, on the SAME
thread, before the FIRST entry's own completion checkpoint (callsite 9) has
fired.** Since `sub_000516B7` has ~35 other call sites scattered across the
whole codebase (grepped: `recomp_0001/0002/0015/0018/0023/0026/0027.c`), this
is not necessarily true recursion in the C sense -- but the object pointer
being `0x1135928` again on the second entry, on the identical thread, with no
intervening return, means whatever reached this second entry did so *while
the first call was still logically in progress* (i.e. during whatever the
single `sub_00052A85`/table-slot-`0x4F5180` native call is doing -- almost
certainly a blocking or alertable wait). This is consistent with either a
same-thread APC/callback firing during an alertable wait inside the native
kernel bridge, or a second, distinct thread that happens to report the same
`GetCurrentThreadId()` only if this environment's thread-ID emulation itself
is suspect (not yet checked) -- both possibilities point away from the
already-fully-read recompiled guest code (`sub_00040821`, `sub_0004086E`,
`sub_000516B7`, `sub_00052A85` are all now confirmed clean) and toward the
**native host implementation of whatever `MEM32(0x4F5180)` currently
resolves to** (the actual disc-read kernel export), not yet inspected this
session.

**Model tier:** unchanged, MAXIMUM -- the recompiled-guest-code side of this
investigation is now exhausted (every function in the chain from
`sub_00040821` down to the kernel-bridge wrapper is read and clean); the
remaining work is entirely in native/host C (`src/kernel/`), a different kind
of code than anything read so far in this arc.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: the entire recompiled-guest call chain implicated in this bug is now
fully read and proven clean; the corruption must originate in native host
code (the kernel-bridge wait implementation, worker-thread pool, or a
buffer-reuse path), which is qualitatively different from the x86-recompiler
analysis this whole investigation arc has used so far and needs its own
careful, from-scratch read.

**Next test (for MAXIMUM):**
1. Identify what `MEM32(0x4F5180)` currently resolves to (the kernel export
   table slot `sub_00052A85` dispatches through) and read its native C
   implementation in `src/kernel/` (candidate: `kernel_file.c`, per Update
   186's reference to `scda_disc_zero_short_read_tail` living there) end to
   end, specifically for: worker-thread hand-off, any buffer pool/free-list
   reuse, or a completion callback that could plausibly zero an unrelated
   0x8070-ish byte region.
2. Verify whether this environment's `GetCurrentThreadId()`-equivalent could
   under any circumstance report the same ID for two genuinely different OS
   threads (would materially change the interpretation of this round's
   "same thread ID, no intervening return" observation) -- a quick, cheap
   check before trusting the single-thread reading further.
3. If a worker-thread pool is confirmed involved: add a native-side
   (non-recompiled) diagnostic directly in the kernel bridge's wait/
   completion path -- record the real OS thread ID (not the guest
   emulation) and a timestamp immediately before and after the actual
   blocking primitive, to see whether a second, genuinely different thread
   is active during that exact window.

**Instrumentation status (unchanged constraint):** two more temporary
checkpoints (callsite 11 in `sub_000516B7`'s entry, callsite 12 after its
`sub_00052A85` call) added in `recomp_0002.c`, same
`SCDA-DISC-ERR-INSTRUMENTATION` tags, same ring-buffer infrastructure from
Update 210. Still diagnostic only; will be reverted once the actual writer is
found in the native kernel-bridge code or this investigation closes.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
