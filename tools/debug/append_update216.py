path = "docs/SCDA_WORKLOG.md"

with open(path, "rb") as f:
    raw = f.read()

text = """
## Update 216 (2026-09-21): Live xemu ground truth obtained for this exact case; the two sub_00040821 call sites' roles fully disentangled; the true root cause turns out to be a THIRD, still-unresolved question already flagged four days earlier in Update 177 -- no fix implemented, by design

**Task followed:** obtain the specific xemu comparison this round's
instructions asked for (record length, remaining space, cursor, file
offset, resulting memcpy destination), using it to decide whether the
"parent/arena allocation" branch of the instructions applies, and only
then choose a fix strategy -- explicitly not yet implementing one.

**Live xemu run performed this round (not reused from Update 185's old
log -- a fresh boot, today):** launched `xemu.exe -s -S` and ran an
extended version of Update 185's own `xemu_object_refill_wantgot.py`
(`tools/debug/xemu_object_refill_wantgot_remaining.py`, new, adds exactly
one more read: `ebx`'s own `+0x4058`/`+0x4060` fields at the `sub_000516B7`
entry breakpoint, giving `remaining_to_buffer_end` for that specific call).
**Result: hit #16 at t=28.297s reproduced Update 185's exact short read
byte-for-byte** (`this=0x00879ED0`, `want=0x00020000 got=0x00015F71`,
return address `0x00040983` -- i.e. call site 2), with
`remaining_to_buffer_end=0x00001E22` at that exact moment. This confirms,
independently and freshly, that real hardware really does reach this exact
short read with these exact numbers, and answers this round's conditional
question directly: since xemu's own destination/remaining values at this
point are recoverable and clearly diverge from recomp's (see below), the
"does the pre-object memory belong to a larger parent/arena allocation"
branch does not apply -- the two platforms are not reaching the same
dangerous state at all, so there is nothing to reinterpret about the
memory layout.

**Correcting this session's own Update 215: which call site does what.**
A full re-read of `sub_00040821`'s complete body in one pass (not
fragments) plus a matching recomp-side capture (new callsite 18,
`recomp_0001.c`, records `buffer_size`/`cursor`/`remaining_to_buffer_end`
immediately before call site 2, mirroring exactly what the xemu script
measures) shows the actual sequence for the fatal cycle, in order:
1. Call site 1 (`0x408E1`, the "<8 bytes before buffer's physical end"
   refill) fires and returns with the ring-buffer subobject **still fully
   valid** (confirmed by callsites 11/13 in the ring, unchanged from
   Updates 213/214).
2. The next record-header parse (`loc_00040909`) reads the two 4-byte
   fields at the new cursor -- **still using valid data at this point**
   (callsite 14, valid).
3. This parse determines there is NOT enough remaining room for the
   declared record length and falls into the compaction block
   (`loc_00040942`, callsite 16, **still valid immediately before the
   `rep movsd`/`rep movsb` pair**).
4. **The compaction's own memcpy is what corrupts the object** -- callsite
   17, recorded immediately after both copies complete, already shows all
   header fields zeroed. This reconfirms Update 213's original finding
   exactly, now anchored to a fully-understood surrounding sequence.
5. Only afterward does call site 2 fire (`0x40983`) -- with the
   already-zeroed object (callsite 18 immediately before it already reads
   all-zero; callsites 11/12/9/10 for this specific invocation are all
   zero too). This call is a *consequence* of the corruption, not an
   independent second short read -- there is no reason to believe this
   specific instance of call site 2 is itself reading real disc data with
   a meaningful want/got at all, since its own `handle` field is already
   destroyed.

This means the actual `common.lin`-exhausting short read on **recomp** most
likely happens via **call site 1**, while on **xemu** the byte-identical
short read happens via **call site 2** -- not necessarily a deep
inconsistency, just a difference in which exact loop iteration each
platform's own (otherwise identical) bookkeeping happens to land the final
partial chunk on. Both call sites share the exact same structural defect
(Update 215), so this does not change which formula is buggy, only which
concrete call instance triggers it in a given run.

**The real root cause is a third, deeper question -- already flagged,
still unresolved, four days before this whole investigation began:**
re-reading further back turned up Updates 176-177 (2026-09-17, the same
day as Updates 171-175, immediately before them chronologically), which
already fully decoded the record format: an 8-byte header (4-byte
type/tag, 4-byte payload size) followed by that many payload bytes, with
the tag field passed through `sub_000503EE` toward what was assumed to be
a deserializer. Update 177 traced this one level further and found
`sub_000503EE` actually calls `sub_0004E275`, a **generic memory-growth
routine** (allocator-callback pattern, `std::vector`-grow-like) that is
**completely independent of the tag's actual value** -- not a
terminator check at all. Update 177's own conclusion, never followed up in
either investigation arc since: *"Die tatsaechliche Terminator-/Zaehl-Logik
liegt folglich hoeher in der Kette, vermutlich in ... `sub_0007DE25`,
`sub_0007F412` oder `sub_00064B00`"* -- i.e., something further up the call
chain is supposed to recognize a terminator/end-of-stream tag and stop
requesting more chunks, and nobody has yet confirmed whether it does, or
why it does not prevent the observed behavior. Later updates (195/205) did
fully read `sub_0007DC43`/`sub_0007DE25`/`sub_001CA0A5` and part of
`sub_00064B00`, but **only while looking for a zero-fill writer** (the
"0x8070" framing) -- none of those reads checked for tag/terminator
handling, so Update 177's specific question remains exactly as open today
as it was on 2026-09-17.

**Why no fix is implemented this round, deliberately:** three internally
consistent explanations for "why does recomp keep pulling on an exhausted
stream while real hardware does not" remain live, and they call for three
different fixes:
1. The higher-level chain *does* correctly recognize a terminator tag on
   real hardware, and something upstream of `sub_00040821` (state, not
   code, since the code is byte-identical) differs enough on recomp that
   the terminator is never presented to it (e.g. real hardware's short
   read happens to land such that its own last real record legitimately
   IS the terminator, whereas recomp's happens to land elsewhere) -- fixed
   by finding and correcting that state difference upstream, not touching
   `sub_00040821`/`sub_000516B7` at all.
2. The higher-level chain does *not* actually implement terminator
   detection at all (Update 177 found no evidence of it in the four
   functions checked, though the three largest candidates remain
   unconfirmed) -- and on real hardware, `sub_00040821` genuinely never
   gets asked to fetch a chunk it cannot deliver, because the game's own
   size-tracking (matched to the real file/ISO's actual byte count) simply
   never requests one -- fixed by making sure the emulated file's exposed
   size and the game's own size expectations agree in the exact same way
   real hardware's do (a data/environment fix, not a code fix).
3. The buggy formula in `sub_00040821` (Update 215) is not actually latent
   on real hardware for an unrelated, still-unknown reason -- fixed by
   correcting the cursor/remaining-space bookkeeping itself.
Implementing any one of these without first ruling out the other two risks
either fixing a symptom while leaving the real defect in place (repeating
`scda_disc_fill_overrun`'s own mistake, explicitly called out in Update
185) or introducing a change to widely-shared, hot-path code
(`sub_00040821`/`sub_000516B7` are used by dozens of unrelated streams
across the whole game, per the call-site grep in Update 212) that could
regress something currently working, with no automated test coverage for
this module to catch it.

**Next test (for STRONG, supersedes this round's item 1-2):**
1. Fully read `sub_0007DE25`, `sub_0007F412`, and `sub_00064B00`
   end-to-end specifically for terminator/tag-checking logic (not for a
   zero-fill writer, which is what excluded them in Updates 195/205) --
   this is Update 177's own four-day-old, never-completed next step.
2. If a terminator check is found: capture, on both recomp and xemu, the
   exact tag value present at the position where each platform's own
   final short read lands, to see whether recomp's happens to differ from
   the terminator sentinel while xemu's does not.
3. If no terminator check is found anywhere in that chain: shift to
   explanation 2 above -- compare the game's own internal "expected total
   size for this file" value (wherever it is initialized, likely from a
   `.ini`/index entry parsed earlier in level load) against `common.lin`'s
   actual extracted size on both platforms.
4. Only once one of these three explanations is confirmed: implement the
   corresponding fix, verify with a full run, then revert every
   `SCDA-DISC-ERR-INSTRUMENTATION` marker and the `scda_capture_wraparound_once`
   /enlarged-ring-buffer infrastructure added this session (Updates
   210-216), run the standard incremental recomp build (a full pipeline
   regenerate is not needed -- these are hand-edits to already-generated
   files, reverting them returns to exactly what a regenerate would
   already produce), and continue to the next blocker.

**This round's action:** per the above, reverting the temporary
instrumentation now (its purpose for the memcpy/cursor mechanism is fully
served; the next phase needs different functions instrumented, not more of
the same) rather than leaving 18 call sites of diagnostic code in place
indefinitely. See the housekeeping note in the next commit.

**Model tier:** STRONG remains appropriate -- the remaining work is a
bounded, specific function-reading task (three named functions) with a
clear success criterion (find or rule out terminator-checking logic),
not open-ended search.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
