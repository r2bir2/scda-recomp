path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 233 (2026-09-22): characterize the post-Update-232 kernel-call steady state -- a real pool-worker thread blocks forever inside a legitimate alertable-wait retry loop; one unrelated dead-bridge bug found and fixed along the way

Direct continuation per the standing instruction: with Update 232's fix
confirmed and closed, characterize the new steady state that appears after
it (repeating kernel calls cycling through ordinals 99/234/246/277/294)
before touching anything, per the same hierarchical-narrowing methodology.

### Step 1: what do these ordinals mean, and what shape does the cycle have

`src/kernel/kernel_bridge.c`'s ordinal tables identify the five: 99 =
KeDelayExecutionThread, 234 = NtWaitForSingleObjectEx, 246 =
ObReferenceObjectByHandle, 250 = ObfDereferenceObject, 277 =
RtlEnterCriticalSection, 294 = RtlLeaveCriticalSection. The existing
`scda_runtime.log` from Update 232's own 90-second verification run (still
on disk, no rerun needed for this first pass) showed two distinct `esp`
bases interleaved in the post-transition tail (`0x00F7B6xx` and
`0x013F9Exx`), i.e. two threads each independently looping through this
small ordinal set with no new `[FILE]`/`[PATH]`/`[READ]`/`[ICALL]` activity
at all afterward.

The transition itself: immediately before the steady state begins, the
game re-opens `splintercell4xboxretail.umd` (the main disc image, already
opened once earlier) and calls ordinal 37 (`FscSetCacheSize`) on it, which
has no bridge and returns 0. A live call-count diagnostic (added and
reverted in `kernel_bridge.c`'s dispatch fallback, not shipped) proved this
warning is a herring: ordinal 37 and the whole IoCompletion/Fsc family
(35, 79, 90, 91, 191, 223, 227) are each called *once* over a multi-hundred-
thousand-call run, never in a loop, so they cannot be the mechanism behind
a sustained cycle -- ruled out with direct evidence rather than left as
"probably fine".

### Step 2: the render-loop Enter/LeaveCriticalSection churn is a pre-existing, already-documented characteristic, not new

The single highest-volume call pair in the post-transition tail
(`RtlEnterCriticalSection`/`RtlLeaveCriticalSection` around a fixed lock at
Xbox VA `0x6B27A0`, reached through a lazy-init wrapper `sub_000767ED`
called from a per-frame timer-smoothing routine, `sub_00039566` ->
`sub_002E2F8B`/`sub_002E2F99`) turned out, via a one-shot live checkpoint,
to be running continuously from very early in the boot sequence onward --
not something that starts at the transition. Its raw call volume (tens of
thousands over a run) is consistent with this project's own already-closed
2026-09-11 finding recorded directly in `kernel_bridge.c`'s `RECOMP_KERNEL_HIST`
comment: this recomp does not yet pace itself to real display/vsync
timing, so per-frame housekeeping runs far faster than 60 Hz by design/
known-limitation, not by bug. Chasing this thread further would have
reopened an already-closed, unrelated finding; ruled out and dropped.

### Step 3: the real signal -- ObfDereferenceObject is a dead bridge, and the poll it's part of never terminates

A live per-ordinal call counter (temporary, in `kernel_thunk_dispatch`'s
"no bridge" fallback, reverted after use) showed ordinal 250
(`ObfDereferenceObject`) growing without bound in the steady state (588,
603, 618, ... +15 every 3 seconds) while every other missing-bridge ordinal
stayed flat at a handful of one-time calls. Reading `kernel_bridge.c`
found `bridge_ObfDereferenceObject` fully implemented (calls the real
`xbox_ObfDereferenceObject`) but never wired into the dispatch switch --
`/* case 250: bridge_ObfDereferenceObject */`, commented out, sitting two
lines below the already-fixed, already-documented `NtSuspendThread`/
`NtResumeThread` entries that describe *exactly this class of bug*: "the
implementation was already here and only the dispatch entry was missing,
which is worse than an outright stub". Whoever wired up 224/231 evidently
missed the adjacent 250 (and 252/253, left alone this entry as unrelated
networking ordinals, out of scope here).

Checking `xbox_ObfDereferenceObject`'s real body (`kernel_ob.c`) shows it
is *itself* a documented no-op by design -- Win32 handles manage their own
lifetime, so this project deliberately doesn't reference-count them.
Wiring the dispatch entry is therefore a genuine, safe, zero-risk
correctness fix (matches this file's own established fix pattern for dead
bridges) but -- confirmed by rerunning after the fix -- has no effect on
the steady state, because the real bridge does nothing observable either.
Fixed anyway in `kernel_bridge.c` (dispatch switch, ordinal 250 -> `bridge_ObfDereferenceObject`)
since leaving a proven dead, already-precedented bridge unwired serves no
purpose and the project's own comments already flag this exact class of
bug as worth fixing on sight.

### Step 4: the ObReferenceObjectByHandle poll resolves fine -- the thread it's waiting on is genuinely still running

A live checkpoint in `bridge_ObReferenceObjectByHandle` (temporary,
reverted after use) showed the polled handle (`0x48000007`, tagged, from
`s_handle_kthread_va[7]`) resolves *consistently and successfully* to a
stable non-null fake-KTHREAD address (`0x013FB000`) on every one of 800+
samples -- not stuck on null, not flapping. `bridge_mark_thread_done`
(which flips this structure's "done" byte) is simply never called for this
particular kthread_va, meaning the underlying worker thread has genuinely
not exited yet, not that the resolution mechanism itself is broken. This
directly rules out a second occurrence of the *already-fixed* Update
169/170 "always resolves null" bug -- different failure shape, confirmed
by direct evidence, not assumed innocent.

### Step 5: tracing the actual worker thread -- a legitimate alertable-wait retry loop that never sees its object signaled

`PsCreateSystemThreadEx` creates exactly 4 worker threads this run, all
through the same generic CRT `_threadstartex`-shaped trampoline
(`sub_000524DA`), which just calls `ctx1(ctx2)` -- so the real per-thread
behavior lives at whichever `ctx1` address each call passes: `0x00054701`,
`0x004BBE90` (used twice, with different `ctx2` data each time -- a
2-worker pool processing distinct task objects), and `0x00036889`.

`sub_004BBE90` (the pool routine) is a short, straight-line, *non-looping*
function: wait once on a handle from its task object, then do a small
amount of conditional cleanup, then return. Tracing its wait call down
(`sub_00052D2E` -> `sub_00052C18`) finds a manual retry loop:

```
loc_00052C2C:
    NtWaitForSingleObjectEx(handle, mode=1, alertable, timeout_ptr);   // ordinal 234
    if (result < 0) goto error;
    if (timeout == 0) goto done;
    if (result == 0x101) goto loc_00052C2C;      // retry
    goto done;
```

`0x101` is `STATUS_ALERTED` -- this is textbook-correct alertable-wait
retry code (a wait interrupted by a delivered APC, not real completion,
should simply wait again), not a bug in the game. The call site pushes
exactly 4 arguments matching ordinal 234's real 16-byte/4-argument size
(`g_slot_arg_bytes[234]`), so there is no repeat of the already-documented
`NtWaitForMultipleObjectsEx` 4-vs-6-argument stack-corruption class of bug
sitting a few hundred lines above this one in the same file -- checked
directly rather than assumed, given how easily this file's history shows
that exact mistake recurring in sibling wait wrappers. One positional
oddity was noted (the pushed value that lands in the bridge's `Alertable`
slot is actually the caller's raw timeout value, not a boolean) but
`WaitForSingleObjectEx`'s `Alertable` parameter only controls whether a
queued APC can interrupt the wait early -- it does not gate whether a
real signal completes it -- so this does not explain a permanent block and
was not chased further as a fix target this entry.

`xbox_NtWaitForSingleObjectEx` (`kernel_sync.c`) calls the real Win32
`WaitForSingleObjectEx` on the resolved native handle. The wait is
genuine, not simulated: if nothing in this recomp ever signals that
specific handle, the worker thread blocks here forever, by design of the
underlying Win32 primitive -- exactly matching the observed behavior (the
worker never returns, `bridge_mark_thread_done` is never called for it,
and the main thread's `ObReferenceObjectByHandle` poll for "is this thread
done" therefore never sees completion).

### Flagged, not chased further this entry

`sub_004BBE90`'s conditional cleanup path calls `sub_004BBD90` when a task
field is non-zero -- `sub_004BBD90` is named directly in the *closed*
Bink-worker/ring-buffer investigation from earlier updates. This is a
structural adjacency (same function family), not the same mechanism (that
investigation's fix was a 4-vs-6-argument stack-corruption bug in the
*multi*-object wait path, `sub_00052D40` -> `sub_00052C5C` ->
`NtWaitForMultipleObjectsEx`; this entry's finding is the sibling
*single*-object wait path, `sub_00052D2E` -> `sub_00052C18` ->
`NtWaitForSingleObjectEx`, and the block happens before `sub_004BBD90` is
ever reached). Per the standing instruction not to reopen that closed path
without new direct evidence, and since this is a different call chain, it
is recorded here as a related-but-distinct area rather than folded into
either investigation. The concrete next step is to identify what this
specific pool worker's handle actually is (an event? a semaphore? tied to
which subsystem's producer?) and whether the producer that should signal
it is unimplemented, unreached, or itself blocked -- not yet done.

### Fix applied this entry

`src/kernel/kernel_bridge.c`: wired ordinal 250 (`ObfDereferenceObject`)
into the kernel dispatch switch, matching the exact precedent set by the
`NtSuspendThread`/`NtResumeThread` fix a few lines above it. Confirmed via
rebuild + rerun: the "no bridge for ordinal 250" warning is gone, no new
warnings or regressions, and (as predicted from reading the real
implementation) the steady state is unchanged, since the real
`xbox_ObfDereferenceObject` is itself a documented no-op.

No stubs, no fake signals, no clamps, no skipped path, and no hand-edit to
any generated `.c` file survives -- every diagnostic added this entry
(missing-bridge call counter, `ObReferenceObjectByHandle` handle/resolve
logger) was temporary, read, and reverted; the only change carried forward
is the one genuine, precedented, verified-safe dispatch-wiring fix.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
