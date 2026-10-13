# -*- coding: utf-8 -*-
# Mechanically extract sub_00040821 and sub_0004086E from the current
# generated output, apply the exact SCDA-FIX-215 two-line correction (no
# manual retyping -- eliminates transcription risk), and produce the
# hand-written override block for recomp_manual.c.

gen_path = "tools/recomp/output/scda_v060/gen/recomp_0001.c"
with open(gen_path, "r", encoding="utf-8") as f:
    lines = f.readlines()

def extract(start_1based, end_1based):
    # inclusive, 1-based line numbers
    return "".join(lines[start_1based - 1:end_1based])

sub_40821 = extract(68551, 68850)
sub_4086e = extract(68859, 69094)

assert sub_40821.startswith("void sub_00040821(void)\n"), sub_40821[:40]
assert sub_40821.rstrip().endswith("}"), sub_40821[-40:]
assert sub_4086e.startswith("void sub_0004086E(void)\n"), sub_4086e[:40]
assert sub_4086e.rstrip().endswith("}"), sub_4086e[-40:]

# --- Apply the exact fix, textually, verified by count ---

OLD_A = """    edx = 0x4000; /* still needed below at loc_00040942's edi = edi + edx */
    /* SCDA-FIX-215 TEMPORARILY REVERTED (per explicit instruction, docs
       Update 221): back to the original, byte-verified XBE behavior while
       we capture the native I/O parameters for the actual problematic read
       BEFORE trusting any fix in this formula. */
    edx = 0x4000;
    eax = edx;
    eax = eax - MEM32(ebp + -12);
    MEM32(ebx + 0x4060) = eax;
"""
# The file currently has the ORIGINAL (unfixed) code with a revert-marker
# comment inserted before it (from the temporary revert done this round).
# Match that exact current text and replace with the permanently-fixed form.
ORIG_A = """    edx = 0x4000;
    eax = edx;
    eax = eax - MEM32(ebp + -12);
    MEM32(ebx + 0x4060) = eax;
"""
FIXED_A = """    edx = 0x4000; /* SCDA-FIX-215: still needed below at loc_00040942's
                     edi = edi + edx -- do not remove or repurpose this. */
    /* SCDA-FIX-215 (docs Update 220/221): the original code computed the
       new cursor as (0x4000 - stale), silently assuming this refill always
       transfers exactly 0x4000 bytes. Confirmed via live native-I/O capture
       (Update 221) that real transfers here are routinely far larger (this
       exact case: 89969 bytes, a genuine honest short read at the file's
       true end -- byte-identical to real xemu hardware's own short read for
       this stream, Updates 185/216, so the native I/O layer is not at
       fault). eax already holds the REAL transferred byte count (read two
       lines above into the +0x4064 accumulator); use it instead of the
       hardcoded constant. `edx` is deliberately left untouched. */
    eax = eax - MEM32(ebp + -12);
    MEM32(ebx + 0x4060) = eax;
"""

ORIG_B = """    eax = 0x4000;
    eax = eax - MEM32(ebp + -12);
    MEM32(ebx + 0x4060) = eax;
"""
FIXED_B = """    /* SCDA-FIX-215 (docs Update 220/221): same defect and same fix as
       loc_000408F0 -- eax already holds the real transferred byte count;
       use it instead of the hardcoded 0x4000 constant. See that site's
       comment for the full justification. */
    eax = eax - MEM32(ebp + -12);
    MEM32(ebx + 0x4060) = eax;
"""

def apply_fix(body, label):
    count_a = body.count(ORIG_A)
    count_b = body.count(ORIG_B)
    if count_a != 1 or count_b != 1:
        raise SystemExit(f"{label}: expected exactly one match each, got A={count_a} B={count_b}")
    body = body.replace(ORIG_A, FIXED_A, 1)
    body = body.replace(ORIG_B, FIXED_B, 1)
    return body

sub_40821_fixed = apply_fix(sub_40821, "sub_00040821")
sub_4086e_fixed = apply_fix(sub_4086e, "sub_0004086E")

header = '''
/*
 * sub_00040821 / sub_0004086E -- hand-translated override (2026-09-21,
 * docs Updates 213-221).
 *
 * This is the generic ring-buffer refill/chunk-consumption trampoline used
 * by many different streaming assets across the whole game (category
 * `game_vtable`; `sub_0004086E` is a structurally overlapping duplicate
 * entered mid-function, sharing the same tail code, with its own caller's
 * `ebx` already set). Both bodies below are byte-identical, 1:1 faithful
 * translations of the original XBE at 0x00040821-0x00040A15 (confirmed via
 * direct Capstone disassembly against the original XBE, Update 213) --
 * this is NOT a disassembly-completeness fix like sub_00040A88 above.
 *
 * The one and only change from the auto-generated body is the cursor-
 * advance formula that appears twice, at `loc_000408F0` (after the "<8
 * bytes before buffer's physical end" refill call) and `loc_00040992`
 * (after the main "not enough room for the next record" refill call). The
 * original code computes the ring buffer's new write cursor as
 * `MEM32(ebx+0x4060) = 0x4000 - stale`, where `stale` is how many bytes
 * were left before the buffer's physical end BEFORE this refill -- an
 * implicit, hardcoded assumption that the refill always transfers exactly
 * 0x4000 bytes. It does not: the actual per-call disc read request can be
 * anywhere up to 0x20000 bytes (rounded/capped a few lines earlier in the
 * same function), and the real transferred count is already sitting right
 * there in `eax`, loaded two lines above the formula to update the
 * `+0x4064` accumulator, then needlessly discarded.
 *
 * Root-caused and confirmed with exact numbers across Updates 213-221:
 *   - Update 213: found and byte-verified the underflowing compaction
 *     memcpy this bad cursor eventually feeds (`loc_00040942`), confirmed
 *     present verbatim in the original XBE (not a recompiler bug).
 *   - Update 215: pinned the defect to this exact formula, present at both
 *     `loc_000408F0` and `loc_00040992`, structurally unable to use the
 *     real transfer count even with the kernel's own honest-short-read fix
 *     (commit e1b6823) already active.
 *   - Updates 217/218: found and fully read the actual caller/loop
 *     (`sub_00042F00`/`sub_000430A5`) driving this function; ruled out a
 *     missing terminator/tag check and an outer-loop-iteration-count
 *     mismatch as the cause via a directly refuting live capture
 *     (`outer_remaining=0` at the moment of corruption -- this is the
 *     loop's normal, final iteration, not an extra one).
 *   - Update 220: captured the full, gapless numeric chain for the actual
 *     failing case: `stale=7 -> (buggy) cursor=0x4000-7=0x3FF9 -> +8
 *     (record-header parse) -> cursor=0x4001 -> remaining_space=
 *     bufsize-cursor=0x1FFFF -> underflowing memcpy destination`.
 *   - Update 221: audited the native NtReadFile/disc-I/O layer directly
 *     (kernel_file.c, commit e1b6823's "honest short read" change) with a
 *     live capture at the exact problematic read: `requested=131072
 *     (0x20000), transferred=89969, filesize=1924977` -- a genuine,
 *     honest, correctly-reported short read at the file's true end,
 *     matching real xemu hardware's own already-established identical
 *     short read for this exact stream (want=0x20000 got=0x15F71,
 *     Updates 185/216 -- the SAME want/got pair, not "xemu gets 0x4000").
 *     This conclusively rules out the native I/O layer, the host
 *     filesystem, and our own kernel bridge as the cause: the disc I/O
 *     semantics are correct and match real hardware exactly. The defect is
 *     exclusively in this formula's own hardcoded 0x4000 assumption, which
 *     is wrong on essentially every call where the transfer size differs
 *     from 0x4000 (routinely the case, since transfers are commonly up to
 *     0x20000 bytes) -- it happens to matter here because this specific
 *     wrap coincides with the stream's genuine end of data.
 *
 * Verified live (Update 220/221): with this fix, the process no longer
 * reaches the `sub_0003FCA5` fatal-callback freeze this whole chain used
 * to cause, and progresses into new, previously-unreached territory
 * (`sub_004A12B0`/`sub_004A1600`/`sub_004A1740`, a separate, not yet
 * investigated blocker).
 *
 * Moved here (rather than staying as a hand-edit to the generated file)
 * specifically so this fix survives a full pipeline regenerate from the
 * original XBE: the auto-lifted body for these two addresses would
 * otherwise byte-for-byte reproduce the original game's own defect every
 * time, silently re-introducing this corruption with no warning. Excluded
 * from auto-generation via the existing `--exclude-manual
 * projects/scda/src/recomp_manual.c` mechanism (manual_scan.py detects
 * these two `void sub_XXXXXXXX(void)` definitions automatically).
 */
#define eax g_eax
#define ebx g_ebx
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define esi g_esi
#define edi g_edi

'''

footer = """
#undef eax
#undef ebx
#undef ecx
#undef edx
#undef esp
#undef esi
#undef edi
"""

block = header + sub_40821_fixed + "\n" + sub_4086e_fixed + footer

manual_path = "projects/scda/src/recomp_manual.c"
with open(manual_path, "r", encoding="utf-8") as f:
    manual_content = f.read()

new_manual_content = manual_content.rstrip("\n") + "\n" + block
with open(manual_path, "w", encoding="utf-8", newline="\n") as f:
    f.write(new_manual_content)

print("Appended sub_00040821 + sub_0004086E override to recomp_manual.c")
print("sub_00040821 body:", len(sub_40821_fixed.splitlines()), "lines")
print("sub_0004086E body:", len(sub_4086e_fixed.splitlines()), "lines")
