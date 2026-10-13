path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 228 (2026-09-21/22): ROOT CAUSE CONFIRMED, no fix yet -- the 0x4AB668 corruption is a genuine architectural gap in this recompiler's memory-aperture model: the contiguous-memory window (0x80000000+) is deliberately NOT backed by the same physical storage as base RAM, but the tiled aperture (0xF0000000+) IS only an alias of base RAM, so a contig-allocated surface accessed through its tiled alias silently lands on unrelated base-RAM bytes instead of its own storage

**Per explicit user instruction, this entry is audit-only: no patch
applied.** Directed to audit `xbox_ContiguousAlloc`/the tiled-memory
model directly (free-region tracking, whether `0xF0464000` should be
allocatable at all under this project's own memory layout, boundaries/
reservations/exclusions vs. the real NV2A/MMIO/framebuffer apertures,
and explicitly 32-bit overflow/wraparound, inclusive/exclusive end
addresses, alignment, and size arithmetic) rather than continuing the
mechanical backward call-chain trace from Updates 223-227.

### The three apertures and how each is actually backed in this recompiler

Read `src/kernel/xbox_memory_layout.c`'s own setup and comments for all
three relevant guest-address windows:

1. **Base RAM** (`XBOX_MAP_START = 0x00000000`, `XBOX_TOTAL_RAM` = 64MB):
   backed by `g_memory_base`, a `CreateFileMappingA` + `MapViewOfFileEx`
   section -- deliberately a *file mapping* (not plain `VirtualAlloc`)
   specifically so other apertures can share the same physical bytes via
   additional views of the same section (the file's own comment: "Using
   file mapping instead of VirtualAlloc allows us to map the same
   physical pages at multiple virtual addresses via MapViewOfFileEx").
2. **Tiled aperture** (`XBOX_TILED_BASE = 0xF0000000`): `g_tiled_view =
   MapViewOfFileEx(g_mapping_handle, ..., tiled_native, ...)` -- a
   genuine *view of the same file-mapping section* as base RAM. Its own
   comment confirms the intent precisely: "A view of the same section
   rather than fresh storage: the title writes a surface through the
   tiled address and reads it back through the normal one, so the two
   have to be the same bytes." **This aperture only, and correctly,
   aliases base RAM.**
3. **Contiguous window** (`XBOX_CONTIG_BASE = 0x80000000`,
   `XBOX_CONTIG_SIZE` = 64MB, both from `src/kernel/kernel.h`): backed by
   `g_contig_memory`, a **separate, plain `VirtualAlloc`** -- explicitly
   and deliberately *not* a view of the same file-mapping section. The
   code's own comment states the reasoning outright: "Deliberately NOT a
   view of the 64 MB RAM mapping. On hardware this window aliases
   physical RAM, but we load the XBE image into the low addresses of
   that same region, so aliasing would put a title's pinned pools on top
   of its own code. Separate storage costs an extra mapping and behaves
   correctly; nothing here depends on the aliasing."

### Where the assumption breaks

That last comment's premise -- "nothing here depends on the aliasing" --
is the crux, and this investigation has now found a concrete case where
it does not hold. This project's own further comments document the
*intended* real-hardware round trip precisely: "`MmAllocateContiguousMemory`
hands back physical memory, and on Xbox physical page P is visible at
`0x80000000 + P`" (`xbox_memory_layout.c`, above `xbox_ContiguousAlloc`),
and separately "Xbox D3D writes its pushbuffer position to the NV2A as
`VA & 0x0FFFFFFF` and reads the GPU's position back as `GET |
0x80000000`" -- i.e. **the exact same physical-page number `P` is meant
to be reachable through *three* apertures on real hardware: base RAM
(`0x00000000 + P`), the contiguous window (`0x80000000 + P`), and the
tiled aperture (`0xF0000000 + P`), all as views of one single, unified
physical address space.**

In this recompiler, `P` is tracked by **two independent counters that
both start at zero and know nothing about each other**: base RAM's own
occupied range (executable image, static data -- including the device
pointer global at guest VA `0x004AB668`, which is a *physical* address
in the `0x00000000+` sense) and `xbox_ContiguousAlloc`'s bump allocator
`g_contig_next` (starting at `XBOX_CONTIG_BASE`, counting up from its own
zero). **A contig allocation that lands `0x464000` bytes into the 64MB
contig window gets `P = 0x464000`.** When the game's own (faithfully
translated, byte-verified) D3D8/driver code converts that surface's
address into its tiled-aperture form via the exact documented formula
(`0xF0000000 | (VA & 0x0FFFFFFF)`), it computes `0xF0464000` -- and this
recompiler's tiled aperture, being wired *only* to base RAM's file
mapping, resolves that address to **base RAM's own physical offset
`0x464000`**, not to `g_contig_memory`'s storage at all. Since
`0x004AB668` (the device pointer) is a *base-RAM* address whose own
physical offset (`0x4AB668`) happens to fall inside the row-copy's
2560-byte-per-row, 480-row destination span starting at physical offset
`0x464000` (span `[0x464000, 0x590000)`, per Update 226), the copy
silently overwrites it -- **not a bounds violation by the copy's own
logic, and not an overflow/off-by-one in `xbox_ContiguousAlloc`'s own
arithmetic (its exhaustion check already correctly uses `uint64_t` to
avoid 32-bit wraparound, and its alignment/size handling is
unremarkable), but a structural consequence of two independently-numbered
"physical" spaces being funneled through one shared-only-with-one-of-them
aliasing aperture.**

### Answering the audit's specific questions

- **Free-region tracking:** `xbox_ContiguousAlloc` is a pure bump
  allocator (`g_contig_next`, never freed -- "contiguous blocks are
  framebuffers and pushbuffers, which a title allocates once", per its
  own comment) bounded only by `XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE -
  XBOX_GPU_INSTANCE_DEFAULT`. It has no concept of, and no interaction
  with, base RAM's own occupied ranges at all -- by design, since the two
  are meant to be (and, for every access path except the tiled aperture,
  successfully are) independent storage.
- **Is `0xF0464000` "allowed" under this project's own memory layout?**
  Yes, unconditionally -- `xbox_ContiguousAlloc` has no exclusion logic
  referencing the tiled aperture, the NV2A MMIO aperture, or base RAM's
  own layout at all, and none of the three is even aware of the other
  two's occupied ranges.
- **Comparison against actual NV2A/MMIO/framebuffer mappings:** the NV2A
  register aperture (`XBOX_NV2A_BASE = 0xFD000000`, 16MB, backed by its
  own separate `g_nv2a_memory` `VirtualAlloc`) and the MCPX aperture are
  both independent of this question -- they do not overlap `0xF0000000
  -0xF4000000` (the tiled aperture's own span, `xbox_TiledApertureSize()`
  bounded) or `0x80000000-0x84000000` (contig window). The collision is
  specifically between the tiled *alias of base RAM* and *contig-window*
  addresses, not with either hardware MMIO aperture.
- **32-bit overflow/wraparound, inclusive/exclusive bounds, alignment,
  size arithmetic:** all individually checked and clean.
  `xbox_ContiguousAlloc`'s exhaustion check casts to `uint64_t` before
  comparing, so it cannot wrap. Alignment is forced to a 4096-byte
  minimum before the bump-pointer arithmetic, which cannot overflow for
  any realistic size. The RAM-mirror code elsewhere in this same file
  (Update 224/225's earlier reading) already handles 64MB-multiple
  wraparound deliberately and correctly for a *different* reason (the
  documented address-bus wrap). **None of these is the bug here** -- the
  bug is the aliasing gap described above, which exists regardless of
  how carefully any individual allocator's arithmetic is written.

### Why this was not caught by the earlier (Update 223) hardware-watchpoint or the RAM-mirror hypothesis

This is the same underlying category of explanation floated as a
"working hypothesis" in Update 224 (host memory aliasing) and refined in
Update 225 to "the tiled aperture, specifically" -- this entry completes
that thread by identifying *which* two guest address ranges are
incorrectly treated as aliasing (base RAM and the contig window, via the
tiled aperture) and *why* (a deliberate, documented, but -- for this one
access pattern -- incorrect simplification), rather than a mirror-wrap
or a copy-loop bug.

### Not yet done

No fix implemented, per instruction. This is now a genuine design
decision, not a one-line correction: possible directions include (a)
making the tiled aperture correctly reachable from *both* base RAM and
the contig window at their respective physical offsets (requires knowing
which aperture a given physical offset "belongs to," which the current
flat-file-mapping design does not track), or (b) giving the contig
allocator its own dedicated, non-overlapping slice of tiled-aperture
address space so its offsets never coincide with base RAM's own layout,
or (c) something else -- not decided or scoped further this entry. No
clamp, no fake pointer, no workaround. This entry made no source changes
of any kind (pure reading/analysis of already-committed, tracked code);
`git status`/`git diff` are clean.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
