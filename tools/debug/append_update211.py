path = "docs/SCDA_WORKLOG.md"

with open(path, "rb") as f:
    raw = f.read()

text = """
## Update 211 (2026-09-21): Root cause window narrowed to a single function call -- the ring-buffer subobject is proven VALID at sub_00040821's entry and PROVEN ZEROED immediately after sub_000516B7 returns, within the same invocation

**Method:** extended the Update 210 ring-buffer instrumentation (still purely
diagnostic, still zero cdb polling) with three more checkpoints, all in the
generated `sub_00040821`/`sub_0004086E` bodies: callsite 7 at the very first
statement of `sub_00040821` (`ecx`="this" fresh from the caller, before
anything else runs), callsite 8 at the equivalent spot in `sub_0004086E`
(reads `ebx`, since that entry point is a mid-function continuation that
already expects `ebx`="this" set by its own caller -- added defensively,
never actually observed to fire), callsite 9 immediately after the
`sub_000516B7` call returns (before either success/failure branch runs), and
callsite 10 immediately before the indirect dispatch through
`MEM32(0x5E6E0C)`. All four read the same three header fields already
established in Updates 187/188 (`+0x3C` file handle, `+0x4058` buffer size,
`+0x405C` ring-buffer pointer). Two fresh, fully undisturbed runs; each
frozen state read exactly once via a single static memory read (ring buffer
proven unchanging across repeated reads), no live attach, no watchpoint.

**Result:** across ~3.03 million total `sub_00040821` invocations in a single
boot (`g_scda_disc_err_ring_index` read back as `0x2e53d3`), the last 8-entry
window before the fatal dispatch was, in exact chronological order:
1. Five consecutive callsite-7 entries (five ordinary prior calls to
   `sub_00040821`), **all fully valid**: `this=0x01135928`,
   `handle=0x48000005`, `buffer_size=0x00024000`, `ring_ptr=0x01139998` --
   identical across all five, all on thread `0x343c`.
2. Callsite 9 -- **immediately after `sub_000516B7` returns, within the very
   next `sub_00040821` invocation after the five valid ones above (no
   callsite-7 entry in between)** -- same `this=0x01135928`, same thread
   `0x343c`, but `handle=0`, `buffer_size=0`, `ring_ptr=0` -- **completely
   zeroed.**
3. Callsite 10, immediately before the dispatch: still all-zero (consistent,
   nothing un-zeros it in between).
4. Callsite 5, the dispatch itself, confirming return address `0x40992`
   (same as Update 210).

**Conclusion -- the exact window is now known:** the object is provably
intact at this invocation's own entry (callsite 7, valid, immediately
preceding callsite 9 with no other `sub_00040821` call in between) and
provably fully zeroed by the time `sub_000516B7` returns from that same
invocation (callsite 9). `sub_00040821`'s own code between its entry and the
`sub_000516B7` call was already fully read in Updates 179-181 and writes
nothing to these fields. This means the zeroing happens **during the
`sub_000516B7` call itself** -- i.e. during the actual blocking disc-read
operation (already traced in Update 196 as `sub_000516B7` -> `xbox_NtReadFile`
-> `sub_00052A85`/`sub_00052A57`, and confirmed clean/byte-verified there).
This is the first time in this entire investigation (spanning Updates
109-210) that the zeroing has been bracketed to a single, specific function
call rather than "somewhere before the fatal handler" or "somewhere in a
1700-call streaming loop."

**What this does and does not prove:** it does not identify the writer. It
strongly narrows the field: since `sub_00040821`'s own code and
`sub_000516B7`'s own translated body were already read and are clean, the
most consistent explanation with all prior evidence (including the
originally-proposed, later only-partially-refuted Update 188 thread-race
hypothesis -- Update 190 only excluded the two known
destructor/cleanup functions `sub_00040B00`/`sub_000407F7` as the mechanism,
never "any other thread activity" in general) is a genuine cross-thread
write: while thread 0 blocks inside `sub_000516B7` waiting for the real I/O
to complete, another thread (the process has several -- `nv2a_ack_thread`,
multiple `TppWorkerThread` instances, per Update 188) writes zero across this
exact object, through a path not yet identified.

**Model tier:** unchanged, MAXIMUM -- the search space is now extremely
narrow (one function call, one specific choke point) but the actual writer
requires either instrumenting inside the native I/O wait path or a reliable
way to observe another thread's write to this address, neither of which this
round attempted.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: root cause is now bracketed to a single function call with byte-exact
before/after evidence, but identifying the actual writer requires either
native-code instrumentation inside the I/O-wait path or a reliable
cross-thread write observation method -- previous attempts at the latter
(Updates 188/189, hardware watchpoints) were unreliable in this sandbox, so a
new approach is needed rather than repeating them.

**Next test (for MAXIMUM):**
1. Add one more checkpoint at `sub_000516B7`'s own entry (record the object
   state there too) to determine whether the object is still valid at that
   point -- narrows further whether the zeroing happens in
   `sub_000516B7`'s own translated setup code (recompiler-side, single
   threaded) vs. genuinely during the blocking wait deeper in the native I/O
   layer (cross-thread).
2. If confirmed to happen during the actual wait: instrument the native
   kernel-bridge wait primitive itself (e.g. wherever `xbox_NtReadFile`/
   `sub_00052A85` blocks) to record which native thread ID is executing at
   the moment the wait begins and ends, to correlate against whichever
   thread performs the actual zero-write.
3. Consider a native (not recompiled-guest) guard: temporarily
   `VirtualProtect` the exact object's page to read-only for the duration of
   the `sub_000516B7` call and catch the writer via the resulting access
   violation's faulting instruction -- likely more reliable in this sandbox
   than the hardware watchpoints that failed in Updates 188/189, since it
   does not depend on cdb debug-register plumbing at all.

**Instrumentation status (unchanged constraint from Update 210):** the
`sub_00040821`/`sub_0004086E` callsite-7/8/9/10 checkpoints added this round
are additional temporary diagnostic code in the same already-noted files,
under the same `SCDA-DISC-ERR-INSTRUMENTATION` tags. Still diagnostic only,
still to be reverted once the actual writer is found or this investigation
closes.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
