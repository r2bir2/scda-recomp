path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 230 (2026-09-22): the post-Update-229 "steady state" identified and CONFIRMED as a genuine new divergence (not a test-environment artifact, not related to the contig-alias fix) -- a menu-tree node pointer that should be a dynamic heap allocation is instead a hardcoded, unaligned address inside the executable image itself, verified against live xemu ground truth

Per the user's explicit instruction, this entry answers one narrow question:
is the steady-state loop observed at the end of Update 228/229's test runs
(originally mischaracterized there as "Bink-worker critical-section
polling") a real new blocker, or an artifact of the test environment? The
governing constraint stood throughout: the Update 229 contig-alias fix
itself is considered closed and was not reopened, since nothing found here
implicates it.

### Step 1-2: what the loop actually is (not what Update 229 guessed)

Update 229's characterization was wrong. The repeating `[KERNEL] summary`
lines with ordinals 277/246/294 are generic RTL critical-section/object-
reference noise that happens to coincide with the real symptom, not the
symptom itself. The actual repeating event, visible in the same log as
`[ICALL] target 0x00000000 is not code -- skipped N time(s) ... source=
0x0005F701` (N climbing past 1000 within the test window), is on the
**main thread**, not a worker thread at all.

One-shot source instrumentation (temporary, in the gitignored
`recomp_0002.c`, fully reverted afterward -- `git status`/`git diff` clean)
at `sub_0005F6FF` (the generic `this=ecx; call [[ecx]+0x54]` vtable-slot
dispatcher that source address belongs to) captured, on every one of 8
consecutive hits: `ecx(this)=0x81660000`, all of `[ecx+0]`, `[ecx+4]`,
`[ecx+8]`, `[ecx+0xC]` reading zero, and (by reading the emulated guest
stack at the exact point of the call, the same trick used throughout this
session to identify a caller without a native backtrace) a full, unbroken
call chain:

```
[caller at 0x622DA, inside sub_00062237's recursive tree-walk]
  -> sub_00060B2E(manager=ebx, cursor=stack-local)
       cursor.bucket = manager             ; = 0x00390037
       cursor.element = MEM32(manager+0x38) ; = 0x81660000
  -> sub_000600AD(cursor)                   ; walks cursor as a linked list
       ecx = cursor.element                 ; = 0x81660000
       -> sub_0005F6FF(ecx)                 ; this=0x81660000, all-zero object
            eax = MEM32(ecx)      = 0        ; vtable pointer itself is 0
            target = MEM32(eax+0x54) = 0     ; -> null indirect call, skipped
```

`sub_00062237` is a **recursive menu/scene-node tree walker** (`ebx` =
current node, tested via `MEM8(ebx+0x90)&4`; child list head at `ebx+0x24`;
recurses into itself for children whose flag bit is set). It is reached,
in this run, via the same deep call chain into the main initializer
already established in earlier updates
(`... -> sub_0003B78F -> sub_001D79F6 -> sub_00065F31 -> sub_00062237 ->
...`), immediately after `Menu.lin.dlf` (the main menu's level file) is
read successfully.

### Step 3-4: the constant is baked into the XBE image itself -- not a runtime computation, not missing disc/streaming data

`ebx = 0x00390037` is used as an object: `MEM8(ebx+0x90)` (a flag byte) and
`MEM32(ebx+0x38)` (the "first registered listener/child" field) are both
read directly. Parsing `default.xbe`'s own section table and reading the
**static, on-disk image bytes** at file offset for guest VA
`0x00390037+0x38` (`0x0039006F`) gives `00 00 66 81 ...` -- i.e. the exact
value `0x81660000` observed at runtime is **hardcoded in the executable
image itself**, section 0 (`vaddr=0x11000`, `vsize=0x454aa8`, the merged
code+data section, `flags=0x36`), not computed, not corrupted by any
allocator, and not dependent on any external disc/streaming file. Both
`lipsyncxbox.umd` and `splintercell4xboxretail.umd` are present in the test
environment and open/read successfully in the same log (`[SCDA-UMD-OPEN]`,
`[SCDA-UMD-READ]` both succeed) -- **the test environment is not missing
data relevant to this symptom.** This also, independently, rules out any
connection to Update 229's contig-alias fix: `0x00390037` is a base-RAM
image address, not a contig-window address, and `0x81660000`'s bytes come
from the static image, not from `xbox_ContiguousAlloc`'s bump pointer.

`0x00390037` is itself suspicious on its own terms: it is not 4-byte
aligned, unusual for a genuine C++ object address. That observation is
exactly what motivated the xemu ground-truth check below, rather than
accepting "the image just contains this constant" as the final answer --
the open question was whether `ebx` legitimately becomes `0x00390037` here
on real hardware too (in which case the constant is an intentional,
harmless "nothing registered yet" sentinel design), or whether `ebx`
itself is wrong in this recompilation.

### Step 5-6: xemu ground truth -- decisive, no ambiguity

A live GDB-remote session against the user's xemu build (real MCPX/BIOS/
disc image, `-s -S`, the established protocol from this project's existing
`tools/debug/xemu_*.py` scripts) set a software breakpoint at guest EIP
`0x000622D1` -- the exact `push ebx` immediately before the call to
`sub_00060B2E`, address-identical on both platforms since it is the same
XBE. No source or binary changes on either side; pure observation.

**92 hits collected** (helped along by the user manually advancing the
game past the passive main menu into a level, which produced far more
menu/HUD tree activity than sitting idle):

- `ebx` took on 40+ distinct values, **all in the `0x0080xxxx`-`0x00Dxxxxx`
  range** -- ordinary dynamically-allocated heap addresses on the real
  console's own allocator, changing hit to hit as objects are
  created/destroyed (e.g. `0x0081FD60`, `0x0082B420`, `0x00A27470`,
  `0x00D6BEF0`, ...).
- `[ebx+0x38]` was **either legitimately `0x00000000`** (empty, no
  children/listeners yet -- the harmless case, which never even reaches
  `sub_000600AD`'s loop body) **or another live heap pointer**
  (`0x0082F1C0`, `0x00822900`, `0x00A99300`, ...), consistent hit-to-hit
  for the same `ebx` as objects persist across frames, changing as the
  real allocator reuses/moves memory (e.g. `0x0083DD70`'s `+0x38` value
  moves from `0x0082D1A0` to `0x00A99300` to `0x00A99550` across
  successive hits -- exactly what a live, dynamically-updated list looks
  like).
- **`ebx == 0x00390037` never occurred, not once, in any of the 92 hits.**
- No hit ever showed a static-image-range value (`0x0000xxxx`-`0x00400000`
  or thereabouts) at either `ebx` or `[ebx+0x38]`.

**Conclusion: this is a genuine, confirmed divergence, not a benign
sentinel design and not a test-environment artifact.** On real hardware,
whatever computation feeds `sub_00062237`'s node pointers always produces
either a valid dynamically-allocated heap object or a clean empty (`0`)
state. In this recompilation, at least one call path instead produces a
raw, unaligned address that resolves to a fixed byte sequence baked into
the executable's own code/data section -- almost certainly a wrong pointer
computed (or a wrong branch taken) somewhere upstream of `sub_00062237`,
not a bug in `sub_00062237`/`sub_000600AD`/`sub_0005F6FF` themselves, which
faithfully do what any correctly-fed call chain would do with whatever
pointer they are handed.

### Not yet done

The producer of the bad `0x00390037` value -- i.e. which caller of
`sub_00062237` (or which upstream write to a node's own `+0x24` child-
pointer field) supplies this address instead of a real heap pointer -- has
not been traced. This is the concrete next step: find that producer,
compare its original x86 against the generated C at that exact point, and
identify the first wrong value, per the user's standing instruction.

No stub, no fake signal, no clamp, and no Bink-specific workaround were
introduced. The one temporary diagnostic added to `sub_0005F6FF`
(gitignored `recomp_0002.c`) was fully reverted and the clean baseline
rebuilt before the xemu comparison; `git status`/`git diff` show no tracked
changes from this entry. The xemu side made no changes at all (read-only
GDB-remote memory/register inspection).
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
