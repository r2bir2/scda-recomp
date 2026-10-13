path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 227 (2026-09-21): Traced the corrupting copy's destination surface back to its constructor and a format-code dispatch table; confirmed this is a generic pixel-format-conversion blit (not a "present to back buffer"), narrowing the open question to where the destination texture's physical/tiled base address gets allocated -- not yet found; flagging for a scoping decision on how far to keep tracing by hand

**Method:** two more rounds of the same temporary, one-shot, fully
reverted source-instrumentation technique (`recomp_0027.c`, gitignored),
plus one live `cdb` stack-trace capture, continuing the backward trace
from Update 226's sharpened question ("where does `dst_desc + 0x18`
= `0xF0464000` come from?").

### The descriptor wrapper is constructed from an already-complete "surface info" structure

Grepped this project's own generated code for writes to the rare offsets
`0x1048`/`0x104C`/`0x1050`/`0x1054`/`0x1058`/`0x105C` (all unique to
`recomp_0027.c`) and found the actual constructor, `sub_004B1377`
(`recomp_0027.c:15583`), called only via an indirect (vtable) dispatch --
confirmed via `grep` that it has no direct callers anywhere in the
generated code, only an entry in `recomp_dispatch.c`. Live one-shot
instrumentation at its body (`ebx` = new object "this", `eax` =
its own first parameter, a "info" struct pointer) captured, for the two
constructions that happen before the corrupting copy:

```
n=1 this=00FA0260 info0=F0464000 info4=00000012 info8=00000A00 infoC=00000000   (becomes DST)
n=2 this=00FA12E0 info0=F020C000 info4=00000012 info8=00000A00 infoC=00000000   (becomes SRC)
```

`info0`/`info8` map directly onto the constructed object's `+0x18` (base
address) and `+0x1048` (pitch) fields -- confirming `0xF0464000` isn't
computed by anything in this constructor or its two thin per-format
subclass wrappers (`sub_004B36B9`/`sub_004B36D5`/`sub_004B36F1`, one per
bit-depth group, each just forwarding the same "info" pointer and fixing
up a vtable afterward); it is copied verbatim from a caller-supplied
"info" structure. `info4 = 0x12` (18) is consistent with a linear 32bpp
pixel format code.

### The immediate caller is a 19-entry format-code dispatch table, not a "present" path

A live stack trace at the constructor's first hit (`cdb`, `kb 15`, one
shot) gives the full chain: `sub_004B1377 <- sub_004B36B9
(recomp_0027.c:21138) <- sub_004B38F7 (recomp_0027.c:21778) <-
sub_004B0F92 (recomp_0027.c:14791) <- sub_004ADE9D
(recomp_0027.c:6061) <- sub_004AE0A7 (recomp_0027.c:6357) <-
sub_002E5F95 <- sub_00037274 <- sub_0004934E <- ... <- main` -- the same
overall skeleton this whole investigation has traced since Update 223,
now extended three levels deeper. `sub_004B38F7`
(`recomp_0027.c:21708`) reads a format-code byte via `MEM32(eax +
0x4B3C00)` from its own "info" argument and dispatches through a
**19-entry jump table** to one of several per-bit-depth wrapper
constructors, after first allocating the wrapper object itself via
`sub_00055936` (a generic allocator, requesting `0x1064` bytes -- the
wrapper struct's own size, not the pixel data). This is a generic
**pixel-format-to-descriptor-wrapper factory**, used for many different
surface/texture operations throughout the game, not something specific
to displaying a frame.

**This rules out "present to the back buffer" as the operation's
identity:** checked whether `AvSetDisplayMode` (this project's own
display-mode-set entry point, previously observed logging `fb=...` once
reached) had even been called yet in this exact run before the
corruption fires -- it had not (`grep` for `SetDisplayMode` in
`scda_runtime.log` up to the kill point: zero matches). Since the
display/back-buffer address isn't even configured yet at this point in
boot, the destination surface here cannot be the primary back buffer;
this is some other texture or render target already created earlier,
undergoing a format conversion.

### Where this leaves the trace, and the scoping question

`0xF0464000` is supplied as a parameter all the way down from whatever
calls `sub_004AE0A7` (traced three more function levels than Update 226,
each one a thin pass-through of the same "info" pointer pair, not a
computation). The actual **allocation** of this destination texture's
physical/tiled address happens further up this chain, at a texture- or
surface-creation call not yet reached. Continuing to trace this
correctly requires the same kind of live, one-shot, stack-trace-based
backward walk used successfully this entry and in Updates 223-226 -- it
is mechanical and has a clear precedent, but the number of remaining
levels is not yet known, and it is now reaching into this project's
texture/contiguous-memory allocation subsystem rather than a single
self-contained copy routine.

**Not yet done:** finding the actual texture/surface-creation call that
assigns `0xF0464000` as this destination's physical base, and (per the
user's own standing instruction) comparing that address's derivation
against xemu ground truth or the original x86 to determine whether this
recompilation's contiguous/tiled memory allocator is placing this
surface somewhere the original title's own allocator would not have
(the working hypothesis from Update 226, still not confirmed). No fix
attempted. No clamp, no fake pointer, no workaround. All temporary
instrumentation added this entry (two more rounds, same pattern as
Updates 225/226) fully reverted; `git status`/`git diff` confirm no
tracked-file changes (the generated file is gitignored). All
`scda_recomp.exe`/`cdb.exe` instances cleaned up at the end.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
