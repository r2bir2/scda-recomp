path = "docs/SCDA_WORKLOG.md"

with open(path, "r", encoding="utf-8") as f:
    content = f.read()

entry = """
## Update 222 (2026-09-21): New blocker diagnosed -- sub_004A12B0's poll loop spins forever waiting on a PFIFO/PGRAPH-style hardware completion bit through a D3D/NV2A device pointer that was never properly created, confirming and sharpening the pre-existing "no video backend wired in" gap

**Task followed:** per this round's own final instruction, investigate the
new `sub_004A12B0`/`sub_004A1600`/`sub_004A1740` poll loop as a separate
next blocker, checking first whether `recomp_manual.c`'s existing
`sub_00040A88` comment (an earlier, 2026-09-05/11 UMOD-reader investigation
that already mentions this same address) already covers it.

**Read `sub_004A12B0` (`recomp_0026.c:133445`, category `game_render`,
original 0x004A12B0-0x004A1360, 176 bytes/47 insns).** The live `cdb`
snapshots from Updates 220/221 (alternating `+0x1e8`/`+0x1d0`, a tight,
2-instruction self-loop in the compiled x64 output) match this exact
source location:

```
loc_004A12C9: ;
    eax = MEM32(0x4AB668);        // the known D3D/NV2A device pointer
    eax = MEM32(eax + 0x1C28);    // -> PFIFO/PGRAPH-style register block
    ecx = MEM32(eax + 0x100410);
    ecx = ecx | 0x10000;
    MEM32(eax + 0x100410) = ecx;  // set a "kick" bit

loc_004A12F0: ;
    if (TEST_NZ(MEM32(eax + 0x100410), 0x10000)) goto loc_004A12F0; // spin
```

This is a classic hardware register poll: set a control bit to kick off
some GPU operation (offset `0x100410` strongly resembles an NV2A
PFIFO/cache-control register by its shape), then spin waiting for the
hardware to clear it on completion.

**Confirmed live that the device pointer is not valid.** Read
`MEM32(0x4AB668)` on a fresh, live instance stuck in this exact loop (via
`cdb`, translating through `g_xbox_mem_offset`): **`0x7400087D`** -- an
address far outside this recompiler's 64MB guest RAM range
(`XBOX_TOTAL_RAM = 0x04000000`), meaning this is not a legitimate,
initialized guest heap pointer to a real device structure. Dereferencing it
(`+0x1C28`, then `+0x100410`) lands on whatever the guest-memory access
implementation resolves an out-of-range guest address to (not a crash --
the process is observed only spinning, at a stable ~16% CPU, never
faulting) -- almost certainly wrapped/masked into some unrelated region of
the 64MB guest RAM that has no reason to ever have its `0x10000` bit
cleared, since nothing in this recompiler emulates real NV2A/GPU hardware
completion for it.

**This is the concrete confirmation of an already-known gap, not a new,
unrelated bug.** Memory (this session's own standing notes, "SCDA no video
pipeline") already recorded: "D3D device (0x4AB668) never created and
scda_recomp has zero video backend wired in, so nothing can render yet;
next step is a cheap cdb reachability check, not more soak or an input
framework." This update *is* that reachability check, and the answer is
now definitive: after the Update 220/221 cursor-formula fix cleared the
earlier, unrelated `sub_00040821`/`sub_0003FCA5` corruption, the boot
sequence now progresses far enough to actually *try* to use the graphics
device for the first time -- and immediately hangs here, because there is
still no real device behind `0x4AB668` for it to talk to.

**`sub_00040A88`'s own comment does not cover this instance.** That
2026-09-05/11 investigation's mention of `sub_004A12B0` ("...why the whole
chain ... looks like it spins forever in sub_004A12B0...") was about a
completely different symptom -- an UMOD archive reader stuck re-parsing a
wildly wrong file-count due to a bad `Seek` position, which that fix
(the `sub_00040A88` override already in `recomp_manual.c`) already
resolved. This is a structurally different hang in the *same* function
(a real hardware-register poll with no backing device, not a software
parsing loop with a wrong iteration count) -- the earlier fix's presence
confirms the address is a recurring point of interest in this codebase,
not that it is already solved for this cause.

**Scope assessment, not started:** resolving this properly means wiring up
an actual NV2A/D3D device (creation of the `0x4AB668` structure, and at
minimum a stub PFIFO/PGRAPH register model that acknowledges kicks) --  a
substantial new subsystem, not a bounded bug fix, and squarely the
"no video backend wired in" milestone already flagged in memory rather
than a quick patch. No fix attempted this round; this is a diagnosis and
scope call, matching the "cheap reachability check" the standing memory
note asked for.

**Model tier:** the diagnosis itself was STRONG-appropriate (bounded,
confirmed via one live read). Actually implementing a video backend is a
different, much larger class of work and should get its own scoping
discussion before starting.
"""

new_content = content.rstrip("\n") + "\n" + entry
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_content)

print("done, new length", len(new_content))
