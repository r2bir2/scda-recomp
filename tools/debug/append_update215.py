path = "docs/SCDA_WORKLOG.md"

with open(path, "rb") as f:
    raw = f.read()

text = """
## Update 215 (2026-09-21): sub_000516B7 fully re-read end to end; the buggy cursor-advance formula is pinned to one exact line, proven structurally incapable of using the true byte count even with Update 186's honest-report kernel fix already active -- this is why the fix from four days earlier never fully closed the bug

**Goal this round:** finish tracing which of `sub_00040821`'s two
`sub_000516B7` call sites is the actual short read of `common.lin`'s last
chunk (Update 214 identified the file/offset but not which call), and
explain precisely why Update 186's already-active, already-verified
kernel-side fix (honest byte-count reporting) does not fully prevent the
corruption Update 213/214 traced.

**Full re-read of `sub_00040821`'s relevant span (`recomp_0001.c:68565-68790`,
guest `0x40821`-`0x40992`) in one pass, replacing several partial/wrong
assumptions from earlier updates this session:**
- `loc_0004087B` computes `remaining_to_buffer_end = buffer_size -
  cursor`. If `remaining_to_buffer_end >= 8` (room for a full record
  header), control goes straight to `loc_00040909` (no call yet). If it is
  `< 8`, control falls into `loc_0004089A`, which does a SEPARATE, smaller
  compaction (relocating the `<8` leftover bytes), then issues **call site
  1** (`sub_000516B7`, return address `0x000408E1`) to refill.
- **Call site 1 (`0x408E1`) is the one that actually performs `common.lin`'s
  final, short (89,969-byte) read** -- confirmed by the arithmetic: the
  buggy formula below produces exactly the `read_cursor=0x4001` this
  session captured in Updates 213/214 only if the pre-read
  `remaining_to_buffer_end` was `7`, and `0x4000 - 7 = 0x3FF9`, matching
  `0x4001 - 8` (the two 4-byte header fields consumed at `loc_00040909`
  land exactly on the captured cursor).
- At `loc_000408E1` (call site 1's return), `sub_00040821` does
  `test eax,eax; jne loc_000408F0` -- skipping the "chunk consumed"
  callback check/dispatch entirely whenever `eax != 0`. A full read of
  `sub_000516B7`'s own body (`recomp_0002.c:45924-46153`, all 100
  instructions, not just the fragment read in Updates 211/212) shows it has
  **two distinct return paths with different `eax` values**: an immediate,
  fully-synchronous completion returns `eax=1` via `loc_00051720`; a
  path that goes through the generic kernel-bridge wrapper
  `sub_00052A85` (used for anything that was `STATUS_PENDING` or is being
  relayed via `GetLastError`) returns `eax=0` via `loc_0005179B`. Since our
  host `ReadFile` is synchronous, a short-but-nonzero read completes
  immediately and returns `eax=1` -- which `sub_00040821` treats as
  "skip the callback", not as an error. This explains why no fatal
  dispatch happens right at call site 1: it is not reached, by design, for
  a plain synchronous completion (the callback exists to signal
  asynchronous completions specifically), not because anything is wrong
  with this particular check.
- **The actual bug, confirmed at the exact line, is unconditional and
  independent of the `eax` branch above:** both the `eax!=0` (skip) and
  `eax==0` (callback-checked) paths converge at `loc_000408F0`, which
  unconditionally executes:
  ```
  eax = MEM32(ebp-4);              // the TRUE number of bytes sub_000516B7 actually
                                    // wrote to the caller's out-param -- available right here
  MEM32(ebx+0x4064) += eax;        // correctly uses it for this running total
  eax = 0x4000 - MEM32(ebp-12);    // MEM32(ebp-12) is remaining_to_buffer_end computed
                                    // BEFORE the read, at loc_0004087B -- stale, and never
                                    // updated with the real transferred count
  MEM32(ebx+0x4060) = eax;         // ... but THIS -- the actual read/write cursor --
                                    // uses the stale pre-read value instead
  ```
  The real byte count is sitting in a register one line above and is
  simply never used for the cursor. This is line-for-line the same defect
  Update 173 already diagnosed on 2026-09-17 ("Diese Formel beruecksichtigt
  an KEINER Stelle die tatsaechlich gelesene Bytezahl"), now confirmed at
  the exact current source line with a concrete, real, captured instance.

**Why Update 186's already-active fix does not close this:** `scda_disc_zero_short_read_tail`
(`src/kernel/kernel_file.c`, unchanged, still active) makes `xbox_NtReadFile`
report the HONEST transferred byte count via `IoStatusBlock->Information`
-- and that honest count does reach the guest as `MEM32(ebp-4)` at
`loc_000408F0`. But the cursor-advance formula structurally never reads
`MEM32(ebp-4)` for the cursor -- only for the unrelated `+0x4064`
accumulator. No amount of honesty in the kernel's reported byte count can
fix a formula that does not consult it. This is a genuine defect in the
original, shipped game code (confirmed byte-identical to the retail XBE,
Updates 171/213) -- not something fixable by adjusting kernel-bridge
NTSTATUS/byte-count semantics, and not a recompiler bug.

**The one remaining open question, precisely scoped:** Update 185 already
proved real hardware receives the *identical* short read
(`want=0x20000 got=0x15F71`) at this exact point, via `xemu`. If real
hardware runs the same buggy formula (byte-identical code) and does not
crash, the only way to reconcile that is if the INPUT to the formula
(`MEM32(ebp-12)`, i.e. `remaining_to_buffer_end` computed one call earlier,
before call site 1 even fires) differs between real hardware and recomp at
the moment call site 1 is issued -- e.g. if real hardware's cursor happens
to sit exactly at a multiple of the chunk size when it reaches this stream
final read (`remaining_to_buffer_end == 0`), the buggy formula degenerates
to `cursor = 0x4000 - 0 = 0x4000` exactly, landing precisely on the
boundary where fresh (real, non-garbage) data begins -- avoiding the
mis-parsed record header entirely, by accident rather than by a real fix.
This has NOT been confirmed live and is the next concrete thing to check.

**No patch implemented, per explicit standing instruction for this round.**
The mechanism is now understood at the exact line level and reconciled
with the four-days-earlier Update 171-186 investigation; what remains is
confirming, via `xemu`, whether `remaining_to_buffer_end` differs from
recomp's `7` at the moment call site 1 fires for `common.lin`'s final
chunk, which would explain the real/recomp divergence without requiring
any code to be wrong beyond the (real, but hardware-invisible) cursor
formula defect.

**Next test (for STRONG):**
1. Extend `tools/debug/xemu_object_refill_wantgot.py` (Update 185, still
   present and directly reusable) to additionally read `MEM32(ebx+0x4058)`
   and `MEM32(ebx+0x4060)` (or the equivalent `ecx`/`eax` register values
   right before the call) at the SAME breakpoint, for the specific hit that
   produces the `want=0x20000 got=0x15F71` short read, to get real
   hardware's `remaining_to_buffer_end` at that exact moment.
2. Compare against recomp's own value (`7`, already known from this
   session's capture). If they differ, the divergence is fully explained
   and the fix decision becomes: correct the cursor formula to use the real
   byte count (a from-scratch bugfix matching original intent, safe
   because it only changes behavior on the already-broken short-read path)
   vs. investigating why recomp's own upstream bookkeeping reaches a
   different `remaining_to_buffer_end` than real hardware in the first
   place (a genuine, different, still-unknown recomp-side state
   divergence).
3. Only after that: choose and implement the fix, verify, then revert all
   `SCDA-DISC-ERR-INSTRUMENTATION` markers, the `scda_capture_wraparound_once`
   infrastructure, and the enlarged ring buffer per the standing
   instruction, run the full recomp pipeline, canonical build, and
   continue to the next blocker.

**Model tier:** unchanged, STRONG -- remaining work is one bounded,
scripted xemu field comparison reusing existing Update 185 tooling.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
