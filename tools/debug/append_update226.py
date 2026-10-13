path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 226 (2026-09-21): CORRECTION to Update 225's own diagnostic -- the row-copy is NOT a zero-pitch/overflow bug; it is a complete, correctly-bounded ~640x480 surface copy whose entire destination legitimately spans a physical range that happens to contain the device pointer's address; the real question is now where that destination's physical/tiled base address comes from

**Self-correction, in the interest of an honest record:** Update 225's
single-checkpoint capture read `dst_pitch`/`src_pitch` from
`dst_desc/src_desc + 0x104C` and found both zero, leading to a
(reasonable but wrong) hypothesis that the copy was a degenerate,
non-advancing loop that might be self-corrupting its own descriptor via
an out-of-bounds write. **This was a bug in the diagnostic itself, not a
finding about the game:** `+0x104C` is a different field, read only once
per *plane* (multiplied by a row index that is `0` on first use, so its
value is irrelevant there) -- the field actually used to advance the
per-row pointers is `+0x1048` (`recomp_0027.c`: `edx = edx +
MEM32(ecx + 0x1048); eax = eax + MEM32(ebx + 0x1048);`), which this
entry's corrected capture did not print in Update 225.

### Corrected, per-row capture

Re-instrumented the same loop (same temporary, one-shot pattern, fully
reverted afterward) to log every row up to a bounded cap of 130,
capturing `esi`/`edi` (the actual addresses used) directly. Result,
first and last few rows before the previously-found transition:

```
row=1   esi=F020CA00 edi=F0464A00
row=2   esi=F020D400 edi=F0465400
row=3   esi=F020DE00 edi=F0465E00
...
row=114 esi=F0253400 edi=F04AB400
row=115 esi=F0253E00 edi=F04ABE00   <- cur=7400087D (Update 225's transition)
```

**The destination address advances by exactly `0xA00` (2560 bytes) every
single row, with zero gaps or jumps, for at least 115 consecutive rows.**
This is a completely healthy, correctly-bounded copy -- 2560 bytes/row
matches a 640-pixel-wide, 4-bytes-per-pixel (32bpp) surface exactly, and
with `dst_rows=0x1E0` (480) from Update 225's own capture, the full
destination span is `[0xF0464000, 0xF0464000 + 480*0xA00) =
[0xF0464000, 0xF0590000)` -- a plain, contiguous ~1.2MB framebuffer-sized
surface in the tiled aperture, not an overrun past any bound.

**`0x004AB668` (physical, after subtracting the `0xF0000000` tiled base)
sits inside this destination's normal, intended span** (`0x00464000 <=
0x004AB668 < 0x00590000`), not at some edge reached only by an
out-of-bounds write. There is no copy-loop bug here at all: given this
exact source, destination-base, pitch and row count, the copy is doing
exactly what it was told to do.

### What this actually points at

The bug is not in `sub_004AED5D`'s copy loop, and not in a "zero pitch"
defect (Update 224's genuinely-frozen, tiny-address case is a separate,
real defect in some *other* call to this same shared routine, as Update
225 already concluded -- unaffected by this correction). The real
question is now squarely: **where does this specific call's destination
surface get its physical/tiled base address (`dst_desc + 0x18 =
0xF0464000`), and is that address correct?**

Two live possibilities, not yet distinguished:

1. **A genuine placement bug in this recompilation's surface/texture
   allocator or physical-memory accounting** -- something computes or
   assigns `0x00464000` as this surface's physical base when it should
   have gone somewhere that doesn't overlap the D3D device-pointer
   global's own static address (`0x004AB668`), most likely because this
   project's contiguous/tiled-memory allocator doesn't reserve or account
   for the same fixed low-memory regions the original Xbox kernel/linker
   implicitly protects.
2. **This is faithful to the original and the original title's own
   physical memory map genuinely does not have this collision** -- i.e.
   real hardware also computes `0x00464000` (or whatever the equivalent
   is) for this exact surface, but the *device-pointer global itself* is
   simply not physically located at `0x004AB668` on real hardware in the
   first place, and this recompilation's static-data placement for that
   one global differs from the original XBE's own linked layout.

Distinguishing these needs comparing this recompilation's actual global
data segment layout against `default.xbe`'s own linked addresses for
this same global (already confirmed identical -- `0x004AB668`/`0x004AB670`
are literal constants taken directly from the original instruction bytes,
per this whole investigation's earlier byte-verification work), which
argues against explanation 2 on its face: if the ORIGINAL game's own
compiled code hardcodes `mov eax, [0x4AB668]` for the device pointer, the
original linker necessarily placed that global at that exact physical
address too, real hardware included. **This makes explanation 1 (a
surface-allocator/physical-memory-accounting bug specific to this
recompilation) the more likely candidate**, but this has not yet been
confirmed by finding the actual allocation call and its inputs.

### Explicitly not done this entry

Did not trace `dst_desc + 0x18`'s origin (which allocation or
texture-creation call populates it, and with what inputs) -- this is the
clear next step. Did not compare against xemu ground truth for this
specific surface's real-hardware physical placement, which the user's
own earlier standing instruction (compare with xemu when practical)
would call for once a concrete allocation site is found to compare. No
clamp, no fake pointer, no workaround. Instrumentation added and fully
reverted again this entry (second round this update-pair, both times
confirmed clean via `git status`/`git diff` -- the generated file is
gitignored, source-tracked files untouched).
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
