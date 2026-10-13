path = "docs/SCDA_WORKLOG.md"

with open(path, "rb") as f:
    raw = f.read()

text = """
## Update 214 (2026-09-21): Full-parameter capture of the underflowing memcpy done; empirically identified the exact file/offset as common.lin @ 0x1C0000 -- and this reconnects the whole 190-213 arc to an ALREADY-INVESTIGATED, partially-fixed bug from Updates 171-188 (2026-09-17/18) that this session had not cross-referenced

**Important correction of scope, before anything else:** this update's own
investigation revealed that Updates 171-188 (2026-09-17/18, four days
before the "0x8070 zeroing" framing in Update 190 began) already found,
byte-verified, and partially fixed the EXACT SAME underflowing memcpy this
session re-discovered independently in Update 213 -- same function
(`sub_00040821`), same lines, same `common.lin` short-read trigger,
confirmed via `xemu` ground truth. This session's Updates 190-213 were
unaware of that arc and re-derived a large part of it from scratch via a
different observational angle (starting from "the object is zeroed at the
fatal callback" rather than "why does the next NtReadFile get a garbage
handle"). This update reconciles the two.

**Task followed:** per explicit instruction, treat the memcpy identified in
Update 213 as the confirmed writer, but not yet the root cause; use the
existing instrumentation for one more fully-undisturbed run to capture
every input to the faulty memcpy in one shot (ring-buffer base, source,
destination, copy length, remaining space, record/chunk length, cursors,
reader/parent, thread, file, exact file offset); trace only the origin of
the values that enable the underflow; do not add a clamp yet.

**Instrumentation:** new `scda_capture_wraparound_once()` in
`recomp_manual.c`, called once from `loc_00040942` (right after `esi`/`edi`/
`eax` are fully computed, immediately before the `rep movsd`/`rep movsb`
pair), guarded to fire only when `remaining_space > 0x4000` (the actual
dangerous condition). Captures: `this`, the ring-buffer's own `databuf`/
`buffer_size` fields, `source`, `destination`, `copy_length`,
`remaining_space` (from `MEM32(ebp-12)`), `record_length` (`MEM32(ebp-16)`),
the other 4-byte header field (`MEM32(ebp-20)`), the read cursor
(`MEM32(this+0x4060)`), a running total (`MEM32(this+0x4064)`), the file
handle (`MEM32(this+0x3C)`), the thread id, plus a live `SetFilePointerEx`/
`GetFinalPathNameByHandleW` query on the handle, and a full linear scan of
guest RAM (`0x00010000`-`0x04000000`, one contiguous `VirtualAlloc`, safe to
scan directly) for any 4-byte value equal to `this`, to find whatever
"reader"/parent object references this subobject.

**Result -- captured once, on a fresh undisturbed run:**
```
this=0x01135928 databuf=0x01139998 buffer_size=0x00024000
source=0x0113D999 destination=0x0111D999 copy_length=0x0001FFFF
remaining_space=0x0001FFFF record_length=0x7E000000 header_field_a=0x00000000
read_cursor=0x00004001 total_field_4064=0x001D5F71 file_handle=0x48000005
reader_candidates: 2 found -- 0x00F7B140 (a guest-STACK slot holding a
  saved copy of `ebx`, a false positive -- return addresses 0x40983/0x40992
  sit right next to it, confirming it's a stack frame, not a persistent
  field) and 0x01105204, which is exactly `0x011051C8 + 0x3C` -- a genuine
  object whose own `+0x3C` field points at our subobject, matching the
  "Reader" pattern already described in Update 188.
```
`SetFilePointerEx`/`GetFinalPathNameByHandleW` on `file_handle=0x48000005`
**both failed** -- it is not a currently-valid Win32 HANDLE at this point
(consistent with the object's own header already being garbage/stale by
the time of a rare, late-stream event; not itself informative beyond
confirming the field is unreliable here).

**Locating the exact file/offset empirically instead of trusting in-memory
strings:** the reader object at `0x011051C8` has a filename-like field a
little further along reading `"DataXb\\Maps\\x06_cozumel_a.unr"` -- but this
turned out to be a red herring (likely an unrelated FName/string-pool
entry, or an original-PC-engine package name baked into engine metadata
rather than the literal file on this disc image; a `.unr` file does not
even exist in the extracted disc image, only `.lin`). Instead of trusting
that string, the captured raw bytes at the read cursor (`source`,
i.e. `0x24` zero bytes followed by `7E C4 67 1C F9 89 FC 4C C6 A3 BC 06 26
05 F0 0A ...`) were searched for verbatim across the entire extracted disc
image (`tools/debug/find_wrap_bytes2.py`). **Exact match found in
`Default/DataXb/LMaps/common.lin` at byte offset `0x1C0000` (1,835,008)** --
confirming the user's own hypothesis that `record_length` traces back to
`common.lin`, not to the cozumel map. `common.lin`'s total size is
1,924,977 bytes; `1,924,977 - 1,835,008 = 89,969` -- and `total_field_4064`
captured above (`0x1D5F71` = 1,924,977, exactly the file's full size) shows
the code's own running total already equals the complete file size at this
point, i.e. this capture is immediately after the file's FINAL chunk.

**This is not a new bug -- it is the exact, already-diagnosed
`common.lin` short-read bug from Updates 171-186, still not fully closed:**
- Update 171 (2026-09-17) found this exact memcpy, this exact function,
  and traced it to `common.lin`'s 15th chunk read live via cdb -- a short
  read of 89,969 bytes after 14 full 128 KiB (`0x20000`) chunks
  (`14 * 0x20000 = 0x1C0000`, exactly the offset found above independently
  today via byte search). Byte-verified via Capstone against the original
  XBE at `0x00040909`-`0x00040992`: **translation is exact, not a
  recompiler bug** (independently re-confirmed by this session in Update
  213).
- Update 173 found the root mechanism: the write-cursor-advance formula at
  `loc_00040992` (`wpos = 0x4000 - remaining_space_before_read`) **never
  reads the actual transferred byte count** -- it always assumes the full
  requested chunk (`0x20000`) was delivered, regardless of what
  `IoStatusBlock->Information` actually reports. On a genuine short read,
  ~41 KiB of never-actually-written buffer space gets marked "valid"
  anyway, and the next record-header parse reads garbage from that
  region.
- Update 185 did the exact xemu ground-truth comparison this round's
  instruction asked for, already: `tools/debug/xemu_object_refill_wantgot.py`
  confirmed real hardware receives this **exact same short read**
  (`want=0x00020000 got=0x00015F71`, byte-identical to recomp) at the same
  object/position -- but afterward, real hardware issues **no further
  `sub_000516B7` calls for this stream at all**; it stops cleanly. No
  crash on real hardware.
- Update 186 fixed the dishonest byte-count reporting (`scda_disc_fill_overrun`,
  which used to pad a short read up to the full requested size) with
  `scda_disc_zero_short_read_tail` (`src/kernel/kernel_file.c`, still
  present, unchanged, and independently re-read by this session earlier in
  Update 212's now-superseded native-code detour): report the true byte
  count, defensively zero only the unwritten tail. This is confirmed
  active in the current build and produces the clean `0x00000000...` zero
  run this session found at the start of the captured `source` bytes today.
- Even with that fix, Update 186 found the SAME fatal callback still fires,
  via what looked at the time like a corrupted-handle `NtReadFile` call
  one step later, and left it explicitly open ("noch nicht auf Zeilenebene
  isoliert"). **Today's Update 213/214 is that missing isolation**: the
  zero-fill fix stops the corruption from writing plausible-looking garbage
  (like the `0xE1F9F6F3` handle Update 171 originally saw) into the
  object's header, but does not stop the underlying mechanism -- the code
  still tries to parse a record header from the (now cleanly zeroed, but
  still logically bogus) unconsumed tail, the `record_length` it reads is
  still nonsense (now shaped by adjacent stale ring-buffer content instead
  of by `scda_disc_fill_overrun`'s old padding), and the same underflowing
  `memcpy` still fires, still zeroing the object's own header fields
  (`+0x3C` handle, `+0x4058` size, `+0x405C` buffer pointer) -- which is
  exactly the "0x8070 zeroing" this entire Update-190-213 arc has been
  chasing under a different name.

**Directly answering this round's specific questions:**
- **Does `record_length` come directly from `common.lin`?** Yes and no --
  it is read from the exact buffer position that maps to `common.lin`
  byte offset `0x1C0000` and just past it, but that position is PAST the
  file's real 89,969-byte final chunk. What is actually parsed there is a
  handful of legitimately-zeroed defensive-padding bytes (Update 186's own
  fix) immediately followed by one stale, leftover byte (`0x7E`) from
  whatever a PREVIOUS, unrelated read cycle left in that same
  reused ring-buffer slot -- not real `common.lin` content at all. The
  `record_length` field is therefore a mis-parse of padding-plus-stale-data,
  not a genuine value ever present in `common.lin` on disk.
- **Does `remaining_space` come from ring-buffer bookkeeping?** Yes,
  confirmed exactly: `remaining_space = buffer_total_size(0x24000) -
  cursor_after_consuming_the_8-byte_header(0x4001) = 0x1FFFF`, both fields
  read directly from the subobject's own persistent bookkeeping fields
  (`+0x4058`, `+0x4060`), matching the code byte-for-byte.
- **Byte comparison against another SCDA copy:** not needed as a separate
  step -- Update 173 already did the equivalent comparison against a real
  retail ISO (`SC4_xbe_hack/output/scda_graphics_fixes_30fps.iso`, common.lin
  located at ISO offset `0x24F22000`, full 1,924,977-byte match confirmed).
  The bytes on the real disc immediately after common.lin's declared end
  are themselves real, deterministic XISO directory-table content (not
  padding) -- but that fact turned out not to be why real hardware
  survives (see below); Update 185 later established real hardware also
  sees a clean short read at this exact point, not a continuation into
  directory bytes.
- **Targeted xemu comparison (record length, remaining space, cursor, file
  offset, destination address):** already done, in Update 185, for the
  short read itself (want/got byte-identical). Real hardware does NOT
  reach the compaction memcpy at all for this stream afterward -- so there
  is no xemu "record_length"/"destination address" to compare at that
  deeper point; the divergence happens one level up, in whatever decides
  whether to call `sub_00040821`/`sub_000516B7` again after this short
  read completes.
- **Does the pre-object region belong to a larger parent/arena
  allocation?** This branch of the instruction was conditioned on "falls
  xemu dieselben Eingangswerte und dieselbe Zieladresse zeigt" -- it does
  not, per Update 185 (xemu never reaches this memcpy for this stream at
  all), so this question does not apply and was not pursued further this
  round.

**What is still genuinely unexplained (the real remaining question):** why
does the recompiled build issue another `sub_00040821`/`sub_000516B7` call
for this stream after the short read, when real hardware does not? Both
the short-read byte counts (Update 185) and `sub_00040821`'s own translated
code (Updates 171/213, byte-verified) are identical to the original XBE.
The write-cursor-advance formula at `loc_00040992` runs unconditionally
regardless of `sub_000516B7`'s return status (success or failure both reach
it), so a plain "return STATUS_END_OF_FILE instead of STATUS_SUCCESS for a
short-but-nonzero read" fix in `xbox_NtReadFile` would not, by itself,
prevent the bad cursor advance -- the divergence must be in whatever
higher-level code decides to loop back for more data at all, which has not
been located yet.

**No patch implemented this round, per explicit instruction** -- the
underflow's mechanism was already understood (Update 213) and is now fully
traced to its real-world data origin (this update), but the actual
trigger -- why recomp keeps pulling on an exhausted stream that real
hardware abandons cleanly -- is not yet found, and patching the symptom
(the memcpy itself) before finding that would hide it exactly as
`scda_disc_fill_overrun` once did (Update 185's own explicit finding).

**Next test (for STRONG):**
1. Find what decides to call `sub_00040821`/`sub_000516B7` again for this
   stream after the short read -- likely one or two frames above
   `sub_00040821` in the call chain already mapped in Updates 173/195/196
   (`sub_0004438C` and callers). Compare against the exact same call chain
   on xemu around Update 185's t=27.828s hit to see where the two diverge.
2. Check whether `sub_00040821`'s own `+0x44`("consumed") style
   bookkeeping field, or a caller-side "expected total size" field, holds a
   different value on recomp vs. xemu at the moment right after the short
   read -- Update 185 already captured this class of field
   (`tools/debug/xemu_object_fileobj.py`) for a similar object; extending
   that same script to this exact point would give a direct xemu-vs-recomp
   diff without needing a new capture mechanism.
3. Only once that divergence is found: choose a fix at the layer where the
   actual bug lives (most likely the higher-level stream-continuation
   decision, not the memcpy itself, and not a defensive clamp inside
   `sub_00040821`, which would fix the symptom while leaving the real
   "why does recomp not stop" question open, as `scda_disc_fill_overrun`
   already once demonstrated is the wrong kind of fix for this family of
   bug).
4. After a real fix is chosen and verified: revert ALL temporary
   `SCDA-DISC-ERR-INSTRUMENTATION` markers and the ring-buffer/capture
   infrastructure per the standing instruction, run the full recomp
   pipeline + canonical build, and continue to the next blocker.

**Model tier:** unchanged, STRONG (Update 213) -- the remaining work is a
bounded comparison against already-existing xemu tooling and scripts from
Update 185, not open-ended reverse-engineering.
"""

with open(path, "wb") as f:
    f.write(raw + text.replace("\n", "\r\n").encode("utf-8"))

print("done, new size", len(raw) + len(text.replace("\n", "\r\n").encode("utf-8")))
