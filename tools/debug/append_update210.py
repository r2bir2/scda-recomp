path = "docs/SCDA_WORKLOG.md"

with open(path, "rb") as f:
    raw = f.read()

text = """
## Update 210 (2026-09-21): One-shot source instrumentation built per explicit user instruction -- proves sub_0003FCA5 is NOT reached via any of the three call sites found in Update 208, but via the indirect callback at MEM32(0x5E6E0C); this reconfirms (does not newly discover) the mechanism already established in Updates 109/117/173-182/186/187, closing the loop back to the still-open Update 190 question

**Instruction followed:** do not solve the blocker via further CDB polling; instead
build a minimally-invasive one-shot instrumentation at the three known
`sub_0003FCA5` call sites, store callsite id/reader pointer/status/GetLastError/
thread id purely in host memory (no hot-path output), and dump the snapshot
exactly once, at actual entry into `sub_0003FCA5`. Implemented exactly this
(ring buffer + record()/dump_once() in `recomp_manual.c`, declarations in
`recomp_types.h`, four call-site injections in `recomp_0001.c` -- see the
`SCDA-DISC-ERR-INSTRUMENTATION` tags). Rebuilt after obtaining explicit user
approval to close the long-protected `cdb.exe`/`scda_recomp.exe` pair that was
holding the exe file locked (confirmed fully crashed, no remaining diagnostic
value).

**Result 1 -- a fully undisturbed run does not reproduce within 60x the
previously-modeled window:** launched the freshly-built, instrumented exe with
zero debugger involvement at any point. After **36 minutes** of wall-clock
time (vs. the ~30-36s window Update 206 had bracketed), the diagnostic log was
still completely empty and the process was still alive, pegging one CPU core
(`SetProcessAffinityMask(..., 1)`, already permanent code from an earlier,
unrelated investigation -- unchanged since before Update 190, so it does not
explain the discrepancy on its own). A second fresh instance, checked at
t=50s with still nothing recorded, then given exactly one non-invasive
`cdb -pv -p <PID> -c "q"` attach-and-immediately-quit (matching Update 206's
own established minimal method, not "polling"), was found frozen in a tight
`jmp $` self-loop by the time of that single attach.

**Result 2 -- the freeze point is genuinely inside `sub_0003FCA5` itself, not
a symbol-resolution artifact:** cross-checked the live host address against
the linker's own sorted `Publics by Value` map-file section (independent of
cdb's PDB, which warned about a checksum mismatch): the frozen instruction
lies at RVA `0x335770`, strictly between `sub_0003FCA5` (`0x333fb0`) and the
next symbol `sub_0003FCDD` (`0x335d70`) -- confirmed genuinely inside
`sub_0003FCA5`'s own compiled body, offset `+0x17c0`. Disassembly around it
shows `mov ecx, 0x49CC50; mov edx, r14d; call recomp_abi_violation_log; jmp $`.
`RECOMP_ABI_CHECK` is a pre-existing, currently-enabled diagnostic for this
target (`projects/scda/CMakeLists.txt`, from an older, unrelated ABI-clobber
investigation, docs line ~8745) that logs (once per distinct VA, to `stderr`)
whenever a `RECOMP_ABI_CALL`-wrapped call returns with `g_ebx`/`g_esi`/`g_edi`
changed or `g_esp` too low. This means: while executing its own body,
`sub_0003FCA5` makes an inner call to guest VA `0x49CC50` whose translated
epilogue does not fully restore the expected registers/stack -- a real,
previously unflagged ABI violation, not caused by our instrumentation (our
injected call sits before this point and does not touch `g_ebx/g_esi/g_edi/
g_esp`). The self-jmp loop itself is not new: it is the same
"`sub_0003FCA5`-`jmp $`-Schleife" already described in Updates 179-182 as
this function's terminal (never-returns) state in this environment.

**Result 3 -- direct proof that none of the three Update-208 call sites fired
this run:** reading the frozen process's own globals (a single static memory
read, safe because the target is now provably frozen/unchanging -- confirmed
unchanged across two reads a minute apart) showed `g_scda_disc_err_dumped=1`
(so `sub_0003FCA5` really was entered and its first statement, our dump call,
really ran) but `g_scda_disc_err_ring_count=0` (so none of the three
instrumented call sites -- `sub_000356F1`, `sub_00038BD5`, `sub_00038F96` --
ever recorded a hit). Since our record call is unconditionally placed right
before each site's existing call to `sub_0003FCA5`, this is conclusive: this
run's entry into `sub_0003FCA5` came from neither the direct-call
`sub_00038A43` status-4 path (Updates 207-209) nor `sub_000356F1`.

**Result 4 -- extended the same technique to the indirect-call dispatch macros
and got an exact hit:** `sub_0003FCA5` also has a `recomp_dispatch.c` table
entry, meaning it can be reached through `RECOMP_ICALL`/`RECOMP_ICALL_SAFE`/
`RECOMP_ITAIL` (looked up by guest VA at runtime) -- a path Update 208's
static call-site enumeration could not have found by grepping for literal
`RECOMP_ABI_CALL(0x0003FCA5u, ...)` text. Added one guarded record call to
each of the three macros in the generated `recomp_types.h`
(`if (_va == 0x0003FCA5u) scda_disc_error_record(...)`), rebuilt, reran. A
fresh instance froze at the same `sub_0003FCA5+0x17c0` spot, and this time the
ring buffer held exactly one entry: `callsite_id=5` (`RECOMP_ICALL_SAFE`),
`a=g_esp` at the call, `b=MEM32(g_esp)` (the guest return address the macro
itself pushes) `=0x00040992`.

**Result 5 -- traced this to a call site that is itself long-documented, not
new:** guest `0x40992` is the return address of `RECOMP_ICALL_SAFE(eax, ...)`
at guest `0x40990`, inside `sub_00040821` (`recomp_0001.c:68556`, category
`game_vtable`) -- this is the ring-buffer refill function already fully read
in Updates 179-181. Reading the source directly (no cdb needed) shows: after
`sub_000516B7` returns 0 (success) AND `MEM32(0x5E6E0C) != 0`, the code calls
through `MEM32(0x5E6E0C)` unconditionally -- **this is the exact global
"fatal callback" pointer already found and hardware-watchpointed in Update
181**, which showed exactly one write to it, ever, in the entire session:
`sub_00048B15+0x185` (`recomp_0002.c:15018`, `MEM32(0x5E6E0C) = 0x3FCA5;`),
unconditionally, right at that function's own entry, before any I/O.
`sub_00048B15` itself was already traced back (Updates 109/117) to being
called exactly once per boot, with three zero arguments, from
`sub_00054722 <- sub_00054701/sub_00054720 <- sub_000546FB` (the last of
which is an indirect tail-jump through yet another global slot,
`MEM32(0x4F5208)`) -- all consistent with what this session independently
re-derived from the current build via source reading, with no new caller
found beyond what Updates 109/117/173-182 already documented.

**What this actually resolves vs. what it does not:** this closes the loop
back to Update 190, not past it. The three-call-site framing from Update 208
(direct `sub_00038A43` status-4 -> `sub_0003FCA5`) was a plausible but, for
this build/run, NOT the path actually taken -- the live mechanism is the much
older, already-understood one-time-armed global callback. The actual open
question is unchanged since Update 190: `sub_00040821` dispatches through
`MEM32(0x5E6E0C)` only after its own preceding `sub_000516B7` call
succeeded, on a ring-buffer subobject that Updates 187/190 already showed is
completely zeroed/never-initialized by the time the fatal path is reached,
through no known constructor/destructor/allocator call. This session did not
newly reproduce or advance that specific zeroing question; it re-confirmed
(via a cdb-independent, source-level method, per this round's explicit
instruction) that the downstream mechanism described in Updates 109-190 is
still exactly what fires in the current build, and additionally flagged one
possibly-new, currently-unexplained data point: a genuine `RECOMP_ABI_CHECK`
violation on an inner call from within `sub_0003FCA5` itself (guest VA
`0x49CC50`), not yet cross-checked against the older sessions' notes.

**Model tier:** unchanged, MAXIMUM (Update 209) -- the underlying open
question (what leaves the ring-buffer subobject zeroed/uninitialized before
`sub_00040821` uses it) is exactly as hard as it was at the end of Update 190,
now reconfirmed current rather than resolved.

MODEL STATUS: ESCALATION RECOMMENDED
Tier: MAXIMUM
Reason: the actual root-cause question (Update 190) remains open despite an
independently re-derived, fully consistent multi-hop mechanism spanning two
widely separated investigation arcs (Updates 109-190 and 205-209); closing it
needs the same kind of non-obvious, multi-file, multi-thread reasoning that
already consumed two long investigation arcs without resolution.

**Next test (for MAXIMUM), superseding Update 209's item 1-2 (now answered):**
1. Apply the SAME reliable technique proven this round (record into a host
   ring buffer at the point of interest; read it once via a single static
   memory read after the target is confirmed frozen -- no live cdb polling,
   no timing dependency) to `sub_00040821`'s own entry: record `ecx` ("this")
   and the ring-buffer subobject's key fields (`+0x3C`, `+0x4058`, `+0x405C`)
   on EVERY call, keeping only the last few in the ring, so the exact call
   at which the subobject first reads back as zeroed can be identified
   without a fragile live watchpoint or attach timing.
2. Once the exact call is isolated, apply the same non-invasive method that
   answered this round's question (map-file cross-check, source reading, no
   trust in cdb' PDB/symbol labels) to whatever precedes that specific call,
   rather than resuming the historical Update-188/189 hardware-watchpoint
   approach that repeatedly proved unreliable in this sandbox.
3. Separately, and lower priority: check whether the `RECOMP_ABI_CHECK`
   violation at guest VA `0x49CC50` (fired from inside `sub_0003FCA5`,
   Result 2 above) is already a known, accepted finding from the older
   ABI-clobber investigation (docs line ~8745) or a genuinely new lead.

**Instrumentation status (per explicit standing instruction --
"Instrumentierung ist diagnostisch und darf nicht als Fix bestehen
bleiben"):** all of it (the four `recomp_0001.c` call-site tags, the three
`recomp_types.h` macro checks, and the `recomp_manual.c` ring
buffer/record/dump-once block) remains in place for now because the
backward trace this instruction requires is still active and this exact
mechanism is the fastest way to continue it (see Next Test #1 above, which
reuses it). It is temporary and will be reverted once the actual zeroing
writer (the real, still-open root cause) is found or this specific
investigation is closed out, not left as a permanent fix.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
