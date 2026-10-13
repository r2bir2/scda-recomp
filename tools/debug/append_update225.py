path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 225 (2026-09-21): CONCRETE WRITER FOUND -- the 0x4AB668 corruption is a tiled-aperture surface/row copy inside sub_004AED5D whose destination range overlaps the D3D device pointer's physical location; a cdb `.frame N` methodology pitfall (retracted) and one round of real, minimal, one-shot source instrumentation confirm it precisely

**Two false leads investigated and explicitly retracted this entry, in the
interest of an honest record:**

1. **Host memory-aliasing via RAM mirrors was checked and does not directly
   apply to this loop.** `XBOX_PTR(addr) = (uintptr_t)(uint32_t)(addr) +
   g_xbox_mem_offset` (`templates/runtime/recomp_types.h:460`) does no
   wraparound/masking, so a small guest address like the `0x394` observed
   earlier in a *different* (unrelated, harmless) call to this same shared
   copy routine cannot itself reach a 64MB RAM mirror. The RAM-mirror
   mechanism in `xbox_memory_layout.c` (documented there in detail) is real
   but was not the mechanism at play here.
2. **A suspected stack-pointer corruption (`ebp` reading as `0x3C0` in three
   different `cdb` stack frames via `.frame N; ? ebp`) was investigated and
   is a debugging-methodology artifact, not a real finding.** Re-checking
   with the frame-aware `dv` command (rather than `? name`, which can
   silently fall back to a pseudo-register/wrong scope when `.frame` context
   switching doesn't fully apply to a same-named local) shows completely
   normal, distinct, correctly-nested stack addresses per frame
   (`ebp=0xf7b0cc` in `sub_004AED5D`, `ebp=0xf7b188` in `sub_004ADE9D`, two
   frames up) -- the stack is healthy. Lesson for future `cdb` work in this
   project: never trust `? name` across a `.frame` switch for a variable
   name that also exists in outer frames; use `dv` (frame-scoped locals
   listing) instead.

### The real mechanism, caught directly via one round of source instrumentation

Per the user's own explicit fallback authorization ("instrumentiere
ausschließlich die Stores dieses kleinen Pfads, die die konkrete
Device-Pointer-Adresse überlappen"), added a minimal, one-shot, static-flag-
gated diagnostic directly to the generated `sub_004AED5D` in
`tools/recomp/output/scda_v060/gen/recomp_0027.c` (gitignored, temporary,
fully reverted at the end of this entry): a small static helper comparing
`MEM32(0x4AB668)` against the known-good `0x004AB670` at five checkpoints
bracketing the row-copy loop's dword copy, byte-tail copy, per-row pointer
recompute, outer-loop test, and function exit -- firing (writing a marker
file and one `fprintf`) only the first time the value is ever seen to
change after having first been observed correct. One fresh launch (build
required recompiling only the one changed translation unit) produced,
via the game's own `scda_runtime.log` (its `stderr` is `_dup2`'d there by
`main.c` -- an external `cdb`/redirect capture never sees it, which is why
the very first launch attempt showed nothing in the console-redirected log
until this was found and `scda_runtime.log` was checked instead):

```
SCDA-DIAG224 cp=after-dwordcopy bad=7400087D esi=F0253E00 edi=F04ABE00
    ecx=00000000 ebx=00000A00 edx=F0253400 eax=F04AB400 ebp=00F7B0CC
```

This checkpoint fires immediately after the loop's dword `memcpy`
(`recomp_0027.c` -- the `if (!g_df) { memcpy(...); esi += ecx*4; edi +=
ecx*4; }` branch at what was line ~8047 before instrumentation). At this
point `esi`/`edi` already reflect the post-copy (advanced) pointers, and
`ebx` still holds the pre-shift byte count (`0xA00` = 2560 bytes) computed
two lines earlier -- so the copy that just executed moved 2560 bytes from
source `[esi_before, esi_before+0xA00) = [0xF0253400, 0xF0253E00)` to
destination `[edi_before, edi_before+0xA00) = [0xF04AB400, 0xF04ABE00)`.

**Both addresses are in the tiled aperture** (`XBOX_TILED_BASE =
0xF0000000`, documented in `xbox_memory_layout.c` as "The NV2A exposes
physical RAM a second time here and titles render through it" -- a `view`
of the *same* file-mapping section as base RAM, at a `+0xF0000000` guest
offset). Subtracting the tiled base gives the physical range this copy's
destination actually touches: `[0x004AB400, 0x004ABE00)`.

**`0x004AB668` (the device pointer's own address) falls squarely inside
that range** (`0x004AB400 <= 0x004AB668 < 0x004ABE00`, at offset `0x268`
into the 2560-byte destination span). **This one `memcpy` call is the
direct writer.** It overwrites the device-pointer global with whatever
happened to be at the corresponding offset in the *source* surface data
(`[0xF0253400, 0xF0253E00)`, i.e. physical `[0x00253400, 0x00253E00)`) --
which is why the corrupted value (`0x7400087D`) looked like arbitrary
data rather than a null or an obviously-invalid sentinel: it is real
pixel/surface bytes from an unrelated buffer, not garbage in the usual
sense.

### Why the earlier hardware watchpoint (Update 224) never fired -- fully explained now

The watchpoint was armed on `g_xbox_mem_offset + 0x004AB668` -- the
**base-RAM view's** host address for that guest byte. The corrupting
write instead goes through `XBOX_PTR(edi)` with `edi` in the **tiled
aperture**, i.e. `g_xbox_mem_offset + 0x004AB668 + 0xF0000000` translated
through `g_tiled_view`'s *separate* `MapViewOfFileEx` mapping (a different
host virtual address, aliasing the same physical bytes via the shared
file-mapping section documented in `xbox_memory_layout.c`). A CPU
data breakpoint matches *linear address*, not physical page identity, so
a watchpoint on the base view's address can never fire for a write that
goes through the tiled view's address, no matter how tightly the
observation window is gated. This is exactly the "host memory aliasing"
working hypothesis from Update 224, now identified precisely (the tiled
aperture specifically, not a 64MB RAM mirror) and confirmed against a live
capture rather than inferred from a process of elimination.

### Also explains the earlier "loop never advances" observation as a separate, correct call

Update 224's finding that this same loop's `esi`/`edi` were frozen at tiny
values (`0x394`/`0x398`) across 480 iterations in an earlier run is not
contradicted by this entry -- `sub_004AED5D` is evidently a **generic,
shared row-copy routine called for many different surfaces** throughout
this phase of boot (both the tiny-address run and this tiled-aperture run
share the exact same call chain skeleton down to
`sub_0004934E`/`sub_00037274`/`sub_002E5F95`/`sub_004AE0A7`/
`sub_004ADE9D`/`sub_004B0F92`, confirmed identical across multiple
captures). Different runs' non-deterministic timing evidently reach
*different individual calls* to this same shared function first --
sometimes a harmless one with degenerate near-zero addresses (a separate,
real defect: that call's own pitch/size fields read as zero, worth a
follow-up but not the cause of this corruption), sometimes -- as in this
entry -- the specific call whose destination overlaps the device pointer.
This also means the "loop that never advances" defect found in Update 224
is confirmed to be independent of this one; both are real, both live in
copies made by this same shared function, for different callers/surfaces.

### Not yet done: tracing the destination address back to its ultimate source, and comparing against the original x86

Per the user's own next-step instruction, the remaining work is to trace
where `edi`'s tiled destination address (and the byte count in `ebx`)
actually come from -- i.e. which surface/texture descriptor feeds
`sub_004AED5D` this particular call's parameters, whether that
descriptor's base address or pitch is itself computed correctly relative
to the original x86, and whether the true bug is (a) the destination
surface's base address/size being computed wrong (an overrun that would
also occur, in some form, on real hardware, just not necessarily hitting
this exact global), or (b) a discrepancy specific to this recompilation's
placement of the device-pointer global relative to where the original
XBE's linker actually placed the colliding surface buffer. This has not
been started this entry -- flagged as the clear next step, matching the
user's own explicit instruction, and squarely a faithful-translation
question this project's established methodology (Capstone byte-level
comparison against `default.xbe`) is well suited to answer.

### Process hygiene

All five `scda_diag224_check` call sites and the helper function were
removed from `recomp_0027.c` after the capture (diffed visually against
the pre-instrumentation reads in this and the previous entry to confirm a
clean revert); the project rebuilt cleanly recompiling only that one
translation unit. `git status`/`git diff` confirm zero tracked-file
changes (the generated file is gitignored). No clamp, no fake pointer, no
workaround. All `scda_recomp.exe`/`cdb.exe` instances and the leftover
`scda_diag224_success.marker` file cleaned up at the end.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
