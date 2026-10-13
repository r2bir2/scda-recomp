path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """

## Update 229 (2026-09-22): FIX IMPLEMENTED for the 0x4AB668 corruption -- a genuine physical-memory alias for dynamically bump-allocated contiguous memory, verified by a startup self-test and a clean extended run with zero recurrence of the old corruption/crash signature

Per explicit user instruction, rejected a pure workaround (e.g. clamping or
relocating the one colliding allocation) in favour of fixing the underlying
architectural gap Update 228 identified: the contiguous-memory window
(0x80000000+) was backed by storage with no physical identity with base RAM,
so a contig surface's tiled-aperture alias silently read/wrote base RAM's
own bytes at the same physical offset instead of the surface's own.

### The fix: a second, genuinely-aliased view for dynamic contig allocations

`src/kernel/xbox_memory_layout.c`:

- **New globals** (declared near `g_contig_memory`, before `xbox_ContiguousAlloc`
  needs them): `g_contig_alias_view` (the new mapping), `g_contig_alias_base`
  (its guest VA), `g_contig_alias_need` (how far the tiled aperture must
  reach to cover it), and `g_contig_next`/`g_contig_top` moved up from their
  old spot next to `xbox_ContiguousAlloc` so `xbox_MemoryLayoutInit` can set
  them before that function is ever called.
- **Physical-offset choice**: `contig_alias_phys_off = max(g_memory_size,
  XBOX_CONTIG_SIZE)` -- the first physical offset guaranteed to be past
  *both* base RAM's own fully-claimed layout (image + stack + heap, which
  reaches exactly `g_memory_size` via `XBOX_HEAP_TOP`) *and* the legacy
  contig window's own `VirtualAlloc` span, so the new region can never
  overlap either. Computed and stored (`g_contig_alias_base`,
  `g_contig_alias_need`) immediately after `g_memory_size` itself, before
  `CreateFileMappingA` is even called.
- **File mapping grown**: the same `g_mapping_handle` section used for base
  RAM, its mirrors, and the tiled aperture is now sized
  `g_contig_alias_need` bytes (`g_memory_size` plus one `XBOX_CONTIG_SIZE`
  tail) instead of exactly `g_memory_size`. The base view and every RAM
  mirror are unchanged -- they only ever map file offset `[0, g_memory_size)`
  -- so this costs nothing for anything already working and cannot shift
  where the 64 MB address-bus-wrap mirrors land (a real, load-bearing
  behaviour for other titles; see the mirror-setup comment).
- **New view mapped right after the legacy contig window, *before* the
  RAM-mirror loop**: `g_contig_alias_view = MapViewOfFileEx(g_mapping_handle,
  ..., contig_alias_phys_off, XBOX_CONTIG_SIZE, XBOX_CONTIG_BASE +
  contig_alias_phys_off)`. Ordering caught a real bug during this session's
  own testing (see below) -- it has to commit before the mirror loop runs,
  not after, or a generic wraparound mirror silently claims the same address
  first.
- **`xbox_TiledApertureSize()` extended** to also cover
  `g_contig_alias_need` (still bounded by the same two hard ceilings --
  0xFD000000 and the 4 GB wrap -- it already encoded), so the tiled
  aperture's own `MapViewOfFileEx` reaches the new region's physical offset
  too.
- **`xbox_ContiguousAlloc`'s bump pointer redirected**: on success,
  `g_contig_next`/`g_contig_top` move to the alias region (no
  `XBOX_GPU_INSTANCE_DEFAULT` carve-out needed there --
  `bridge_MmClaimGpuInstanceMemory` always computes its own address from the
  legacy `XBOX_CONTIG_BASE` window directly and never consults this bump
  pointer, so the two mechanisms stay independent exactly as before). Falls
  back to the legacy, unaliased window (today's old behaviour, known gap) on
  any failure -- mapping failure, tiled aperture not reaching far enough, or
  the self-test itself failing -- rather than refusing to start, matching
  every other aperture's failure policy in this function.
- **`fence_readable()` extended** to also recognise the alias region as
  backed, so the NV2A-fence-polling thread still follows a chain into a
  GPU-written semaphore living in a dynamically-allocated contig buffer.
- **The legacy window (`g_contig_memory`, 0x80000000-0x84000000 by default)
  is completely untouched**: still its own independent `VirtualAlloc`,
  still what Halo's fixed low-address pins and the fake kernel page at
  0x80010000 use. Nothing here is reachable through the tiled aperture on
  its own, so the missing three-way alias that fix targets was never
  observable for it -- see the long comment on that block for the full
  reasoning.

### Startup self-test

Runs once the tiled aperture is mapped (the alias-view mapping itself
happens earlier -- see the mirror-collision note below -- but proving the
round trip needs the tiled window to already exist): writes a marker
through the contig alias, reads it back through the tiled aperture at the
matching physical offset, then writes a different marker through tiled and
reads it back through contig. Both directions have to match or the fix
falls back to the legacy window and prints a WARNING rather than silently
trusting an unproven mapping. Confirmed passing in this session's test runs
(`Contig alias verified (both directions, contig<->tiled)`).

**Not a three-way test.** Base RAM itself deliberately does not reach this
region (see the long comment on `g_contig_alias_view` in the source) --
extending base RAM's own reach there was rejected because it would either
shift the mirror wraparound boundary (breaking the RenderWare memory
walker's documented 64 MB wrap behaviour for other titles) or require
shrinking the heap's own ceiling to free genuinely spare room in the same
64 MB (a real reduction in what a title can allocate). The two-way
contig<->tiled alias this fix provides is sufficient: a dynamic contig
allocation's bytes can no longer physically coincide with any base-RAM
global, because the two no longer share a physical offset at all -- there
is nothing left for a three-way test to prove that the two-way one does not
already cover.

### A real bug caught during this session's own verification: mirror/alias address collision

The first build of this fix mapped `g_contig_alias_view` *after* the tiled
aperture (so the self-test could run immediately), which put it after the
RAM-mirror loop too. The mirror loop tiles the entire 4 GB range above
`g_memory_base` in 64 MB steps with no awareness of what any particular
slice conceptually means, and mirror index 32 (0-based) covers exactly
guest `[0x84000000, 0x88000000)` -- this fix's chosen alias address. That
mirror mapped successfully first (nothing occupied the address yet), and
`g_contig_alias_view`'s own `MapViewOfFileEx` then failed with
`ERROR_INVALID_ADDRESS` (error 487) against an address that looked free but
was not:

```
WARNING: contig alias view at 0x84000000 failed (error 487); dynamic
contiguous allocations will use the legacy window and remain subject to
the tiled-aperture aliasing gap (Update 228)
```

Fixed by moving the alias-view mapping to right after the legacy contig
window, *before* the mirror loop -- the same position the legacy window and
the kernel page already occupy. The mirror loop's own `VirtualQuery`-based
occupied-range fallback (already there for exactly this situation -- see
its own comment about the legacy contig window, NV2A, MCPX, and flash
apertures) then correctly detects the now-already-committed alias region
and fails over/skips it:

```
Mirror 32: FAILED at 0000700080000000 (error 487)   <- legacy contig window (unchanged, pre-existing)
Mirror 33: FAILED at 0000700084000000 (error 487)   <- new alias region (expected, now correctly detected)
```

The self-test itself stays where it was (after the tiled aperture is
mapped) since it only needs the view to already exist, not to be mapped at
that exact point in the function.

### Verification

Full pipeline rebuild (`cmake --build build/scda_upgrade --config
RelWithDebInfo --target scda_recomp`), clean except pre-existing warnings
unrelated to this change (implicit `getenv`/`strtoul`/`atoi`/`_exit`
declarations, an `old_protect` shadow warning, both present before this
session).

Two independent test runs (`scda_recomp.exe`, `build/scda_upgrade/bin/
RelWithDebInfo`), ~20 s and ~100 s respectively, both reaching roughly
180,000+ kernel calls and 5,000+ runtime-log lines:

- `Contig alias verified (both directions, contig<->tiled)` printed in both.
- Zero occurrences, in either full log, of the old corruption value
  (`7400087D`), the old crash site (`004A12B0`), a `[CONTIG] arena
  exhausted` message, or any access-violation/exception text.
- Both runs settle into the same steady-state loop (repeating `[KERNEL]
  summary` lines, ordinals 277/246/294 -- `RtlEnterCriticalSection`/
  `RtlLeaveCriticalSection`/`ObReferenceObjectByHandle`, consistent with a
  Bink worker thread's own critical-section polling, not a new fault or
  hang introduced by this change) regardless of run length, so this is a
  plateau rather than a truncated run. Whether this steady state is itself
  the next real blocker, or an artifact of this test environment lacking
  real disc data, is **not yet determined** -- flagged for the next entry
  rather than assumed either way.

Not done this entry: locating and single-stepping the exact former
corruption site (`sub_004AED5D`'s row-copy / `sub_004B1377`'s construction
path) live under `cdb` to watch it *not* corrupt 0x004AB668 -- the self-test
proves the underlying aliasing mechanism directly and the extended run shows
no recurrence, which is the evidence this entry relies on; a live
re-confirmation at the exact original site would be strictly more thorough
but was not required to reach this conclusion.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
