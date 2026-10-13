path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 224 (2026-09-21): Minimally-invasive GOOD->BAD bisection along the actually-executed path narrows the 0x4AB668 corruption to a specific row-copy loop inside sub_004AED5D; the loop's own destination range does not overlap 0x4AB668, no other thread is active at the moment of corruption, and a precisely-gated hardware write watchpoint still does not fire -- pointing at a host memory-aliasing gap rather than an ordinary guest store

**Method, per explicit instruction this round:** hierarchical, one-shot
checkpoint bisection along the real call path between the last confirmed
GOOD device-pointer read (right after `sub_0049D210` returns) and the
first confirmed BAD read (inside `sub_004A12B0`), reading only
`MEM32(0x4AB668)` (plus a few registers) at each checkpoint via live `cdb`
breakpoints -- no source-code instrumentation, no hot-path output, fresh
process per launch. Narrowed hierarchically:

```
sub_0004934E (icall +0x3263f, GOOD) -> sub_002E75CE -> sub_002E2BB1 ->
sub_0049D210 [[device creation succeeds, writes 0x004AB670]]
    ... (sub_0004934E's own body, ~60 generated-C lines, one candidate
         store at recomp_0002.c:8023-ish ruled out directly) ...
sub_0004934E (icall +0x34b3e, still GOOD right before) -> sub_00037274 ->
sub_002E5F95 -> sub_0049B0C0 [[BAD already at entry]]
```

Re-bisecting inside `sub_002E5F95` (714 generated-C lines) pinned the
transition to a 150-line window, then to one call: `sub_004AE0A7`
(GOOD before, BAD after). Re-bisecting inside `sub_004AE0A7` pinned it to
one call: `sub_004ADE9D` (GOOD before, BAD after). Re-bisecting inside
`sub_004ADE9D` (its own ~260 lines, no direct non-local stores by static
grep) pinned it to one call: `sub_004B0F92` (GOOD before line 14747, BAD
by its own exit at line 14967). Re-bisecting `sub_004B0F92`'s nine
chained state-setter calls found only one (`sub_004AED5D`) is even
reached this run -- the other eight are skipped via an early-success
branch. **Final localization: the corruption happens somewhere inside
`sub_004AED5D` (`recomp_0027.c:7881-8080`), specifically after its
per-row copy loop begins (`recomp_0027.c:8016-8041`, live-confirmed still
GOOD at line 8023) and before its own exit (BAD by line 14967's
outer-caller checkpoint).**

### A genuine, independent defect found along the way: the row-copy loop never advances

Sweeping every iteration of the loop at `recomp_0027.c:8023` (480
iterations captured in one run) shows `esi`/`edi` **frozen at the exact
same values (`0x00000394`/`0x00000398`) on every single iteration** --
the per-row pitch added at the end of each iteration
(`recomp_0027.c:8034-8035`, `MEM32(ecx+0x1048)`/`MEM32(ebx+0x1048)`) must
be reading as zero. This is a real, reproducible defect (a "copy every
row to the same place forever" bug) independent of the 0x4AB668 question,
not yet root-caused or fixed -- flagged for follow-up, not chased further
this entry per the user's bisection-first instruction.

### The addresses this loop copies to/from do not overlap 0x4AB668 at all

`esi=0x394, edi=0x398, ecx(dwords)=0x3a4` (destination range
`[0x398, 0x1228]`) is nowhere near `0x4AB668`. Live-confirmed the device
pointer flips from GOOD to BAD exactly between loop iteration 115 and 116
in one run (`dd 0x7000004ab668` checked every iteration) -- but the
copy's own addresses never change across any of those iterations. **This
specific memcpy cannot be the direct writer of 0x4AB668 by its own stated
source/destination range.**

### Cross-thread race ruled out directly

At the exact moment of the GOOD->BAD transition, dumped all 5 live
threads (`~*kb 3`): thread 0 (ours) sitting at the checkpoint inside
`sub_004AED5D`/`sub_004B0F92`/`sub_004ADE9D`; threads 1-3 are idle
thread-pool workers in `NtWaitForWorkViaWorkerFactory`; thread 4 is this
project's own `nv2a_ack_thread`, sitting in `SleepEx` (mid-sleep, not
touching guest memory). **No other thread is active or could be writing
guest memory at this instant** -- the corruption is single-threaded on
thread 0, ruling out the race-condition hypothesis this update initially
suspected.

### Plan B (hardware write watchpoint), precisely gated this time: still does not fire

Per instruction ("Hardware-Watchpoint nur als Plan B"), armed
`ba w4 <host address of 0x4AB668>` but held it *disabled* until the loop
counter at `recomp_0027.c:8023` reached 100 (comfortably before the
115-116 transition observed above), then enabled it
(`bd`/`be` on the breakpoint id, avoiding the nested-quote nesting a
dynamic `ba` inside another breakpoint's action would need) and let the
process run free from there. Confirmed via a `cdb -pv` snapshot afterward
that the process had already reached the terminal `sub_004A12B0` spin
(i.e., the corruption already happened) -- **the watchpoint never fired**,
even armed for only the ~15-iteration window in which the transition is
known to occur. This is a much tighter, more conclusive negative result
than the wide, noisy attempt in Update 223 (there, the watchpoint had to
survive minutes of unrelated execution; here it only had to survive a
few dozen loop iterations of already-isolated code, and still missed it).

### Requested check: does 0x7400087D appear elsewhere in guest memory?

Searched the entire 64MB guest RAM (`s -d` over
`0x7000000000000` L?`0x4000000`) for the dword pattern `0x7400087D` on
the already-corrupted, still-running process: **zero matches anywhere
else** (a small-range sanity search around `0x4AB668` itself confirmed
the search command works correctly). Inconclusive on its own -- the
source data, if any, could have been overwritten by further execution in
the time since the corrupting write -- but does not support a simple
"stray copy from a live, still-intact buffer" explanation either.

### Working hypothesis, not yet confirmed: host-side memory aliasing

The combination of (a) the identified copy's own addresses never
overlapping `0x4AB668`, (b) no other thread active, and (c) a precisely
gated hardware watchpoint still not firing, most plausibly points at
something already flagged as an open architectural gap much earlier in
this project's history (docs, "continued 53", 2026-09-06): guest memory
may be reachable through more than one host virtual address (e.g. a
separate mapping/view used by a bulk-copy fast path), so a watchpoint
armed on the "canonical" `g_xbox_mem_offset`-translated address would
never see a write that goes through a different host VA backing the same
guest page. This was never confirmed for THIS corruption specifically --
it is the most consistent explanation of the evidence gathered this
entry, not a proven root cause.

### Explicitly not done, per instruction

No clamp, no fake/stub device pointer, no other workaround. No source
files modified this entry -- `git status`/`git diff` confirm doc-only
(plus new debug scripts in `tools/debug/`). All `scda_recomp.exe`/`cdb.exe`
instances terminated at the end.

### Next step recommendation

Root-causing this fully now needs a different kind of investigation than
the bisection done so far: auditing this project's guest-memory
allocation/mapping code (`src/kernel/xbox_memory_layout.c`,
`kernel_bridge.c`'s `NtAllocateVirtualMemory` path already flagged once
before) for any case where the address range containing `0x4AB668` (or
the loop's own destination range) is reachable through more than one host
VA. If no such aliasing exists, the alternative is a page-guard-based
(not debug-register-based) memory trap, which catches any access
regardless of host-address aliasing at the cost of being far slower and
more invasive -- a materially different technique from anything tried so
far this session.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
