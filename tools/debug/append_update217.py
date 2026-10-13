path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 217 (2026-09-21): Located the actual caller/loop that drives sub_00040821 -- no terminator tag anywhere, confirms explanation 2, but also finds the ring buffer is stack-allocated (alloca) and surfaces a concrete, testable unified hypothesis combining explanations 2 and 3

**Task followed:** continue Update 216's own next step (read `sub_0007DE25`,
`sub_0007F412`, `sub_00064B00` for terminator/tag logic). Read `sub_0007DE25`
in full immediately; before investing in the other two (714 and 656 lines),
pivoted to a more direct method -- grep the whole generated output for every
call site that targets `sub_00040821` and read its actual, real caller
first, since that is guaranteed to contain whatever governs the read loop,
rather than continuing to guess among Update 177's three candidates.

**`sub_0007DE25` ruled out:** full read (98 lines) shows a small, generic
vtable-dispatch object-teardown routine -- releases three sub-objects via
indirect calls through `MEM32(eax+0x20)`, conditionally calls `sub_00036582`,
and touches refcount-shaped fields at `+0x14/+0x18/+0x1C`. Not terminator
logic in any form.

**The real caller found: `sub_00042F00`/`sub_000430A5`.** Both of
`sub_00040821`'s call sites (`recomp_0001.c:80465` and `:80760`) return to
the same label `0x431C9`, and both live inside one logical function split
across two `void sub_...(void)` blocks at the C level (`sub_000430A5` is
marked `frameless: inherit caller's frame` -- it is a fallthrough/jump
target of `sub_00042F00`, not an independent call). This function is the
resource loader that opens the file, sets up the ring-buffer object, and
runs a loop that calls `sub_00040821` (refill) then `sub_000517A4`
(parse/consume) repeatedly.

**New, unexpected finding: the ring-buffer object is not a heap/arena
allocation at all -- it is `alloca`'d directly on this loader function's own
stack frame.** At the very top of `sub_00042F00`: `eax = 0x40B0; call
sub_00365C30;` -- `0x40B0` = 16560 decimal, and every later reference to the
ring-buffer object inside this function uses `ecx = ebp + -16560` as its
`this` pointer. `sub_00365C30` is called with the reservation size in `eax`
before any of the object's own field writes, matching the standard
MSVC `_alloca_probe`/`_chkstk` calling convention exactly. This means the
question this round's original instructions posed ("does the region before
the object belong to a larger parent/arena allocation") has a concrete
answer for this specific object: there is no heap arena involved here at
all -- "before the object" (in the underflow direction) means lower
addresses within this same loader function's own stack frame, i.e. its own
other locals and, past the alloca's base, its saved `ebx/esi/edi` and
eventually its own caller's frame.

**The actual read/parse loop, fully read (`recomp_0001.c:80746-80793`):**

```
loc_000431AE:
    ebx = min(MEM32(ebp-8), edi)      // edi = 0x1000 fixed chunk size
    MEM32(ebp-8) -= ebx
    call sub_00040821(this=ebp-16560, ..., ebx, MEM32(ebp+8))   // refill
loc_000431C9:
    call sub_000517A4(...)             // consume/parse
loc_000431DA:
    if (MEM32(ebp-8) != 0) goto loc_000431AE;   // loop while remaining != 0
```

This is a plain, unconditional byte-counted loop. There is no tag or
terminator comparison anywhere in it -- it runs until `MEM32(ebp-8)` reaches
zero, full stop. This directly confirms explanation 2 from Update 216 in
concrete form, one level higher than `sub_00040821` itself, and makes it
very unlikely that `sub_0007F412`/`sub_00064B00` (not yet fully read, but
already recognized to be a large constructor and a file-open/path-format
routine respectively based on partial reads) contain a terminator check
either -- they sit even further from this loop, not inside it.

**Where `MEM32(ebp-8)` (the loop's total) comes from -- fully traced:**
immediately before the loop, `sub_000430A5` does, in order: `SetFilePointer
(handle, -4, NULL, FILE_END)` (seek to 4 bytes before the file's true
physical end) -> `sub_000516B7(handle, &MEM32(ebp-8), length=4, &bytesRead)`
(a genuine disc read of exactly 4 bytes into `MEM32(ebp-8)`, checked
`bytesRead==4`) -> `SetFilePointer(handle, 0, NULL, FILE_BEGIN)` (seek back
to the start). **`MEM32(ebp-8)` is therefore the file's own embedded 4-byte
trailer value, read directly from `common.lin`'s last 4 bytes, and used
unconditionally as the total byte budget for the entire loop.**

**A second, independent size value exists alongside it.** Right after the
seek-back, the same function also calls `sub_00051B8B` -> `sub_00051B4D`,
which is a genuine `NtQueryInformationFile` wrapper: args match exactly
(`FileHandle`, `IoStatusBlock=ebp-8`, `FileInformation=ebp-64`,
`Length=0x38`, `FileInformationClass=0x22` = `FileNetworkOpenInformation`),
and the two dwords copied out (`MEM32(ebp-64+40)`/`+44`) land exactly at
that structure's `EndOfFile` field offset (0x28). This is a **live, genuine
query of the file's real physical size**, not a derived or cached value.
`(PhysicalSize - 4)` is then passed into `sub_00040766(this=ring_buffer_obj,
0, 0, PhysicalSize-4)` -- almost certainly telling the ring buffer object
how many bytes are actually available to it on disk, independent of what
the outer loop's trailer-derived counter says it should still ask for.

**Checked `kernel_file.c`'s own `NtQueryInformationFile` implementation for
a static bug -- found none.** Both the Windows path (`GetFileInformationByHandle`,
`recomp`'s `xbox_NtQueryInformationFile` around line 575/587) and the POSIX
path (`stat()`, around line 1164/1170) perform a genuine, live query against
the actually-open host file handle for `FileNetworkOpenInformation`'s
`EndOfFile`, with no hardcoded or derived value. This rules out a trivial
"wrong constant" bug in our own kernel bridge for this specific call.

**Checked the physical byte count of every `common.lin` copy on disk --
all match retail exactly.** All three build output trees
(`build/scda/bin/{Release,RelWithDebInfo}`, `build/scda_upgrade/bin/RelWithDebInfo`)
report `Default/DataXb/LMaps/common.lin` at exactly 1,924,977 bytes, matching
the retail ISO extraction (`SC4_debug_compare/unpacked iso/Retail/...`)
byte-for-byte, consistent with Update 173's already-established full-content
match. (Aside: a completely unrelated, much larger `adv/System/common.lin`,
7,758,556 bytes, also exists in the disc tree -- different file, same
name/extension, not relevant here. A separate "proto" extraction under
`SC4_debug_compare` has a *different-sized* `LMaps/common.lin`, 1,922,128
bytes -- almost certainly an unrelated prototype/beta build artifact, not
used by any of our build outputs, noted only so it is not mistaken for a
live discrepancy later.) This rules out the naive "recomp is reading a
wrong-sized file" explanation -- our file is the correct, retail-matching
one on every build output checked.

**Refined, unified hypothesis (not yet confirmed empirically):** since the
file's own trailer value (`MEM32(ebp-8)`'s initial value) is read directly
from file bytes already proven byte-identical to retail, it must be
numerically identical on recomp and on real hardware -- both platforms start
this loop with the exact same total budget. Update 185/216 already showed
xemu experiences the *same* short read (`want=0x20000 got=0x15F71`) that
recomp does. The concrete difference established in Update 216 is only that
xemu issues *no further* `sub_000516B7` calls afterward while recomp does.
Combining this with what was found this round: **if real hardware's outer
loop counter (`MEM32(ebp-8)`) happens to reach exactly zero at the same
iteration where this short read occurs, the loop exits cleanly on its own
before ever asking `sub_00040821` for one more refill -- and the already-
confirmed cursor-formula defect (Update 215, explanation 3) never gets
exercised, because nothing calls into it again.** If recomp's own count of
"effective bytes consumed per iteration" differs even slightly (plausible,
given Update 216 already found recomp and xemu land the equivalent cycle on
different internal call sites of `sub_00040821`), recomp's counter would
still be nonzero after the same short read, causing the loop to call
`sub_00040821` one more time -- which is exactly the call that walks into
the already-corrupted/stale ring-buffer state and triggers the compaction
memcpy. This would mean explanations 2 and 3 are not competing alternatives
but two halves of one mechanism: no terminator check exists (2), and the
long-latent, byte-identical-since-the-original-XBE cursor defect (3) is
harmless as long as the outer trailer-driven loop never asks for a refill
after a short read -- which is a matter of exact byte-accounting parity
between the two platforms' loop iterations, not of either platform having
different code.

**Not yet done -- concrete next test:** capture, on a live recomp run, the
value of `MEM32(ebp_outer - 8)` (the outer loop's remaining-byte counter,
reachable from the existing corrupting-memcpy capture point as
`MEM32(ebx_object_this + 0x40B0 - 8)`, since `ebp_outer = this + 0x40B0`)
at the exact moment the ring-buffer object gets corrupted, to see whether it
is already zero (loop should have stopped -- points to a different bug) or
still nonzero (confirms the unified hypothesis above). This requires one
new, small, one-shot capture (not yet added) rather than reusing anything
already reverted.

**No fix implemented this round either** -- per the standing instruction,
still clarifying. **Model tier:** STRONG remains appropriate; the next step
is one bounded, well-specified memory read added to a single known location.
"""

marker = "## Update 216 (2026-09-21)"
idx = content.index(marker)
end_idx = content.index("\n## ", idx + 1) if "\n## " in content[idx+1:] else len(content)
# Find the actual end of update 216 section (end of file in this case)
insertion_point = len(content)
new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
