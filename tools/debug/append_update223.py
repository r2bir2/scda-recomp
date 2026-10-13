path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 223 (2026-09-21): CORRECTION to Update 222 -- this is NOT a missing video backend, it is a real memory-corruption bug; live breakpoints prove device creation succeeds and the existing NV2A_ACK machinery is already sufficient, but MEM32(0x4AB668) gets overwritten with garbage between two good uses and a bad one

**Trigger:** user asked to start building a "minimal video backend stub" based on
Update 222's framing ("no video backend wired in"). Before writing any stub
code, checked `projects/scda/CMakeLists.txt`'s own extensive prior-session
history (2026-09-04 through 2026-09-11) on this exact `sub_004A12B0` address,
which Update 222 had not cross-referenced. That history flatly contradicts
the "missing backend" framing and explicitly warns against the exact kind of
fix a stub would be.

### What the prior sessions already established (missed by Update 222)

1. `src/kernel/xbox_memory_layout.c` already implements a working, generic
   NV2A acknowledgement mechanism: `NV2A_ACK[]` (a table of
   `{register_offset, busy_mask}` pairs serviced by a background
   `nv2a_ack_thread`) already contains `{0x100410, 0x00010000u}` -- comment:
   "PFB flush kick, Halo 0x001EF930" -- i.e. **the exact register
   `sub_004A12B0` polls is already handled**, added for a different title
   that hit the same hardware behavior.
2. A dedicated xemu ground-truth script
   (`tools/debug/xemu_004a12b0_devptr_check.py`, still present, reusable)
   already answered the "is this missing hardware emulation?" question for
   this exact spin: on real hardware, `MEM32(0x4AB668) = 0x004AB670` (a valid
   device struct) and `struct+0x1C28 = 0xFD000000` (the real NV2A aperture
   base) **every single time**, ~500/500 hits. Real hardware's own PFB
   register is answered by real silicon; our `NV2A_ACK` table already
   reproduces that correctly once the pointer chain is valid.
3. That same investigation (docs, pre-"Update N" numbering, the "continued
   52" entry) found `scda_recomp` reaching the same spin with
   `MEM32(0x4AB668) = 0x0C47C708` -- not zero, not the valid device pointer,
   a stray value aliasing into `.text` -- and concluded explicitly:
   **"forcing the device-pointer chain to a fake value here would be exactly
   the kind of workaround this project rules out."** The recommended next
   step, never completed, was to trace what writes garbage into `0x4AB668`
   on this specific call path.

This means Update 222's "no video backend wired in" framing, while true of
the *original* 2026-09-11 finding (`MEM32(0x4AB668)` read as `0` for an
entire 10-minute soak, before the cursor-formula fix), does not describe the
*current* situation at all: substantial NV2A bring-up work already exists
and was already found sufficient for this exact register in this exact
function, by a session that got further than Update 222 did.

### Live re-verification this round

Built a real (non-`-pv`, breakpoint-based) `cdb` session launching a fresh
`scda_recomp.exe` directly under the debugger (`cdb -cf <script>
scda_recomp.exe`, not `-pv -p <pid>` -- genuine breakpoints require owning
the debug session), with breakpoints at `scda_recomp!sub_0049D210` (device
creation) and `scda_recomp!sub_004A12B0` (the poll loop), each printing
`MEM32(0x4AB668)` (via guest-VA-to-host translation using this build's own
`0x0000700000000000` guest memory offset) and a short stack, then
auto-continuing (`g`).

Result, three hits in order:

1. `sub_0049D210` entry, called via `sub_002E2BB1 -> sub_002E75CE ->
   sub_0004934E` (the exact chain earlier "continued 38" xemu work
   identified as the real hardware device-creation trigger) -- device
   creation runs.
2. `sub_004A12B0`, called via `sub_004A13D0 -> sub_004A2220 ->
   sub_0049D210` (immediately following device creation):
   `MEM32(0x4AB668) = 0x004AB670` -- **the correct, valid device pointer**,
   matching real hardware exactly and matching the literal constant
   `sub_0049D210` itself writes on success.
3. `sub_004A12B0` again, called via `sub_0049B0C0 -> sub_004A1740 ->
   sub_004A1600` (a different call path, further downstream):
   `MEM32(0x4AB668) = 0x7400087D` -- **the same garbage value Update 222**
   **captured**, now confirmed to appear *after* two confirmed-good reads,
   not from the start.

**Device creation is not the problem, and the existing NV2A acknowledgement
machinery is not the problem. Something overwrites a previously-valid
`0x4AB668` with garbage between hit #2 and hit #3.** This is a corruption
bug, structurally the same class as every other bug fixed this entire
session (the ring-buffer cursor formula, the disc-error trap, etc.) -- not
a new subsystem.

### First attempt to catch the write: inconclusive, not a dead end

Re-launched fresh under `cdb` with a hardware write breakpoint
(`ba w4 <host address of 0x4AB668>`) armed from process start. It correctly
fired once, on the same legitimate `sub_0049D210` write observed above
(value `0x004AB670`), proving the breakpoint and the address translation are
both correct. It did not fire a second time in two separate ~2-3 minute
observation windows, even though a follow-up non-invasive `cdb -pv` snapshot
on the same running process confirmed it had, by then, already reached the
corrupted spin (`sub_004A12B0+0x1e8`, stack showing `0x7400087D`). So the
corrupting change to that memory happened without tripping a 4-byte-aligned
write watchpoint on it -- consistent with this project's own prior
experience on this exact question (`continued 53`'s cdb hardware-watchpoint
attempts were also "defeated by tooling/environment issues rather than a
clean negative result"). Not yet resolved; candidate explanations to try
next: a wider/differently-aligned store that still overlaps those 4 bytes
but was set up before the breakpoint synced to a later-created thread, a
non-CPU-store write path (unlikely but not excluded), or simply needing a
longer observation window / multiple attempts given this project's
well-documented run-to-run non-determinism in thread scheduling.

### Explicit correction and recommendation

**Withdrawing Update 222's "needs a new video backend subsystem, scope with
user first" framing.** The right next step is the same kind of root-cause
hunt this whole session has already done successfully six times over: find
what writes into `0x4AB668` between the two confirmed calls above (most
likely another guest write through a miscomputed/stale pointer, given
`sub_0049D210`'s own three static writers were already ruled out by the
prior session's static grep). **Explicitly not recommended:** writing a
"video backend stub" that forces `0x4AB668` to a fake valid value -- this
was already tried in spirit and rejected by name in this exact codebase's
own history for this exact address, for good reason (it would hide the real
bug rather than fix it, and the existing `NV2A_ACK`/`nv2a_ack_thread`
machinery already proves no new hardware-emulation code is needed once the
pointer is valid).

### Process hygiene

Two temporary `cdb` command scripts used from the scratchpad directory only
(`cdb_devcreate_check.txt`, `cdb_write_watch_4ab668.txt`); no source files
touched this entry. Logs kept for reference:
`tools/debug/devcreate_check_out.log`,
`tools/debug/write_watch_4ab668_out.log`. `git diff --stat` confirms no
source changes. All `scda_recomp.exe`/`cdb.exe` instances killed at the end
of this entry.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
