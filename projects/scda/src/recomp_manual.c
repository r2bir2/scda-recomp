#include <stdint.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "recomp_types.h"
#include "recomp_funcs.h"

/* Provided by the kernel compatibility layer. KeTickCount is a direct guest
 * data export, therefore it must be refreshed independently of thunk calls. */
extern void xbox_KernelDataTickUpdate(void);
extern void scda_ring_trace_begin(uint32_t object, uint32_t callsite,
                                  uint32_t writer, uint32_t cursor_before,
                                  uint32_t buffer_size, uint32_t remaining,
                                  uint32_t wrap, uint32_t record_tag,
                                  uint32_t record_length);
extern void scda_ring_trace_state(uint32_t object, uint32_t state,
                                  uint32_t cursor_before,
                                  uint32_t cursor_after,
                                  uint32_t remaining, uint32_t wrap,
                                  uint32_t record_tag,
                                  uint32_t record_length,
                                  uint32_t stack);
extern void scda_ring_trace_boundary(uint32_t object, uint32_t entering,
                                     uint32_t requested_or_remaining,
                                     uint32_t caller, uint32_t copied);
extern void scda_ring_trace_output(uint32_t object, uint32_t output);
extern void scda_ring_trace_callstack(uint32_t object, uint32_t stack,
                                      uint32_t output, uint32_t requested);
extern void scda_ring_trace_refillstack(uint32_t object, uint32_t callsite,
                                        uint32_t stack);
extern void scda_ring_trace_commit(uint32_t object, uint32_t writer);

/* Retained diagnostic utility.  Update 375 disproved the old assumption that
 * a page-watch hit identified the producer of the already consumed slot:
 * its 0x0049EFC0 attribution came from a stale build map.  The corresponding
 * callsite hook is intentionally compiled out in recomp_types.h until a
 * producer can be observed before PGRAPH consumes its packet. */
extern unsigned xbox_FramebufferBurstGeneration(void);
void scda_gpu_emit_call_trace(uint32_t va)
{
    static unsigned last_burst, records;
    unsigned burst = xbox_FramebufferBurstGeneration();
    uint32_t ret, word0, word1;

    if (!burst || va != 0x0049EFC0u)
        return;
    if (burst != last_burst) {
        last_burst = burst;
        records = 0;
    }
    if (records >= 64u)
        return;

    ret = MEM32(g_esp);
    word0 = g_ecx;
    word1 = g_edx;
    if (word0 != 0x0064D2B4u && word1 != 0x0064D2B4u &&
        word0 != 0xFFFFFFFEu && word1 != 0xFFFFFFFEu)
        return;
    records++;
    fprintf(stderr,
            "[SCDA-GPU-EMIT] burst=%u n=%u ret=%08X word0=%08X word1=%08X "
            "c184=%u nan=%u\\n",
            burst, records, ret, word0, word1,
            word0 == 0x0064D2B4u || word1 == 0x0064D2B4u,
            word0 == 0xFFFFFFFEu || word1 == 0xFFFFFFFEu);
    fflush(stderr);
}

/* Diagnostic utility (kept, currently unused): proves whether a raw,
 * hand-inserted call at the very top of a generated function's body is ever
 * reached, completely bypassing RECOMP_TRACE_PC/scda_trace_pc. To use,
 * temporarily insert `scda_raw_entry_mark(0xAAAAxxxxu);` as the first
 * statement in a generated function's body (revert before committing;
 * generated code is not to be hand-edited permanently) and check
 * scda_raw_mark.log or the g_scda_raw_entry_mark_count global (readable via
 * `dd scda_recomp!g_scda_raw_entry_mark_count` even if the log write
 * fails). See docs/SCDA_WORKLOG.md 2026-08-27 for how this proved
 * sub_000464EC is never entered from sub_0004721E despite being its only
 * call and despite sub_0004721E's own entry/near-exit trace firing
 * normally -- a confirmed, unexplained anomaly. */
volatile long g_scda_raw_entry_mark_count = 0;

/* Exposed (not static) so an external debugger can arm a data
 * breakpoint on this exact symbol -- fires exactly once, when the
 * hash-insert at 0x00047488 hands back the object under investigation
 * for the sub_00077049 crash (2026-09-02) -- instead of breaking on
 * every scda_trace_pc call (prohibitively invasive: it measurably
 * changes the game's own timing-sensitive code paths). See
 * docs/SCDA_WORKLOG.md for the two-stage dynamic watchpoint procedure
 * this supports. */
volatile uint32_t g_scda_watch_object = 0;
static uint32_t g_scda_46ea2_saved_esi = 0;
/* See the 0x001D589D/0x001D5A5E handlers in scda_trace_pc. */
static uint32_t g_scda_1d589d_entry_esp = 0;

/* File-open helper 0x511E7 is shared by thousands of normal boot-time
 * accesses.  Match only the two optional SCDA config names at its original
 * entry (where [ESP] is the immediate guest caller and [ESP+4] the path), so
 * the post-open control-flow comparison can be tied to the same invocation
 * in xemu rather than an unrelated helper use. */
static int scda_is_optional_ini_path(uint32_t guest_path)
{
    static const char marker[] = "splintercell4";
    uint32_t i;
    for (i = 0; i < 220u; ++i) {
        uint32_t j;
        for (j = 0; marker[j] != '\0'; ++j) {
            uint8_t c = MEM8(guest_path + i + j);
            if (c >= 'A' && c <= 'Z')
                c = (uint8_t)(c + ('a' - 'A'));
            if (c != (uint8_t)marker[j])
                break;
        }
        if (marker[j] == '\0')
            return 1;
    }
    return 0;
}

/* 2026-09-03 (continued 6): sequential event counter and bin[6] walker
 * shared by the sub_00047A76/sub_00047150/sub_00045FB0/sub_00045F56
 * teardown-chain event log, to compare xemu vs scda_recomp event-for-
 * event inside the semantic window bounded by marker A
 * (sub_000366A2 entry, 0x000366A2) and marker B (sub_001D589D entry,
 * 0x001D589D / [SCDA-BIN6-PRE-MAPSLOOP]). */
static unsigned int g_scda_teardown_event_seq = 0;
/* 2026-09-03 (continued 7): sub_00046F46 declares its OWN private local
 * "ebp" (a true C local, NOT aliased to g_ebp the way esi/esp are) --
 * g_ebp gets clobbered by every nested callee's own frame-publish, so
 * reading g_ebp at a later point in this function is unreliable. Same
 * entry-ESP reconstruction trick used earlier for sub_001D589D. */
static uint32_t g_scda_46f46_entry_esp = 0;
/* 2026-09-03 (continued 12): sub_001D5208's own true_ebp, same
 * entry-ESP reconstruction trick, for the type-lookup branch bisection
 * feeding the 0x6513A4/0x6513A8 registry-population root cause. */
static uint32_t g_scda_1d5208_entry_esp = 0;
static unsigned int g_scda_1d5208_call_seq = 0;
static uint32_t g_scda_43a2c_entry_esp = 0;
/* 2026-09-03 (continued 16): direct entry/exit instrumentation on
 * sub_0004628D itself -- the 720-byte deficit was proven present
 * immediately after the 0x1D55D7 icall returns from this exact function,
 * contradicting the expectation that its own ebp-based `leave` epilogue
 * (esp=ebp, a private C local, set once at entry) must deterministically
 * restore esp regardless of internal drift. Need ground truth for what
 * esp actually is right at its own return point. */
static uint32_t g_scda_4628d_entry_esp = 0;

static unsigned int scda_bin6_chain_len(void)
{
    const uint32_t bin6_addr = 0x0064D2B4u;
    uint32_t cur = MEM32(bin6_addr);
    unsigned int chain_len = 0;
    while (cur != 0 && chain_len < 64u) {
        chain_len++;
        cur = MEM32(cur + 8u);
    }
    return chain_len;
}

/* Ring buffer for sub_00036D4A entries (the linked-list membership check
 * that has crashed three separate times this investigation, each from a
 * different caller / different root cause). The per-run report caps used
 * elsewhere in this file exhaust long before a crash that only happens
 * after many minutes of unattended running, so this keeps only the most
 * recent SCDA_ACTORLIST_RING_SIZE calls unconditionally (cheap -- no I/O
 * per call) and main.c's crash handler dumps it right before writing the
 * minidump. See docs/SCDA_WORKLOG.md 2026-09-01. */
#define SCDA_ACTORLIST_RING_SIZE 32
static uint32_t g_actorlist_ring_ecx[SCDA_ACTORLIST_RING_SIZE];
static uint32_t g_actorlist_ring_head[SCDA_ACTORLIST_RING_SIZE];
static uint32_t g_actorlist_ring_arg[SCDA_ACTORLIST_RING_SIZE];
static uint32_t g_actorlist_ring_prev[SCDA_ACTORLIST_RING_SIZE];
static unsigned int g_actorlist_ring_index = 0;
static unsigned int g_actorlist_ring_count = 0;

/* Same idea, for every sub_000748BB construction call (the object
 * constructor at the root of this whole investigation) -- records who
 * called it (previous_pc) and what "owner" argument it received, so a
 * future crash can be traced back to exactly which caller passed a bad
 * owner. */
static uint32_t g_ctor_ring_prev[SCDA_ACTORLIST_RING_SIZE];
static uint32_t g_ctor_ring_this[SCDA_ACTORLIST_RING_SIZE];
static uint32_t g_ctor_ring_owner[SCDA_ACTORLIST_RING_SIZE];
static unsigned int g_ctor_ring_index = 0;
static unsigned int g_ctor_ring_count = 0;

/* sub_001FA1BB is one of sub_000748BB's callers, invoked inside
 * sub_00246CB4's ~312-iteration loop. Its own stack-forwarded arguments
 * (ebp+0xC/0x10/0x14/0x18/0x1C) are the actual immediate source of
 * whatever gets passed as sub_000748BB's "owner" argument on this path;
 * recording all of them (plus ecx, its own "this"-to-be) removes any
 * doubt about which forwarded slot ends up as owner. */
static uint32_t g_fa1bb_ring_ecx[SCDA_ACTORLIST_RING_SIZE];
static uint32_t g_fa1bb_ring_c[SCDA_ACTORLIST_RING_SIZE];
static uint32_t g_fa1bb_ring_10[SCDA_ACTORLIST_RING_SIZE];
static uint32_t g_fa1bb_ring_14[SCDA_ACTORLIST_RING_SIZE];
static uint32_t g_fa1bb_ring_18[SCDA_ACTORLIST_RING_SIZE];
static uint32_t g_fa1bb_ring_1c[SCDA_ACTORLIST_RING_SIZE];
static unsigned int g_fa1bb_ring_index = 0;
static unsigned int g_fa1bb_ring_count = 0;

/* Xbox kernel debug-print trap (int 0x2D; int3), fused by the lifter (see
 * try_match_xbox_dbgprint in tools/recomp/lifter.py -- that comment has the
 * full byte-pattern rationale and the crash this replaces). eax=1 is the
 * only service selector observed so far: ecx points to an in-place
 * ANSI_STRING-shaped header the caller built on its own stack --
 * {Length:WORD; MaximumLength:WORD; Buffer:PCHAR} -- describing a
 * non-NUL-terminated ANSI byte string. Forward it to the host log rather
 * than silently discarding the game's own debug output; any other
 * selector is left as a documented no-op. */
void xbox_dbgprint_service(uint32_t eax, uint32_t ecx)
{
    if (eax == 1u) {
        uint32_t length = MEM16(ecx);
        uint32_t buffer = MEM32(ecx + 4u);
        uint32_t i;
        fprintf(stderr, "[SCDA-DBGPRINT] \"");
        for (i = 0; i < length; i++)
            fputc((int)(unsigned char)MEM8(buffer + i), stderr);
        fprintf(stderr, "\"\n");
        fflush(stderr);
    }
}

void scda_dump_actorlist_ring(void)
{
    unsigned int i, n, start;
    n = g_actorlist_ring_count < SCDA_ACTORLIST_RING_SIZE ? g_actorlist_ring_count : SCDA_ACTORLIST_RING_SIZE;
    start = (g_actorlist_ring_index + SCDA_ACTORLIST_RING_SIZE - n) % SCDA_ACTORLIST_RING_SIZE;
    fprintf(stderr, "[SCDA-ACTORLIST-RING] dumping last %u sub_00036D4A entries before crash\n", n);
    for (i = 0; i < n; i++) {
        unsigned int slot = (start + i) % SCDA_ACTORLIST_RING_SIZE;
        fprintf(stderr, "[SCDA-ACTORLIST-RING] #%u prev=0x%08X ecx=0x%08X head=0x%08X arg=0x%08X\n",
                i, g_actorlist_ring_prev[slot], g_actorlist_ring_ecx[slot],
                g_actorlist_ring_head[slot], g_actorlist_ring_arg[slot]);
    }
    n = g_ctor_ring_count < SCDA_ACTORLIST_RING_SIZE ? g_ctor_ring_count : SCDA_ACTORLIST_RING_SIZE;
    start = (g_ctor_ring_index + SCDA_ACTORLIST_RING_SIZE - n) % SCDA_ACTORLIST_RING_SIZE;
    fprintf(stderr, "[SCDA-CTOR-RING] dumping last %u sub_000748BB entries before crash\n", n);
    for (i = 0; i < n; i++) {
        unsigned int slot = (start + i) % SCDA_ACTORLIST_RING_SIZE;
        fprintf(stderr, "[SCDA-CTOR-RING] #%u prev=0x%08X this=0x%08X owner=0x%08X\n",
                i, g_ctor_ring_prev[slot], g_ctor_ring_this[slot], g_ctor_ring_owner[slot]);
    }
    n = g_fa1bb_ring_count < SCDA_ACTORLIST_RING_SIZE ? g_fa1bb_ring_count : SCDA_ACTORLIST_RING_SIZE;
    start = (g_fa1bb_ring_index + SCDA_ACTORLIST_RING_SIZE - n) % SCDA_ACTORLIST_RING_SIZE;
    fprintf(stderr, "[SCDA-FA1BB-RING] dumping last %u sub_001FA1BB entries before crash\n", n);
    for (i = 0; i < n; i++) {
        unsigned int slot = (start + i) % SCDA_ACTORLIST_RING_SIZE;
        fprintf(stderr, "[SCDA-FA1BB-RING] #%u ecx=0x%08X [C]=0x%08X [10]=0x%08X [14]=0x%08X [18]=0x%08X [1C]=0x%08X\n",
                i, g_fa1bb_ring_ecx[slot], g_fa1bb_ring_c[slot], g_fa1bb_ring_10[slot],
                g_fa1bb_ring_14[slot], g_fa1bb_ring_18[slot], g_fa1bb_ring_1c[slot]);
    }
    fflush(stderr);
}

void scda_raw_entry_mark(unsigned int tag)
{
    long n = ++g_scda_raw_entry_mark_count;
    FILE *bf = NULL;
    if (fopen_s(&bf, "scda_raw_mark.log", "a") == 0 && bf) {
        fprintf(bf, "RAW-MARK #%ld tag=0x%08X\n", n, tag);
        fclose(bf);
    }
}

/* Internal zero-remainder target of the optimized memcpy jump table at
 * 0x003672A5.  Linear sweep decodes the final table byte together with this
 * seven-byte epilogue, so it cannot be emitted as a normal function entry. */
static void scda_memcpy_epilogue_003673CC(void)
{
    uint32_t ebp = g_seh_ebp;
    uint32_t discarded_ebp;

    g_eax = MEM32(ebp + 8);
    POP32(g_esp, g_esi);
    POP32(g_esp, g_edi);
    g_esp = ebp;
    POP32(g_esp, discarded_ebp);
    g_esp += 4; /* ret */
}

/* Full shared allocator epilogue.  Three branches in sub_000778BC enter at
 * 0x00077AC5 (pop edi), while two shorter exit paths enter one byte later at
 * 0x00077AC6 (pop esi).  The disassembler can represent only the suffix as a
 * separate function, so direct calls to the unresolved 0x77AC5 entry are
 * completed here without editing generated C. */
static void scda_allocator_epilogue_00077AC5(void)
{
    uint32_t ebp = g_seh_ebp;
    uint32_t discarded_ebp;

    POP32(g_esp, g_edi);
    POP32(g_esp, g_esi);
    g_esp = ebp;
    POP32(g_esp, discarded_ebp);
    g_esp += 4; /* ret */
}

/* CRT math helper epilogue - tail jump target of sub_00368348.
 * This function handles the stack restoration for floating point mathematics
 * in order to avoid guest stack loss during CRT math operations.
 */
static void scda_crt_math_epilogue_0036D415(void)
{
    uint32_t ebp = g_seh_ebp;
    
    /* The CRT math helper functions restore the x87 control word,
     * pop temporary dword, and return. The guest stack should be properly
     * managed by restoring ESP to original EBP value */
    g_esp = ebp;
    POP32(g_esp, ebp); /* Pop discarded EBP */
    g_esp += 4; /* Return address */
}

/* CRT math helper epilogue - shared CRT math body.
 */
static void scda_crt_math_epilogue_0036D2F8(void)
{
    uint32_t ebp = g_seh_ebp;
    
    /* The CRT math helper functions restore the x87 control word,
     * pop temporary dword, and return. The guest stack should be properly
     * managed by restoring ESP to original EBP value */
    g_esp = ebp;
    POP32(g_esp, ebp); /* Pop discarded EBP */
    g_esp += 4; /* Return address */
}

/* CRT math helper epilogue - tail jump target at 0x0036D3CA.
 * This function handles the same stack restoration as other CRT math helpers.
 */
static void scda_crt_math_epilogue_0036D3CA(void)
{
    uint32_t ebp = g_seh_ebp;
    
    /* Restore from EBP, pop discarded EBP, and return */
    g_esp = ebp;
    POP32(g_esp, ebp); /* Pop discarded EBP */
    g_esp += 4; /* Return address */
}

/* CRC-32 table generator continuation, called from sub_0007123E.
 *
 * sub_00071217 (0x00071217-0x0007123E per the disassembler) computes a
 * standard reflected-input, MSB-first CRC-32 table (polynomial
 * 0x04C11DB7) into 256 dwords starting at guest VA 0x0062A5D8: for each
 * table index it derives edx = index<<24, runs 8 shift/xor steps, then
 * writes edx and advances. Its own disassembled range only covers the
 * ONE-TIME setup plus the first table entry (index 0) -- the per-entry
 * reload for every remaining index (edx = index<<24; 8 more shift/xor
 * steps) starts at 0x00071223, which sub_0007123E reaches only via an
 * explicit call, not a fallthrough/jump the boundary detector could
 * follow, so it was never disassembled as part of any function and
 * fell to the unresolved-stub fallback. That stub returned immediately
 * without finishing the table or restoring ebx/esi/edi, corrupting the
 * caller's registers -- this is the confirmed root cause of
 * [SCDA-CRT-ITERATOR-CLOBBER]: the CRT static-constructor iterator's
 * EDI end-pointer (expected 0x005B1D58) got overwritten with an
 * address into this half-built table (~0x0062A5DC) instead of being
 * restored, corrupting every constructor-table entry read afterward.
 * See docs/SCDA_WORKLOG.md, 2026-09-02.
 *
 * Finish the table here (native C, not translated x86 -- the algorithm
 * is a well-known, unambiguous standard, safer than trying to recover
 * the missing bytes), then perform the same epilogue the "table
 * finished" path (loc_0007124C: pop edi, esi, ebx; consume the return
 * address) would. */
static void scda_crc32_table_finish_00071223(void)
{
    while (g_esi < 0x100u) {
        uint32_t edx = g_esi << 24;
        int bit;
        for (bit = 0; bit < 8; bit++) {
            if ((int32_t)edx >= 0)
                edx = edx << 1;
            else
                edx = (edx << 1) ^ 0x04C11DB7u;
        }
        MEM32(g_edi) = edx;
        g_esi++;
        g_edi += 4;
    }

    POP32(g_esp, g_edi);
    POP32(g_esp, g_esi);
    POP32(g_esp, g_ebx);
    g_esp += 4; /* return address */
}

/* Shared "slot occupied" cleanup + epilogue for the hash-node insert
 * helper sub_00046887/sub_000468DF/sub_00046903, reached only via the tail
 * jump inside sub_00046903 (jne 0x468BE) when the newly probed slot is
 * already occupied (edi != 0 after the sub_00067863 lookup). The
 * disassembler split this one shared-prologue/shared-epilogue routine into
 * three separate "functions" -- sub_00046887 pushes ebp/ebx/esi/edi and
 * reserves a 0x194-byte local frame, then tail-jumps into the shared
 * epilogue at sub_00046903; sub_00046903's own early branch (slot
 * occupied) tail-jumps into 0x000468BE -- but 0x468BE has no seed of its
 * own: its only inbound edge is that internal jne, invisible to
 * per-function boundary analysis, and it falls straight through into
 * sub_000468DF's own entry point at 0x468DF (right after "push eax"). It
 * fell to the generic unresolved-stub fallback, which only ever consumed
 * the dummy return address (4 bytes) instead of leaving sub_00046887's
 * still-open 0x1A4-byte local frame (push ebp; sub esp,0x194; push
 * ebx/esi/edi) for the real shared epilogue (sub_00046903's "leave; ret")
 * to unwind -- a confirmed, exact 0x1AC-byte guest ESP leak (0x1A4 frame +
 * the caller's 2 still-unpopped pushed args at the observation point) on
 * every call that takes this branch. This is the proven root cause of the
 * post-0x46F00-fix crash: the leaked ESP made a later `pop esi` read a
 * stray UTF-16 string fragment (0x006E0045) instead of the saved
 * cache-payload pointer, which propagated into sub_0004721E's return
 * value and then into sub_0004693B as a garbage ECX (Read AV). See
 * docs/SCDA_WORKLOG.md, 2026-09-02.
 *
 * Real x86 bytes (0x000468BE-0x000468DE, confirmed via standalone XBE
 * disassembly): zero the word at [edi], call sub_00045992, branch on
 * MEM32(0x5B5AC4) to call either sub_00036EC0 or sub_00036EA0, push eax --
 * then fall straight into sub_000468DF's own entry, so re-enter it
 * directly rather than reimplementing its body. */
static void scda_hashinsert_notfound_000468BE(void)
{
    MEM16(g_edi) = 0;

    PUSH32(g_esp, 0x000468C7u); sub_00045992();

    g_ecx = g_eax;
    if (MEM32(0x5B5AC4u) == 0) {
        PUSH32(g_esp, 0x000468DEu); sub_00036EC0();
    } else {
        PUSH32(g_esp, 0x000468D7u); sub_00036EA0();
    }

    PUSH32(g_esp, g_eax);

    sub_000468DF();
}

/* 2026-09-03 (continued 7): sub_00047008's 0x00047017 (je) and
 * 0x00047020 (jne) are conditional JUMPS -- not calls -- to 0x00047147,
 * a shared early-exit tail INSIDE sub_00047008's own body (real bytes,
 * confirmed via tools/disasm/output/scda_v060/asm/text.asm:
 *   0x00047147  33c0  xor eax, eax
 *   0x00047149  40    inc eax
 *   0x0004714A  5f    pop edi
 *   0x0004714B  5b    pop ebx
 *   0x0004714C  c9    leave
 *   0x0004714D  c20400 ret 4
 * ). The disassembler's function-boundary detector split this off as
 * its own unit (0x00047130-0x00047150, itself reached via a *different*
 * fall-through path), so the recompiler had no way to emit a plain
 * `goto` for the two je/jne edges and fell back to an unresolved-call
 * stub. The generic "not detected" fallback in
 * recomp_stubs_unresolved.c only performs `g_esp += 4`, replicating
 * none of the real target's frame teardown -- leaving a 0x54-byte
 * (84 = pop edi + pop ebx + leave's "esp=ebp" gain of 0x44 + pop ebp +
 * ret 4's extra 4, minus the 4 already (mis)consumed) guest-stack
 * deficit on every one of these early exits. Proven, event-by-event,
 * to be the sole cause of the previously-investigated bin[6] 39-vs-40
 * discrepancy (see docs/SCDA_WORKLOG.md, 2026-09-03 continued 7):
 * sub_00047A92's second field free (esi+0x18, inside sub_00046F46's
 * loop, inside sub_00047008's je/jne path) ends up operating on
 * garbage-near-null addresses once esi is corrupted by this esp
 * imbalance propagating up through sub_00046F46's own epilogue. */
static void scda_47008_earlyexit_00047147(void)
{
    uint32_t ebp = g_seh_ebp;
    uint32_t discarded_ebp;

    g_eax = 1;              /* xor eax, eax; inc eax */
    POP32(g_esp, g_edi);    /* pop edi -- sub_00047008's own saved edi */
    POP32(g_esp, g_ebx);    /* pop ebx -- sub_00047008's own saved ebx */
    g_esp = ebp;             /* leave (1/2): esp = ebp */
    POP32(g_esp, discarded_ebp); /* leave (2/2): pop ebp */
    g_esp += 4;              /* ret 4 (1/2): pop return address */
    g_esp += 4;              /* ret 4 (2/2): extra stack-arg cleanup */
}

/* 2026-09-03 (continued 19): same bug class as 0x00047147 above, found
 * this time via a dedicated call-level bisection of sub_001D5208's proven
 * 720-byte stack deficit (Maps-loop type-lookup registry root cause).
 * sub_000460F0's own "je 0x4612B" (real bytes, confirmed via
 * tools/disasm/output/scda_v060/asm/text.asm:
 *   0x0004612B  33c0    xor eax, eax
 *   0x0004612D  c9      leave
 *   0x0004612E  c21000  ret 0x10
 * ) jumps to a shared epilogue that is the disassembler's OWN dead code
 * embedded inside the *next* function, sub_0004611A (whose true, reachable
 * entry is a genuine fall-through, not this jump target) -- so 0x4612B was
 * never recognized as an independently callable address at all. The
 * generic "not detected" stub only does `g_esp += 4`, replicating none of
 * the real target's frame teardown: leaves the 0xA0-byte (160) local
 * buffer sub_000460F0 allocated, its saved ebp, and the 0x10 (16) bytes of
 * caller-pushed args all unreclaimed -- an exact 0xA0+4+16=0xB4 (180)
 * byte guest-stack deficit on every call that takes this branch. Proven,
 * call-by-call via g_icall_source-gated dispatch probes, to be the sole
 * cause of the 720-byte (4 * 180) deficit inside sub_001D5208's Maps-
 * loop type-lookup chain: it corrupts edi/esi/ebx (all callee-saved,
 * restored via POP at the wrong, misaligned stack offset once this
 * deficit accumulates) that sub_001D589D's loop relies on staying valid
 * across all 32 Maps-file iterations, explaining why only the very first
 * iteration's type-lookup succeeds and every subsequent one sees edi=0.
 * See docs/SCDA_WORKLOG.md. */
static void scda_460f0_earlyexit_0004612B(void)
{
    uint32_t ebp = g_seh_ebp;
    uint32_t discarded_ebp;

    g_eax = 0;                    /* xor eax, eax */
    g_esp = ebp;                  /* leave (1/2): esp = ebp */
    POP32(g_esp, discarded_ebp);  /* leave (2/2): pop ebp */
    g_esp += 4;                   /* ret 0x10 (1/2): pop return address */
    g_esp += 16;                  /* ret 0x10 (2/2): stack-arg cleanup */
}

/* 2026-09-10: sub_0003EA27 is one of a contiguous family of MSVC
 * "adjustor"/delegating-thunk labels (0x3EA10, 0x3EA17, 0x3EA1F, 0x3EA27,
 * 0x3EA2F, ...) that the disassembler correctly placed inside the span of
 * sub_0003EA10 (it IS reachable code, part of that function's own byte
 * range), but which the recompiler's dispatch table never registers as
 * independently callable -- only a function's own entry address gets a
 * dispatch-table slot, and these are internal labels, not entries. The
 * original game reaches 0x3EA27 exclusively via indirect vtable dispatch
 * (confirmed statically: vtable 0x004F61E0, slot 4 at guest VA 0x4F61F0
 * holds this exact address), never via a direct call or fallthrough --
 * so it was never going to surface as a missing *function* the way a
 * dead branch target would.
 *
 * Effect when unresolved: RECOMP_ITAIL's generic "target not found in
 * dispatch table" fallback sets eax=0 and returns. For nearly every
 * other icall site in this codebase that's an inert failure. For this
 * one call site it is not: it is reached from sub_00045ADE's resource-
 * search fallback chain (itself an ordinary, correct "not found in this
 * catalogue -- delegate to the parent catalogue" tail-chain), and in
 * that one API eax==0 means "found, at index 0", not "not found". A
 * table with zero real entries (an unpopulated resource-manager
 * instance, itself downstream of an unrelated failed config load) walks
 * this exact chain, hits this exact unresolved thunk, and the resulting
 * spurious "found" return is what makes sub_0004721E treat an empty
 * search string as a successful lookup -- which is what ultimately
 * drives sub_000464EC/sub_0007188A/sub_000778BC into requesting a
 * Partition5.img-sized allocation. Traced end-to-end and confirmed live
 * (both the icall_target=0x0003EA27 value and, before this fix, the
 * silent eax=0 fallback) -- see PROJECT_STATUS.md, Update 91.
 *
 * Original semantics (traced statically from the XBE and confirmed via
 * a live g_icall_source-gated diagnostic -- this hand-written body is a
 * disassembly-completeness fix, not new behavior; it does exactly what
 * the original three instructions do):
 *     ecx = MEM32(ecx + 4);   // this->parent (delegate up one level)
 *     eax = MEM32(ecx);       // parent's vtable
 *     jmp [eax + 0x10];       // tail-jump to parent's own slot 4
 */
/* Full family, statically re-derived byte-for-byte from
 * compare/orig/default.xbe (0x3EA10-0x3EA9F): 19 entries total, each
 * `mov ecx,[ecx+4]; mov eax,[ecx]; jmp [eax+N]` (7 bytes when N==0,
 * encoded as `jmp [eax]`; 8 bytes otherwise, encoded as `jmp [eax+disp8]`).
 * 0x3EA10 itself (N=0) is already a real disassembled function entry
 * (`void sub_0003EA10(void)` exists in the generated output and is
 * reachable through the normal dispatch table) -- only the 18 *internal*
 * labels after it need a manual override, one function pointer per
 * address since recomp_lookup_manual() dispatches by exact match. */
static void scda_thunk_03EA17(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA1Fu;
    RECOMP_ITAIL(MEM32(g_eax + 0x0Cu));
}
static void scda_thunk_03EA1F(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA27u;
    RECOMP_ITAIL(MEM32(g_eax + 0x14u));
}
static void scda_thunk_03EA27(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA2Fu;
    RECOMP_ITAIL(MEM32(g_eax + 0x10u));
}
static void scda_thunk_03EA2F(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA37u;
    RECOMP_ITAIL(MEM32(g_eax + 0x20u));
}
static void scda_thunk_03EA37(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA3Fu;
    RECOMP_ITAIL(MEM32(g_eax + 0x24u));
}
static void scda_thunk_03EA3F(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA47u;
    RECOMP_ITAIL(MEM32(g_eax + 0x28u));
}
static void scda_thunk_03EA47(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA4Fu;
    RECOMP_ITAIL(MEM32(g_eax + 0x2Cu));
}
static void scda_thunk_03EA4F(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA57u;
    RECOMP_ITAIL(MEM32(g_eax + 0x30u));
}
static void scda_thunk_03EA57(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA5Fu;
    RECOMP_ITAIL(MEM32(g_eax + 0x34u));
}
static void scda_thunk_03EA5F(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA67u;
    RECOMP_ITAIL(MEM32(g_eax + 0x38u));
}
static void scda_thunk_03EA67(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA6Fu;
    RECOMP_ITAIL(MEM32(g_eax + 0x4Cu));
}
static void scda_thunk_03EA6F(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA77u;
    RECOMP_ITAIL(MEM32(g_eax + 0x50u));
}
static void scda_thunk_03EA77(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA7Fu;
    RECOMP_ITAIL(MEM32(g_eax + 0x54u));
}
static void scda_thunk_03EA7F(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA87u;
    RECOMP_ITAIL(MEM32(g_eax + 0x58u));
}
static void scda_thunk_03EA87(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA8Fu;
    RECOMP_ITAIL(MEM32(g_eax + 0x5Cu));
}
static void scda_thunk_03EA8F(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA97u;
    RECOMP_ITAIL(MEM32(g_eax + 0x60u));
}
static void scda_thunk_03EA97(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EA9Fu;
    RECOMP_ITAIL(MEM32(g_eax + 0x64u));
}
static void scda_thunk_03EA9F(void)
{
    g_ecx = MEM32(g_ecx + 4);
    g_eax = MEM32(g_ecx);
    g_icall_source = 0x0003EAA7u;
    RECOMP_ITAIL(MEM32(g_eax + 0x68u));
}

/* 2026-09-10: sub_00074792 is a tiny 4-byte inline getter
 * (mov eax, [ecx+0x2C]; ret -- confirmed by reading the raw bytes at
 * guest VA 0x00074792 from compare/orig/default.xbe) that was never
 * disassembled as an independently callable function, the same failure
 * mode as sub_0003EA27 above: too small/isolated to have been picked up
 * as its own seed, reached only via indirect (vtable) dispatch. Found
 * immediately after fixing 0x0003EA27, via the same
 * live-icall-ring-before-crash diagnostic, as the next unresolved
 * target on the newly-reached code path (post-OOM-fix boot now gets as
 * far as loading menu_loading*.tga/menu_intro.bik/lipsyncxbox.umd
 * before this one surfaces). Not yet root-caused to a specific crash
 * effect the way 0x0003EA27 was -- added because it is a confirmed
 * unresolved target on the active path, same evidence standard as
 * above (static byte read + live icall_target confirmation), not
 * because its downstream consequence has been fully traced. See
 * PROJECT_STATUS.md, Update 91. */
static void scda_getter_00074792(void)
{
    g_eax = MEM32(g_ecx + 0x2Cu);
    g_esp += 4; /* real bytes end in a plain `ret`: this model charges 4
                 * bytes for the emulated return-address slot on every
                 * return even when (as via RECOMP_ITAIL) nothing was
                 * actually pushed for this call -- there it correctly
                 * pops the outer, tail-chained caller's own return
                 * address, exactly matching what a real ret does after
                 * a tail jmp. Missing this is the same bug class as the
                 * sub_00040A88 `ret 4` -> `esp += 4` (should have been
                 * `esp += 8`) mistake fixed in Update 94: found live via
                 * a canary that pinned esi corruption in sub_000661F1
                 * to exactly this getter's invocation through
                 * sub_000600AD's RECOMP_ICALL_SAFE call at 0x600D7 (see
                 * PROJECT_STATUS.md Update 96/97). */
}

/* 2026-09-10: same failure mode again, found immediately after fixing
 * 0x0003EA* and 0x00074792 let the boot progress further (now loading
 * menu videos/lipsync data before hitting this one). Raw bytes at guest
 * VA 0x00039E08 (compare/orig/default.xbe): 33 C0 39 41 34 0F 94 C0 C3
 * = `xor eax,eax; cmp [ecx+0x34],eax; setz al; ret` -- a small boolean
 * getter (`return this->field_0x34 == 0;`), self-contained, no internal
 * calls or branches. Two further unresolved targets seen alongside it
 * in the same icall-failure ring (0x00045738, 0x00045775) are NOT this
 * simple -- both have internal `call`/conditional-branch instructions
 * and need proper disassembly + function_seeds.json + a pipeline
 * regeneration, not a hand-written stand-in; left unfixed, see
 * PROJECT_STATUS.md Update 91 for the concrete next step. */
static void scda_getter_00039E08(void)
{
    g_eax = (MEM32(g_ecx + 0x34u) == 0u) ? 1u : 0u;
    g_esp += 4; /* same missing-`ret`-cleanup bug as scda_getter_00074792
                 * above -- see that function's comment. */
}

/* ── Diagnostics (docs Update 345): Bink sound-track starvation ──────────
 *
 * 0x004BD380 is Bink's per-track sound update (installed as vtable slot 0
 * of the track sound object at 0x004BDC39; called only indirectly). It
 * queries the DirectSound buffer cursors (sub_004BD310 ->
 * GetCurrentPosition sub_004CEE96), and sets [obj+0x5C] = 1 ("sound ran
 * dry") at 0x004BD4D7 when either the time since its last check exceeds
 * [obj+0xC4] or the free space ahead of Bink's producer reaches [obj+0xBC].
 * With RECOMP_BINKSND_TRACE=1 every call is recorded (no per-call log) into
 * a small host ring; the first 0 -> 1 transition of [obj+0x5C] dumps the
 * ring and the object. Wrapper only: it runs the generated function
 * unchanged. */
typedef struct {
    uint64_t qpc;
    uint32_t obj, phase, ret, radt;
    uint32_t play_b4, write_b8, prod_a8, size_84, chunk_88, thr_bc;
    uint32_t tlim_c4, tlast_c0, flag_5c, a0, a4, ac, dsbuf_7c;
} scda_binksnd_rec;

#define SCDA_BINKSND_RING 320u
static scda_binksnd_rec g_binksnd_ring[SCDA_BINKSND_RING];
static unsigned g_binksnd_pos;
static int g_binksnd_dumps;

static void scda_binksnd_record(uint32_t obj, uint32_t phase, uint32_t ret)
{
    scda_binksnd_rec *r = &g_binksnd_ring[g_binksnd_pos++ % SCDA_BINKSND_RING];
    r->qpc = recomp_rdtsc(); /* 733.33 MHz guest TSC */
    r->obj = obj; r->phase = phase; r->ret = ret;
    r->radt = MEM32(0x005EADF0u); /* RADTimerRead's last returned ms */
    r->play_b4 = MEM32(obj + 0xB4u); r->write_b8 = MEM32(obj + 0xB8u);
    r->prod_a8 = MEM32(obj + 0xA8u); r->size_84 = MEM32(obj + 0x84u);
    r->chunk_88 = MEM32(obj + 0x88u); r->thr_bc = MEM32(obj + 0xBCu);
    r->tlim_c4 = MEM32(obj + 0xC4u); r->tlast_c0 = MEM32(obj + 0xC0u);
    r->flag_5c = MEM32(obj + 0x5Cu); r->a0 = MEM32(obj + 0xA0u);
    r->a4 = MEM32(obj + 0xA4u); r->ac = MEM32(obj + 0xACu);
    r->dsbuf_7c = MEM32(obj + 0x7Cu);
}

static void scda_binksnd_dump(uint32_t obj)
{
    unsigned n = g_binksnd_pos < SCDA_BINKSND_RING ? g_binksnd_pos : SCDA_BINKSND_RING;
    uint64_t t0;
    unsigned i;

    t0 = g_binksnd_ring[(g_binksnd_pos - n) % SCDA_BINKSND_RING].qpc;
    fprintf(stderr, "[BINKSND] starvation obj=0x%08X after %u calls; last %u "
            "records (t=guest-TSC ms rel. to first shown; phase 0/1=update entry/exit 2=lock 3=commit)\n",
            obj, g_binksnd_pos, n);
    fprintf(stderr, "[BINKSND]  t_ms  ph obj      ret radt   play  write "
            "prod  size chunk thr  tlim tlast flag a0 a4 ac dsbuf\n");
    for (i = 0; i < n; i++) {
        const scda_binksnd_rec *r =
            &g_binksnd_ring[(g_binksnd_pos - n + i) % SCDA_BINKSND_RING];
        fprintf(stderr, "[BINKSND] %7.1f %u %08X %u %6u %5X %5X %5X %5X %4X %5X "
                "%4u %6u %u %u %u %5X %08X\n",
                (double)(r->qpc - t0) / 733333.333, r->phase,
                r->obj, r->ret, r->radt, r->play_b4, r->write_b8, r->prod_a8,
                r->size_84, r->chunk_88, r->thr_bc, r->tlim_c4, r->tlast_c0,
                r->flag_5c, r->a0, r->a4, r->ac, r->dsbuf_7c);
    }
    fprintf(stderr, "[BINKSND] object words 0x00..0xFC:");
    for (i = 0; i < 0x40; i++)
        fprintf(stderr, "%s%08X", (i % 8) ? " " : "\n[BINKSND]   ",
                MEM32(obj + i * 4u));
    fprintf(stderr, "\n");
    fflush(stderr);
}

static void scda_binksnd_update_wrapper(void)
{
    uint32_t entry_esp = g_esp;
    uint32_t obj = MEM32(entry_esp + 4u);
    uint32_t flag_before = MEM32(obj + 0x5Cu);

    scda_binksnd_record(obj, 0, 0);
    sub_004BD380();
    scda_binksnd_record(obj, 1, g_eax);
    if (!flag_before && MEM32(obj + 0x5Cu) && g_binksnd_dumps < 3) {
        g_binksnd_dumps++;
        scda_binksnd_dump(obj);
    }
    {
        /* RECOMP_BINKSND_DUMP_AT=N: also dump once at ring position N, for
         * comparing runs that never starve. */
        static long dump_at = -1;
        if (dump_at < 0) {
            const char *e = getenv("RECOMP_BINKSND_DUMP_AT");
            dump_at = e ? strtol(e, NULL, 0) : 0;
        }
        if (dump_at > 0 && g_binksnd_pos >= (unsigned)dump_at) {
            fprintf(stderr, "[BINKSND] fixed dump (RECOMP_BINKSND_DUMP_AT=%ld)\n",
                    dump_at);
            scda_binksnd_dump(obj);
            dump_at = 0;
        }
    }
}

/* Producer side: slot 1 = Lock (0x004BD530: locks [obj+0xAC] bytes at
 * [obj+0xA8], returns ptr [obj+0xCC] / len [obj+0xC8]) and slot 2 = Commit
 * (0x004BD670(obj, bytes): advances [obj+0xA8], unlocks). Phase 2 = Lock
 * exit (ret = eax, 'ac' column shows the locked length [obj+0xC8]),
 * phase 3 = Commit entry (ret = committed bytes). */
static void scda_binksnd_lock_wrapper(void)
{
    uint32_t obj = MEM32(g_esp + 4u);
    sub_004BD530();
    scda_binksnd_record(obj, 2, g_eax);
    g_binksnd_ring[(g_binksnd_pos - 1u) % SCDA_BINKSND_RING].ac =
        MEM32(obj + 0xC8u);
}

static void scda_binksnd_commit_wrapper(void)
{
    uint32_t obj = MEM32(g_esp + 4u);
    scda_binksnd_record(obj, 3, MEM32(g_esp + 8u));
    sub_004BD670();
}

/* Bink Update 432: save one 640x480 planar input at the exact entry to the
 * game's YUV->RGB converter.  This is diagnostic only, opt-in, and uses
 * CREATE_NEW so an existing capture is never overwritten.  The observed
 * Bink layout is Y (307200 bytes), V (76800), U (76800). */
static void scda_bink_yuv_entry_wrapper(void)
{
    static volatile LONG attempted;
    static volatile LONG convert_count;
    static __declspec(thread) LONGLONG convert_batch_ticks;
    static __declspec(thread) LONGLONG convert_batch_max_ticks;
    static __declspec(thread) unsigned convert_batch_count;
    const char *path = getenv("RECOMP_BINK_YUV_ENTRY_DUMP");
    int time_convert = getenv("RECOMP_BINK_CONVERT_TRACE") != NULL;
    LARGE_INTEGER convert_begin = {0}, convert_end = {0};
    const char *movie = getenv("RECOMP_BINK_YUV_MOVIE");
    int logo = movie && strcmp(movie, "logos") == 0;
    uint32_t luma_size = logo ? 230400u : 307200u;
    uint32_t chroma_size = luma_size / 4u;
    uint32_t expected_dst = logo ? 0xF49E9000u : 0xF4908000u;
    uint32_t dst = MEM32(0x006BF270u);
    uint32_t y = MEM32(0x006BF278u);
    uint32_t u = MEM32(0x006BF280u);
    uint32_t v = MEM32(0x006BF284u);
    if (path && logo && dst == expected_dst) {
        static volatile LONG layout_logs;
        if (InterlockedIncrement(&layout_logs) <= 3)
            fprintf(stderr,
                    "[BINK-LOGO-LAYOUT] dst=%08X y=%08X v=%08X u=%08X "
                    "v_minus_y=%u u_minus_v=%u\n",
                    dst, y, v, u, v - y, u - v);
    }

    if (path && dst == expected_dst && v == y + luma_size &&
        u == v + chroma_size &&
        InterlockedCompareExchange(&attempted, 1, 0) == 0) {
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            BOOL ok = WriteFile(file, (const void *)XBOX_PTR(y),
                                luma_size + 2u * chroma_size,
                                &written, NULL);
            CloseHandle(file);
            fprintf(stderr,
                    "[BINK-YUV-ENTRY] dst=%08X y=%08X v=%08X u=%08X "
                    "written=%lu ok=%d path=%s\n",
                    dst, y, v, u, (unsigned long)written, (int)ok, path);
        } else {
            fprintf(stderr, "[BINK-YUV-ENTRY] CreateFile failed err=%lu path=%s\n",
                    (unsigned long)GetLastError(), path);
        }
    }
    if (time_convert) QueryPerformanceCounter(&convert_begin);
    sub_004C6140();
    if (time_convert) {
        LONG n;
        QueryPerformanceCounter(&convert_end);
        n = InterlockedIncrement(&convert_count);
        convert_batch_ticks += convert_end.QuadPart - convert_begin.QuadPart;
        if (convert_end.QuadPart - convert_begin.QuadPart > convert_batch_max_ticks)
            convert_batch_max_ticks = convert_end.QuadPart - convert_begin.QuadPart;
        convert_batch_count++;
        if (convert_batch_count == 1000u) {
            LARGE_INTEGER hz;
            QueryPerformanceFrequency(&hz);
            fprintf(stderr,
                    "[BINK-CONVERT-BATCH] n=%ld qpc=%lld hz=%lld tid=%lu "
                    "calls=%u sum_ticks=%lld max_ticks=%lld dst=%08X y=%08X\n",
                    n, (long long)convert_end.QuadPart,
                    (long long)hz.QuadPart,
                    (unsigned long)GetCurrentThreadId(), convert_batch_count,
                    (long long)convert_batch_ticks,
                    (long long)convert_batch_max_ticks, dst, y);
            convert_batch_ticks = 0;
            convert_batch_max_ticks = 0;
            convert_batch_count = 0;
        }
    }
}

/* Update 509: bracket the complete frame decoder without changing its
 * inputs/outputs.  Two pixels in isolated corrupt blocks are sampled before
 * and after the call; all nine guest arguments are recorded to locate the
 * output Y plane even if the converter globals are not populated yet. */
static __declspec(thread) uint32_t t_bink_probe_y;
static __declspec(thread) unsigned t_bink_probe_subcall;
static void scda_bink_transform_probe(const char *name, void (*body)(void))
{
    uint32_t target = t_bink_probe_y;
    uint32_t caller = 0, before0 = 0, before1 = 0;
    int coeff_bp = 0;
    if (target) {
        caller = MEM32(g_esp);
        before0 = MEM8(target + 288u * 640u + 560u);
        before1 = MEM8(target + 368u * 640u + 192u);
        if (strcmp(name, "4C3BE0") == 0 && g_ecx && g_ecx < 0x04000000u)
            coeff_bp = (int16_t)MEM16(g_ecx + 0x60u);
    }
    body();
    if (target) {
        uint32_t after0 = MEM8(target + 288u * 640u + 560u);
        uint32_t after1 = MEM8(target + 368u * 640u + 192u);
        if (after0 != before0 || after1 != before1)
            fprintf(stderr,
                    "[BINK-TRANSFORM] helper=%s caller=%08X coeff_bp=%d "
                    "values=%u,%u->%u,%u\n",
                    name, caller, coeff_bp, before0, before1, after0, after1);
    }
}
extern void sub_004C38B0_gen(void);
extern void sub_004C3BE0_gen(void);
extern void sub_004C3F80_gen(void);
extern void sub_004C3FB0_gen(void);
extern void sub_004C3FE0_gen(void);
extern void sub_004C4370_gen(void);
extern void sub_004C4C80_gen(void);
extern void sub_004C5160_gen(void);
void sub_004C38B0(void) { scda_bink_transform_probe("4C38B0", sub_004C38B0_gen); }
void sub_004C3BE0(void) { scda_bink_transform_probe("4C3BE0", sub_004C3BE0_gen); }
void sub_004C3F80(void) { scda_bink_transform_probe("4C3F80", sub_004C3F80_gen); }
void sub_004C3FB0(void) { scda_bink_transform_probe("4C3FB0", sub_004C3FB0_gen); }
void sub_004C3FE0(void) { scda_bink_transform_probe("4C3FE0", sub_004C3FE0_gen); }
void sub_004C4370(void) { scda_bink_transform_probe("4C4370", sub_004C4370_gen); }
void sub_004C4C80(void) { scda_bink_transform_probe("4C4C80", sub_004C4C80_gen); }
void sub_004C5160(void) { scda_bink_transform_probe("4C5160", sub_004C5160_gen); }
extern void sub_004BF700_gen(void);
void sub_004BF700(void)
{
    uint32_t target = t_bink_probe_y;
    uint32_t before0 = 0, before1 = 0, caller = 0, entry_eax = 0;
    unsigned call_number = 0;
    if (target) {
        call_number = ++t_bink_probe_subcall;
        caller = MEM32(g_esp);
        entry_eax = g_eax;
        before0 = MEM8(target + 288u * 640u + 560u);
        before1 = MEM8(target + 368u * 640u + 192u);
    }
    sub_004BF700_gen();
    if (target)
        fprintf(stderr,
                "[BINK-BLOCK-DECODE] call=%u caller=%08X entry_eax=%08X "
                "samples=%u,%u->%u,%u\n", call_number, caller, entry_eax,
                before0, before1,
                MEM8(target + 288u * 640u + 560u),
                MEM8(target + 368u * 640u + 192u));
}
extern void sub_004C15D0_gen(void);
void sub_004C15D0(void)
{
    static volatile LONG total, menu;
    uint32_t args[9], caller, y, dst, plane_y, g0 = 0, g1 = 0;
    uint32_t a0 = 0, a1 = 0, b0 = 0, b1 = 0;
    LONG n, m = 0;
    int i, report;
    if (!getenv("RECOMP_BINK_DECODE_TRACE")) {
        if (getenv("RECOMP_BINK_FRAME_DECODE_TIMING")) {
            static volatile LONG decode_sequence;
            LARGE_INTEGER begin, end;
            uint32_t return_address = MEM32(g_esp);
            LONG sequence;
            QueryPerformanceCounter(&begin);
            sub_004C15D0_gen();
            QueryPerformanceCounter(&end);
            sequence = InterlockedIncrement(&decode_sequence);
            if (sequence <= 2400)
                fprintf(stderr,
                        "[BINK-DECODE-TIME] n=%ld qpc_begin=%lld qpc_end=%lld "
                        "tid=%lu caller=%08X\n",
                        sequence, (long long)begin.QuadPart,
                        (long long)end.QuadPart,
                        (unsigned long)GetCurrentThreadId(), return_address);
            return;
        }
        sub_004C15D0_gen();
        return;
    }
    n = InterlockedIncrement(&total);
    caller = MEM32(g_esp);
    for (i = 0; i < 9; ++i)
        args[i] = MEM32(g_esp + 4u + (uint32_t)i * 4u);
    dst = MEM32(0x006BF270u);
    y = MEM32(0x006BF278u);
    /* The 640x480 menu decode starts before the converter's dst global is
     * updated.  The full-plane pointer is arg1 + stride*height: this agrees
     * with the converter Y pointer for the observed 640x360 logo frames. */
    if (args[5] == 640u && args[6] == 480u)
        m = InterlockedIncrement(&menu);
    report = n <= 3 || (m > 0 && m <= 3);
    plane_y = args[1] + args[5] * args[6];
    if (report && plane_y && plane_y < 0x04000000u) {
        g0 = MEM8(plane_y + 288u * 640u + 560u);
        g1 = MEM8(plane_y + 368u * 640u + 192u);
    }
    if (report && args[0] && args[0] < 0x04000000u) {
        a0 = MEM8(args[0] + 288u * 640u + 560u);
        a1 = MEM8(args[0] + 368u * 640u + 192u);
    }
    if (report && args[1] && args[1] < 0x04000000u) {
        b0 = MEM8(args[1] + 288u * 640u + 560u);
        b1 = MEM8(args[1] + 368u * 640u + 192u);
    }
    if (report)
        fprintf(stderr,
                "[BINK-DECODE-IN] n=%ld menu=%ld caller=%08X eax=%08X edx=%08X "
                "dst=%08X y=%08X plane_y=%08X sample=%u,%u "
                "arg0_y=%u,%u arg1_y=%u,%u args=%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\n",
                n, m, caller, g_eax, g_edx, dst, y, plane_y, g0, g1,
                a0, a1, b0, b1,
                args[0], args[1], args[2], args[3], args[4],
                args[5], args[6], args[7], args[8]);
    if (m == 1) {
        t_bink_probe_y = args[0];
        t_bink_probe_subcall = 0;
    }
    sub_004C15D0_gen();
    if (m == 1)
        t_bink_probe_y = 0;
    if (args[5] == 640u && args[6] == 360u) {
        const char *logo_dump = getenv("RECOMP_BINK_LOGO_DECODE_DUMP");
        const char *frame_env = getenv("RECOMP_BINK_LOGO_DECODE_FRAME");
        long wanted_frame = frame_env ? strtol(frame_env, NULL, 10) : 1;
        if (logo_dump && n == wanted_frame) {
            HANDLE file = CreateFileA(logo_dump, GENERIC_WRITE, 0, NULL,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                BOOL ok = WriteFile(file, (const void *)XBOX_PTR(args[0]),
                                    353280u, &written, NULL);
                CloseHandle(file);
                fprintf(stderr,
                        "[BINK-LOGO-DECODE-DUMP] src=%08X bytes=%lu ok=%d path=%s\n",
                        args[0], (unsigned long)written, (int)ok, logo_dump);
            } else {
                fprintf(stderr,
                        "[BINK-LOGO-DECODE-DUMP] CreateFile failed err=%lu\n",
                        (unsigned long)GetLastError());
            }
        }
    }
    if (report) {
        dst = MEM32(0x006BF270u);
        y = MEM32(0x006BF278u);
        g0 = g1 = 0;
        if (plane_y && plane_y < 0x04000000u) {
            g0 = MEM8(plane_y + 288u * 640u + 560u);
            g1 = MEM8(plane_y + 368u * 640u + 192u);
        }
        a0 = a1 = b0 = b1 = 0;
        if (args[0] && args[0] < 0x04000000u) {
            a0 = MEM8(args[0] + 288u * 640u + 560u);
            a1 = MEM8(args[0] + 368u * 640u + 192u);
        }
        if (args[1] && args[1] < 0x04000000u) {
            b0 = MEM8(args[1] + 288u * 640u + 560u);
            b1 = MEM8(args[1] + 368u * 640u + 192u);
        }
        fprintf(stderr,
                "[BINK-DECODE-OUT] n=%ld menu=%ld eax=%08X dst=%08X y=%08X "
                "plane_y=%08X sample=%u,%u arg0_y=%u,%u arg1_y=%u,%u\n",
                n, m, g_eax, dst, y, plane_y, g0, g1,
                a0, a1, b0, b1);
    }
}

/* Diagnostic: timestamp Bink's real frame selection.  The generated body is
 * retained, and the wrapper changes no guest register or playback state. */
extern void sub_004B8CD0_gen(void);
void sub_004B8CD0(void)
{
    static int enabled = -1;
    static int pace = -1;
    static int pace_trace = -1;
    static LARGE_INTEGER clock_frequency;
    static __declspec(thread) uint32_t paced_object;
    static __declspec(thread) LONGLONG paced_at;
    static volatile LONG sequence;
    static LONG trace_limit = -1;
    static LARGE_INTEGER frequency;
    LARGE_INTEGER stamp;
    LARGE_INTEGER pace_entry = {0}, pace_exit = {0}, gen_exit = {0};
    LONGLONG deadline_tick = 0;
    int paced_this_frame = 0;
    uint32_t object = g_ecx;
    uint32_t requested_frame = g_eax;
    uint32_t caller = MEM32(g_esp);
    uint32_t before = object ? MEM32(object + 0x0Cu) : 0;

    if (pace < 0) {
        const char *override = getenv("RECOMP_BINK_PACE");
        const char *fps60 = getenv("RECOMP_60FPS");
        pace = override ? atoi(override) != 0 :
               fps60 && atoi(fps60) != 0;
        if (pace)
            QueryPerformanceFrequency(&clock_frequency);
    }
    if (pace_trace < 0)
        pace_trace = getenv("RECOMP_BINK_PACE_TRACE") != NULL;
    if (pace && caller == 0x004BB921u && object) {
        uint32_t rate_num = MEM32(object + 0x14u);
        uint32_t rate_den = MEM32(object + 0x18u);
        if (rate_den && rate_num / rate_den >= 10u &&
            rate_num / rate_den <= 120u && clock_frequency.QuadPart > 0) {
            LONGLONG period = (clock_frequency.QuadPart * rate_den +
                               rate_num / 2u) / rate_num;
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            pace_entry = now;
            paced_this_frame = 1;
            if (paced_object == object && paced_at && before > 1u) {
                LONGLONG deadline = paced_at + period;
                deadline_tick = deadline;
                while (now.QuadPart < deadline) {
                    LONGLONG remaining_ms =
                        (deadline - now.QuadPart) * 1000 / clock_frequency.QuadPart;
                    Sleep((DWORD)(remaining_ms > 1 ? remaining_ms - 1 : 0));
                    QueryPerformanceCounter(&now);
                }
            }
            pace_exit = now;
            paced_object = object;
            paced_at = now.QuadPart;
        }
    }

    if (enabled < 0) {
        enabled = getenv("RECOMP_BINK_FRAME_TRACE") != NULL;
        if (enabled)
            QueryPerformanceFrequency(&frequency);
    }
    sub_004B8CD0_gen();
    if (pace_trace && paced_this_frame && before <= 2400u) {
        QueryPerformanceCounter(&gen_exit);
        fprintf(stderr,
                "[BINK-PACE] frame=%u qpc_entry=%lld qpc_exit=%lld "
                "qpc_gen_exit=%lld deadline=%lld hz=%lld tid=%lu obj=%08X\n",
                before, (long long)pace_entry.QuadPart,
                (long long)pace_exit.QuadPart,
                (long long)gen_exit.QuadPart, (long long)deadline_tick,
                (long long)clock_frequency.QuadPart,
                (unsigned long)GetCurrentThreadId(), object);
    }
    if (enabled) {
        if (trace_limit < 0) {
            const char *limit = getenv("RECOMP_BINK_FRAME_TRACE_MAX");
            LONG requested = limit ? (LONG)atoi(limit) : 600;
            trace_limit = requested > 0 && requested <= 6000 ? requested : 600;
        }
        LONG n = InterlockedIncrement(&sequence);
        if (n <= trace_limit) {
            QueryPerformanceCounter(&stamp);
            fprintf(stderr,
                    "[BINK-FRAME] n=%ld qpc=%lld hz=%lld tid=%lu caller=%08X "
                    "obj=%08X requested=%u before=%u after=%u total=%u fps=%u/%u stats=%u/%u\n",
                    n, (long long)stamp.QuadPart, (long long)frequency.QuadPart,
                    (unsigned long)GetCurrentThreadId(), caller, object,
                    requested_frame, before,
                    object ? MEM32(object + 0x0Cu) : 0,
                    object ? MEM32(object + 0x08u) : 0,
                    object ? MEM32(object + 0x14u) : 0,
                    object ? MEM32(object + 0x18u) : 0,
                    object ? MEM32(object + 0x154u) : 0,
                    object ? MEM32(object + 0x158u) : 0);
        }
    }
}


/* Update 603: isolate the multi-second gap observed between the Bink frame
 * selection and the actual decoder call.  These wrappers leave every guest
 * function intact; their clocks/logs are enabled only for a diagnostic run.
 * Both QPC endpoints are recorded so a gap *between* wrapped calls remains
 * distinguishable from time spent inside a particular Bink stage. */
static void scda_bink_stage_timing(const char *stage, void (*original)(void))
{
    static int enabled = -1;
    LARGE_INTEGER begin, end, frequency;
    uint32_t caller, arg0;
    if (enabled < 0)
        enabled = getenv("RECOMP_BINK_STAGE_TIMING") != NULL;
    if (!enabled) {
        original();
        return;
    }
    caller = MEM32(g_esp);
    arg0 = MEM32(g_esp + 4u);
    QueryPerformanceCounter(&begin);
    original();
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    if (frequency.QuadPart > 0 &&
        (end.QuadPart - begin.QuadPart) * 1000 >= 20 * frequency.QuadPart)
        fprintf(stderr,
                "[BINK-STAGE] name=%s qpc_begin=%lld qpc_end=%lld hz=%lld "
                "tid=%lu caller=%08X arg0=%08X\n",
                stage, (long long)begin.QuadPart, (long long)end.QuadPart,
                (long long)frequency.QuadPart,
                (unsigned long)GetCurrentThreadId(), caller, arg0);
}

extern void sub_004BB930_gen(void);
void sub_004BB930(void) { scda_bink_stage_timing("goto", sub_004BB930_gen); }
extern void sub_004BB1B0_gen(void);
void sub_004BB1B0(void) { scda_bink_stage_timing("decode", sub_004BB1B0_gen); }
extern void sub_004BB020_gen(void);
void sub_004BB020(void) { scda_bink_stage_timing("track", sub_004BB020_gen); }
extern void sub_004BC1C0_gen(void);
void sub_004BC1C0(void) { scda_bink_stage_timing("callback", sub_004BC1C0_gen); }
extern void sub_004BB790_gen(void);
void sub_004BB790(void) { scda_bink_stage_timing("next", sub_004BB790_gen); }
extern void sub_004BC570_gen(void);
void sub_004BC570(void) { scda_bink_stage_timing("sync", sub_004BC570_gen); }

/* The game's video update invokes BinkDecode near its entry and BinkNext
 * near its exit.  A stall between a BINK-FRAME mark and the next decode can
 * therefore be outside Bink altogether.  Keep this boundary probe opt-in
 * and log only outliers; the original generated function still runs once. */
extern void sub_00038F96_gen(void);
void sub_00038F96(void)
{
    static int enabled = -1;
    static LARGE_INTEGER hz;
    static __declspec(thread) LONGLONG previous_exit;
    LARGE_INTEGER begin, end;
    LONGLONG outside_us = 0, inside_us;
    uint32_t owner, movie, frame, caller;
    if (enabled < 0) {
        enabled = getenv("RECOMP_BINK_UPDATE_TIMING") != NULL;
        if (enabled)
            QueryPerformanceFrequency(&hz);
    }
    if (!enabled || hz.QuadPart <= 0) {
        sub_00038F96_gen();
        return;
    }
    owner = g_ecx;
    movie = owner ? MEM32(owner + 0x18u) : 0;
    frame = movie ? MEM32(movie + 0x0Cu) : 0;
    caller = MEM32(g_esp);
    QueryPerformanceCounter(&begin);
    if (previous_exit)
        outside_us = (begin.QuadPart - previous_exit) * 1000000 / hz.QuadPart;
    sub_00038F96_gen();
    QueryPerformanceCounter(&end);
    previous_exit = end.QuadPart;
    inside_us = (end.QuadPart - begin.QuadPart) * 1000000 / hz.QuadPart;
    if (movie && (outside_us >= 100000 || inside_us >= 100000))
        fprintf(stderr,
                "[BINK-UPDATE] qpc_begin=%lld qpc_end=%lld hz=%lld "
                "tid=%lu caller=%08X owner=%08X movie=%08X frame_before=%u "
                "outside_us=%lld inside_us=%lld\n",
                (long long)begin.QuadPart, (long long)end.QuadPart,
                (long long)hz.QuadPart, (unsigned long)GetCurrentThreadId(),
                caller, owner, movie, frame,
                (long long)outside_us, (long long)inside_us);
}


/* Update 346: the menu object's vtable+0x88 lookup (0x00290428) returns 0
 * for "main" in recomp, so sub_00185C2F never reaches its BinkOpen of
 * back_menu.bik. Log the inputs it checks first: the engine singleton
 * [0x6A6638] (lazy-initialised behind [0x683568]), its +0x34 object and that
 * object's +0xCC/+0xC8 flags (sub_00035CD0), and the result. Indirect-call
 * wrapper only; the generated function runs unchanged. */
static void scda_menu_lookup_wrapper(void)
{
    static int reports;
    uint32_t self = g_ecx, esp0 = g_esp;
    uint32_t name = MEM32(esp0 + 4u);
    uint32_t eng = MEM32(0x006A6638u);
    uint32_t lvl = eng ? MEM32(eng + 0x34u) : 0;
    char nm[24];
    unsigned i;

    for (i = 0; i < sizeof(nm) - 1; i++) {
        nm[i] = name ? (char)MEM8(name + i) : 0;
        if (!nm[i]) break;
    }
    nm[i] = 0;
    sub_00290428();
    if (reports < 12) {
        reports++;
        fprintf(stderr, "[SCDA-MENULOOKUP] this=0x%08X name=\"%s\" ret=0x%08X "
                "[this+4]=0x%08X init=%u eng=0x%08X eng+34=0x%08X "
                "+CC=0x%08X +C8=0x%08X\n", (unsigned)self, nm, (unsigned)g_eax,
                (unsigned)MEM32(self + 4u), (unsigned)MEM8(0x00683568u),
                (unsigned)eng, (unsigned)lvl,
                lvl ? (unsigned)MEM32(lvl + 0xCCu) : 0u,
                lvl ? (unsigned)MEM32(lvl + 0xC8u) : 0u);
        {
            /* sub_00290A8C registers the UI screens in a loop over
             * [[this+4]+0xA8] (count [[this+4]+0xAC]); none -> no children. */
            uint32_t o4 = MEM32(self + 4u);
            fprintf(stderr, "[SCDA-MENULOOKUP]   screens: [this+4]=0x%08X +AC=%d "
                    "+A8=0x%08X\n", (unsigned)o4, o4 ? (int)MEM32(o4 + 0xACu) : -1,
                    o4 ? (unsigned)MEM32(o4 + 0xA8u) : 0u);
        }
        /* +CC == 0 takes eng->sub_0029B122(name, [this+0xC]): search
         * [eng+0x3C] (count [eng+0x40]) for an entry whose +0x1C == id. */
        if (eng) {
            uint32_t n = MEM32(eng + 0x40u), arr = MEM32(eng + 0x3Cu), k;
            fprintf(stderr, "[SCDA-MENULOOKUP]   id=[this+0xC]=0x%08X eng+40=%u "
                    "eng+3C=0x%08X entries(+1C):", (unsigned)MEM32(self + 0xCu),
                    (unsigned)n, (unsigned)arr);
            for (k = 0; k < n && k < 16 && arr; k++) {
                uint32_t e = MEM32(arr + 4u * k);
                fprintf(stderr, " %08X:%08X", (unsigned)e,
                        e ? (unsigned)MEM32(e + 0x1Cu) : 0u);
            }
            /* The matching entry's children (sub_0029AD66: circular list at
             * [e+0xC], child = [node+8], name = [child+0x40+0x14]). */
            for (k = 0; k < n && k < 16 && arr; k++) {
                uint32_t e = MEM32(arr + 4u * k), sent, node, c = 0;
                if (!e || MEM32(e + 0x1Cu) != MEM32(self + 0xCu))
                    continue;
                sent = MEM32(e + 0xCu);
                fprintf(stderr, " | entry vtable 0x%08X sentinel 0x%08X [sent]=0x%08X"
                        " | children of %08X:", (unsigned)MEM32(e), (unsigned)sent,
                        sent ? (unsigned)MEM32(sent) : 0u, (unsigned)e);
                for (node = sent ? MEM32(sent) : 0; node && node != sent && c < 40;
                     node = MEM32(node), c++) {
                    uint32_t child = MEM32(node + 8u);
                    uint32_t np = child ? MEM32(child + 0x54u) : 0;
                    char nb[24]; unsigned j;
                    for (j = 0; j < sizeof(nb) - 1 && np; j++) {
                        nb[j] = (char)MEM8(np + j);
                        if (!nb[j]) break;
                    }
                    nb[np ? j : 0] = 0;
                    fprintf(stderr, " [%s]", nb);
                }
            }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
}

static int scda_binksnd_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_BINKSND_TRACE");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on;
}

/* Update 347: viewport tick sub_001C97B3 (thiscall, float DeltaTime) reads
 * the pads via vtable+0x98 only while [this+0x6C] == 0 and DeltaTime !=
 * -1.0; the engine tick clears the flag every frame at 0x001D2F9B. Xemu
 * calls it twice per frame (dt -1.0 from 0x00272256, real dt from
 * 0x00272264) and reads the pads ~10x/s; recomp read them twice. Sparse log:
 * calls 1, 2, 4, 8, ... Indirect-call wrapper only. */
static void scda_viewport_tick_wrapper(void)
{
    static uint32_t calls;
    uint32_t self = g_ecx;
    uint32_t ret = MEM32(g_esp), dtbits = MEM32(g_esp + 4u);
    uint32_t flag = MEM32(self + 0x6Cu);
    float dt;

    memcpy(&dt, &dtbits, 4);
    sub_001C97B3();
    if ((++calls & (calls - 1)) == 0)
        fprintf(stderr, "[SCDA-VPTICK] call %u this=0x%08X flag %u->%u dt=%f ret=0x%08X\n",
                calls, self, flag, MEM32(self + 0x6Cu), dt, ret);
}

/* Update 390: arm twitch. sub_002EF25E(matrices, bones) is the skinning
 * palette upload (c131 = 768, then 3*bones constants from c132 through
 * D3D SetVertexShaderConstant sub_0049E900); it is reached only through a
 * render-device vtable (slot 0x569BFC). After F11, log caller, palette
 * address and bone 0/5 translations of the next 800 uploads so the
 * alternating bone-5 pose can be tied to a guest source buffer and caller.
 * RECOMP_BONE_TRACE=1, indirect-call wrapper only, observational. */
extern unsigned xbox_FramebufferBurstGeneration(void);
extern void xbox_DebugWatchGuestRange(uint32_t lo, uint32_t len);

static void scda_bone_upload_wrapper(void)
{
    static uint32_t logged;
    uint32_t ret = MEM32(g_esp), mats = MEM32(g_esp + 4u);
    uint32_t bones = MEM32(g_esp + 8u), self = g_ecx;

    if (xbox_FramebufferBurstGeneration() && logged < 800 && bones >= 6) {
        logged++;
        fprintf(stderr, "[SCDA-BONE] n=%u ret=0x%08X this=0x%08X "
                "mats=0x%08X bones=%u b0t=%08X,%08X,%08X b5r0=%08X,%08X,%08X,%08X "
                "b5t=%08X,%08X,%08X\n", logged, ret, self, mats, bones,
                MEM32(mats + 12u), MEM32(mats + 28u), MEM32(mats + 44u),
                MEM32(mats + 240u), MEM32(mats + 244u), MEM32(mats + 248u),
                MEM32(mats + 252u), MEM32(mats + 252u), MEM32(mats + 268u),
                MEM32(mats + 284u));
    }
    sub_002EF25E();
}

/* Update 391: sub_00217912 is the skeletal-mesh instance render (ecx =
 * instance, vtable-only). sub_002111D1 composes the instance's animated
 * bone coords -- FCoords[count] at [inst+0x170], count at [inst+0x174] --
 * with the reference pose into the palette source. After F11, log every
 * bone origin (relative to bone 0) of each instance with >= 20 bones after
 * the render, so the alternating skeleton bone can be named.
 * RECOMP_BONE_TRACE=1, indirect-call wrapper only, observational. */
static size_t scda_dump_hex(char *out, uint32_t va, uint32_t bytes);

static void scda_mesh_render_wrapper(void)
{
    static uint32_t logged;
    uint32_t inst = g_ecx;
    /* Update 400: render args. arg1 = [ebp+0x64] is a pointer whose first
     * dword is the owning actor (sub_00217912 tests [actor+0x7C] & 0x40). */
    uint32_t arg1 = MEM32(g_esp + 4u), arg2 = MEM32(g_esp + 8u);

    sub_00217912();
    if (xbox_FramebufferBurstGeneration() && getenv("RECOMP_ACTOR_DUMP")) {
        static uint32_t dumped61, dumped64;
        static char line[0x800 * 2 + 0x3B0 * 2 + 256];
        uint32_t bones = MEM32(inst + 0x174u);
        uint32_t *count = bones == 61u ? &dumped61 : bones == 64u ? &dumped64 : NULL;
        if (count && *count < 40u && arg1) {
            uint32_t actor = MEM32(arg1);
            size_t n;
            (*count)++;
            n = (size_t)snprintf(line, sizeof(line),
                                 "[SCDA-ACTOR] bones=%u inst=0x%08X arg1=0x%08X "
                                 "arg2=0x%08X actor=0x%08X I=", bones, inst, arg1,
                                 arg2, actor);
            n += scda_dump_hex(line + n, inst, 0x3B0u);
            n += (size_t)snprintf(line + n, sizeof(line) - n, " ACT=");
            if (actor >= 0x00100000u && actor < 0x04000000u)
                n += scda_dump_hex(line + n, actor, 0x800u);
            line[n++] = '\n';
            fwrite(line, 1, n, stderr);
        }
    }
    if (xbox_FramebufferBurstGeneration() && getenv("RECOMP_BONE_WATCH23")) {
        /* Update 392: Sam's skeleton has 61 bones; bones 23 and 37 (the two
         * upper arms) rotate back and forth. Watch who writes bone 23's axes. */
        static int armed;
        uint32_t base = MEM32(inst + 0x170u);
        if (!armed && base && MEM32(inst + 0x174u) == 61u) {
            uint32_t lo = base + 23u * 48u + 12u, len = 36u;
            if ((lo ^ (lo + len - 1u)) & ~0xFFFu)
                len = 0x1000u - (lo & 0xFFFu);
            armed = 1;
            fprintf(stderr, "[SCDA-POSE] watch inst=0x%08X base=0x%08X bone 23\n",
                    inst, base);
            xbox_DebugWatchGuestRange(lo, len);
        }
        return;
    }
    if (xbox_FramebufferBurstGeneration() && logged < 2500) {
        uint32_t base = MEM32(inst + 0x170u), count = MEM32(inst + 0x174u);
        if (base && count >= 20 && count <= 128) {
            float o0[3], o[3];
            uint32_t i, k, bits;
            logged++;
            for (k = 0; k < 3; k++) {
                bits = MEM32(base + 4u * k);
                memcpy(&o0[k], &bits, 4);
            }
            fprintf(stderr, "[SCDA-POSE] n=%u inst=0x%08X bones=%u o0=%.3f,%.3f,%.3f d=",
                    logged, inst, count, o0[0], o0[1], o0[2]);
            for (i = 1; i < count; i++) {
                for (k = 0; k < 3; k++) {
                    bits = MEM32(base + i * 48u + 4u * k);
                    memcpy(&o[k], &bits, 4);
                }
                fprintf(stderr, "%.2f,%.2f,%.2f;", o[0] - o0[0], o[1] - o0[1],
                        o[2] - o0[2]);
            }
            fputc('\n', stderr);
        }
    }
}

/* Update 393: the bone-23 writer is sub_002052DB, called from the 12 KB
 * animation evaluation sub_0021B0DD (vtable[0x124] of the skeletal mesh
 * instance), which sub_0021AF22 (vtable-only, ecx = instance) invokes.
 * After F11, for the 61-bone instance (Sam), dump the instance's first
 * 0x400 bytes plus the first 0x200 bytes of up to 12 TArray-like fields
 * (heap pointer followed by a count 1..63) after every evaluation, so the
 * field that flips in step with the upper-arm alternation can be found
 * offline. RECOMP_ANIM_DUMP=1, indirect-call wrapper only, observational. */
static size_t scda_dump_hex(char *out, uint32_t va, uint32_t bytes)
{
    static const char hex[] = "0123456789ABCDEF";
    uint32_t i;
    size_t n = 0;
    for (i = 0; i < bytes; i += 4) {
        uint32_t v = MEM32(va + i);
        int s;
        for (s = 28; s >= 0; s -= 4)
            out[n++] = hex[(v >> s) & 15u];
    }
    return n;
}

static void scda_anim_eval_wrapper(void)
{
    static uint32_t logged;
    static char line[0x400 * 2 + 12 * (0x200 * 2 + 8) + 256];
    uint32_t inst = g_ecx;

    sub_0021AF22();
    /* Update 404: the script bone-controller TArray [inst+0xA8] (elements
     * of 0x9C, written by sub_00208069 SetBoneRotation / sub_002076BE
     * SetBoneLocation) has 13 entries in recomp vs 11 in xemu during
     * speech. RECOMP_BONECTL_DUMP=1: after F11, one compact line per
     * evaluation of the 61-bone instance with every entry decoded. */
    if (xbox_FramebufferBurstGeneration() && getenv("RECOMP_BONECTL_DUMP") &&
        MEM32(inst + 0x174u) == 61u) {
        static uint32_t lines;
        static char out[64 + 256 * 200];
        if (lines < 1500u) {
            uint32_t arr = MEM32(inst + 0xA8u), cnt = MEM32(inst + 0xACu), i;
            size_t n;
            lines++;
            n = (size_t)snprintf(out, sizeof(out), "[SCDA-BONECTL] n=%u i218=%X cnt=%u",
                                 lines, (unsigned)MEM32(inst + 0x218u), cnt);
            for (i = 0; i < cnt && i < 64u; i++) {
                uint32_t e = arr + i * 0x9Cu, k;
                n += (size_t)snprintf(out + n, sizeof(out) - n, " |%u:%u,%u,%u,%08X",
                                      i, MEM8(e), MEM8(e + 1u), MEM8(e + 2u),
                                      (unsigned)MEM32(e + 8u));
                for (k = 0x0Cu; k < 0x3Cu; k += 4u)
                    n += (size_t)snprintf(out + n, sizeof(out) - n, ",%08X",
                                          (unsigned)MEM32(e + k));
            }
            out[n++] = '\n';
            fwrite(out, 1, n, stderr);
        }
    }
    if (xbox_FramebufferBurstGeneration() && logged < 700 &&
        MEM32(inst + 0x174u) == 61u) {
        uint32_t o, arrays = 0;
        size_t n;
        logged++;
        n = (size_t)snprintf(line, sizeof(line), "[SCDA-ANIM] n=%u inst=0x%08X I=",
                             logged, inst);
        n += scda_dump_hex(line + n, inst, 0x400u);
        for (o = 0; o + 8u <= 0x400u && arrays < 12; o += 4u) {
            uint32_t p = MEM32(inst + o), c = MEM32(inst + o + 4u);
            if (p >= 0x00100000u && p < 0x04000000u && !(p & 3u) &&
                c >= 1u && c < 64u) {
                arrays++;
                n += (size_t)snprintf(line + n, sizeof(line) - n, " A%03X=",
                                      (unsigned)o);
                n += scda_dump_hex(line + n, p, 0x200u);
            }
        }
        line[n++] = '\n';
        fwrite(line, 1, n, stderr);
    }
}

/* Update 347: after START the loading path stops a Bink sound track through
 * sub_004BD850(track, 0): IDirectSoundStream_Flush, then
 * `do GetStatus(&s) while (s)`. GetStatus (sub_004D5DA9) only reads the
 * voice flag word [voice+0x12] (voice = [[track+0x7C] - 0x1C + 0x20]); the
 * loop never ends in recomp. Log the flag word on entry and every 2 s while
 * the call has not returned. Indirect-call wrapper only. */
static volatile uint32_t g_scda_bsoff_voice;
static volatile LONG g_scda_bsoff_inside;

static DWORD WINAPI scda_bsoff_watch(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(2000);
        if (g_scda_bsoff_inside && g_scda_bsoff_voice) {
            uint32_t v = g_scda_bsoff_voice;
            uint32_t v80 = MEM32(v + 0x80u);
            /* The idle-voice handler sub_004D25F8(h) acts only if
             * [apu+0x84] == 0, !(hw voice h CFG_FMT & 0x800000),
             * [apu+h*4+0x88] is this voice (its handle == h) and its list
             * at +0x4C is not empty. */
            uint32_t apu = MEM32(v + 8u);
            uint32_t h = MEM16(v + 0xAu + 2u * MEM8(v + 0x64u));
            uint32_t vmem = MEM32(0x004EC338u);
            fprintf(stderr, "[SCDA-BSOFF] still inside: voice=0x%08X flags=0x%04X "
                    "+80=0x%08X [+80]+8=0x%08X | handle=%u apu=0x%08X busy=%u "
                    "hwfmt=0x%08X map=0x%08X list=%s\n", v, MEM16(v + 0x12u), v80,
                    v80 ? MEM32(v80 + 8u) : 0u, h, apu,
                    apu ? MEM32(apu + 0x84u) : 0u,
                    vmem ? MEM32(vmem + h * 128u + 4u) : 0u,
                    apu ? MEM32(apu + h * 4u + 0x88u) : 0u,
                    MEM32(v + 0x4Cu) == v + 0x4Cu ? "empty" : "non-empty");
        }
    }
}

static void scda_binksnd_onoff_wrapper(void)
{
    static int reports;
    static LONG watch_started;
    uint32_t track = MEM32(g_esp + 4u), on = MEM32(g_esp + 8u);
    uint32_t p = MEM32(track + 0x7Cu);
    uint32_t stream = p ? p - 0x1Cu : 0, voice = stream ? MEM32(stream + 0x20u) : 0;

    if (!on && reports < 16) {
        reports++;
        fprintf(stderr, "[SCDA-BSOFF] stop track=0x%08X +64=%u stream=0x%08X voice=0x%08X "
                "flags=0x%04X\n", track, MEM32(track + 0x64u), stream, voice,
                voice ? MEM16(voice + 0x12u) : 0u);
        if (!InterlockedExchange(&watch_started, 1))
            CloseHandle(CreateThread(NULL, 0, scda_bsoff_watch, NULL, 0, NULL));
    }
    g_scda_bsoff_voice = voice;
    InterlockedIncrement(&g_scda_bsoff_inside);
    sub_004BD850();
    InterlockedDecrement(&g_scda_bsoff_inside);
    if (!on && reports <= 16)
        fprintf(stderr, "[SCDA-BSOFF] returned, flags=0x%04X\n",
                voice ? MEM16(voice + 0x12u) : 0u);
}

/* Update 348: creating a profile ends in ProfileErrorCreateMsg ("not enough
 * free blocks to save a profile") although profile.sav is written. The
 * profile save sub_002947EC fails if the save manager ([0x5B5804], vtable
 * 0x4F6420) reports too little space (vtable+0x6C = sub_0003F382:
 * GetDiskFreeSpaceEx("U:\") vs. the needed bytes) or cannot create the
 * container (vtable+0x64 = sub_000426C7). RECOMP_SAVE_TRACE=1 logs both;
 * indirect-call wrappers only. */
static int scda_save_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_SAVE_TRACE");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on;
}

static void scda_save_hasfree_wrapper(void)
{
    uint32_t need_lo = MEM32(g_esp + 4u), need_hi = MEM32(g_esp + 8u);
    uint32_t out = MEM32(g_esp + 12u), ret = MEM32(g_esp);

    sub_0003F382();
    fprintf(stderr, "[SCDA-SAVE] HasFreeSpace need=0x%08X%08X -> %u free=0x%08X%08X ret=0x%08X\n",
            need_hi, need_lo, g_eax, out ? MEM32(out + 4u) : 0u, out ? MEM32(out) : 0u, ret);
}

static void scda_save_create_wrapper(void)
{
    uint32_t a0 = MEM32(g_esp + 4u), a1 = MEM32(g_esp + 8u), ret = MEM32(g_esp);

    sub_000426C7();
    fprintf(stderr, "[SCDA-SAVE] CreateContainer(0x%08X, 0x%08X) -> 0x%08X ret=0x%08X\n",
            a0, a1, g_eax, ret);
}

/* vtable+0x50 = sub_0003F514(path, roam): signs a written save file
 * (XCalculateSignature over its bytes, signature appended) and returns
 * size_before + sig_size == GetFileSize(path) (vtable+0x10 = sub_00040FAC). */
static void scda_save_path(uint32_t va, char *out, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n && va; i++) {
        out[i] = (char)MEM8(va + i);
        if (!out[i]) break;
    }
    out[va ? i : 0] = 0;
}

static void scda_save_sign_wrapper(void)
{
    uint32_t path = MEM32(g_esp + 4u), roam = MEM32(g_esp + 8u), ret = MEM32(g_esp);
    char p[96];

    scda_save_path(path, p, sizeof p);
    sub_0003F514();
    fprintf(stderr, "[SCDA-SAVE] SignFile(\"%s\", %u) -> %u ret=0x%08X\n", p, roam, g_eax, ret);
}

static void scda_save_size_wrapper(void)
{
    uint32_t path = MEM32(g_esp + 4u), ret = MEM32(g_esp);
    char p[96];

    scda_save_path(path, p, sizeof p);
    sub_00040FAC();
    fprintf(stderr, "[SCDA-SAVE] GetFileSize(\"%s\") -> %d (0x%08X) ret=0x%08X\n", p,
            (int)g_eax, g_eax, ret);
}

/* Update 349: the first mission load crashes in TArray::Remove
 * (sub_001C445D) called from sub_00112276(index, flag), which removes entry
 * `index` from five parallel arrays of one object (a sound/music fade
 * manager): +0x60 (byte), +0x6C (byte), +0x78 (float), +0x84 (byte),
 * +0x90 (float); Num at +4 of each. memmove got (Num - index - 1) * 4 = -4,
 * so one array is a slot shorter than the others. Every add/insert
 * (sub_00112425) and remove touches all five, so log the first time their
 * counts disagree and who did it. Wrappers around the generated bodies. */
extern void sub_00112276_gen(void);
extern void sub_00112425_gen(void);

#define SCDA_FADE_RING_SIZE 64u

typedef struct scda_fade_ring_record {
    LONG sequence;
    uint32_t operation;
    uint32_t thread_id;
    uint32_t self;
    uint32_t index;
    uint32_t ret;
    uint32_t counts[5];
} scda_fade_ring_record;

static scda_fade_ring_record g_scda_fade_ring[SCDA_FADE_RING_SIZE];
static volatile LONG g_scda_fade_ring_sequence;

static void scda_fade_ring_record_event(uint32_t operation, uint32_t self,
                                        uint32_t index, uint32_t ret,
                                        const uint32_t counts[5])
{
    LONG sequence = InterlockedIncrement(&g_scda_fade_ring_sequence);
    scda_fade_ring_record *record =
        &g_scda_fade_ring[((uint32_t)sequence - 1u) % SCDA_FADE_RING_SIZE];

    /* Publish sequence last.  A crash can interrupt this write, in which case
     * the dumper ignores the incomplete slot instead of printing mixed data. */
    record->sequence = 0;
    record->operation = operation;
    record->thread_id = GetCurrentThreadId();
    record->self = self;
    record->index = index;
    record->ret = ret;
    memcpy(record->counts, counts, sizeof record->counts);
    MemoryBarrier();
    InterlockedExchange(&record->sequence, sequence);
}

void scda_dump_fade_ring(void)
{
    LONG newest = g_scda_fade_ring_sequence;
    LONG first = newest > (LONG)SCDA_FADE_RING_SIZE
        ? newest - (LONG)SCDA_FADE_RING_SIZE + 1 : 1;
    LONG sequence;

    fprintf(stderr, "[SCDA-FADE-RING] newest=%ld entries=%ld\n", newest,
            newest >= first ? newest - first + 1 : 0);
    for (sequence = first; sequence <= newest; sequence++) {
        scda_fade_ring_record *record =
            &g_scda_fade_ring[((uint32_t)sequence - 1u) % SCDA_FADE_RING_SIZE];
        if (record->sequence != sequence)
            continue;
        fprintf(stderr,
                "[SCDA-FADE-RING] #%ld op=%c tid=%u this=0x%08X index=%u "
                "counts=%u/%u/%u/%u/%u ret=0x%08X\n",
                sequence, (int)record->operation, record->thread_id,
                record->self, record->index, record->counts[0],
                record->counts[1], record->counts[2], record->counts[3],
                record->counts[4], record->ret);
    }
    fflush(stderr);
}

static int scda_fade_arrays_bad(uint32_t self, uint32_t n[5])
{
    static const uint32_t off[5] = { 0x64u, 0x70u, 0x7Cu, 0x88u, 0x94u };
    int i, bad = 0;
    for (i = 0; i < 5; i++) {
        n[i] = MEM32(self + off[i]);
        if (n[i] != n[0]) bad = 1;
    }
    return bad;
}

void sub_00112276(void)
{
    static int reports;
    uint32_t self = g_ecx, index = MEM32(g_esp + 4u), ret = MEM32(g_esp);
    uint32_t n[5], after[5];

    if ((scda_fade_arrays_bad(self, n) || index >= n[4]) && reports < 20) {
        reports++;
        fprintf(stderr, "[SCDA-FADE] remove this=0x%08X index=%u counts 60:%u 6C:%u 78:%u "
                "84:%u 90:%u ret=0x%08X\n", self, index, n[0], n[1], n[2], n[3], n[4], ret);
    }
    scda_fade_ring_record_event('R', self, index, ret, n);
    sub_00112276_gen();
    (void)scda_fade_arrays_bad(self, after);
    scda_fade_ring_record_event('r', self, index, ret, after);
}

void sub_00112425(void)
{
    static int reports;
    uint32_t self = g_ecx, ret = MEM32(g_esp);
    uint32_t before[5], after[5];
    int was_bad = scda_fade_arrays_bad(self, before);

    scda_fade_ring_record_event('A', self, UINT32_MAX, ret, before);
    sub_00112425_gen();
    (void)scda_fade_arrays_bad(self, after);
    scda_fade_ring_record_event('a', self, UINT32_MAX, ret, after);
    if (!was_bad && scda_fade_arrays_bad(self, after) && reports < 20) {
        reports++;
        fprintf(stderr, "[SCDA-FADE] add broke counts this=0x%08X before %u/%u/%u/%u/%u "
                "after %u/%u/%u/%u/%u ret=0x%08X\n", self, before[0], before[1], before[2],
                before[3], before[4], after[0], after[1], after[2], after[3], after[4], ret);
    }
}

/* The fade manager's per-frame update (sub_00112A5B, the crashing loop):
 * report the first frame that starts with disagreeing counts. */
extern void sub_00112A5B_gen(void);

void sub_00112A5B(void)
{
    static int reports;
    static LONG first_entry;
    static uint32_t last_good[5], last_self;
    uint32_t self = g_ecx, n[5];
    int bad = scda_fade_arrays_bad(self, n);

    if (InterlockedCompareExchange(&first_entry, 1, 0) == 0) {
        fprintf(stderr, "[SCDA-FADE] tick wrapper active this=0x%08X "
                "counts=%u/%u/%u/%u/%u\n", self, n[0], n[1], n[2], n[3], n[4]);
    }
    if (bad) {
        if (reports < 10) {
            reports++;
            fprintf(stderr, "[SCDA-FADE] tick finds bad counts this=0x%08X %u/%u/%u/%u/%u "
                    "(last good 0x%08X %u/%u/%u/%u/%u) data 60:0x%08X 90:0x%08X\n",
                    self, n[0], n[1], n[2], n[3], n[4], last_self, last_good[0],
                    last_good[1], last_good[2], last_good[3], last_good[4],
                    MEM32(self + 0x60u), MEM32(self + 0x90u));
        }
    } else {
        memcpy(last_good, n, sizeof n);
        last_self = self;
    }
    sub_00112A5B_gen();
}

/* Update 435: the mission music (mus_01_iceland.SS2) is looked up only on
 * the Z: cache and never copied there, while the ambience streams are.
 * sub_0003815F(this=ecx, a=[esp+4], b=[esp+8]) builds "d" + b + a, opens it
 * on the disc, and appends "+ %09d z%s" (size, path) to the cache list
 * (count at [this+0x23C]). Log each call with both strings and the list
 * count before/after, so the files the title decides to cache can be read.
 * RECOMP_CACHE_TRACE=1, indirect-call wrapper only, observational. */
static void scda_cache_add_wrapper(void)
{
    char a[128], b[128];
    uint32_t self = g_ecx, pa = MEM32(g_esp + 4u), pb = MEM32(g_esp + 8u);
    uint32_t before = MEM32(self + 0x23Cu);
    int i;
    for (i = 0; i < 127 && pa && MEM8(pa + (uint32_t)i); i++)
        a[i] = (char)MEM8(pa + (uint32_t)i);
    a[i] = 0;
    for (i = 0; i < 127 && pb && MEM8(pb + (uint32_t)i); i++)
        b[i] = (char)MEM8(pb + (uint32_t)i);
    b[i] = 0;
    sub_0003815F();
    fprintf(stderr, "[SCDA-CACHE] add a=\"%s\" b=\"%s\" count %u->%u\n", a, b,
            before, MEM32(self + 0x23Cu));
}

/* Update 442: the action music opens z:\...\mus_01_iceland.SS2 in the
 * recomp but z:\...\STREAM.SS2 in xemu. The name is data-driven:
 * sub_0001AD33(desc=[esp+4], out=[esp+8], ?, base=[esp+0x10]) opens
 * base + [desc+0x4C] through sub_00024AEE when [desc+0x20] & 8. Log the
 * return address, the resulting name and the raw descriptor (0x50 bytes),
 * so the same breakpoint in xemu (tools/debug/xemu_stream_desc.py) can be
 * compared. RECOMP_CACHE_TRACE=1, indirect-call wrapper, observational. */
static void scda_stream_setup_wrapper(void)
{
    uint32_t ret = MEM32(g_esp), desc = MEM32(g_esp + 4u);
    uint32_t base = MEM32(g_esp + 0x10u);
    char name[128], line[640];
    int i, len;
    uint32_t np = (desc && (MEM8(desc + 0x20u) & 8)) ? MEM32(desc + 0x4Cu) + base : 0;
    for (i = 0; i < 127 && np && MEM8(np + (uint32_t)i); i++)
        name[i] = (char)MEM8(np + (uint32_t)i);
    name[i] = 0;
    len = snprintf(line, sizeof(line),
                   "[SCDA-STREAM] ret=0x%08X desc=0x%08X base=0x%08X name=\"%s\" d=",
                   ret, desc, base, name);
    for (i = 0; i < 0x50 && desc && len < (int)sizeof(line) - 10; i += 4)
        len += snprintf(line + len, sizeof(line) - len, "%08X",
                        MEM32(desc + (uint32_t)i));
    len += snprintf(line + len, sizeof(line) - len, "\n");
    fwrite(line, 1, (size_t)len, stderr);
    sub_0001AD33();
}

/* Update 449: the in-level music is mixed in software. The mixer thread
 * (sub_00018CF7) polls the play cursor of its output buffer every 10 ms and,
 * per packet, sub_0001900A calls the source object ([mixer+0x48], vtable
 * +0x38, icall 0x0001904D) with (&len, &out, mixer). Return codes 3/5/7/8
 * make the mixer write silence. In the menu the music plays, in-level it is
 * silent and the SS2 file is never read again after the first fill.
 * RECOMP_MIXER_TRACE=1 logs, every ~3 s, the call count and return-code
 * histogram of that source call, and once per new (icall source, target)
 * pair every indirect call made while inside it, so the menu and in-level
 * decode paths can be compared. Observational only. */
static int scda_mixer_trace_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_MIXER_TRACE") != NULL;
    return on;
}

static RECOMP_TLS uint32_t t_mixer_target;
static RECOMP_TLS int t_mixer_depth;
static uint32_t g_mixer_pairs[512][2];
static int g_mixer_npairs;

static void scda_mixer_note_pair(uint32_t src, uint32_t target)
{
    int i;
    for (i = 0; i < g_mixer_npairs; i++)
        if (g_mixer_pairs[i][0] == src && g_mixer_pairs[i][1] == target)
            return;
    if (g_mixer_npairs >= 512)
        return;
    g_mixer_pairs[g_mixer_npairs][0] = src;
    g_mixer_pairs[g_mixer_npairs][1] = target;
    g_mixer_npairs++;
    fprintf(stderr, "[MIXER-ICALL] tick=%lu src=0x%08X target=0x%08X\n",
            (unsigned long)GetTickCount(), src, target);
}

static void scda_mixer_source_wrapper(void)
{
    static uint32_t calls, rc_hist[10], last_tick, last_target, last_obj;
    static uint64_t out_sum;
    uint32_t target = t_mixer_target, obj = g_ecx;
    uint32_t plen = MEM32(g_esp + 4u), pout = MEM32(g_esp + 8u);
    uint32_t mixer = MEM32(g_esp + 12u), len = plen ? MEM32(plen) : 0;
    uint32_t rc, out, now;
    recomp_func_t fn = recomp_lookup(target);
    if (!fn)
        fn = recomp_lookup_kernel(target);
    /* Clear the source so a later lookup on this thread that does not come
     * from a fresh icall (DPC/APC dispatch) is not mistaken for this one. */
    g_icall_source = 0;
    if (!fn) {
        fprintf(stderr, "[MIXER] no function for target 0x%08X\n", target);
        return;
    }
    t_mixer_depth++;
    fn();
    t_mixer_depth--;
    rc = g_eax;
    out = pout ? MEM32(pout) : 0;
    calls++;
    rc_hist[rc < 9u ? rc : 9u]++;
    out_sum += out;
    now = GetTickCount();
    if (target != last_target || obj != last_obj) {
        fprintf(stderr, "[MIXER] source changed obj=0x%08X vtbl=0x%08X target=0x%08X "
                "mixer=0x%08X len=%u rc=%u out=%u\n", obj, MEM32(obj), target,
                mixer, len, rc, out);
        last_target = target;
        last_obj = obj;
    }
    if (now - last_tick >= 3000u) {
        fprintf(stderr, "[MIXER] tick=%lu calls=%u rc0..9=%u/%u/%u/%u/%u/%u/%u/%u/%u/%u "
                "out_sum=%llu len=%u wpos=0x%X size=0x%X pkt=0x%X\n",
                (unsigned long)now, calls, rc_hist[0], rc_hist[1], rc_hist[2],
                rc_hist[3], rc_hist[4], rc_hist[5], rc_hist[6], rc_hist[7],
                rc_hist[8], rc_hist[9], (unsigned long long)out_sum, len,
                MEM32(mixer + 0x40u), MEM32(mixer + 0x3Cu), MEM32(mixer + 0x44u));
        calls = 0;
        memset(rc_hist, 0, sizeof rc_hist);
        out_sum = 0;
        last_tick = now;
    }
}

/* Update 449: sub_004528E7 is the retail build's compiled-out debug print
 * (body is a bare `ret`), still called from 320 sites with
 * (category, message, [severity|line]) -- e.g. "Memory Full", "Cannot
 * change volume for object, out of memory.", "Failed to open file : %s".
 * With RECOMP_DEBUG_PRINT=1 log the caller, category, message text and the
 * next two argument words; first 8 hits per call site, then every 1000th.
 * The generated body still runs (it only returns), so behavior is
 * unchanged. */
extern void sub_004528E7_gen(void);

static int scda_debug_print_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_DEBUG_PRINT") != NULL;
    return on;
}

void sub_004528E7(void)
{
    if (scda_debug_print_on()) {
        static uint32_t sites[512], hits[512];
        static int nsites;
        uint32_t ret = MEM32(g_esp), cat = MEM32(g_esp + 4u);
        uint32_t msg = MEM32(g_esp + 8u);
        uint32_t a3 = MEM32(g_esp + 12u), a4 = MEM32(g_esp + 16u);
        uint32_t n = 0;
        int i;
        for (i = 0; i < nsites; i++)
            if (sites[i] == ret)
                break;
        if (i == nsites && nsites < 512)
            sites[nsites++] = ret;
        if (i < 512)
            n = ++hits[i];
        if (n <= 8u || n % 1000u == 0u) {
            char text[160];
            int k = 0;
            if (msg >= 0x10000u && msg < 0x04000000u)
                for (; k < 159; k++) {
                    uint8_t c = MEM8(msg + (uint32_t)k);
                    if (!c)
                        break;
                    text[k] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
                }
            text[k] = 0;
            char s3[96];
            k = 0;
            if (strstr(text, "%s") && a3 >= 0x10000u && a3 < 0x04000000u)
                for (; k < 95; k++) {
                    uint8_t c = MEM8(a3 + (uint32_t)k);
                    if (!c)
                        break;
                    s3[k] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
                }
            s3[k] = 0;
            fprintf(stderr, "[SCDA-DBG] tick=%lu ret=0x%08X cat=%u n=%u msg=0x%08X \"%s\" "
                    "a3=0x%08X a4=0x%08X s3=\"%s\"\n", (unsigned long)GetTickCount(), ret,
                    cat, n, msg, text, a3, a4, s3);
        }
    }
    sub_004528E7_gen();
}

/* Update 450: sub_00028F2C builds the playable source for a sound resource
 * (holder in eax, res=[eax], type=[res+4]: 1 -> sub_0002D4B7, 6 ->
 * sub_0002D92A/sub_0002E5ED, 8 -> timed silence sub_0002F5E4). At the
 * in-level music trigger only a type-8 silence ever reaches the software
 * mixer. With RECOMP_MIXER_TRACE=1 log every call: caller, holder, type,
 * result and the first 0x40 bytes of the resource, for comparison with
 * the same breakpoint in xemu (tools/debug/xemu_sound_sources.py).
 * Observational only. */
extern void sub_00028F2C_gen(void);

/* Update 450: music voice objects (voice = holder - 4 when called from
 * sub_0002995E, instance = [voice+0x64]); every icall whose `this` is one
 * of them is logged as [SND-OBJ] by recomp_lookup_manual. */
static uint32_t g_snd_objs[32];
static int g_snd_nobjs;

static void scda_snd_track(uint32_t obj)
{
    int i;
    if (!obj)
        return;
    for (i = 0; i < g_snd_nobjs; i++)
        if (g_snd_objs[i] == obj)
            return;
    g_snd_objs[g_snd_nobjs++ % 32] = obj;
    if (g_snd_nobjs > 32)
        g_snd_nobjs = 32;
}

static int scda_snd_tracked(uint32_t obj)
{
    int i;
    for (i = 0; i < g_snd_nobjs; i++)
        if (g_snd_objs[i] == obj)
            return 1;
    return 0;
}

void sub_00028F2C(void)
{
    uint32_t ret = MEM32(g_esp), holder = g_eax, res = 0, type = 0;
    char line[512];
    int len, i;
    if (scda_mixer_trace_on() && holder) {
        res = MEM32(holder);
        type = res ? MEM32(res + 4u) : 0;
    }
    sub_00028F2C_gen();
    if (!scda_mixer_trace_on())
        return;
    if (ret == 0x000299C2u && holder >= 4u) {
        scda_snd_track(holder - 4u);
        scda_snd_track(MEM32(holder - 4u + 0x64u));
    }
    len = snprintf(line, sizeof(line),
                   "[SND-SRC] tick=%lu ret=0x%08X holder=0x%08X res=0x%08X type=%u "
                   "result=0x%08X res40=", (unsigned long)GetTickCount(), ret, holder,
                   res, type, g_eax);
    for (i = 0; i < 0x40 && res && len < (int)sizeof(line) - 10; i += 4)
        len += snprintf(line + len, sizeof(line) - len, "%08X ", MEM32(res + (uint32_t)i));
    len += snprintf(line + len, sizeof(line) - len, "\n");
    fwrite(line, 1, (size_t)len, stderr);
}

/* Update 450: after sub_00028F2C builds the level music source,
 * sub_0002995E registers it: icall 0x000299D8 = source->vtbl[0x28](0),
 * icall 0x000299DE = [ebx+0x64]->vtbl[0](value) -> handle ([ebx+0x68],
 * -1 = failure). Log target, this, first argument and result of both. */
static RECOMP_TLS uint32_t t_icall_log_target, t_icall_log_source;

static void scda_icall_log_wrapper(void)
{
    uint32_t target = t_icall_log_target, src = t_icall_log_source;
    uint32_t self = g_ecx, arg0 = MEM32(g_esp + 4u);
    recomp_func_t fn = recomp_lookup(target);
    if (!fn)
        fn = recomp_lookup_kernel(target);
    g_icall_source = 0;
    if (!fn) {
        fprintf(stderr, "[SND-ICALL] src=0x%08X no function for 0x%08X\n", src, target);
        return;
    }
    fn();
    {
        static volatile long lines;
        if (InterlockedIncrement(&lines) > 4000 && src != 0x000299DEu && src != 0x000299D8u)
            return;
    }
    fprintf(stderr, "[SND-ICALL] tick=%lu src=0x%08X target=0x%08X this=0x%08X "
            "arg0=0x%08X result=0x%08X\n", (unsigned long)GetTickCount(), src, target,
            self, arg0, g_eax);
    if (src == 0x000299DEu && self) {
        /* Same layout as tools/debug/xemu_bus_child.py prints for the
         * xemu bus child: instance, its source [this+0x24] and vtable. */
        char line[1024];
        int len = snprintf(line, sizeof(line), "  inst80:");
        uint32_t s = MEM32(self + 0x24u), i;
        for (i = 0; i < 0x80u; i += 4)
            len += snprintf(line + len, sizeof(line) - len, " %08X", MEM32(self + i));
        len += snprintf(line + len, sizeof(line) - len, "\n  src=0x%08X vtbl=0x%08X src80:",
                        s, s ? MEM32(s) : 0);
        for (i = 0; s && i < 0x80u; i += 4)
            len += snprintf(line + len, sizeof(line) - len, " %08X", MEM32(s + i));
        len += snprintf(line + len, sizeof(line) - len, "\n");
        fwrite(line, 1, (size_t)len, stderr);
    }
}

/* Update 469: AActor::execTouchingActors (0x002743CC, native table entry
 * "intAActorexecTouchingActors" at 0x005B4988) iterates this->Touching
 * (TArray data +0x170, count +0x174). Coverage of the level-end trigger
 * showed its loop body running in a fresh run and never after loading a
 * save. With RECOMP_TOUCH_TRACE=1 log every calling actor once (address,
 * class, name hash, first 0x200 bytes) and again whenever its Touching
 * count changes, with the touching actors and their classes. */
static uint32_t g_touch_objs[256], g_touch_counts[256];
static int g_touch_nobjs;

static void scda_touch_dump_obj(const char *tag, uint32_t obj, int words)
{
    char line[4096];
    int len = snprintf(line, sizeof(line), "%s obj=0x%08X class=0x%08X name=0x%08X "
                       "outer=0x%08X classname=0x%08X:", tag, obj, MEM32(obj + 0x24u),
                       MEM32(obj + 0x20u), MEM32(obj + 0x18u),
                       MEM32(obj + 0x24u) ? MEM32(MEM32(obj + 0x24u) + 0x20u) : 0);
    int i;
    for (i = 0; i < words && len < (int)sizeof(line) - 12; i++)
        len += snprintf(line + len, sizeof(line) - len, "%s%08X", (i & 7) ? " " : "\n   ",
                        MEM32(obj + (uint32_t)i * 4u));
    len += snprintf(line + len, sizeof(line) - len, "\n");
    fwrite(line, 1, (size_t)len, stderr);
}

static void scda_touching_wrapper(void)
{
    uint32_t self = g_ecx, n, i;
    int k;
    sub_002743CC();
    if (!self)
        return;
    n = MEM32(self + 0x174u);
    for (k = 0; k < g_touch_nobjs; k++)
        if (g_touch_objs[k] == self)
            break;
    if (k == g_touch_nobjs) {
        if (g_touch_nobjs >= 256)
            return;
        g_touch_objs[g_touch_nobjs] = self;
        g_touch_counts[g_touch_nobjs] = n;
        g_touch_nobjs++;
        fprintf(stderr, "[TOUCH] tick=%lu new caller count=%u\n",
                (unsigned long)GetTickCount(), n);
        scda_touch_dump_obj("[TOUCH]", self, 0x80);
    } else if (g_touch_counts[k] == n) {
        return;
    } else {
        fprintf(stderr, "[TOUCH] tick=%lu obj=0x%08X count %u -> %u\n",
                (unsigned long)GetTickCount(), self, g_touch_counts[k], n);
        g_touch_counts[k] = n;
    }
    for (i = 0; i < n && i < 8; i++) {
        uint32_t a = MEM32(MEM32(self + 0x170u) + i * 4u);
        fprintf(stderr, "[TOUCH]   touching[%u]=0x%08X class=0x%08X classname=0x%08X "
                "name=0x%08X\n", i, a, a ? MEM32(a + 0x24u) : 0,
                a && MEM32(a + 0x24u) ? MEM32(MEM32(a + 0x24u) + 0x20u) : 0,
                a ? MEM32(a + 0x20u) : 0);
    }
}

/* Update 477: RECOMP_60FPS=1 -- 60 FPS as a runtime switch. The renderer's
 * present-interval setter sub_002E3375 (vtable entry at 0x005697F8, so it
 * arrives here as an icall) stores [this+0x4DD0] = 1 / 2 / 0x80000002
 * (D3DPRESENT_INTERVAL_TWO_OR_IMMEDIATE) for modes 1 / 2 / other; 60 FPS
 * needs 0x80000001 (ONE_OR_IMMEDIATE) there instead (the immediate operand
 * at 0x002E3387). Set here, before the state flush sub_002E3EC4 reads it.
 *
 * Update 649: co-op never calls that setter (the interval comes from the
 * device's present parameters), so the switch also acts on D3D's own
 * presentation-interval render state 0x4AB5CC (D3D__RenderState[0x7F], the
 * flush's slot for [this+0x4DD0]) at the D3D swap entry sub_0049CC50,
 * before its two output paths sub_0049C910 / sub_0049CB00 read it -- see
 * the wrapper below. Co-op has its own option,
 * RECOMP_60FPS_COOP (default on): partners on another machine need the
 * same frame rate. */
static int s_scda_coop = -1;

/* The relaunched process's launch page (XLaunchNewImage data) holds the
 * mode the menu picked: tag 'sc3m' (bytes "m3cs") = 1 single player,
 * 2 co-op (the co-op page also carries a 'pad2' entry). Read before the
 * entry point, while the page is untouched. */
void scda_fps_init(void)
{
    uint32_t page = MEM32(0x00740000u + 0x30u), off;
    s_scda_coop = 0;
    if (!page)
        return;
    for (off = 0x400u; off < 0x480u; off += 4u)
        if (MEM32(page + off) == 0x7363336Du) {
            uint32_t mode = MEM32(page + off + 4u);
            s_scda_coop = mode == 2u;
            fprintf(stderr, "[SCDA-60FPS] launch mode sc3m=%u%s\n", mode,
                    s_scda_coop ? " (co-op)" : "");
            break;
        }
}

static int scda_want_60fps(void)
{
    static int want = -1;
    if (want < 0) {
        const char *on = getenv("RECOMP_60FPS");
        const char *coop = getenv("RECOMP_60FPS_COOP");
        want = on && atoi(on) != 0;
        if (want && s_scda_coop > 0 && coop && atoi(coop) == 0)
            want = 0;
        fprintf(stderr, "[SCDA-60FPS] %s fps%s\n", want ? "60" : "30",
                s_scda_coop > 0 ? " in co-op" : "");
    }
    return want;
}

static void scda_present_interval_wrapper(void)
{
    uint32_t self = g_ecx;
    sub_002E3375();
    if (self && MEM32(self + 0x4DD0u) == 0x80000002u) {
        static int logged;
        MEM32(self + 0x4DD0u) = 0x80000001u;
        if (!logged++)
            fprintf(stderr, "[SCDA-60FPS] present interval 0x80000002 -> 0x80000001\n");
    }
}

/* D3D swap entry (both output paths read 0x4AB5CC every frame to program
 * the vblank wait; co-op takes sub_0049CB00, not sub_0049C910): with
 * 60 FPS wanted, interval TWO becomes ONE (2 -> 1, 0x80000002 ->
 * 0x80000001), the IPS's change for every path that sets the interval. */
extern void sub_0049CC50_gen(void);
void sub_0049CC50(void)
{
    uint32_t v = MEM32(0x004AB5CCu);
    static uint32_t last_seen = 0xFFFFFFFFu;
    static int seen_logs;
    if (v != last_seen && seen_logs < 8) {
        seen_logs++;
        last_seen = v;
        fprintf(stderr, "[SCDA-60FPS] swap sees interval 0x%08X (flags 0x%02X, caller 0x%08X)\n",
                v, MEM8(g_esp + 4u), MEM32(g_esp));
    }
    {
        /* RECOMP_SWAP_TRACE=1: swaps per 5 s by caller, wall time spent in
         * the swap (vblank / flip waits included). */
        static int trace = -1;
        static uint32_t callers[4], counts[4];
        static ULONGLONG t0;
        static double in_swap_ms;
        if (trace < 0)
            trace = getenv("RECOMP_SWAP_TRACE") != NULL;
        if (trace) {
            uint32_t c = MEM32(g_esp);
            LARGE_INTEGER a, b, f;
            int k;
            ULONGLONG now = GetTickCount64();
            for (k = 0; k < 4 && callers[k] && callers[k] != c; k++)
                ;
            if (k < 4) {
                callers[k] = c;
                counts[k]++;
            }
            if (!t0)
                t0 = now;
            if (now - t0 >= 5000) {
                fprintf(stderr, "[SWAP-TRACE] 5 s: %08X x%u, %08X x%u, %08X x%u, %08X x%u; "
                        "%.0f ms inside swap\n", callers[0], counts[0], callers[1], counts[1],
                        callers[2], counts[2], callers[3], counts[3], in_swap_ms);
                memset(counts, 0, sizeof counts);
                in_swap_ms = 0;
                t0 = now;
            }
            QueryPerformanceCounter(&a);
            if ((v & 0x7FFFFFFFu) == 2u && scda_want_60fps())
                MEM32(0x004AB5CCu) = v - 1u;
            sub_0049CC50_gen();
            QueryPerformanceCounter(&b);
            QueryPerformanceFrequency(&f);
            in_swap_ms += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
            return;
        }
    }
    if ((v & 0x7FFFFFFFu) == 2u && scda_want_60fps()) {
        static int logged;
        MEM32(0x004AB5CCu) = v - 1u;
        if (!logged++)
            fprintf(stderr, "[SCDA-60FPS] swap interval 0x%08X -> 0x%08X\n", v, v - 1u);
    }
    sub_0049CC50_gen();
}

/* Update 504: RECOMP_MKDIR_TRACE=1 -- the level-cache copy hung at the
 * prison level: sub_00042082 (create one directory: wide path -> alloca'd
 * narrow copy -> sub_0005355F) returned with ebx/esi/edi holding path bytes
 * and never created LMaps\x02_prison_a. It sizes the alloca from the 2nd of
 * four identical calls to vtable slot 0x48 (sub_00043DA9, builds the full
 * path FString) and copies the 3rd/4th result into it. Logs each of those
 * results and the function's entry/exit; the game itself is unchanged. */
static void mkdir_trace_wstr(char *out, size_t n, uint32_t w, uint32_t maxch)
{
    size_t i = 0;
    if (!w) {
        snprintf(out, n, "(null)");
        return;
    }
    for (; i + 1 < n && i < maxch; i++) {
        uint16_t c = MEM16(w + 2u * (uint32_t)i);
        if (!c)
            break;
        out[i] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    out[i] = 0;
}

static void scda_mkdir_path_wrapper(void)
{
    uint32_t src = g_icall_source, self = g_ecx;
    uint32_t out = MEM32(g_esp + 4u), path = MEM32(g_esp + 8u);
    uint32_t a3 = MEM32(g_esp + 12u), a4 = MEM32(g_esp + 16u);
    char in_s[160], res_s[160];
    g_icall_source = 0;
    mkdir_trace_wstr(in_s, sizeof in_s, path, 150);
    sub_00043DA9();
    mkdir_trace_wstr(res_s, sizeof res_s, MEM32(g_eax + 0u), 150);
    fprintf(stderr, "[MKDIR] site=0x%08X this=0x%08X out=0x%08X a3=%08X a4=%08X "
            "in=\"%s\" -> eax=0x%08X data=0x%08X num=%u max=%u \"%s\"\n",
            src, self, out, a3, a4, in_s, g_eax, MEM32(g_eax), MEM32(g_eax + 4u),
            MEM32(g_eax + 8u), res_s);
}

static void scda_mkdir_wrapper(void)
{
    uint32_t ebx0 = g_ebx, esi0 = g_esi, edi0 = g_edi, esp0 = g_esp;
    uint32_t path = MEM32(g_esp + 4u), a2 = MEM32(g_esp + 8u);
    char in_s[160];
    g_icall_source = 0;
    {
        /* RECOMP_MKDIR_WATCH=1: report writes by OTHER threads anywhere in
         * this stack page (it holds the four result FStrings). */
        static int armed;
        if (!armed && getenv("RECOMP_MKDIR_WATCH")) {
            extern void xbox_DebugWatchGuestRangeForeign(uint32_t, uint32_t, long);
            armed = 1;
            xbox_DebugWatchGuestRangeForeign((esp0 - 0x38u) & ~0xFFFu, 0x1000u, 40);
        }
    }
    mkdir_trace_wstr(in_s, sizeof in_s, path, 150);
    fprintf(stderr, "[MKDIR] enter this=0x%08X path=\"%s\" a2=0x%08X esp=0x%08X\n",
            g_ecx, in_s, a2, esp0);
    sub_00042082();
    fprintf(stderr, "[MKDIR] leave eax=%u esp=0x%08X (+%d) regs %s\n", g_eax, g_esp,
            (int)(g_esp - esp0),
            (g_ebx == ebx0 && g_esi == esi0 && g_edi == edi0) ? "kept" : "CHANGED");
}

static int scda_mkdir_trace_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_MKDIR_TRACE") != NULL;
    return on;
}

/* recomp_lookup_manual runs for every indirect call; getenv() there (a
 * locked scan of the whole environment) cost ~5 % of the game thread in the
 * JBA profile (docs Update 637). These diagnostic switches are read once. */
static const char *const s_lookup_env_names[] = {
    "RECOMP_CACHE_TRACE", "RECOMP_BINK_YUV_ENTRY_DUMP", "RECOMP_BINK_CONVERT_TRACE",
    "RECOMP_INPUT_TRACE", "RECOMP_BONE_TRACE", "RECOMP_ANIM_DUMP", "RECOMP_MENU_TRACE",
};
static int scda_env_set(int i)
{
    static signed char cached[sizeof(s_lookup_env_names) / sizeof(s_lookup_env_names[0])];
    if (!cached[i])
        cached[i] = getenv(s_lookup_env_names[i]) ? 1 : -1;
    return cached[i] > 0;
}

static recomp_func_t recomp_lookup_manual_slow(uint32_t xbox_va)
{
    if (scda_mkdir_trace_on()) {
        if (xbox_va == 0x00042082u)
            return scda_mkdir_wrapper;
        if (xbox_va == 0x00043DA9u &&
            (g_icall_source == 0x000420BCu || g_icall_source == 0x000420EFu ||
             g_icall_source == 0x0004212Au || g_icall_source == 0x0004214Bu))
            return scda_mkdir_path_wrapper;
    }
    if (xbox_va == 0x002E3375u && scda_want_60fps())
        return scda_present_interval_wrapper;
    if (xbox_va == 0x002743CCu) {
        static int on = -1;
        if (on < 0)
            on = getenv("RECOMP_TOUCH_TRACE") != NULL;
        if (on)
            return scda_touching_wrapper;
    }
    if (scda_mixer_trace_on()) {
        /* Only wrap resolvable targets, so an unresolved one still takes
         * the normal icall failure path ([ICALL]/[ICALL-NEW]). */
        int resolvable = recomp_lookup(xbox_va) || recomp_lookup_kernel(xbox_va);
        if (resolvable &&
            (g_icall_source == 0x000299D8u || g_icall_source == 0x000299DEu)) {
            t_icall_log_target = xbox_va;
            t_icall_log_source = g_icall_source;
            return scda_icall_log_wrapper;
        }
        if (resolvable && g_ecx && scda_snd_tracked(g_ecx) && g_icall_source != 0x0001904Du &&
            g_icall_source != 0x00028AD7u) {
            t_icall_log_target = xbox_va;
            t_icall_log_source = g_icall_source;
            return scda_icall_log_wrapper;
        }
        if (resolvable && g_icall_source == 0x0001904Du) {
            t_mixer_target = xbox_va;
            return scda_mixer_source_wrapper;
        }
        if (t_mixer_depth > 0)
            scda_mixer_note_pair(g_icall_source, xbox_va);
    }
    if (xbox_va == 0x0001AD33u && scda_env_set(0))
        return scda_stream_setup_wrapper;
    if (xbox_va == 0x0003815Fu && scda_env_set(0))
        return scda_cache_add_wrapper;
    if (xbox_va == 0x004C6140u &&
        (scda_env_set(1) ||
         scda_env_set(2)))
        return scda_bink_yuv_entry_wrapper;
    if (xbox_va == 0x0003F514u && scda_save_trace_on())
        return scda_save_sign_wrapper;
    if (xbox_va == 0x00040FACu && scda_save_trace_on())
        return scda_save_size_wrapper;
    if (xbox_va == 0x0003F382u && scda_save_trace_on())
        return scda_save_hasfree_wrapper;
    if (xbox_va == 0x000426C7u && scda_save_trace_on())
        return scda_save_create_wrapper;
    if (xbox_va == 0x004BD850u && scda_binksnd_trace_on())
        return scda_binksnd_onoff_wrapper;
    if (xbox_va == 0x001C97B3u && scda_env_set(3))
        return scda_viewport_tick_wrapper;
    if (xbox_va == 0x002EF25Eu && scda_env_set(4))
        return scda_bone_upload_wrapper;
    if (xbox_va == 0x00217912u && scda_env_set(4))
        return scda_mesh_render_wrapper;
    if (xbox_va == 0x0021AF22u && scda_env_set(5))
        return scda_anim_eval_wrapper;
    if (xbox_va == 0x00290428u && scda_env_set(6))
        return scda_menu_lookup_wrapper;
    if (xbox_va == 0x004BD380u && scda_binksnd_trace_on())
        return scda_binksnd_update_wrapper;
    if (xbox_va == 0x004BD530u && scda_binksnd_trace_on())
        return scda_binksnd_lock_wrapper;
    if (xbox_va == 0x004BD670u && scda_binksnd_trace_on())
        return scda_binksnd_commit_wrapper;
    if (xbox_va == 0x00074792u)
        return scda_getter_00074792;
    if (xbox_va == 0x00039E08u)
        return scda_getter_00039E08;
    if (xbox_va == 0x0003EA17u)
        return scda_thunk_03EA17;
    if (xbox_va == 0x0003EA1Fu)
        return scda_thunk_03EA1F;
    if (xbox_va == 0x0003EA27u)
        return scda_thunk_03EA27;
    if (xbox_va == 0x0003EA2Fu)
        return scda_thunk_03EA2F;
    if (xbox_va == 0x0003EA37u)
        return scda_thunk_03EA37;
    if (xbox_va == 0x0003EA3Fu)
        return scda_thunk_03EA3F;
    if (xbox_va == 0x0003EA47u)
        return scda_thunk_03EA47;
    if (xbox_va == 0x0003EA4Fu)
        return scda_thunk_03EA4F;
    if (xbox_va == 0x0003EA57u)
        return scda_thunk_03EA57;
    if (xbox_va == 0x0003EA5Fu)
        return scda_thunk_03EA5F;
    if (xbox_va == 0x0003EA67u)
        return scda_thunk_03EA67;
    if (xbox_va == 0x0003EA6Fu)
        return scda_thunk_03EA6F;
    if (xbox_va == 0x0003EA77u)
        return scda_thunk_03EA77;
    if (xbox_va == 0x0003EA7Fu)
        return scda_thunk_03EA7F;
    if (xbox_va == 0x0003EA87u)
        return scda_thunk_03EA87;
    if (xbox_va == 0x0003EA8Fu)
        return scda_thunk_03EA8F;
    if (xbox_va == 0x0003EA97u)
        return scda_thunk_03EA97;
    if (xbox_va == 0x0003EA9Fu)
        return scda_thunk_03EA9F;
    if (xbox_va == 0x00047147u)
        return scda_47008_earlyexit_00047147;
    if (xbox_va == 0x0004612Bu)
        return scda_460f0_earlyexit_0004612B;
    if (xbox_va == 0x000468BEu)
        return scda_hashinsert_notfound_000468BE;
    if (xbox_va == 0x003673CCu)
        return scda_memcpy_epilogue_003673CC;
    if (xbox_va == 0x00077AC5u)
        return scda_allocator_epilogue_00077AC5;
    if (xbox_va == 0x0036D415u)
        return scda_crt_math_epilogue_0036D415;
    if (xbox_va == 0x0036D2F8u)
        return scda_crt_math_epilogue_0036D2F8;
    if (xbox_va == 0x0036D3CAu)
        return scda_crt_math_epilogue_0036D3CA;
    if (xbox_va == 0x00071223u)
        return scda_crc32_table_finish_00071223;
    return NULL;
}

/* recomp_lookup_manual runs for every indirect call and walks ~70 address
 * compares to answer "no override" for nearly all of them (~3 % of the game
 * thread in the JBA profile, docs Update 642). Unless a trace that depends on
 * the call site is on (mkdir/mixer), the answer depends only on the address
 * and the once-read environment, so it is remembered per address. Slots are
 * written va-last and read with a re-check, so a concurrent update by another
 * guest thread can never hand out another address's function. */
#define SCDA_LOOKUP_MEMO_BITS 14
static struct {
    volatile uint32_t va;
    recomp_func_t volatile fn;
} s_lookup_memo[1u << SCDA_LOOKUP_MEMO_BITS];

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    static int call_site_dependent = -1;
    /* Zero-initialised slots read as "VA 0 -> no override", which is what
     * the slow path answers for 0 as well. */
    if (call_site_dependent < 0)
        call_site_dependent = scda_mkdir_trace_on() || scda_mixer_trace_on();
    if (call_site_dependent)
        return recomp_lookup_manual_slow(xbox_va);

    unsigned h = (xbox_va * 2654435761u) >> (32 - SCDA_LOOKUP_MEMO_BITS);
    if (s_lookup_memo[h].va == xbox_va) {
        recomp_func_t fn = s_lookup_memo[h].fn;
        _ReadWriteBarrier();
        if (s_lookup_memo[h].va == xbox_va)
            return fn;
    }
    recomp_func_t fn = recomp_lookup_manual_slow(xbox_va);
    s_lookup_memo[h].va = 0xFFFFFFFFu;
    _ReadWriteBarrier();
    s_lookup_memo[h].fn = fn;
    _ReadWriteBarrier();
    s_lookup_memo[h].va = xbox_va;
    return fn;
}

#define SCDA_ICALL_RING_SIZE 32
static uint32_t g_icall_ring_source[SCDA_ICALL_RING_SIZE];
static uint32_t g_icall_ring_target[SCDA_ICALL_RING_SIZE];
static uint32_t g_icall_ring_ecx[SCDA_ICALL_RING_SIZE];
static uint32_t g_icall_ring_esi[SCDA_ICALL_RING_SIZE];
static uint32_t g_icall_ring_eax[SCDA_ICALL_RING_SIZE];
static unsigned int g_icall_ring_index = 0;
static unsigned int g_icall_ring_count = 0;

void scda_dump_icall_ring(void)
{
    unsigned int i, n, start;
    n = g_icall_ring_count < SCDA_ICALL_RING_SIZE ? g_icall_ring_count : SCDA_ICALL_RING_SIZE;
    start = (g_icall_ring_index + SCDA_ICALL_RING_SIZE - n) % SCDA_ICALL_RING_SIZE;
    fprintf(stderr, "[SCDA-ICALL-RING] dumping last %u failed-icall entries before crash\n", n);
    for (i = 0; i < n; i++) {
        unsigned int slot = (start + i) % SCDA_ICALL_RING_SIZE;
        fprintf(stderr, "[SCDA-ICALL-RING] #%u source=0x%08X target=0x%08X ecx=0x%08X esi=0x%08X eax=0x%08X\n",
                i, g_icall_ring_source[slot], g_icall_ring_target[slot],
                g_icall_ring_ecx[slot], g_icall_ring_esi[slot], g_icall_ring_eax[slot]);
    }
    fflush(stderr);
}

void recomp_icall_fail_log(uint32_t xbox_va)
{
    static unsigned int reports;
    reports++;
    g_icall_ring_source[g_icall_ring_index] = (uint32_t)g_icall_source;
    g_icall_ring_target[g_icall_ring_index] = xbox_va;
    g_icall_ring_ecx[g_icall_ring_index] = g_ecx;
    g_icall_ring_esi[g_icall_ring_index] = g_esi;
    g_icall_ring_eax[g_icall_ring_index] = g_eax;
    g_icall_ring_index = (g_icall_ring_index + 1) % SCDA_ICALL_RING_SIZE;
    if (g_icall_ring_count < SCDA_ICALL_RING_SIZE)
        g_icall_ring_count++;
    if (reports <= 50) {
        fprintf(stderr,
                "[ICALL] source=0x%08X target=0x%08X esp=0x%08X "
                "eax=0x%08X ecx=0x%08X esi=0x%08X edi=0x%08X "
                "(report %u, total %llu)\n",
                (uint32_t)g_icall_source,
                xbox_va,
                g_esp,
                g_eax,
                g_ecx,
                g_esi,
                g_edi,
                reports,
                (unsigned long long)g_icall_count);
    } else {
        /* Past the first 50 reports, still name every distinct
         * source/target pair once: a repeating target used to hide every
         * later unresolved function start (docs Update 325). */
        static uint32_t seen_src[1024], seen_dst[1024];
        static unsigned int seen_n;
        unsigned int i;
        if (reports == 51)
            fprintf(stderr,
                    "[ICALL] further repeat reports suppressed; new "
                    "source/target pairs follow as [ICALL-NEW]\n");
        for (i = 0; i < seen_n; i++)
            if (seen_src[i] == (uint32_t)g_icall_source && seen_dst[i] == xbox_va)
                return;
        if (seen_n < 1024) {
            seen_src[seen_n] = (uint32_t)g_icall_source;
            seen_dst[seen_n] = xbox_va;
            seen_n++;
            fprintf(stderr,
                    "[ICALL-NEW] source=0x%08X target=0x%08X ecx=0x%08X "
                    "(report %u, total %llu)\n",
                    (uint32_t)g_icall_source, xbox_va, g_ecx, reports,
                    (unsigned long long)g_icall_count);
        }
    }
}

/* Update 456: the retail CRT's x87 exp/pow/sinh core. sub_0036D062 turns the
 * scaled exponent y = x*log2(e) in ST(0) into 2^frac(y) and returns to its
 * caller -- except when |y| >= 32767.5 (or NaN): then it jumps to 0x0036D014,
 * `pop eax` discards its own return address and the following `ret` returns
 * straight to the caller's caller with the overflow result:
 *   y < 0 or NaN (ftst C0): [ebp-0x90] = 4 (underflow), jmp 0x0036D212 =
 *                           fstp st(0); fldz; ret          -> result 0
 *   otherwise:              fstp st(0); fld +inf; if (ch) fchs; ret
 * A two-level return cannot be expressed as C calls, and 0x0036D014 was an
 * unresolved stub, so the call was silently skipped ([STUB-HIT], docs
 * Update 455) and the callers went on computing 2^y with a huge y. Both
 * callers -- sub_0036CF32 (exp/pow, entered with y already in ST(0)) and
 * sub_00452470 (sinh/cosh, starts with fldl2e; fmulp; xor ebx,ebx) -- take
 * the overflow branch here, before their `call 0x0036D062`, and otherwise
 * run their generated bodies unchanged. */
extern void sub_0036CF32_gen(void);
extern void sub_00452470_gen(void);

static int scda_crt_exp_overflow(double y)
{
    return isnan(y) || fabs(y) >= 32767.5;
}

/* Tail of the overflow branch; ST(0) = y. Mirrors 0x0036D014..0x0036D036
 * and 0x0036D212 exactly, including the status byte the caller tests. */
static void scda_crt_exp_overflow_result(double y)
{
    uint32_t ebp = g_seh_ebp;  /* fpo_leaf frame, as the generated bodies */
    uint8_t sw_hi = 0;
    if (isnan(y))
        sw_hi = 0x45;          /* ftst unordered: C3 C2 C0 */
    else if (y < 0.0)
        sw_hi = 0x01;          /* C0 */
    MEM8(ebp - 0x9Fu) = sw_hi; /* fnstsw [ebp-0xA0], high byte */
    if (sw_hi & 0x01) {
        MEM8(ebp - 0x90u) = 4;                   /* underflow */
        g_fp_top = (g_fp_top + 1u) & 7u;         /* fstp st(0) */
        g_fp_top = (g_fp_top + 7u) & 7u;         /* fldz */
        g_fp_stack[g_fp_top] = 0.0;
    } else {
        g_fp_stack[g_fp_top] = ((g_ecx >> 8) & 0xFFu) ? -INFINITY : INFINITY;
    }
    g_esp += 4;                                  /* ret */
}

void sub_0036CF32(void)
{
    double y = g_fp_stack[g_fp_top];
    if (scda_crt_exp_overflow(y)) {
        scda_crt_exp_overflow_result(y);
        return;
    }
    sub_0036CF32_gen();
}

void sub_00452470(void)
{
    double y = g_fp_stack[g_fp_top] * 1.4426950408889634;  /* fldl2e; fmulp */
    if (scda_crt_exp_overflow(y)) {
        g_fp_stack[g_fp_top] = y;
        g_ebx = 0;                                /* xor ebx, ebx */
        scda_crt_exp_overflow_result(y);
        return;
    }
    sub_00452470_gen();
}

/* A direct call reached an unresolved-target stub (see recomp_types.h): the
 * call is skipped. Log each address once with the caller's return address,
 * so a missing function start can be seeded (docs Update 455). Unlocked on
 * purpose: a lost race only means a duplicate line. */
void recomp_unresolved_stub_hit(uint32_t va)
{
    enum { SLOTS = 256 };
    static uint32_t seen[SLOTS];
    static int count;
    int i;
    for (i = 0; i < count; i++)
        if (seen[i] == va)
            return;
    if (count < SLOTS)
        seen[count++] = va;
    fprintf(stderr, "[STUB-HIT] unresolved direct-call target 0x%08X skipped "
            "(ret=0x%08X, first hit)\n", va, MEM32(g_esp));
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * New with the v0.7.x merge (2026-09-03): RECOMP_ICALL/RECOMP_ICALL_SAFE now
 * check RECOMP_ICALL_IS_CODE before ever calling recomp_lookup*, and route a
 * non-code target here instead of falling through to recomp_icall_fail_log
 * (which is for a code-range target with no *registered* function). Ported
 * from templates/new-game/src/recomp_manual.c -- SCDA's own recomp_manual.c
 * predates this template addition, and the symbol has to exist for every
 * generated translation unit to link.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */

void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    /* Update 346: every null virtual call so far came from an object whose
     * constructor never ran (vtable 0). For a null target, name the thiscall
     * object (ecx) once per call site. */
    if (va == 0) {
        static uint32_t null_src[64];
        static int null_n;
        int k, known = 0;
        for (k = 0; k < null_n; k++)
            if (null_src[k] == g_icall_source) { known = 1; break; }
        if (!known && null_n < 64) {
            null_src[null_n++] = g_icall_source;
            fprintf(stderr, "[SCDA-NULLVCALL] source=0x%08X ecx=0x%08X words=%08X "
                    "%08X %08X %08X %08X esi=0x%08X edi=0x%08X\n",
                    (unsigned)g_icall_source, (unsigned)g_ecx,
                    g_ecx ? (unsigned)MEM32(g_ecx) : 0u,
                    g_ecx ? (unsigned)MEM32(g_ecx + 4u) : 0u,
                    g_ecx ? (unsigned)MEM32(g_ecx + 8u) : 0u,
                    g_ecx ? (unsigned)MEM32(g_ecx + 12u) : 0u,
                    g_ecx ? (unsigned)MEM32(g_ecx + 16u) : 0u,
                    (unsigned)g_esi, (unsigned)g_edi);
            fflush(stderr);
        }
    }
    /* Update 346: null destructor call at 0x0029B471 (sub_0029B36A failure
     * path: call [[edi]]) -- the stream object from the provider factory. */
    if (g_icall_source == 0x0029B471u) {
        static int s_reports;
        if (s_reports++ < 2) {
            unsigned k;
            fprintf(stderr, "[SCDA-MGBSTREAM] edi=0x%08X words:", (unsigned)g_edi);
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", (unsigned)MEM32(g_edi + 4u * k));
            fprintf(stderr, " | provider=0x%08X prov_vt=0x%08X factory=0x%08X "
                    "factory_vt=0x%08X fac_vt[1]=0x%08X\n",
                    (unsigned)MEM32(0x006A660Cu),
                    (unsigned)MEM32(MEM32(0x006A660Cu)),
                    (unsigned)MEM32(MEM32(0x006A660Cu) + 8u),
                    (unsigned)MEM32(MEM32(MEM32(0x006A660Cu) + 8u)),
                    (unsigned)MEM32(MEM32(MEM32(MEM32(0x006A660Cu) + 8u)) + 4u));
            fflush(stderr);
        }
    }
    /* Update 346: the null call at 0x002D7373 (sub_002D7350:
     * call [[esi+0x20]]+4) breaks the .mgb screen load. Name the object. */
    if (g_icall_source == 0x002D7373u) {
        static int d7350_reports;
        if (d7350_reports++ < 4) {
            uint32_t obj = MEM32(g_esi + 0x20u);
            uint32_t vt = obj ? MEM32(obj) : 0;
            fprintf(stderr, "[SCDA-D7350] this(esi)=0x%08X [+1C]=0x%08X [+20]=0x%08X "
                    "vtable=0x%08X vt[0]=0x%08X vt[1]=0x%08X vt[2]=0x%08X "
                    "obj[1..3]=%08X %08X %08X ret=0x%08X\n", (unsigned)g_esi,
                    (unsigned)MEM32(g_esi + 0x1Cu), (unsigned)obj, (unsigned)vt,
                    vt ? (unsigned)MEM32(vt) : 0u, vt ? (unsigned)MEM32(vt + 4u) : 0u,
                    vt ? (unsigned)MEM32(vt + 8u) : 0u,
                    obj ? (unsigned)MEM32(obj + 4u) : 0u,
                    obj ? (unsigned)MEM32(obj + 8u) : 0u,
                    obj ? (unsigned)MEM32(obj + 12u) : 0u,
                    (unsigned)MEM32(g_esp + 8u));
            fflush(stderr);
        }
    }

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu) source=0x%08X\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count, (uint32_t)g_icall_source);
    fflush(stderr);
}

/* 2026-09-03: bisecting the proven 720-byte (0x2D0) stack deficit inside
 * sub_001D5208's call_seq=1 pass. It was narrowed to already being fully
 * present by loc_001D5634 (right after the icall at 0x1D560B returns from
 * sub_0004628D). sub_0004628D itself uses a proper ebp-based frame whose
 * epilogue recomputes esp from the local `ebp` C variable, which should
 * mask any internal imbalance -- so the next suspect is its OWN nested
 * icall at 0x462D0 (`call dword ptr [eax+0x10]`, 7 stack args = 28 bytes
 * pushed, expecting the resolved callee to clean up via its own `ret`).
 * g_scda_462d0_pre_esp captures esp right before that dispatch (from
 * entry_esp, which RECOMP_ICALL_SAFE passes in as saved_esp == esp before
 * the call is issued) so the paired hook at loc_000462D3 (in
 * scda_trace_pc(), unconditionally compiled) can check whether esp came
 * back balanced. Declared outside the SCDA_ICALL_PROBE ifdef below since
 * the paired consumer hook must exist regardless of that gate. */
static uint32_t g_scda_462d0_pre_esp;

#ifdef SCDA_ICALL_PROBE
/* 2026-09-03: root-caused why this never printed -- it was wired into
 * RECOMP_ICALL (the non-safe variant), but the actual generated call
 * site at 0x00071BE6 uses RECOMP_ICALL_SAFE, a separate macro that
 * never carried the RECOMP_SCDA_ICALL_PROBE hook. Proven via /P
 * preprocessor output on tools/recomp/output/scda_v060/gen/recomp_0003.c
 * with the exact canonical build flags (see docs/SCDA_WORKLOG.md) --
 * not guessed. Fixed by adding the hook to RECOMP_ICALL_SAFE too
 * (templates/runtime/recomp_types.h). Also fixed a literal "\\n" (two
 * chars, backslash+n) in the old format string -- harmless but sloppy,
 * left the line without a real newline terminator. */
void scda_icall_probe(uint32_t xbox_va, recomp_func_t fn, uint32_t entry_esp)
{
    uint32_t ret_addr = MEM32(entry_esp);
    fprintf(stderr,
            "[SCDA-71BE6-DISPATCH] callsite_source=0x%08X raw_target=0x%08X "
            "host_fn=%p guest_esp_before=0x%08X guest_return_addr=0x%08X\n",
            (unsigned)g_icall_source, (unsigned)xbox_va, (void *)fn,
            (unsigned)entry_esp, (unsigned)ret_addr);
    fflush(stderr);
    if (g_icall_source == 0x000462D0u) {
        g_scda_462d0_pre_esp = entry_esp;
    }
}
#endif

void scda_trace_pc(uint32_t xbox_va)
{
    /* Update 356: in-level save. sub_0018A40D prepares the save and calls
     * the request sub_002944F7 at 0x18A6E8 only if [[ebx+0x20]+4] and
     * sub_0023067C's result are nonzero. Log its block path (first 600
     * blocks) and every entry into the request. */
    if (xbox_va >= 0x0018A40Du && xbox_va < 0x0018A6F7u) {
        static int logged;
        if (logged < 600) {
            logged++;
            fprintf(stderr, "[SCDA-SAVEPREP] block 0x%08X eax=0x%08X ebx=0x%08X "
                    "ecx=0x%08X esi=0x%08X\n", (unsigned)xbox_va,
                    (unsigned)g_eax, (unsigned)g_ebx, (unsigned)g_ecx,
                    (unsigned)g_esi);
            fflush(stderr);
        }
        return;
    }
    if (xbox_va == 0x002944F7u) {
        fprintf(stderr, "[SCDA-SAVEREQ] sub_002944F7 ret=0x%08X ecx=0x%08X "
                "args=0x%08X 0x%08X 0x%08X 0x%08X\n",
                (unsigned)MEM32(g_esp), (unsigned)g_ecx,
                (unsigned)MEM32(g_esp + 4u), (unsigned)MEM32(g_esp + 8u),
                (unsigned)MEM32(g_esp + 12u), (unsigned)MEM32(g_esp + 16u));
        fflush(stderr);
        return;
    }
    /* Update 346: xemu reaches BinkOpen("back_menu.bik") through
     * sub_001CA2DA -> sub_0017BF36 -> sub_00185C2F; log whether recomp enters
     * the latter two (trace range 2 covers them), and nothing else there. */
    if (xbox_va >= 0x0029B36Au && xbox_va < 0x0029B4D0u) {
        /* Update 346: block path through sub_0029B36A (register one .mgb
         * screen), first 120 blocks, with the registers that carry its
         * intermediate results. */
        static int logged;
        if (logged < 120) {
            logged++;
            if (xbox_va == 0x0029B36Au)
                fprintf(stderr, "[SCDA-MGB] enter id=0x%08X path=0x%08X name=0x%08X\n",
                        (unsigned)MEM32(g_esp + 4u), (unsigned)MEM32(g_esp + 8u),
                        (unsigned)MEM32(g_esp + 12u));
            else
                fprintf(stderr, "[SCDA-MGB] block 0x%08X eax=0x%08X esi=0x%08X "
                        "edi=0x%08X\n", (unsigned)xbox_va, (unsigned)g_eax,
                        (unsigned)g_esi, (unsigned)g_edi);
        }
        return;
    }
    if (xbox_va >= 0x0017BF36u && xbox_va < 0x00186200u) {
        static int hits_17bf36, hits_185c2f;
        int *hits = xbox_va == 0x0017BF36u ? &hits_17bf36
                  : xbox_va == 0x00185C2Fu ? &hits_185c2f : NULL;
        /* Path through sub_00185C2F: each basic block once, in first-visit
         * order (xemu calls sub_00292318 from 0x001861E5). */
        if (xbox_va > 0x00185C2Fu && xbox_va < 0x001861F9u) {
            static uint8_t seen[0x1861F9u - 0x185C2Fu];
            static int logged;
            if (!seen[xbox_va - 0x185C2Fu] && logged < 300) {
                seen[xbox_va - 0x185C2Fu] = 1;
                logged++;
                fprintf(stderr, "[SCDA-MENUCHAIN] block 0x%08X eax=0x%08X "
                        "ecx=0x%08X esi=0x%08X edi=0x%08X\n", (unsigned)xbox_va,
                        (unsigned)g_eax, (unsigned)g_ecx, (unsigned)g_esi,
                        (unsigned)g_edi);
                if (xbox_va == 0x00185C73u && g_esi)
                    fprintf(stderr, "[SCDA-MENUCHAIN]   menu obj 0x%08X vtable "
                            "0x%08X slot+0x88 -> 0x%08X (lookup \"main\")\n",
                            (unsigned)g_esi, (unsigned)MEM32(g_esi),
                            (unsigned)MEM32(MEM32(g_esi) + 0x88u));
            }
        }
        if (hits && *hits < 3) {
            (*hits)++;
            fprintf(stderr, "[SCDA-MENUCHAIN] enter sub_%08X ret=0x%08X "
                    "ecx=0x%08X esp=0x%08X t_ms=%llu\n", (unsigned)xbox_va,
                    (unsigned)MEM32(g_esp), (unsigned)g_ecx, (unsigned)g_esp,
                    (unsigned long long)(recomp_rdtsc() / 733333u));
            fflush(stderr);
        }
        return;
    }
    static unsigned long long trace_calls;
    static unsigned int sequence;
    static unsigned int initializer_reports;
    static unsigned int nested_initializer_reports;
    static uint32_t previous_fs4;
    static uint32_t previous_fs28;
    static uint32_t previous_allocator;
    static uint32_t previous_heap_large_head;
    static uint32_t previous_startup_status_140;
    static uint32_t previous_startup_status_144;
    static uint32_t previous_file_manager;
    static uint32_t previous_file_manager_vtable;
    static uint32_t previous_file_manager_open;
    static uint32_t previous_pc;
    static uint32_t iterator_target;
    static int iterator_bad_edi_reported;
    static unsigned int vector_frame_reports;
    static unsigned int vector_reserve_reports;
    static int vector_stack_path_reported;
    static unsigned int hash61570_reports;
    static unsigned int file_manager_result_reports;
    static unsigned int file_manager_call_index;
    static unsigned int hash61570_entry_index;
    static unsigned int actorlist_trace_reports;
    static unsigned int umd_scan_reports;
    static unsigned int strcmp_arg_reports;
    /* A shared 0x412B4 return only becomes a valid cross-platform marker
     * when we follow it through 0x412F4 into the real sub_0004721E caller.
     * Keep this trace deliberately tiny: the prior 600-PC dump obscured the
     * required branch comparison with unrelated destructor internals. */
    static unsigned int optional_ini_outer_phase;
    static uint32_t optional_ini_40fac_entry_esp;
    static uint32_t optional_ini_40fac_entry_return;
    static uint32_t lookup_ebx_saved_slot;
    static uint32_t lookup_ebx_expected;
    static int lookup_ebx_watch_active;
    static int lookup_ebx_watch_reported;
    static int fs4_watch_initialized;
    static int fs28_watch_initialized;
    static int allocator_watch_initialized;
    static uint32_t previous_launch_array_count;
    static int launch_array_watch_initialized;
    static int heap_large_head_watch_initialized;
    static int startup_status_watch_initialized;
    static int file_manager_watch_initialized;
    static int edi57570c_watch_reported;
    static unsigned int ini_caller_bisect_reports;
    static int ini_caller_bisect_active;
    uint32_t current_fs4 = xbox_fs_read32(0x00000004u);
    uint32_t current_fs28 = xbox_fs_read32(0x00000028u);
    uint32_t current_allocator = MEM32(0x005B57F8u);
    uint32_t current_heap_large_head = MEM32(0x00F80180u);
    uint32_t current_startup_status_140 = MEM32(0x005ED140u);
    uint32_t current_startup_status_144 = MEM32(0x005ED144u);
    uint32_t current_file_manager = MEM32(0x005EAE74u);
    uint32_t current_file_manager_vtable =
        current_file_manager ? MEM32(current_file_manager) : 0;
    uint32_t current_file_manager_open =
        current_file_manager_vtable ? MEM32(current_file_manager_vtable + 0x10u) : 0;
    /* 2026-09-03 (continued 11): sub_00048F1D's array lookup at
     * 0x6513A4 (base)/0x6513A8 (count)/0x6513C8 (index) is the proven
     * first divergence toward the HalReturnToFirmware(2) exit -- real
     * hardware has base=0x00857390, count=0x20 (32 entries) at this
     * point; scda_recomp has base=0, count=0 (never populated), which
     * forces the out-of-bounds/sentinel path and the premature tail-
     * call exit into sub_0004919F. No literal-address write to
     * 0x6513A8 exists anywhere in generated code (grep confirmed) --
     * it must be written indirectly through a register-held pointer,
     * most likely by a per-translation-unit static-object registration
     * routine (matching AGENTS.md's "large constructor list"). This
     * cheap, address-independent watcher (evaluated on every
     * scda_trace_pc call, not gated behind the whitelist) catches the
     * FIRST write to the count field wherever it happens, without
     * needing to find the writer via static analysis first. */
    uint32_t current_launch_array_count = MEM32(0x006513A8u);

    xbox_KernelDataTickUpdate();
    trace_calls++;

    /* RECOMP_TRACE_PC runs before 0x40FAC's native `push ebp`, hence the
     * guest stack top is the only authoritative caller identity.  Preserve
     * it until the nested optional-open probe fires; static proximity is not
     * sufficient because this vtable method is reached through several
     * independent startup paths. */
    if (xbox_va == 0x00040FACu) {
        optional_ini_40fac_entry_esp = g_esp;
        optional_ini_40fac_entry_return = MEM32(g_esp);
        if (optional_ini_40fac_entry_return == 0x00071C68u ||
            optional_ini_40fac_entry_return == 0x00071D11u) {
            fprintf(stderr,
                    "[SCDA-OPTIONAL-INI-CALLEE] edge=sub_00040FAC-entry "
                    "return=0x%08X eax=0x%08X ecx=0x%08X edx=0x%08X "
                    "ebx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X "
                    "stack0=0x%08X\\n",
                    optional_ini_40fac_entry_return, g_eax, g_ecx, g_edx,
                    g_ebx, g_esi, g_edi, g_esp, MEM32(g_esp));
            fflush(stderr);
        }
    }

    if (xbox_va == 0x000511E7u) {
        uint32_t path_ptr = MEM32(g_esp + 4u);
        if (path_ptr != 0 && scda_is_optional_ini_path(path_ptr)) {
            char path[241];
            uint32_t i;
            for (i = 0; i < sizeof(path) - 1u; ++i) {
                uint8_t c = MEM8(path_ptr + i);
                path[i] = (char)c;
                if (c == 0)
                    break;
            }
            path[sizeof(path) - 1u] = '\0';
            fprintf(stderr,
                    "[SCDA-OPTIONAL-INI-OPEN] caller_return=0x%08X entry_esp=0x%08X "
                    "path_ptr=0x%08X path=%s eax=0x%08X ecx=0x%08X ebx=0x%08X "
                    "esi=0x%08X edi=0x%08X outer_40fac_entry_esp=0x%08X "
                    "outer_40fac_return=0x%08X\\n",
                    MEM32(g_esp), g_esp, path_ptr, path, g_eax, g_ecx,
                    g_ebx, g_esi, g_edi, optional_ini_40fac_entry_esp,
                    optional_ini_40fac_entry_return);
            fflush(stderr);
            /* 0x412B4 is the final D:\\Default probe in sub_00040FAC.
             * It can belong to several callers, so do not draw a conclusion
             * here.  Arm the tiny state machine below and prove the actual
             * outer return at 0x47456/0x474AC first. */
            if (MEM32(g_esp) == 0x000412B4u) {
                optional_ini_outer_phase = 1u;
            }
        }
    }

    if (optional_ini_outer_phase == 1u && xbox_va == 0x000412B4u) {
        fprintf(stderr,
                "[SCDA-OPTIONAL-INI-SYNC] edge=last-open-return pc=0x%08X previous=0x%08X "
                "eax=0x%08X ecx=0x%08X edx=0x%08X ebx=0x%08X esi=0x%08X "
                "edi=0x%08X esp=0x%08X ebp=0x%08X seh_ebp=0x%08X stack0=0x%08X "
                "entry_return=0x%08X\\n",
                xbox_va, previous_pc, g_eax, g_ecx, g_edx, g_ebx, g_esi,
                g_edi, g_esp, g_ebp, g_seh_ebp, MEM32(g_esp),
                optional_ini_40fac_entry_return);
        fflush(stderr);
        optional_ini_outer_phase = 2u;
    } else if (optional_ini_outer_phase == 2u && xbox_va == 0x000412F4u) {
        uint32_t entry_esp = optional_ini_40fac_entry_esp;
        fprintf(stderr,
                "[SCDA-OPTIONAL-INI-SYNC] edge=sub_00040FAC-exit pc=0x%08X "
                "eax=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X "
                "ebp=0x%08X seh_ebp=0x%08X entry_return=0x%08X "
                "saved=[edi:%08X,esi:%08X,ebx:%08X] stack=[%08X,%08X,%08X,%08X]\\n",
                xbox_va, g_eax, g_ebx, g_esi, g_edi, g_esp, g_ebp,
                g_seh_ebp, optional_ini_40fac_entry_return,
                MEM32(entry_esp - 0x4Cu), MEM32(entry_esp - 0x48u),
                MEM32(entry_esp - 0x44u),
                MEM32(g_esp), MEM32(g_esp + 4u),
                MEM32(g_esp + 8u), MEM32(g_esp + 12u));
        fflush(stderr);
        optional_ini_outer_phase = 3u;
    } else if (optional_ini_outer_phase == 3u &&
               (xbox_va == 0x00047456u || xbox_va == 0x00047458u ||
                xbox_va == 0x0004749Au || xbox_va == 0x000474ACu ||
                xbox_va == 0x000474AEu || xbox_va == 0x000474FCu)) {
        fprintf(stderr,
                "[SCDA-OPTIONAL-INI-OUTER] pc=0x%08X eax=0x%08X ecx=0x%08X "
                "edx=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X "
                "ebp=0x%08X seh_ebp=0x%08X slot5B5800=0x%08X slot5B57FC=0x%08X "
                "stack=[%08X,%08X,%08X,%08X]\\n",
                xbox_va, g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp,
                g_ebp, g_seh_ebp, MEM32(0x005B5800u), MEM32(0x005B57FCu),
                MEM32(g_esp), MEM32(g_esp + 4u), MEM32(g_esp + 8u),
                MEM32(g_esp + 12u));
        fflush(stderr);
        if (xbox_va == 0x000474FCu)
            optional_ini_outer_phase = 0u;
    }

    /* The actual post-INI callers are the large startup function's two
     * virtual calls, returning to 0x71C68 and 0x71D11.  At these points the
     * native frame offsets are fixed by the raw x86 (0x418 before the three
     * callee-saved pops, then 0x40C).  This gives the real `[EBP+0x28]`
     * operand without relying on g_ebp, which nested generated functions may
     * legitimately republish. */
    if (optional_ini_outer_phase == 3u &&
        (xbox_va == 0x00071C68u || xbox_va == 0x00071C6Au ||
         xbox_va == 0x00071C6Eu || xbox_va == 0x00071C71u ||
         xbox_va == 0x00071C73u || xbox_va == 0x00071C79u ||
         xbox_va == 0x00071CB9u || xbox_va == 0x00071D11u ||
         xbox_va == 0x00071D13u || xbox_va == 0x00071D15u ||
         xbox_va == 0x00071D1Au || xbox_va == 0x00071D1Cu ||
         xbox_va == 0x00071D22u || xbox_va == 0x00071D62u)) {
        uint32_t native_ebp = g_esp +
            (xbox_va < 0x00071D00u ? 0x418u : 0x40Cu);
        fprintf(stderr,
                "[SCDA-OPTIONAL-INI-CALLER] pc=0x%08X eax=0x%08X ecx=0x%08X "
                "edx=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X "
                "native_ebp=0x%08X arg28=0x%08X status140=0x%08X status144=0x%08X "
                "entry_return=0x%08X stack0=0x%08X\\n",
                xbox_va, g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp,
                native_ebp, MEM32(native_ebp + 0x28u),
                MEM32(0x005ED140u), MEM32(0x005ED144u),
                optional_ini_40fac_entry_return, MEM32(g_esp));
        fflush(stderr);
        if (xbox_va == 0x00071D62u)
            optional_ini_outer_phase = 0u;
    }

    /* Arm only the one allocator vcall at 0x71BE6.  `status140 == 1` is
     * shared by many unrelated startup allocations, so it is not a unique
     * scope marker by itself. */
    if (xbox_va == 0x00071BDCu)
        ini_caller_bisect_active = 1;

    /* Raw x86 establishes that sub_00071B49 sets EBX=0x100 at 0x71B64 and
     * ESI=0x0062A9F0 at 0x71B8C, then never writes either register before
     * its 0x71C65 vcall into sub_00040FAC.  Sample only the returns from the
     * intervening calls to find the first callee that violates the required
     * x86 callee-saved contract. */
    if ((xbox_va == 0x00071B8Cu || xbox_va == 0x00071B97u ||
         xbox_va == 0x00071BA6u || xbox_va == 0x00071BD7u ||
         xbox_va == 0x00071BDCu || xbox_va == 0x0003EDBFu ||
         xbox_va == 0x0003EDC8u || xbox_va == 0x00077B38u ||
         xbox_va == 0x000767EDu || xbox_va == 0x0007680Du ||
         xbox_va == 0x00076814u || xbox_va == 0x00077B3Du ||
         xbox_va == 0x00077B44u || xbox_va == 0x000774E6u ||
         xbox_va == 0x000778B9u || xbox_va == 0x00077B4Eu ||
         xbox_va == 0x00077B5Du || xbox_va == 0x00071BE8u ||
         xbox_va == 0x00071BEFu || xbox_va == 0x00071BFEu ||
         xbox_va == 0x00071C09u || xbox_va == 0x00071C3Bu ||
         xbox_va == 0x00071C56u || xbox_va == 0x00071C65u) &&
        ini_caller_bisect_active &&
        ini_caller_bisect_reports < 48u) {
        fprintf(stderr,
                "[SCDA-71B49-BISECT] n=%u pc=0x%08X eax=0x%08X ecx=0x%08X "
                "edx=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X "
                "status140=0x%08X target=0x%08X\\n",
                ++ini_caller_bisect_reports, xbox_va, g_eax, g_ecx, g_edx,
                g_ebx, g_esi, g_edi, g_esp, MEM32(0x005ED140u),
                (xbox_va == 0x00071BDCu || xbox_va == 0x00071C65u)
                    ? MEM32(g_eax) : 0u);
        fflush(stderr);
    }
    if (xbox_va == 0x00071BE8u)
        ini_caller_bisect_active = 0;

    /* Single-field lifetime differential for the sub_00077049 crash
     * (2026-09-02): once the hash-insert at 0x00047488 hands back a
     * fresh record, watch its +0x4 field across EVERY subsequent traced
     * PC (not gated behind the whitelist below, since the construction
     * chain spans many different functions) and log every value change,
     * to find the exact point where it goes wrong -- or confirm it's
     * simply never written at all. */
    {
        static uint32_t watched_last_value = 0xFFFFFFFFu;
        static int watch_reports;
        if (xbox_va == 0x00047492u) {
            g_scda_watch_object = g_esi;
            watched_last_value = g_scda_watch_object ? MEM32(g_scda_watch_object + 4u) : 0xFFFFFFFFu;
            watch_reports = 0;
            fprintf(stderr,
                    "[SCDA-FIELD4-WATCH] ARMED object=0x%08X initial_field4=0x%08X pc=0x%08X\n",
                    g_scda_watch_object, watched_last_value, xbox_va);
            fflush(stderr);
        } else if (g_scda_watch_object != 0 && watch_reports < 200) {
            uint32_t cur = MEM32(g_scda_watch_object + 4u);
            if (cur != watched_last_value) {
                fprintf(stderr,
                        "[SCDA-FIELD4-WATCH] CHANGE pc=0x%08X previous_pc=0x%08X "
                        "old=0x%08X new=0x%08X eax=0x%08X ebx=0x%08X ecx=0x%08X "
                        "edx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X ebp=0x%08X\n",
                        xbox_va, previous_pc, watched_last_value, cur,
                        g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp, g_seh_ebp);
                fflush(stderr);
                watched_last_value = cur;
                watch_reports++;
            }
        }
    }

    /* Unconditional ESP bisection checkpoints for the sub_0004721E ->
     * sub_000464EC -> sub_0007188A call chain.  These run independent of
     * the shared else-if diagnostic chain below (whose earlier branches can
     * otherwise swallow a specific-address match on the same call). */
    {
        /* Durable, unbuffered diagnostic: opens/writes/closes a dedicated
         * file on every single hit so a forced process kill can never lose
         * buffered output the way stderr->scda_runtime.log apparently can.
         * Deliberately bypasses fprintf(stderr,...) entirely to rule out
         * any stdio-buffering explanation for the earlier silent checks. */
        static unsigned int esp_bisect_reports;
        const char *label = NULL;
        if (esp_bisect_reports < 4000u) {
            switch (xbox_va) {
            case 0x00046504u: label = "post-prologue"; break;
            case 0x00046549u: label = "pre-7188A"; break;
            case 0x00046558u: label = "post-7188A"; break;
            case 0x00046865u: label = "pre-final"; break;
            case 0x00046877u: label = "pre-36116"; break;
            case 0x0004687Fu: label = "pre-final-pops"; break;
            case 0x000718A4u: label = "post-icall1"; break;
            case 0x000718B7u: label = "post-icall2"; break;
            case 0x000718C7u: label = "post-10D5C1"; break;
            case 0x000718D3u: label = "post-icall3"; break;
            case 0x000718DAu: label = "post-icall4"; break;
            case 0x000718E5u: label = "post-icall5"; break;
            case 0x00071902u: label = "post-1E460F"; break;
            case 0x000719F9u: label = "pre-return"; break;
            case 0x0004721Eu: label = "0004721E-entry"; break;
            case 0x000474FCu: label = "000474FC-pre-epilogue"; break;
            default: break;
            }
        }
        if (label != NULL) {
            FILE *bf = NULL;
            if (fopen_s(&bf, "scda_esp_bisect.log", "a") == 0 && bf) {
                fprintf(bf, "pc=0x%08X %s esp=0x%08X eax=0x%08X edi=0x%08X\n",
                        xbox_va, label, g_esp, g_eax, g_edi);
                fclose(bf);
            }
            esp_bisect_reports++;
        }
    }

    /* sub_0004721E saves EBX after its EBP frame and 0x220-byte local area.
     * The failing virtual call returns with that saved slot replaced by the
     * file-manager VTable address.  Watch only this slot to identify the
     * first guest PC at which it changes. */
    /* Narrow same-frame trace for the later 0x4693B crash.  g_ebp is a
     * shared register shadow and is overwritten by nested guest calls, so it
     * cannot identify the saved ESI of sub_000464EC.  Instead arm only when
     * that function receives the exact payload just returned by 0x46CE5 and
     * retain the native frame addresses derived from its entry ESP. */
    {
        static uint32_t cleanup_entry_esp;
        static uint32_t cleanup_frame_ebp;
        static uint32_t cleanup_saved_esi_slot;
        static uint32_t cleanup_expected_esi;
        static int cleanup_frame_armed;
        const int cleanup_checkpoint =
            xbox_va == 0x00046504u || xbox_va == 0x00046512u ||
            xbox_va == 0x00046549u || xbox_va == 0x00046558u ||
            xbox_va == 0x00046865u || xbox_va == 0x00046877u ||
            xbox_va == 0x00036116u || xbox_va == 0x00036128u ||
            xbox_va == 0x000360A3u || xbox_va == 0x000360B2u ||
            xbox_va == 0x0004687Fu || xbox_va == 0x000474FCu;

        if (xbox_va == 0x000464ECu && g_scda_watch_object != 0 &&
            g_ecx == g_scda_watch_object) {
            cleanup_entry_esp = g_esp;
            cleanup_frame_ebp = g_esp - 4u;       /* after native push ebp */
            cleanup_saved_esi_slot = g_esp - 0x40Cu;
            cleanup_expected_esi = g_esi;
            cleanup_frame_armed = 1;
            fprintf(stderr,
                    "[SCDA-464EC-FRAME] ARM pc=0x%08X entry_esp=0x%08X frame_ebp=0x%08X saved_esi_slot=0x%08X saved_esi=0x%08X expected_esi=0x%08X ret=0x%08X\n",
                    xbox_va, cleanup_entry_esp, cleanup_frame_ebp,
                    cleanup_saved_esi_slot, MEM32(cleanup_saved_esi_slot),
                    cleanup_expected_esi, MEM32(g_esp));
            fflush(stderr);
        } else if (cleanup_frame_armed && cleanup_checkpoint) {
            /* The target frame occupies at most 0x450 bytes below entry;
             * reject nested cleanup calls even when they visit the same PCs. */
            const int same_stack_band =
                g_esp <= cleanup_entry_esp && g_esp >= cleanup_entry_esp - 0x450u;
            const int same_vector =
                (xbox_va != 0x00036116u && xbox_va != 0x00036128u &&
                 xbox_va != 0x000360A3u && xbox_va != 0x000360B2u) ||
                g_ecx == cleanup_frame_ebp - 0x18u;
            if (same_stack_band && same_vector) {
                fprintf(stderr,
                        "[SCDA-464EC-FRAME] pc=0x%08X esp=0x%08X ebp_shadow=0x%08X eax=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X saved_esi=0x%08X expected_esi=0x%08X ret=0x%08X\n",
                        xbox_va, g_esp, g_ebp, g_eax, g_ebx, g_esi, g_edi,
                        MEM32(cleanup_saved_esi_slot), cleanup_expected_esi,
                        MEM32(g_esp));
                fflush(stderr);
            }
            /* Do not disarm on 0x474FC: nested lookups can legitimately
             * return there while this outer cleanup frame is still active. */
        }
        /* After sub_0007188A returns, log only basic blocks belonging to the
         * armed native cleanup function.  This deliberately excludes its
         * callees: the purpose is to find the first *returned-to* block whose
         * guest ESP no longer matches the caller frame, without a global
         * trace flood. */
        if (cleanup_frame_armed &&
            (((xbox_va >= 0x00046558u && xbox_va <= 0x0004687Fu) &&
              g_esp != cleanup_entry_esp - 0x414u) ||
             (xbox_va >= 0x00046800u && xbox_va <= 0x0004687Fu) ||
             xbox_va == 0x00046558u || xbox_va == 0x00046563u ||
             xbox_va == 0x0004656Eu || xbox_va == 0x00046573u ||
             xbox_va == 0x0004657Au)) {
            static unsigned int cleanup_path_reports;
            if (cleanup_path_reports < 800u &&
                (g_esp != cleanup_entry_esp - 0x414u ||
                 xbox_va >= 0x00046800u)) {
                fprintf(stderr,
                        "[SCDA-464EC-PATH] pc=0x%08X esp=0x%08X eax=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X saved_esi=0x%08X\n",
                        xbox_va, g_esp, g_eax, g_ebx, g_esi, g_edi,
                        MEM32(cleanup_saved_esi_slot));
                fflush(stderr);
                cleanup_path_reports++;
            }
        }
        if (cleanup_frame_armed && xbox_va == 0x0004686Au &&
            g_esp != cleanup_entry_esp - 0x414u) {
            fprintf(stderr,
                    "[SCDA-464EC-FIRST-BAD] prev_pc=0x%08X pc=0x%08X esp=0x%08X expected=0x%08X stack={0x%08X,0x%08X,0x%08X,0x%08X} eax=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X\n",
                    previous_pc, xbox_va, g_esp, cleanup_entry_esp - 0x414u,
                    MEM32(g_esp), MEM32(g_esp + 4u), MEM32(g_esp + 8u),
                    MEM32(g_esp + 12u), g_eax, g_ebx, g_esi, g_edi);
            fflush(stderr);
        }
        if (cleanup_frame_armed &&
            (xbox_va == 0x000465F8u || xbox_va == 0x00046604u)) {
            fprintf(stderr,
                    "[SCDA-464EC-4693B-EDGE] pc=0x%08X prev=0x%08X esp=0x%08X eax=0x%08X ecx=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X ret=0x%08X\n",
                    xbox_va, previous_pc, g_esp, g_eax, g_ecx, g_ebx,
                    g_esi, g_edi, MEM32(g_esp));
            fflush(stderr);
        }
    }
    if (xbox_va == 0x0004721Eu) {
        lookup_ebx_saved_slot = g_esp - 0x228u;
        lookup_ebx_expected = g_ebx;
        lookup_ebx_watch_active = 1;
        lookup_ebx_watch_reported = 0;
    } else if (lookup_ebx_watch_active && !lookup_ebx_watch_reported &&
               MEM32(lookup_ebx_saved_slot) != lookup_ebx_expected) {
        fprintf(stderr,
                "[SCDA-FM] 4721E-saved-ebx changed at pc=0x%08X slot=0x%08X old=0x%08X new=0x%08X esp=0x%08X\n",
                xbox_va, lookup_ebx_saved_slot, lookup_ebx_expected,
                MEM32(lookup_ebx_saved_slot), g_esp);
        lookup_ebx_watch_reported = 1;
    }
    /* The corrupt vector is at 0x00F7FD8C, exactly EBP-0x38 of its owner.
     * Record this one frame before the normal range filter so its creator and
     * final destructor call can be identified without changing heap behavior. */
    if (g_seh_ebp == 0x00F7FDC4u) {
        fprintf(stderr,
                "[SCDA-VECTOR-OWNER] pc=0x%08X eax=0x%08X ecx=0x%08X "
                "edx=0x%08X esp=0x%08X\n",
                xbox_va, g_eax, g_ecx, g_edx, g_esp);
    }
    if ((trace_calls % 10000000ull) == 0) {
        fprintf(stderr,
                "[SCDA-HEARTBEAT] calls=%llu pc=0x%08X eax=0x%08X "
                "ecx=0x%08X edx=0x%08X ebx=0x%08X esi=0x%08X "
                "edi=0x%08X esp=0x%08X ebp=0x%08X\n",
                trace_calls, xbox_va, g_eax, g_ecx, g_edx, g_ebx, g_esi,
                g_edi, g_esp, g_seh_ebp);
        fflush(stderr);
    }

    /* Software watchpoint for the emulated TIB TLS-vector field.  This runs
     * before the normal output-range filter, so it identifies the first basic
     * block observed after a write without flooding the log with every PC. */
    if (!fs4_watch_initialized) {
        previous_fs4 = current_fs4;
        fs4_watch_initialized = 1;
    } else if (current_fs4 != previous_fs4) {
        fprintf(stderr,
                "[SCDA-FS4-WRITE] previous_pc=0x%08X current_pc=0x%08X "
                "old=0x%08X new=0x%08X eax=0x%08X ecx=0x%08X "
                "edx=0x%08X esp=0x%08X esi=0x%08X edi=0x%08X\n",
                previous_pc, xbox_va, previous_fs4, current_fs4,
                g_eax, g_ecx, g_edx, g_esp, g_esi, g_edi);
        previous_fs4 = current_fs4;
    }
    /* The CDB dump from 2026-08-26 faulted after fs:[0x28] had become
     * 0x00010000.  Record the first guest basic block after any mutation of
     * this emulated TIB field, independently from the existing fs:[4] watch. */
    if (!fs28_watch_initialized) {
        previous_fs28 = current_fs28;
        fs28_watch_initialized = 1;
    } else if (current_fs28 != previous_fs28) {
        fprintf(stderr,
                "[SCDA-FS28-WRITE] previous_pc=0x%08X current_pc=0x%08X "
                "old=0x%08X new=0x%08X eax=0x%08X ecx=0x%08X "
                "edx=0x%08X esp=0x%08X esi=0x%08X edi=0x%08X\n",
                previous_pc, xbox_va, previous_fs28, current_fs28,
                g_eax, g_ecx, g_edx, g_esp, g_esi, g_edi);
        previous_fs28 = current_fs28;
    }
    if (!allocator_watch_initialized) {
        previous_allocator = current_allocator;
        allocator_watch_initialized = 1;
    } else if (current_allocator != previous_allocator) {
        fprintf(stderr,
                "[SCDA-ALLOCATOR-WRITE] previous_pc=0x%08X current_pc=0x%08X "
                "old=0x%08X new=0x%08X eax=0x%08X ecx=0x%08X "
                "edx=0x%08X esp=0x%08X ebp=0x%08X\n",
                previous_pc, xbox_va, previous_allocator, current_allocator,
                g_eax, g_ecx, g_edx, g_esp, g_seh_ebp);
        previous_allocator = current_allocator;
    }
    if (!launch_array_watch_initialized) {
        previous_launch_array_count = current_launch_array_count;
        launch_array_watch_initialized = 1;
    } else if (current_launch_array_count != previous_launch_array_count) {
        fprintf(stderr,
                "[SCDA-6513A8-WRITE] previous_pc=0x%08X current_pc=0x%08X "
                "old=%u new=%u base=0x%08X index=0x%08X eax=0x%08X "
                "ecx=0x%08X edx=0x%08X esp=0x%08X ebp=0x%08X\n",
                previous_pc, xbox_va, previous_launch_array_count,
                current_launch_array_count, MEM32(0x006513A4u),
                MEM32(0x006513C8u), g_eax, g_ecx, g_edx, g_esp, g_seh_ebp);
        fflush(stderr);
        previous_launch_array_count = current_launch_array_count;
    }
    /* The first Seed-21 crash after the allocator-epilogue fix is caused by
     * the low dword of this heap's large-free-list sentinel changing from its
     * valid self pointer to zero.  This watch identifies the first following
     * Guest basic block; it deliberately watches only the confirmed heap VA. */
    if (!heap_large_head_watch_initialized) {
        previous_heap_large_head = current_heap_large_head;
        heap_large_head_watch_initialized = 1;
    } else if (current_heap_large_head != previous_heap_large_head) {
        fprintf(stderr,
                "[SCDA-HEAP-LARGE-HEAD-WRITE] previous_pc=0x%08X current_pc=0x%08X "
                "old=0x%08X new=0x%08X sibling=0x%08X eax=0x%08X ecx=0x%08X "
                "edx=0x%08X esp=0x%08X\n",
                previous_pc, xbox_va, previous_heap_large_head,
                current_heap_large_head, MEM32(0x00F80184u), g_eax, g_ecx,
                g_edx, g_esp);
        previous_heap_large_head = current_heap_large_head;
    }
    /* These two globals are the error/status outputs passed into the main
     * startup path at 0x48B9B.  A non-zero result selects the cleanup route
     * that ultimately requests HalReturnToFirmware(2), so retain the first
     * guest block after every mutation. */
    if (!startup_status_watch_initialized) {
        previous_startup_status_140 = current_startup_status_140;
        previous_startup_status_144 = current_startup_status_144;
        startup_status_watch_initialized = 1;
    } else if (current_startup_status_140 != previous_startup_status_140 ||
               current_startup_status_144 != previous_startup_status_144) {
        fprintf(stderr,
                "[SCDA-STARTUP-STATUS] previous_pc=0x%08X current_pc=0x%08X "
                "status140=0x%08X->0x%08X status144=0x%08X->0x%08X "
                "eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
                previous_pc, xbox_va,
                previous_startup_status_140, current_startup_status_140,
                previous_startup_status_144, current_startup_status_144,
                g_eax, g_ecx, g_edx, g_esp);
        fflush(stderr);
        previous_startup_status_140 = current_startup_status_140;
        previous_startup_status_144 = current_startup_status_144;
    }
    /* Both unresolved calls which immediately precede the failed singleton
     * construction dispatch through [ [0x5EAE74] + 0x10 ].  This monitor
     * determines whether the file-manager object or only its vtable slot is
     * first corrupted, and records the basic block immediately after it. */
    if (!file_manager_watch_initialized) {
        previous_file_manager = current_file_manager;
        previous_file_manager_vtable = current_file_manager_vtable;
        previous_file_manager_open = current_file_manager_open;
        file_manager_watch_initialized = 1;
    } else if (current_file_manager != previous_file_manager ||
               current_file_manager_vtable != previous_file_manager_vtable ||
               current_file_manager_open != previous_file_manager_open) {
        fprintf(stderr,
                "[SCDA-FILE-MANAGER-WRITE] previous_pc=0x%08X current_pc=0x%08X "
                "object=0x%08X->0x%08X vtable=0x%08X->0x%08X "
                "open=0x%08X->0x%08X eax=0x%08X ecx=0x%08X edx=0x%08X "
                "esp=0x%08X ebp=0x%08X\\n",
                previous_pc, xbox_va, previous_file_manager,
                current_file_manager, previous_file_manager_vtable,
                current_file_manager_vtable, previous_file_manager_open,
                current_file_manager_open, g_eax, g_ecx, g_edx, g_esp,
                g_seh_ebp);
        fflush(stderr);
        previous_file_manager = current_file_manager;
        previous_file_manager_vtable = current_file_manager_vtable;
        previous_file_manager_open = current_file_manager_open;
    }

    /* Seed 28 reaches sub_00049F9B but later forms the invalid destination
     * 0x00000120 at 0x4A14C.  The original instruction is
     * `mov ecx,[0x5ED158]; add ecx,0x120`: therefore the actual bad value is
     * the engine singleton at 0x5ED158, not the vector object itself.  Keep
     * the few producer/consumer PCs so a fresh run identifies the first
     * write of zero without touching generated code. */
    if (xbox_va == 0x00049EFFu || xbox_va == 0x00049F11u ||
        xbox_va == 0x00049F3Au || xbox_va == 0x00049FF3u ||
        xbox_va == 0x001D8842u || xbox_va == 0x001D885Cu ||
        xbox_va == 0x0004A13Au || xbox_va == 0x0004A14Cu) {
        fprintf(stderr,
                "[SCDA-ENGINE-SINGLETON] pc=0x%08X previous_pc=0x%08X "
                "singleton=0x%08X eax=0x%08X ecx=0x%08X edx=0x%08X "
                "ebx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X ebp=0x%08X\\n",
                xbox_va, previous_pc, MEM32(0x005ED158u), g_eax, g_ecx,
                g_edx, g_ebx, g_esi, g_edi, g_esp, g_seh_ebp);
        fflush(stderr);
    }
    /* Do not watch a fixed guest-stack address here: nested functions reuse
     * the same physical stack slots, so such a watch cannot identify one
     * particular C++ vector.  The destructor-entry trace below records the
     * actual object address and its immediate caller instead. */

    /* The CRT iterator at 0x580E7 must retain EDI as its fixed end pointer
     * across each constructor call.  Record the exact call target which first
     * violates that callee-saved-register contract, without logging its hot
     * loop on every iteration. */
    if (xbox_va == 0x00058133u) {
        iterator_target = g_eax;
    } else if (xbox_va == 0x00058135u &&
               g_edi != 0x005B1D58u && !iterator_bad_edi_reported) {
        fprintf(stderr,
                "[SCDA-CRT-ITERATOR-CLOBBER] target=0x%08X esi=0x%08X "
                "edi=0x%08X esp=0x%08X previous_pc=0x%08X\n",
                iterator_target, g_esi, g_edi, g_esp, previous_pc);
        fflush(stderr);
        iterator_bad_edi_reported = 1;
    }

    if (xbox_va == 0x0004449Cu || xbox_va == 0x000444A4u ||
        xbox_va == 0x000444ABu || xbox_va == 0x000444B0u ||
        xbox_va == 0x00043582u || xbox_va == 0x00043733u) {
        fprintf(stderr,
                "[SCDA-ALLOC-CTOR] pc=0x%08X prev=0x%08X eax=0x%08X "
                "ecx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X ebp=0x%08X\n",
                xbox_va, previous_pc, g_eax, g_ecx, g_esi, g_edi,
                g_esp, g_seh_ebp);
        fflush(stderr);
    }

    if (xbox_va == 0x00048F7Eu || xbox_va == 0x00049058u ||
        xbox_va == 0x00054762u || xbox_va == 0x00055806u ||
        xbox_va == 0x00055821u) {
        fprintf(stderr,
                "[SCDA-EXIT-PATH] pc=0x%08X previous_pc=0x%08X "
                "status140=0x%08X status144=0x%08X eax=0x%08X ecx=0x%08X "
                "edx=0x%08X esp=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X\n",
                xbox_va, previous_pc, current_startup_status_140,
                current_startup_status_144, g_eax, g_ecx, g_edx, g_esp,
                g_ebx, g_esi, g_edi);
        fflush(stderr);
    }

    /* The XDK request at 0x55823 is the normal process-end path after the
     * title's main routine returns.  These probes therefore follow the two
     * UMD/boot-resource attempts immediately before that return.  In
     * particular, 0x43547 holds the eax:edx result that decides whether the
     * actual launch call at 0x4354B is skipped. */
    if (xbox_va == 0x00048BBEu || xbox_va == 0x00048C14u ||
        xbox_va == 0x00048C5Du || xbox_va == 0x00048C9Au ||
        xbox_va == 0x00048CE7u || xbox_va == 0x00048ED1u ||
        xbox_va == 0x00043455u || xbox_va == 0x000434A5u ||
        xbox_va == 0x00043539u || xbox_va == 0x00043547u ||
        xbox_va == 0x0004354Bu || xbox_va == 0x00043568u ||
        xbox_va == 0x00041F56u || xbox_va == 0x00041F76u ||
        xbox_va == 0x00042024u || xbox_va == 0x00042055u ||
        xbox_va == 0x00042060u || xbox_va == 0x0004206Cu ||
        xbox_va == 0x00042072u || xbox_va == 0x00042082u ||
        xbox_va == 0x00042178u || xbox_va == 0x0004217Cu ||
        xbox_va == 0x00042181u || xbox_va == 0x00042188u ||
        xbox_va == 0x000421CDu) {
        fprintf(stderr,
                "[SCDA-BOOT-PATH] pc=0x%08X previous_pc=0x%08X "
                "status140=0x%08X status144=0x%08X eax=0x%08X edx=0x%08X "
                "ecx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X ebp=0x%08X\n",
                xbox_va, previous_pc, current_startup_status_140,
                current_startup_status_144, g_eax, g_edx, g_ecx, g_esi,
                g_edi, g_esp, g_seh_ebp);
        fflush(stderr);
    }

    if (xbox_va == 0x0024A50Bu || xbox_va == 0x0024A525u) {
        uint32_t last_icall = g_icall_trace[(g_icall_trace_idx - 1u) &
                                             (ICALL_TRACE_SIZE - 1u)];
        fprintf(stderr,
                "[SCDA-ALLOC-FREE] pc=0x%08X prev=0x%08X esp=0x%08X "
                "ecx=0x%08X edx=0x%08X esi=0x%08X edi=0x%08X last_icall=0x%08X\n",
                xbox_va, previous_pc, g_esp, g_ecx, g_edx, g_esi, g_edi,
                last_icall);
        fflush(stderr);
    }
    if (xbox_va == 0x000371A6u || xbox_va == 0x000371D4u ||
        xbox_va == 0x000371E2u || xbox_va == 0x000A8D0Au ||
        xbox_va == 0x000A8D38u || xbox_va == 0x00040001u ||
        xbox_va == 0x00040003u ||
        xbox_va == 0x00040008u || xbox_va == 0x000778BCu ||
        xbox_va == 0x00077934u || xbox_va == 0x00077A89u ||
        xbox_va == 0x00077AC2u || xbox_va == 0x00077AC6u ||
        xbox_va == 0x00077B7Cu || xbox_va == 0x00077B89u ||
        xbox_va == 0x002B9F23u || xbox_va == 0x002B9F6Eu ||
        xbox_va == 0x002B9F83u || xbox_va == 0x002B9F9Au ||
        xbox_va == 0x002B9FA7u || xbox_va == 0x002BA02Fu ||
        xbox_va == 0x002BA0EEu || xbox_va == 0x002BA166u ||
        xbox_va == 0x002BA1C7u || xbox_va == 0x002BA1E0u ||
        xbox_va == 0x002BA1FBu ||
        xbox_va == 0x002BA348u || xbox_va == 0x002BA365u ||
        xbox_va == 0x002BA3C9u || xbox_va == 0x002BA3D5u) {
        fprintf(stderr,
                "[SCDA-FRAGMENT] pc=0x%08X previous_pc=0x%08X "
                "eax=0x%08X ecx=0x%08X ebx=0x%08X esi=0x%08X "
                "edi=0x%08X esp=0x%08X seh_ebp=0x%08X\n",
                xbox_va, previous_pc, g_eax, g_ecx, g_ebx, g_esi, g_edi,
                g_esp, g_seh_ebp);
    } else if (xbox_va == 0x0003721Bu) {
        fprintf(stderr,
                "[SCDA-WSTRING-HELPER-ENTRY] previous_pc=0x%08X "
                "ecx=0x%08X arg1=0x%08X arg2=0x%08X esp=0x%08X\n",
                previous_pc, g_ecx, MEM32(g_esp + 4), MEM32(g_esp + 8),
                g_esp);
    } else if (xbox_va == 0x000371EAu) {
        fprintf(stderr,
                "[SCDA-WSTRING-ASSIGN-ENTRY] previous_pc=0x%08X "
                "ecx=0x%08X arg_dest=0x%08X arg_source=0x%08X "
                "esp=0x%08X seh_ebp=0x%08X\n",
                previous_pc, g_ecx, MEM32(g_esp + 4), MEM32(g_esp + 8),
                g_esp, g_seh_ebp);
    } else if (xbox_va == 0x00057AB6u) {
        uint32_t heap = g_eax;
        fprintf(stderr,
                "[SCDA-HEAP-CREATE] heap=0x%08X list180=0x%08X list184=0x%08X "
                "bucket3=0x%08X/0x%08X esp=0x%08X\n",
                heap, MEM32(heap + 0x180), MEM32(heap + 0x184),
                MEM32(heap + 0x198), MEM32(heap + 0x19C), g_esp);
    } else if (xbox_va == 0x00056D21u) {
        uint32_t heap = g_esi;
        fprintf(stderr,
                "[SCDA-HEAP-LARGE-LIST] heap=0x%08X list180=0x%08X list184=0x%08X "
                "size=0x%08X esp=0x%08X\n",
                heap, MEM32(heap + 0x180), MEM32(heap + 0x184),
                MEM32(g_seh_ebp + 0x10), g_esp);
    } else if (xbox_va == 0x000459C4u && !vector_stack_path_reported &&
               g_esp >= 0x00F7A000u && g_esp < 0x00F7B000u) {
        fprintf(stderr,
                "[SCDA-VECTOR-STACK] pc=0x000459C4 before-ret8 esi=0x%08X esp=0x%08X "
                "stack={0x%08X,0x%08X,0x%08X,0x%08X}\n",
                g_esi, g_esp, MEM32(g_esp), MEM32(g_esp + 4u),
                MEM32(g_esp + 8u), MEM32(g_esp + 12u));
    } else if (xbox_va == 0x00061570u && hash61570_entry_index < 4000u) {
        uint32_t object = g_ecx;
        hash61570_entry_index++;
        fprintf(stderr,
                "[SCDA-FM] pc=0x00061570 entry #%u ecx_object=0x%08X base=0x%08X buckets=0x%08X mask=0x%08X arg_key=0x%08X arg_out=0x%08X esp=0x%08X\n",
                hash61570_entry_index, object, MEM32(object), MEM32(object + 0x0Cu), MEM32(object + 0x10u),
                MEM32(g_esp + 4u), MEM32(g_esp + 8u), g_esp);
    } else if (xbox_va == 0x00061589u && hash61570_reports < 20u) {
        uint32_t base = MEM32(g_edi);
        fprintf(stderr,
                "[SCDA-61570] pc=0x00061589 index=0x%08X table=0x%08X key=0x%08X "
                "entry_key=0x%08X esp=0x%08X\n",
                g_esi, base, MEM32(g_ebx),
                base <= 0x03FFFFF4u ? MEM32(base + g_esi * 20u + 4u) : 0xFFFFFFFFu,
                g_esp);
        hash61570_reports++;
    } else if (xbox_va == 0x000615B3u) {
        uint32_t base = MEM32(g_edi);
        uint32_t source = base + g_esi + 8u;
        if ((source < 0x00010000u || hash61570_reports < 24u) &&
            hash61570_reports < 60u) {
            fprintf(stderr,
                    "[SCDA-61570] pc=0x000615B3 base=0x%08X byte_offset=0x%08X "
                    "copy_source=0x%08X source_word=0x%08X esp=0x%08X\n",
                    base, g_esi, source,
                    source <= 0x03FFFFFCu ? MEM32(source) : 0xFFFFFFFFu, g_esp);
            hash61570_reports++;
        }
    } else if (xbox_va == 0x000615C1u) {
        uint32_t base = MEM32(g_edi);
        uint32_t next = base <= 0x03FFFFFCu && g_esi <= 0x03FFFFFCu - base
                            ? MEM32(base + g_esi) : 0xFFFFFFFFu;
        if ((next == 9u || hash61570_reports < 24u) &&
            hash61570_reports < 60u) {
            fprintf(stderr,
                    "[SCDA-61570] pc=0x000615C1 base=0x%08X byte_offset=0x%08X "
                    "next_index=0x%08X esp=0x%08X\n",
                    base, g_esi, next, g_esp);
            hash61570_reports++;
        }
    } else if (xbox_va == 0x00046243u && !vector_stack_path_reported) {
        fprintf(stderr,
                "[SCDA-VECTOR-EBX] pc=0x00046243 after-47861 ebx=0x%08X esp=0x%08X\n",
                g_ebx, g_esp);
    } else if (xbox_va == 0x00047861u && !vector_stack_path_reported) {
        fprintf(stderr,
                "[SCDA-VECTOR-EBX] pc=0x00047861 entry ebx=0x%08X esp=0x%08X\n",
                g_ebx, g_esp);
    } else if (xbox_va == 0x00047965u && !vector_stack_path_reported) {
        fprintf(stderr,
                "[SCDA-VECTOR-EBX] pc=0x00047965 pre-restore ebx=0x%08X esp=0x%08X "
                "saved={edi=0x%08X,esi=0x%08X,ebx=0x%08X}\n",
                g_ebx, g_esp, MEM32(g_esp), MEM32(g_esp + 4u), MEM32(g_esp + 8u));
    } else if (xbox_va == 0x00072D07u && !vector_stack_path_reported) {
        fprintf(stderr,
                "[SCDA-VECTOR-EBX] pc=0x00072D07 before-5EE8E ebx=0x%08X esi=0x%08X esp=0x%08X\n",
                g_ebx, g_esi, g_esp);
    } else if (xbox_va == 0x00072D1Bu && !vector_stack_path_reported) {
        fprintf(stderr,
                "[SCDA-VECTOR-EBX] pc=0x00072D1B after-5EE8E ebx=0x%08X eax=0x%08X esi=0x%08X esp=0x%08X\n",
                g_ebx, g_eax, g_esi, g_esp);
    } else if (xbox_va == 0x00072D1Bu && file_manager_result_reports < 4000u) {
        uint32_t manager = MEM32(0x005EAE74u);
        uint32_t vtable = MEM32(manager);
        fprintf(stderr,
                "[SCDA-FM] pc=0x00072D1B manager=0x%08X vtable=0x%08X target_plus20=0x%08X eax_arg=0x%08X esp=0x%08X\n",
                manager, vtable, MEM32(vtable + 0x20u), g_eax, g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x00072D25u && file_manager_result_reports < 4000u) {
        fprintf(stderr,
                "[SCDA-FM] pc=0x00072D25 virtual-return eax=0x%08X ebx=0x%08X ecx=0x%08X esi_before=0x%08X esp=0x%08X\n",
                g_eax, g_ebx, g_ecx, g_esi, g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x00072D4Eu && file_manager_result_reports < 4000u) {
        fprintf(stderr,
                "[SCDA-FM] pc=0x00072D4E before-61570 eax=0x%08X ecx=0x%08X esi_container=0x%08X esp=0x%08X\n",
                g_eax, g_ecx, g_esi, g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x000477D2u && file_manager_result_reports < 4000u) {
        fprintf(stderr, "[SCDA-FM] pc=0x000477D2 primary-lookup eax=0x%08X esi=0x%08X ecx=0x%08X esp=0x%08X\n",
                g_eax, g_esi, g_ecx, g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x00047797u && file_manager_result_reports < 4000u) {
        file_manager_call_index++;
        fprintf(stderr, "[SCDA-FM] pc=0x00047797 vmethod-entry #%u ecx=0x%08X args={%08X,%08X,%08X,%08X} esp=0x%08X\n",
                file_manager_call_index, g_ecx, MEM32(g_esp + 4u), MEM32(g_esp + 8u), MEM32(g_esp + 12u), MEM32(g_esp + 16u), g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x000477ACu && file_manager_result_reports < 4000u) {
        fprintf(stderr, "[SCDA-FM] pc=0x000477AC 4721E-return eax=0x%08X ebx=0x%08X esp=0x%08X\n", g_eax, g_ebx, g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x0004785Cu && file_manager_result_reports < 4000u) {
        fprintf(stderr, "[SCDA-FM] pc=0x0004785C vmethod-early-return #%u eax=0x%08X ebx=0x%08X esp=0x%08X\n",
                file_manager_call_index, g_eax, g_ebx, g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x0004721Eu && file_manager_result_reports < 4000u) {
        fprintf(stderr, "[SCDA-FM] pc=0x0004721E lookup-entry ecx=0x%08X args={%08X,%08X} esp=0x%08X\n",
                g_ecx, MEM32(g_esp + 4u), MEM32(g_esp + 8u), g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x00047265u && file_manager_result_reports < 4000u) {
        fprintf(stderr, "[SCDA-FM-STACK] pc=0x00047265 after-67886+678C0 esp=0x%08X\n", g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x000472CCu && file_manager_result_reports < 4000u) {
        fprintf(stderr, "[SCDA-FM-STACK] pc=0x000472CC after-6784D esp=0x%08X\n", g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x000472F4u && file_manager_result_reports < 4000u) {
        fprintf(stderr, "[SCDA-FM-STACK] pc=0x000472F4 after-6787C esp=0x%08X\n", g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x000474ACu && file_manager_result_reports < 4000u) {
        fprintf(stderr, "[SCDA-FM-STACK] pc=0x000474AC before-jump esp=0x%08X eax=0x%08X\n", g_esp, g_eax);
        file_manager_result_reports++;
    } else if (xbox_va == 0x000474FCu && file_manager_result_reports < 4000u) {
        fprintf(stderr, "[SCDA-FM] pc=0x000474FC lookup-return eax=0x%08X ebx=0x%08X esi=0x%08X esp=0x%08X\n",
                g_eax, g_ebx, g_esi, g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x0004780Cu && file_manager_result_reports < 4000u) {
        fprintf(stderr, "[SCDA-FM] pc=0x0004780C fallback-lookup eax=0x%08X esi=0x%08X ecx=0x%08X esp=0x%08X\n",
                g_eax, g_esi, g_ecx, g_esp);
        file_manager_result_reports++;
    } else if (xbox_va == 0x00076402u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00076402 entry #%u prev=0x%08X arg_ebx=0x%08X arg_esi=0x%08X esp=0x%08X\n",
                actorlist_trace_reports, previous_pc, MEM32(g_esp + 4u), MEM32(g_esp + 8u), g_esp);
    } else if (xbox_va == 0x00036D4Au) {
        uint32_t head_val = 0xFFFFFFFFu;
        /* Guard the speculative read: only dereference ecx if it looks like
         * a plausible guest address (well within the ~64MB Xbox memory
         * window), otherwise the ring-buffer capture itself could crash
         * before the real crash handler ever runs. */
        if (g_ecx < 0x04000000u)
            head_val = MEM32(g_ecx + 0x24u);
        g_actorlist_ring_prev[g_actorlist_ring_index] = previous_pc;
        g_actorlist_ring_ecx[g_actorlist_ring_index] = g_ecx;
        g_actorlist_ring_head[g_actorlist_ring_index] = head_val;
        g_actorlist_ring_arg[g_actorlist_ring_index] = MEM32(g_esp + 4u);
        g_actorlist_ring_index = (g_actorlist_ring_index + 1) % SCDA_ACTORLIST_RING_SIZE;
        if (g_actorlist_ring_count < SCDA_ACTORLIST_RING_SIZE)
            g_actorlist_ring_count++;
        if (actorlist_trace_reports < 20000u) {
            actorlist_trace_reports++;
            fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00036D4A entry #%u prev=0x%08X ecx=0x%08X head=0x%08X arg=0x%08X esp=0x%08X\n",
                    actorlist_trace_reports, previous_pc, g_ecx, head_val, MEM32(g_esp + 4u), g_esp);
        }
    } else if (xbox_va == 0x001FA1BBu) {
        /* Bare entry, before this function's own "push ebp; ebp=esp"
         * executes -- so ebp+0xC/0x10/0x14/0x18/0x1C (as the function's own
         * body reads them) correspond to esp+8/0xC/0x10/0x14/0x18 here. */
        g_fa1bb_ring_ecx[g_fa1bb_ring_index] = g_ecx;
        g_fa1bb_ring_c[g_fa1bb_ring_index] = MEM32(g_esp + 8u);
        g_fa1bb_ring_10[g_fa1bb_ring_index] = MEM32(g_esp + 0xCu);
        g_fa1bb_ring_14[g_fa1bb_ring_index] = MEM32(g_esp + 0x10u);
        g_fa1bb_ring_18[g_fa1bb_ring_index] = MEM32(g_esp + 0x14u);
        g_fa1bb_ring_1c[g_fa1bb_ring_index] = MEM32(g_esp + 0x18u);
        g_fa1bb_ring_index = (g_fa1bb_ring_index + 1) % SCDA_ACTORLIST_RING_SIZE;
        if (g_fa1bb_ring_count < SCDA_ACTORLIST_RING_SIZE)
            g_fa1bb_ring_count++;
    } else if (xbox_va == 0x00036D58u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00036D58 iter #%u eax_node=0x%08X esp=0x%08X\n",
                actorlist_trace_reports, g_eax, g_esp);
    } else if (xbox_va == 0x000748BBu) {
        g_ctor_ring_prev[g_ctor_ring_index] = previous_pc;
        g_ctor_ring_this[g_ctor_ring_index] = g_ecx;
        g_ctor_ring_owner[g_ctor_ring_index] = MEM32(g_esp + 0x14u);
        g_ctor_ring_index = (g_ctor_ring_index + 1) % SCDA_ACTORLIST_RING_SIZE;
        if (g_ctor_ring_count < SCDA_ACTORLIST_RING_SIZE)
            g_ctor_ring_count++;
        if (actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x000748BB ctor-entry prev=0x%08X ecx=0x%08X esp=0x%08X "
                "args={%08X,%08X,%08X,%08X,%08X}\n",
                previous_pc, g_ecx, g_esp,
                MEM32(g_esp + 4u), MEM32(g_esp + 8u), MEM32(g_esp + 0xCu),
                MEM32(g_esp + 0x10u), MEM32(g_esp + 0x14u));
        }
    } else if (xbox_va == 0x000748EFu && actorlist_trace_reports < 20000u) {
        uint32_t owner = MEM32(g_esp + 0x18u);
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x000748EF ctor this=0x%08X owner=0x%08X "
                "owner_vtable=0x%08X esp=0x%08X\n",
                g_esi, owner, owner != 0 ? MEM32(owner) : 0xFFFFFFFFu, g_esp);
    } else if (xbox_va == 0x00246CC4u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00246CC4 edi-zeroed edi=0x%08X esp=0x%08X\n", g_edi, g_esp);
    } else if (xbox_va == 0x00246CD7u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00246CD7 after-45992a edi=0x%08X esp=0x%08X\n", g_edi, g_esp);
    } else if (xbox_va == 0x00246CDCu && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00246CDC after-3ba73 edi=0x%08X esp=0x%08X\n", g_edi, g_esp);
    } else if (xbox_va == 0x00246CE4u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00246CE4 after-1fa111 edi=0x%08X esp=0x%08X\n", g_edi, g_esp);
    } else if (xbox_va == 0x00246D0Du && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00246D0D after-45992b edi=0x%08X esp=0x%08X\n", g_edi, g_esp);
    } else if (xbox_va == 0x00246D15u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00246D15 after-244917 edi=0x%08X esp=0x%08X\n", g_edi, g_esp);
    } else if (xbox_va == 0x00244933u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00244933 after-5f4e6 edi=0x%08X eax=0x%08X esp=0x%08X\n",
                g_edi, g_eax, g_esp);
    } else if (xbox_va == 0x00076402u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00076402 bare-entry esp=0x%08X arg1=0x%08X\n",
                g_esp, MEM32(g_esp + 4u));
    } else if (xbox_va == 0x00076652u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00076652 epilogue esp=0x%08X edi_tobepopped=0x%08X\n",
                g_esp, MEM32(g_esp));
    } else if (xbox_va == 0x000764FDu && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x000764FD before-36ef5x3 esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x0007651Bu && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x0007651B after-36ef5x3 esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x0007652Du && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x0007652D after-710de esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x00076539u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00076539 after-70854 esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x000764D2u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x000764D2 before-36D4A-call esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x00076541u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00076541 found-branch esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x00076546u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00076546 after-5f354 esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x0007654Cu && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x0007654C after-7471e esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x000765EDu && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x000765ED after-loop esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x00076615u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00076615 before-icalls esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x00076627u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00076627 after-icall1 esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x00076630u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00076630 after-icall2 esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x0007663Cu && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x0007663C after-icall3 esp=0x%08X\n", g_esp);
    } else if (xbox_va == 0x00074902u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x00074902 link-call this=0x%08X ecx_owner=0x%08X esp=0x%08X\n",
                g_esi, g_ecx, g_esp);
    } else if (xbox_va == 0x000724A2u && actorlist_trace_reports < 20000u) {
        actorlist_trace_reports++;
        fprintf(stderr, "[SCDA-ACTORLIST] pc=0x000724A2 entry #%u prev=0x%08X ecx=0x%08X esp=0x%08X\n",
                actorlist_trace_reports, previous_pc, g_ecx, g_esp);
    } else if (xbox_va == 0x00045B3Cu && strcmp_arg_reports < 20000u) {
        /* Top of sub_00045AFE's per-element loop body (before esi is
         * recomputed). Tracking edi here on every single iteration to
         * catch the exact iteration where it silently changes value --
         * would confirm/refute an edi push/pop imbalance in one of the
         * loop's nested calls (sub_00045C06, sub_000371A6, sub_00035FF3,
         * sub_0025C273, sub_00036116, ...), the same bug family as
         * Seeds 33/34/36 this session. */
        static uint32_t last_loop_edi;
        if (last_loop_edi != g_edi) {
            fprintf(stderr, "[SCDA-STRARG] pc=0x00045B3C EDI CHANGED old=0x%08X new=0x%08X esp=0x%08X\n",
                    last_loop_edi, g_edi, g_esp);
            last_loop_edi = g_edi;
        }
    } else if (xbox_va == 0x00045B27u && strcmp_arg_reports < 20000u) {
        /* sub_00045AFE, right where it checks MEM32(edi+0x24) (loop bound,
         * confirmed via source: loop increments an index 0..count and the
         * per-element pointer is MEM32(edi+0x20) + index*0x18, so +0x20 is
         * the array base and +0x24 is the element count). Logged once per
         * call to see whether the array's own reported base/count look
         * sane (small, plausible) or corrupted (huge/garbage) before any
         * iteration runs. */
        fprintf(stderr, "[SCDA-STRARG] pc=0x00045B27 array-entry edi=0x%08X base(+0x20)=0x%08X count(+0x24)=0x%08X "
                "field(+0x1C)=0x%08X field(+0x28)=0x%08X esp=0x%08X\n",
                g_edi, MEM32(g_edi + 0x20u), MEM32(g_edi + 0x24u),
                MEM32(g_edi + 0x1Cu), MEM32(g_edi + 0x28u), g_esp);
    } else if (xbox_va == 0x00045B49u && strcmp_arg_reports < 20000u) {
        /* sub_00045AFE: esi = MEM32(edi+0x20) + MEM32(ebp+0x10) (array
         * base + index*stride) was just computed; this branch is only
         * taken when MEM32(esi+4) != 0, and about to read MEM32(esi) as
         * the "ecx" it hands onward. Logged here (before any further use)
         * to see whether the array element itself is already garbage. */
        fprintf(stderr, "[SCDA-STRARG] pc=0x00045B49 elem-field-nonzero edi=0x%08X esi=0x%08X ecx=MEM32(esi)=0x%08X esp=0x%08X\n",
                g_edi, g_esi, MEM32(g_esi), g_esp);
    } else if (xbox_va == 0x00045B4Du && strcmp_arg_reports < 20000u) {
        fprintf(stderr, "[SCDA-STRARG] pc=0x00045B4D elem-field-zero edi=0x%08X esi=0x%08X esp=0x%08X\n",
                g_edi, g_esi, g_esp);
    } else if (xbox_va == 0x00045B6Du && strcmp_arg_reports < 20000u) {
        /* sub_00045AFE, just before it decides esi's source for the
         * sub_00045C06 call: g_ebx here is still the OLD (pre-reassignment)
         * value the je test actually branches on. */
        fprintf(stderr, "[SCDA-STRARG] pc=0x00045B6D old_ebx=0x%08X esi=0x%08X esp=0x%08X\n",
                g_ebx, g_esi, g_esp);
    } else if (xbox_va == 0x00045B76u && strcmp_arg_reports < 20000u) {
        /* Taken only when old_ebx != 0: esi = MEM32(esi), a dereference of
         * whatever esi already pointed to. Log both the pointer and what it
         * points at so a garbage read is visible before it propagates. */
        fprintf(stderr, "[SCDA-STRARG] pc=0x00045B76 deref edi=0x%08X esi_before=0x%08X MEM32(esi)=0x%08X esp=0x%08X\n",
                g_edi, g_esi, MEM32(g_esi), g_esp);
    } else if (xbox_va == 0x00045B7Cu && strcmp_arg_reports < 20000u) {
        strcmp_arg_reports++;
        fprintf(stderr, "[SCDA-STRARG] pc=0x00045B7C call-45C06 #%u edi=0x%08X esi_arg=0x%08X esp=0x%08X\n",
                strcmp_arg_reports, g_edi, g_esi, g_esp);
    } else if (xbox_va == 0x000366A2u) {
        /* 2026-09-03: sub_000366A2's own entry -- MEM32(g_esp) gives the
         * real caller's guest return address regardless of whether it
         * was reached via a static call or an indirect/virtual
         * dispatch, sidestepping the need to fully resolve the vtable
         * family it belongs to. */
        fprintf(stderr, "[SCDA-366A2-CALLER] ret=0x%08X ecx=0x%08X esp=0x%08X\n",
                MEM32(g_esp), g_ecx, g_esp);
        fflush(stderr);
    } else if (xbox_va == 0x0004A2D6u) {
        /* 2026-09-03: the true initializer of MEM32(0x5B57FC) (the
         * archive/file-manager pointer gating the registry-teardown
         * path). If both startup_status_140/144 are 0 here, it gets
         * the fixed placeholder 0x5E6E20; otherwise MEM32(0x5B5800)
         * (the presumably-real, fully-initialized manager). Record
         * both status globals and MEM32(0x5B5800) directly, at this
         * exact point, rather than inferring from later trace lines. */
        fprintf(stderr,
                "[SCDA-5B57FC-INIT] status140=0x%08X status144=0x%08X "
                "mem5B5800=0x%08X\n",
                MEM32(0x5ED140u), MEM32(0x5ED144u), MEM32(0x5B5800u));
        fflush(stderr);
    } else if (xbox_va == 0x0004A2FCu) {
        fprintf(stderr, "[SCDA-5B57FC-RESULT] mem5B57FC=0x%08X\n", MEM32(0x5B57FCu));
        fflush(stderr);
    } else if (xbox_va == 0x00047150u) {
        /* 2026-09-03: direct verification -- static analysis of
         * sub_00047150's only known caller (sub_00046F46, via
         * sub_00047A92, which passes literal args 0/1) suggested it
         * should be structurally unreachable via that path. Confirm
         * dynamically whether/how it's actually entered. */
        fprintf(stderr, "[SCDA-47150-ENTRY] ret=0x%08X ecx=0x%08X esp=0x%08X mem5B57FC=0x%08X\n",
                MEM32(g_esp), g_ecx, g_esp, MEM32(0x5B57FCu));
        fflush(stderr);
        g_scda_teardown_event_seq++;
        fprintf(stderr,
                "[SCDA-CHAIN] E%u OUTER pc=0x00047150 ret=0x%08X this=0x%08X "
                "entry_array=0x%08X entry_count=%u bin6_len=%u\n",
                g_scda_teardown_event_seq, MEM32(g_esp), g_ecx,
                MEM32(g_ecx), MEM32(g_ecx + 4u), scda_bin6_chain_len());
        fflush(stderr);
    } else if (xbox_va == 0x00045FB0u) {
        /* sub_00045FB0(ecx=hashtable_obj) tail-jumps into sub_00045F9B,
         * which reads MEM32(ecx+4) as the bucket COUNT and MEM32(ecx)
         * as the bucket array base for sub_00045F56's real destroy-N
         * loop (stride 0x1C). This is the count that ultimately drives
         * the bin[6] push cluster feeding the pre-Maps-loop deficit. */
        fprintf(stderr,
                "[SCDA-45FB0-CALLER] pc=0x00045FB0 ret=0x%08X container=0x%08X "
                "bucket_array=0x%08X bucket_count=%u(0x%X) esp=0x%08X\n",
                MEM32(g_esp), g_ecx, MEM32(g_ecx), MEM32(g_ecx + 4u),
                MEM32(g_ecx + 4u), g_esp);
        fflush(stderr);
        g_scda_teardown_event_seq++;
        fprintf(stderr,
                "[SCDA-CHAIN] E%u INNER pc=0x00045FB0 ret=0x%08X container=0x%08X "
                "bucket_array=0x%08X bucket_count=%u bin6_len=%u\n",
                g_scda_teardown_event_seq, MEM32(g_esp), g_ecx,
                MEM32(g_ecx), MEM32(g_ecx + 4u), scda_bin6_chain_len());
        fflush(stderr);
    } else if (xbox_va == 0x00047A76u) {
        /* Destructor entry (scalar-deleting-destructor wrapper). ecx is
         * the object; MEM32(ecx) is its vtable pointer. */
        g_scda_teardown_event_seq++;
        fprintf(stderr,
                "[SCDA-CHAIN] E%u DTOR pc=0x00047A76 ret=0x%08X this=0x%08X "
                "vtable=0x%08X bin6_len=%u\n",
                g_scda_teardown_event_seq, MEM32(g_esp), g_ecx,
                MEM32(g_ecx), scda_bin6_chain_len());
        fflush(stderr);
    } else if (xbox_va == 0x00045F56u) {
        /* Real destroy-N loop entry: this=ecx (container), args are
         * still on the stack (trace fires before the "push ebp" of this
         * function's own prologue) -- start=MEM32(g_esp+4),
         * count=MEM32(g_esp+8) per the C source's own
         * "eax=MEM32(ebp+0xC); edi=MEM32(ebp+8); eax=eax+edi" reading
         * (arg1=start index, arg2=count), stride 0x1C. */
        g_scda_teardown_event_seq++;
        fprintf(stderr,
                "[SCDA-CHAIN] E%u DESTROYN pc=0x00045F56 ret=0x%08X this=0x%08X "
                "start=%u count=%u bin6_len=%u\n",
                g_scda_teardown_event_seq, MEM32(g_esp), g_ecx,
                MEM32(g_esp + 4u), MEM32(g_esp + 8u), scda_bin6_chain_len());
        fflush(stderr);
    } else if (xbox_va == 0x00046F46u) {
        /* sub_00046F46's own entry: RECOMP_TRACE_PC fires before its
         * "PUSH32(esp,ebp)" prologue instruction, so g_esp here is
         * still the pristine entry esp (return-address slot). */
        g_scda_46f46_entry_esp = g_esp;
    } else if (xbox_va == 0x0004707Eu || xbox_va == 0x00047086u) {
        /* Net-stack-effect probe around ONE specific sub_00036116 call
         * inside sub_00047008 (which itself tail-chains through
         * sub_000360A3 -> sub_0024A50B -> ...) -- 0x4707E is
         * immediately before the call (esp not yet touched by it),
         * 0x47086 is immediately after it returns. A thiscall with no
         * stack args should show esp unchanged between the two. */
        fprintf(stderr, "[SCDA-36116-NETPROBE] pc=0x%08X esp=0x%08X\n",
                xbox_va, g_esp);
        fflush(stderr);
    } else if (xbox_va == 0x001D5208u) {
        /* 2026-09-03 (continued 12/13): sub_001D5208's own entry --
         * capture entry ESP to reconstruct its true (private,
         * non-g_ebp-aliased) "ebp" local later, the same way
         * sub_00046F46's was. Also sequence-number each call -- proven
         * via xemu (real hardware) to be called 32 times, once per
         * Maps file, via an icall at 0x1D5BB6 inside sub_001D589D's
         * loop, always with the SAME edi parameter (0x0081FD40 on real
         * hardware, constant across all 3 sampled calls) -- i.e. edi
         * is sub_001D589D's OWN register, expected to survive
         * unchanged across all 32 icalls. */
        g_scda_1d5208_entry_esp = g_esp;
        g_scda_1d5208_call_seq++;
        fprintf(stderr, "[SCDA-1D5208-ENTRY] call_seq=%u ecx=0x%08X "
                "param_edi=0x%08X param2=0x%08X esp=0x%08X\n",
                g_scda_1d5208_call_seq, g_ecx, MEM32(g_esp + 4u),
                MEM32(g_esp + 8u), g_esp);
        fflush(stderr);
    } else if (xbox_va == 0x001D5770u) {
        /* sub_001D5208's own epilogue block (pop edi; xor eax,eax;
         * pop esi; inc eax; pop ebx; leave; ret 8) -- check whether ESP
         * is balanced relative to entry (expected_esp = entry_esp,
         * since the trace fires before any of this block's own pops)
         * for the SPECIFIC call currently executing, to see whether
         * imbalance is present only on the (single) successful call or
         * on every call. */
        if (g_scda_1d5208_entry_esp != 0) {
            /* prologue: push ebp(4) + sub esp,0x1C(0x1C) + push ebx(4)
             * + push esi(4) + push edi(4) = 0x2C bytes below entry_esp,
             * assuming no unbalanced pushes remain inside the body. */
            uint32_t expected_esp = g_scda_1d5208_entry_esp - 0x2Cu;
            fprintf(stderr,
                    "[SCDA-1D5208-EPILOGUE] call_seq=%u entry_esp=0x%08X "
                    "expected_esp=0x%08X live_esp=0x%08X balanced=%d\n",
                    g_scda_1d5208_call_seq, g_scda_1d5208_entry_esp,
                    expected_esp, g_esp, g_esp == expected_esp);
            fflush(stderr);
        }
    } else if (xbox_va == 0x001D5243u || xbox_va == 0x001D52CFu ||
               xbox_va == 0x001D5334u || xbox_va == 0x001D539Bu ||
               xbox_va == 0x001D53E7u || xbox_va == 0x001D5453u ||
               xbox_va == 0x001D54BEu || xbox_va == 0x001D551Eu ||
               xbox_va == 0x001D5573u || xbox_va == 0x001D55B6u ||
               /* 2026-09-03 (continued 18): fine bisection inside the two
                * 360-byte jumps (0x1D53E7->0x1D5453 and 0x1D5453->
                * 0x1D54BE), each containing three MEM32(edx+4)-slot
                * icalls (vs. the earlier, proven-balanced MEM32(edx+0xC)
                * slot calls) -- checkpoint right after each one. */
               xbox_va == 0x001D53FCu || xbox_va == 0x001D5420u ||
               xbox_va == 0x001D5444u || xbox_va == 0x001D5468u ||
               xbox_va == 0x001D548Cu || xbox_va == 0x001D54AFu) {
        /* 2026-09-03 (continued 17): coarse bisection sweep across the
         * ~100 blocks between sub_001D5208's prologue and the 0x1D55D7
         * icall -- the 720-byte deficit was proven to already be fully
         * present by the time execution reaches 0x1D55BF (right before
         * that icall's own pushes), and sub_0004628D itself was proven
         * to return perfectly balanced. So the real leak is somewhere in
         * this large stretch of straight-line field-lookup/name-push
         * blocks (the repeating sub_00036DF2/sub_00037CA7/sub_00036058/
         * sub_00036116 cluster). Evenly-spaced checkpoints to narrow it
         * down in one pass instead of one rebuild per candidate. */
        if (g_scda_1d5208_entry_esp != 0) {
            uint32_t expected_esp = g_scda_1d5208_entry_esp - 0x2Cu;
            fprintf(stderr,
                    "[SCDA-1D5208-SWEEP] pc=0x%08X call_seq=%u "
                    "expected_esp=0x%08X live_esp=0x%08X deficit=%d\n",
                    xbox_va, g_scda_1d5208_call_seq, expected_esp, g_esp,
                    (int)(expected_esp - g_esp));
            fflush(stderr);
        }
    } else if (xbox_va == 0x001D5634u || xbox_va == 0x001D5749u ||
               xbox_va == 0x001D5753u || xbox_va == 0x001D5760u ||
               xbox_va == 0x001D5768u) {
        /* 2026-09-03 (continued 13): finer ESP-balance bisection through
         * sub_001D5208's success-only tail (the only code path call_seq
         * #1 takes that #2-32 never reach) -- proven to leave a 0x2D0
         * (720) byte deficit by the time it reaches its own epilogue
         * (0x1D5770). No "add esp" instructions exist anywhere in this
         * function's disassembly, so the entire cleanup is via callee
         * "ret N" -- if any callee's ret N is smaller than what this
         * function pushed for it, esp drifts and never gets corrected. */
        if (g_scda_1d5208_entry_esp != 0) {
            uint32_t expected_esp = g_scda_1d5208_entry_esp - 0x2Cu;
            fprintf(stderr,
                    "[SCDA-1D5208-MIDBISECT] pc=0x%08X call_seq=%u "
                    "expected_esp=0x%08X live_esp=0x%08X deficit=%d\n",
                    xbox_va, g_scda_1d5208_call_seq, expected_esp, g_esp,
                    (int)(expected_esp - g_esp));
            fflush(stderr);
        }
    } else if (xbox_va == 0x001D55DAu) {
        /* 2026-09-03 (continued 15): the 720-byte deficit was proven to
         * ALREADY be present at guest_esp_before of the 0x1D560B icall
         * (captured right before that call's own 4 pushes) -- i.e.
         * BEFORE sub_0004628D is ever dispatched. The only thing between
         * sub_001D5208's prologue and that point is the EARLIER icall at
         * 0x1D55D7 (vtable+0xC on edi, 4 args). This checkpoint is the
         * very first block after THAT icall returns, before any further
         * pushes -- if the deficit already shows up here, the leak is in
         * the 0x1D55D7 dispatch, not 0x1D560B/sub_0004628D at all. */
        if (g_scda_1d5208_entry_esp != 0) {
            uint32_t expected_esp = g_scda_1d5208_entry_esp - 0x2Cu;
            fprintf(stderr,
                    "[SCDA-1D55DA-POST-FIRST-ICALL] call_seq=%u "
                    "expected_esp=0x%08X live_esp=0x%08X deficit=%d\n",
                    g_scda_1d5208_call_seq, expected_esp, g_esp,
                    (int)(expected_esp - g_esp));
            fflush(stderr);
        }
    } else if (xbox_va == 0x001D560Eu) {
        /* Right after the icall at 0x1D560B (vtable+0xC on the "edi"
         * object, args: ebx, 0x54CCC8, &[ebp-0x1C], esi) -- a type/name
         * lookup whose result at [ebp-0x18] gates whether the
         * subsequent sub_00043A2C "get-or-create singleton" call (and
         * hence the whole 0x6513A4/0x6513A8 registry population for
         * this Maps-loop iteration) runs at all ("je 0x1D5768" skips it
         * on failure). This is the branch bisection point for the
         * proven registry-population root cause (see docs/SCDA_WORKLOG.md,
         * continued 11/12). */
        if (g_scda_1d5208_entry_esp != 0) {
            uint32_t true_ebp = g_scda_1d5208_entry_esp - 4u;
            uint32_t lookup_result = MEM32(true_ebp - 24u);
            fprintf(stderr,
                    "[SCDA-1D560E-TYPELOOKUP] pc=0x001D560E true_ebp=0x%08X "
                    "lookup_result=0x%08X will_skip=%d eax=0x%08X ecx=0x%08X "
                    "edx=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X\n",
                    true_ebp, lookup_result, lookup_result == 0,
                    g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp);
            fflush(stderr);
        }
    } else if (xbox_va == 0x00043A2Cu) {
        /* 2026-09-03 (continued 13): sub_00043A2C's own entry -- the
         * 0x2D0-byte stack deficit proven at sub_001D5208's own
         * 0x1D5634 checkpoint (right after this call returns) is
         * already fully present there, meaning the leak is inside this
         * call (or its own callees) or inside the immediately-preceding
         * 0x1D560B type-lookup icall -- bisecting this function's own
         * body next, since it contains a proven loop
         * (loc_00043A3E..loc_00043A95, "jg 0x43a3e") that could
         * accumulate a per-iteration leak. */
        g_scda_43a2c_entry_esp = g_esp;
    } else if (xbox_va == 0x00043A3Eu) {
        /* Loop top/continue point inside sub_00043A2C -- sampled on
         * every iteration (fires many times per call), to see whether
         * esp drifts progressively or jumps all at once. */
        if (g_scda_43a2c_entry_esp != 0) {
            uint32_t expected_esp = g_scda_43a2c_entry_esp - 0x24u;
            fprintf(stderr,
                    "[SCDA-43A2C-LOOP] esi=0x%08X expected_esp=0x%08X "
                    "live_esp=0x%08X deficit=%d\n",
                    g_esi, expected_esp, g_esp,
                    (int)(expected_esp - g_esp));
            fflush(stderr);
        }
    } else if (xbox_va == 0x00043AA7u) {
        /* Loop-exit / function-tail point. */
        if (g_scda_43a2c_entry_esp != 0) {
            uint32_t expected_esp = g_scda_43a2c_entry_esp - 0x24u;
            fprintf(stderr,
                    "[SCDA-43A2C-EXIT] expected_esp=0x%08X live_esp=0x%08X "
                    "deficit=%d\n",
                    expected_esp, g_esp, (int)(expected_esp - g_esp));
            fflush(stderr);
            g_scda_43a2c_entry_esp = 0;
        }
    } else if (xbox_va == 0x0004628Du) {
        g_scda_4628d_entry_esp = g_esp;
    } else if (xbox_va == 0x000462E4u) {
        /* Right before this function's own POP32(edi)/POP32(esi)/
         * esp=ebp/POP32(ebp)/ret 0x10. If esp isn't exactly
         * entry_esp - 0x1C here (push ebp(4) + the chkstk call's own net
         * ESP effect... no, chkstk's effect is captured in entry_esp
         * already at THIS checkpoint since it happened after entry -- so
         * compare directly against entry_esp itself is wrong; instead
         * just report live_esp and entry_esp raw so the actual formula
         * can be derived from real numbers instead of guessed. */
        if (g_scda_4628d_entry_esp != 0) {
            fprintf(stderr,
                    "[SCDA-4628D-PRE-EPILOGUE] entry_esp=0x%08X "
                    "live_esp=0x%08X esi=0x%08X edi=0x%08X\n",
                    g_scda_4628d_entry_esp, g_esp, g_esi, g_edi);
            fflush(stderr);
            g_scda_4628d_entry_esp = 0;
        }
    } else if (xbox_va == 0x000462D3u) {
        /* 2026-09-03: paired balance check for sub_0004628D's own nested
         * icall at 0x462D0 (`call dword ptr [eax+0x10]`, 7 stack args =
         * 28 bytes). g_scda_462d0_pre_esp was captured by
         * scda_icall_probe() from the RECOMP_ICALL_SAFE saved_esp (esp
         * right before the 7 pushes). If the resolved callee's own `ret`
         * doesn't clean up exactly what was pushed, live esp here will
         * differ from that baseline -- this is the leading candidate for
         * the 720-byte (0x2D0) deficit already proven present by
         * loc_001D5634 in sub_001D5208. */
        if (g_scda_462d0_pre_esp != 0) {
            fprintf(stderr,
                    "[SCDA-462D0-BALANCE] pre_esp=0x%08X live_esp=0x%08X "
                    "deficit=%d eax=0x%08X\n",
                    g_scda_462d0_pre_esp, g_esp,
                    (int)(g_scda_462d0_pre_esp - g_esp), g_eax);
            fflush(stderr);
            g_scda_462d0_pre_esp = 0;
        }
    } else if (xbox_va == 0x001D58F6u) {
        /* 2026-09-03 (continued 13): CORRECTED attribution -- the prior
         * portion's "edi bisection" mistakenly attributed 0x1D5770 to
         * sub_001D5208 and 0x1D5BB9 to the same function; 0x1D5770 is
         * actually sub_001D5208's OWN epilogue ("pop edi; ...; ret 8"),
         * and 0x1D5BB9 belongs to a wholly different, later function
         * (sub_001D589D, the already-established "MAPSLOOP" function).
         * There is no direct edi-continuity between them at all -- that
         * whole prior bisection chain is superseded by this finding.
         *
         * The REAL source: sub_001D589D's own loc_001D58F0 calls
         * sub_00047A53 (the well-known 48-byte/bin[6] allocator
         * wrapper) and loc_001D58F6 does "edi = eax" -- i.e. edi is a
         * FRESH allocation result, not a carried-over parameter. eax
         * here is the live post-call return value (trace fires before
         * "edi=eax" executes, but eax itself is already the real
         * allocator result). If the allocation fails (returns 0), edi
         * becomes 0 and the whole per-file registry-push section is
         * skipped (following code jumps to loc_001D5F57). This directly
         * explains the observed 1-success/31-failure pattern without
         * needing any register-corruption theory. */
        fprintf(stderr,
                "[SCDA-47A53-ALLOC-RESULT] pc=0x001D58F6 alloc_result=0x%08X "
                "bin6_len=%u esp=0x%08X ecx=0x%08X\n",
                g_eax, scda_bin6_chain_len(), g_esp, g_ecx);
        fflush(stderr);
    } else if (xbox_va == 0x0003EDBFu) {
        /* 2026-09-03: direct, macro-independent proof that scda_recomp
         * actually enters sub_0003EDBF (the resolved icall target at
         * the 0x71BE6 callsite) -- fires unconditionally on guest-PC
         * entry via the normal RECOMP_TRACE_PC path, regardless of
         * whether RECOMP_SCDA_ICALL_PROBE's own dispatch-time hook
         * works. sub_0003EDBF is a well-formed, correctly-detected
         * 13-byte allocator-delete thunk (push [esp+4]; call
         * sub_00077B38; pop ecx; ret 0x10) -- confirmed via
         * tools/disasm/output/scda_v060/asm/text.asm. */
        fprintf(stderr, "[SCDA-3EDBF-ENTRY] pc=0x0003EDBF ecx=0x%08X esp=0x%08X "
                "arg0=0x%08X\n",
                g_ecx, g_esp, MEM32(g_esp + 4u));
        fflush(stderr);
    } else if (xbox_va == 0x00046F8Fu || xbox_va == 0x00046FB0u ||
               xbox_va == 0x00046F63u) {
        /* Finer bisection inside sub_00046F46's own loop: 0x46F63 is
         * the top of each iteration (baseline), 0x46F8F is right
         * before sub_00036EA0/sub_00036EC0, 0x46FB0 is right before
         * sub_00047008 -- narrows down which specific call introduces
         * the 0x54-byte deficit. */
        if (g_scda_46f46_entry_esp != 0) {
            uint32_t true_ebp = g_scda_46f46_entry_esp - 4u;
            fprintf(stderr,
                    "[SCDA-46F46-LOOP] pc=0x%08X true_ebp=0x%08X "
                    "expected_esp=0x%08X live_esp=0x%08X\n",
                    xbox_va, true_ebp, true_ebp - 0x14u, g_esp);
            fflush(stderr);
        }
    } else if (xbox_va == 0x00046FC5u) {
        /* Loop-exit checkpoint (the loop at loc_00046F63..loc_00046FBF,
         * calling sub_00045992/sub_00036EA0/sub_00036EC0/sub_00047008
         * conditionally per iteration, runs BEFORE the branch that
         * leads to sub_00047150) -- bisect whether the deficit is
         * already present here (introduced inside this loop) or still
         * absent (introduced later, between here and loc_00046FEB). */
        if (g_scda_46f46_entry_esp != 0) {
            uint32_t true_ebp = g_scda_46f46_entry_esp - 4u;
            fprintf(stderr,
                    "[SCDA-46F46-LOOPEXIT] pc=0x00046FC5 true_ebp=0x%08X "
                    "expected_esp=0x%08X live_esp=0x%08X\n",
                    true_ebp, true_ebp - 0x14u, g_esp);
            fflush(stderr);
        }
    } else if (xbox_va == 0x00046FEBu) {
        /* Right before sub_00047150 (E4/OUTER) is called -- baseline
         * esp check to bisect whether the 0x54-byte deficit already
         * exists BEFORE this call (introduced earlier in sub_00046F46,
         * e.g. in its own loop calling sub_00045992) or is introduced
         * BY sub_00047150/its callees. */
        if (g_scda_46f46_entry_esp != 0) {
            uint32_t true_ebp = g_scda_46f46_entry_esp - 4u;
            fprintf(stderr,
                    "[SCDA-46F46-PRE47150] pc=0x00046FEB true_ebp=0x%08X "
                    "expected_esp=0x%08X live_esp=0x%08X\n",
                    true_ebp, true_ebp - 0x14u, g_esp);
            fflush(stderr);
        }
    } else if (xbox_va == 0x00046FF3u) {
        /* Right after sub_00047150 (E4/OUTER) returns inside
         * sub_00046F46 -- check whether the CALLER's saved esi
         * (sub_00047A92's object pointer, pushed at sub_00046F46's own
         * entry and living at [true_ebp-0x10] per its prologue's push
         * order: ebp,ecx,ecx,ebx,esi,edi) is already corrupted by this
         * point. true_ebp = entry_esp - 4 (one push of the old ebp). */
        if (g_scda_46f46_entry_esp != 0) {
            uint32_t true_ebp = g_scda_46f46_entry_esp - 4u;
            fprintf(stderr,
                    "[SCDA-46F46-CHECK] pc=0x00046FF3 true_ebp=0x%08X "
                    "expected_esp=0x%08X live_esp=0x%08X "
                    "saved_esi_slot=0x%08X live_esi=0x%08X\n",
                    true_ebp, true_ebp - 0x14u, g_esp,
                    MEM32(true_ebp - 0x10u), g_esi);
            fflush(stderr);
        }
    } else if (xbox_va == 0x00047001u) {
        if (g_scda_46f46_entry_esp != 0) {
            uint32_t true_ebp = g_scda_46f46_entry_esp - 4u;
            fprintf(stderr,
                    "[SCDA-46F46-EXIT] pc=0x00047001 true_ebp=0x%08X "
                    "expected_esp=0x%08X live_esp=0x%08X saved_esi_slot=0x%08X\n",
                    true_ebp, true_ebp - 0x14u, g_esp,
                    MEM32(true_ebp - 0x10u));
            fflush(stderr);
        }
        g_scda_46f46_entry_esp = 0;
    } else if (xbox_va == 0x00047A92u) {
        /* 2026-09-03 (continued 7): sub_00047A92 -- already known to be
         * called unconditionally right after E3/sub_00047A76 -- reads
         * its own object (esi==ecx here) BEFORE any of its own field
         * frees (esi+0x24 via sub_00036116, then esi+0x18 via
         * sub_00036116 again -- xemu proved THIS SECOND call is the
         * first bin[6]-changing operation with no scda_recomp
         * counterpart in the previous capture) and the sub_0004606B
         * call for esi+4, then resets the vtable to the base class. */
        g_scda_teardown_event_seq++;
        fprintf(stderr,
                "[SCDA-CHAIN] E%u A92-ENTRY pc=0x00047A92 ret=0x%08X this=0x%08X "
                "field24_data=0x%08X field24_size=%u field24_cap=%u "
                "field18_data=0x%08X field18_size=%u field18_cap=%u "
                "bin6_len=%u\n",
                g_scda_teardown_event_seq, MEM32(g_esp), g_ecx,
                MEM32(g_ecx + 0x24u), MEM32(g_ecx + 0x28u), MEM32(g_ecx + 0x2Cu),
                MEM32(g_ecx + 0x18u), MEM32(g_ecx + 0x1Cu), MEM32(g_ecx + 0x20u),
                scda_bin6_chain_len());
        fflush(stderr);
    } else if (xbox_va == 0x00047AA4u) {
        /* Right before the FIRST field free (esi+0x24). NOTE: ecx is
         * NOT yet valid here -- "ecx = esi + 0x24" is the *next*
         * statement in this same block, executed after this trace
         * fires, so read the field via g_esi (callee-preserved across
         * sub_00046F46 -- verified: it saves/restores esi via a
         * PUSH/POP pair around its own body) instead. */
        fprintf(stderr,
                "[SCDA-A92-FIELD24] pc=0x00047AA4 esi=0x%08X field_addr=0x%08X "
                "data=0x%08X size=%u capacity=%u bin6_len=%u\n",
                g_esi, g_esi + 0x24u, MEM32(g_esi + 0x24u),
                MEM32(g_esi + 0x28u), MEM32(g_esi + 0x2Cu),
                scda_bin6_chain_len());
        fflush(stderr);
    } else if (xbox_va == 0x00047AACu) {
        /* Right before the SECOND field free (esi+0x18) -- the one
         * proven on real hardware to be the first bin[6]-changing
         * operation with no scda_recomp counterpart. Same ecx caveat
         * as above -- use g_esi. */
        fprintf(stderr,
                "[SCDA-A92-FIELD18] pc=0x00047AAC esi=0x%08X field_addr=0x%08X "
                "data=0x%08X size=%u capacity=%u bin6_len=%u\n",
                g_esi, g_esi + 0x18u, MEM32(g_esi + 0x18u),
                MEM32(g_esi + 0x1Cu), MEM32(g_esi + 0x20u),
                scda_bin6_chain_len());
        fflush(stderr);
    } else if (xbox_va == 0x00047AB4u) {
        fprintf(stderr, "[SCDA-A92-FIELD18-DONE] pc=0x00047AB4 bin6_len=%u\n",
                scda_bin6_chain_len());
        fflush(stderr);
    } else if (xbox_va == 0x00045F37u) {
        fprintf(stderr, "[SCDA-45F37-CALLER] pc=0x00045F37 ret=0x%08X container=0x%08X esp=0x%08X\n",
                MEM32(g_esp), g_ecx, g_esp);
        fflush(stderr);
    } else if (xbox_va == 0x00045599u) {
        /* 2026-09-02 (continued 6): dedicated, unconditional print (the
         * shared last_cluster_edi de-dup below suppresses this address
         * whenever edi happens to match a recent unrelated hit) -- who
         * calls this specific link in the generic vector-clear chain,
         * right before the pre-Maps-loop bin[6] push cluster. */
        fprintf(stderr,
                "[SCDA-45599-CALLER] pc=0x00045599 ret=0x%08X container=0x%08X "
                "data=0x%08X count=%u(0x%X) capacity=%u(0x%X) esp=0x%08X "
                "eax=0x%08X edx=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X "
                "bin6_len_before=%u\n",
                MEM32(g_esp), g_ecx, MEM32(g_ecx), MEM32(g_ecx + 4u),
                MEM32(g_ecx + 4u), MEM32(g_ecx + 8u), MEM32(g_ecx + 8u),
                g_esp, g_eax, g_edx, g_ebx, g_esi, g_edi,
                scda_bin6_chain_len());
        fflush(stderr);
    } else if ((xbox_va == 0x00036116u || xbox_va == 0x00077049u ||
                xbox_va == 0x0007708Fu || xbox_va == 0x00077088u ||
                xbox_va == 0x0007714Cu || xbox_va == 0x00077159u ||
                xbox_va == 0x00077171u || xbox_va == 0x00077172u ||
                xbox_va == 0x00077173u || xbox_va == 0x00077AC5u ||
                xbox_va == 0x00077AC6u || xbox_va == 0x00045599u) &&
               strcmp_arg_reports < 20000u) {
        /* Bisecting the small-block-allocator free-path cluster reached
         * from sub_00036116 (destructor call inside sub_00045AFE's loop):
         * change-only logging of g_edi at each fragment's own entry point,
         * to find exactly which one enters with the loop's correct
         * edi=0x00F7FD84 and exits (or is entered next) with the wrong
         * edi=0x0119B110 -- the same bisection technique used for Seed 35's
         * icall investigation. */
        static uint32_t last_cluster_edi = 0xFFFFFFFFu;
        if (last_cluster_edi != g_edi) {
            /* 2026-09-02: also log the guest return address on top of
             * the stack -- valid here since 0x00036116/0x00077049/etc.
             * are all reached via a genuine "call", so MEM32(g_esp) is
             * the real caller's return PC at this exact entry point.
             * Lets a specific pass in this generically-reused cluster
             * be attributed to its actual enclosing function. */
            fprintf(stderr, "[SCDA-CLUSTER] pc=0x%08X edi_now=0x%08X (was 0x%08X) esp=0x%08X ret=0x%08X ret2=0x%08X ret3=0x%08X start_arg=0x%08X count_arg=0x%08X\n",
                    xbox_va, g_edi, last_cluster_edi, g_esp, MEM32(g_esp), MEM32(g_esp + 8u),
                    MEM32(g_esp + 28u), MEM32(g_esp + 32u), MEM32(g_esp + 36u));
            last_cluster_edi = g_edi;
        }
    } else if (xbox_va == 0x00047858u && file_manager_result_reports < 4000u) {
        fprintf(stderr, "[SCDA-FM] pc=0x00047858 return #%u eax=0x%08X esi=0x%08X ebx=0x%08X edi=0x%08X esp=0x%08X "
                "container_base=0x%08X container_buckets=0x%08X container_mask=0x%08X\n",
                file_manager_call_index, g_eax, g_esi, g_ebx, g_edi, g_esp,
                MEM32(g_esi), MEM32(g_esi + 0x0Cu), MEM32(g_esi + 0x10u));
        file_manager_result_reports++;
    } else if (xbox_va == 0x00072D25u && !vector_stack_path_reported) {
        fprintf(stderr,
                "[SCDA-VECTOR-EBX] pc=0x00072D25 after-icall ebx=0x%08X eax=0x%08X esi=0x%08X esp=0x%08X\n",
                g_ebx, g_eax, g_esi, g_esp);
    } else if (xbox_va == 0x00072D2Fu && !vector_stack_path_reported) {
        fprintf(stderr,
                "[SCDA-VECTOR-STACK] pc=0x00072D2F before-zero ebp=0x%08X ebx=0x%08X "
                "local=0x%08X words={0x%08X,0x%08X,0x%08X}\n",
                g_seh_ebp, g_ebx, g_seh_ebp - 0x64u,
                MEM32(g_seh_ebp - 0x64u), MEM32(g_seh_ebp - 0x60u),
                MEM32(g_seh_ebp - 0x5Cu));
    } else if (xbox_va == 0x00072D4Eu && !vector_stack_path_reported) {
        fprintf(stderr,
                "[SCDA-VECTOR-STACK] pc=0x00072D4E after-45992 eax=0x%08X ecx=0x%08X "
                "ebp=0x%08X ebx=0x%08X local={0x%08X,0x%08X,0x%08X} "
                "esp=0x%08X stack={0x%08X,0x%08X,0x%08X,0x%08X}\n",
                g_eax, g_ecx, g_seh_ebp, g_ebx, MEM32(g_seh_ebp - 0x64u),
                MEM32(g_seh_ebp - 0x60u), MEM32(g_seh_ebp - 0x5Cu), g_esp,
                MEM32(g_esp), MEM32(g_esp + 4u), MEM32(g_esp + 8u), MEM32(g_esp + 12u));
    } else if (xbox_va == 0x00061570u && !vector_stack_path_reported &&
               MEM32(g_esp + 8u) >= 0x00F00000u && MEM32(g_esp + 8u) < 0x01000000u) {
        fprintf(stderr,
                "[SCDA-VECTOR-STACK] pc=0x00061570 entry ecx=0x%08X "
                "arg1=0x%08X arg2=0x%08X esp=0x%08X stack={0x%08X,0x%08X,0x%08X}\n",
                g_ecx, MEM32(g_esp + 4u), MEM32(g_esp + 8u), g_esp,
                MEM32(g_esp), MEM32(g_esp + 4u), MEM32(g_esp + 8u));
    } else if (xbox_va == 0x000615A2u && !vector_stack_path_reported &&
               MEM32(g_esp + 0x14u) >= 0x00F00000u && MEM32(g_esp + 0x14u) < 0x01000000u) {
        fprintf(stderr,
                "[SCDA-VECTOR-STACK] pc=0x000615A2 forward arg2=0x%08X "
                "esp=0x%08X stack={0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X}\n",
                MEM32(g_esp + 0x14u), g_esp, MEM32(g_esp), MEM32(g_esp + 4u),
                MEM32(g_esp + 8u), MEM32(g_esp + 12u), MEM32(g_esp + 16u),
                MEM32(g_esp + 20u));
    } else if (xbox_va == 0x0025C273u && !vector_stack_path_reported &&
               MEM32(g_esp + 8u) >= 0x00F00000u && MEM32(g_esp + 8u) < 0x01000000u) {
        uint32_t source = MEM32(g_esp + 8u);
        fprintf(stderr,
                "[SCDA-VECTOR-STACK] pc=0x0025C273 bad-vector=0x%08X "
                "words={0x%08X,0x%08X,0x%08X} esp=0x%08X\\n",
                source, source <= 0x03FFFFF4u ? MEM32(source) : 0xFFFFFFFFu,
                source <= 0x03FFFFF4u ? MEM32(source + 4u) : 0xFFFFFFFFu,
                source <= 0x03FFFFF4u ? MEM32(source + 8u) : 0xFFFFFFFFu,
                g_esp);
        vector_stack_path_reported = 1;
    } else if (xbox_va == 0x001D1CF1u && vector_frame_reports < 20) {
        uint32_t source = MEM32(g_esp + 4);
        fprintf(stderr,
                "[SCDA-VECTOR-COPY-OWNER] pc=0x001D1CF1 prev=0x%08X "
                "dest=0x%08X source=0x%08X source28={0x%08X,0x%08X,0x%08X} "
                "esp=0x%08X\\n",
                previous_pc, g_ecx, source,
                source <= 0x03FFFFD8u ? MEM32(source + 0x28u) : 0xFFFFFFFFu,
                source <= 0x03FFFFD8u ? MEM32(source + 0x2Cu) : 0xFFFFFFFFu,
                source <= 0x03FFFFD8u ? MEM32(source + 0x30u) : 0xFFFFFFFFu,
                g_esp);
        vector_frame_reports++;
    } else if (xbox_va == 0x001D1D1Du && vector_frame_reports < 40) {
        fprintf(stderr,
                "[SCDA-VECTOR-COPY-OWNER] pc=0x001D1D1D dest28=0x%08X "
                "source28=0x%08X source_words={0x%08X,0x%08X,0x%08X} esp=0x%08X\\n",
                g_esi + 0x28u, g_edi + 0x28u,
                MEM32(g_edi + 0x28u), MEM32(g_edi + 0x2Cu),
                MEM32(g_edi + 0x30u), g_esp);
        vector_frame_reports++;
    } else if (xbox_va == 0x00036135u && vector_frame_reports < 60) {
        uint32_t source = MEM32(g_esp + 4);
        fprintf(stderr,
                "[SCDA-VECTOR-FRAME] pc=0x001D1D24 enter dest=0x%08X "
                "source_container=0x%08X words={0x%08X,0x%08X,0x%08X} esp=0x%08X\\n",
                g_ecx, source, MEM32(source), MEM32(source + 4u),
                MEM32(source + 8u), g_esp);
        vector_frame_reports++;
    } else if (xbox_va == 0x00036157u && vector_frame_reports < 100) {
        fprintf(stderr,
                "[SCDA-VECTOR-FRAME] pc=0x00036157 iter=%u source_container=0x%08X "
                "data=0x%08X count=%u offset=0x%08X dest=0x%08X\\n",
                g_ebx, g_esi, MEM32(g_esi), MEM32(g_esi + 4u),
                MEM32(g_seh_ebp + 8u), g_edi);
        vector_frame_reports++;
    } else if (xbox_va == 0x000360E1u) {
        uint32_t source = MEM32(g_esp + 4);
        fprintf(stderr,
                "[SCDA-VECTOR-COPY-ENTRY] previous_pc=0x%08X dest=0x%08X "
                "source=0x%08X source_data=0x%08X source_count=%u "
                "source_capacity=%u esp=0x%08X ebp=0x%08X "
                "stack8=0x%08X stackC=0x%08X\n",
                previous_pc, g_ecx, source,
                source <= 0x03FFFFF4u ? MEM32(source) : 0xFFFFFFFFu,
                source <= 0x03FFFFF4u ? MEM32(source + 4) : 0xFFFFFFFFu,
                source <= 0x03FFFFF4u ? MEM32(source + 8) : 0xFFFFFFFFu,
                g_esp, g_seh_ebp, MEM32(g_esp + 8), MEM32(g_esp + 0xC));
    } else if (xbox_va == 0x000372FAu && umd_scan_reports < 4000u) {
        umd_scan_reports++;
        fprintf(stderr, "[SCDA-UMDSCAN] pc=0x000372FA entry #%u prev=0x%08X ecx=0x%08X esp=0x%08X arg8=0x%08X argC=0x%08X\n",
                umd_scan_reports, previous_pc, g_ecx, g_esp, MEM32(g_esp + 8u), MEM32(g_esp + 0xCu));
    } else if (xbox_va == 0x00037326u && umd_scan_reports < 4000u) {
        umd_scan_reports++;
        fprintf(stderr, "[SCDA-UMDSCAN] pc=0x00037326 loop-top #%u esp=0x%08X\n", umd_scan_reports, g_esp);
    } else if (xbox_va == 0x00037341u && umd_scan_reports < 4000u) {
        umd_scan_reports++;
        fprintf(stderr, "[SCDA-UMDSCAN] pc=0x00037341 findfirst-branch #%u esp=0x%08X\n", umd_scan_reports, g_esp);
    } else if (xbox_va == 0x00037364u && umd_scan_reports < 4000u) {
        umd_scan_reports++;
        fprintf(stderr, "[SCDA-UMDSCAN] pc=0x00037364 findnext-branch #%u esp=0x%08X\n", umd_scan_reports, g_esp);
    } else if (xbox_va == 0x0003736Au && umd_scan_reports < 4000u) {
        umd_scan_reports++;
        fprintf(stderr, "[SCDA-UMDSCAN] pc=0x0003736A findnext-result #%u eax=0x%08X esp=0x%08X\n", umd_scan_reports, g_eax, g_esp);
    } else if (xbox_va == 0x00037372u && umd_scan_reports < 4000u) {
        umd_scan_reports++;
        fprintf(stderr, "[SCDA-UMDSCAN] pc=0x00037372 entry-type-check #%u esp=0x%08X\n", umd_scan_reports, g_esp);
    } else if (xbox_va == 0x000373B6u && umd_scan_reports < 4000u) {
        umd_scan_reports++;
        fprintf(stderr, "[SCDA-UMDSCAN] pc=0x000373B6 recurse-into-subdir #%u esp=0x%08X\n", umd_scan_reports, g_esp);
    } else if (xbox_va == 0x0003745Eu && umd_scan_reports < 4000u) {
        umd_scan_reports++;
        fprintf(stderr, "[SCDA-UMDSCAN] pc=0x0003745E compare-vs-dynamicxbox #%u esp=0x%08X\n", umd_scan_reports, g_esp);
    } else if (xbox_va == 0x00037473u && umd_scan_reports < 4000u) {
        umd_scan_reports++;
        fprintf(stderr, "[SCDA-UMDSCAN] pc=0x00037473 compare-driver-entry #%u esp=0x%08X\n", umd_scan_reports, g_esp);
    } else if (xbox_va == 0x001D0AF4u || xbox_va == 0x001D0AFAu ||
               xbox_va == 0x001D19CFu || xbox_va == 0x001D19F8u) {
        fprintf(stderr,
                "[SCDA-SOURCE-LOCAL] pc=0x%08X previous_pc=0x%08X "
                "ebp=0x%08X local48=0x%08X eax=0x%08X ecx=0x%08X "
                "edx=0x%08X esp=0x%08X esi=0x%08X edi=0x%08X\n",
                xbox_va, previous_pc, g_seh_ebp,
                MEM32(g_seh_ebp - 0x48u), g_eax, g_ecx, g_edx,
                g_esp, g_esi, g_edi);
    } else if (xbox_va == 0x001D589Du) {
        /* sub_001D589D's own entry: capture the guest ESP the function
         * was called with, so a later checkpoint (0x001D5A5E) can
         * reconstruct its local "ebp" pseudo-register (which the
         * generated code computes as entry_esp - 4 - 120 and never
         * publishes to any global) well enough to read the
         * Maps\*.ini enumeration result directly out of memory. See
         * docs/SCDA_WORKLOG.md, 2026-09-02 (continued 6).
         *
         * Also walk bin[6] (the 48-byte smallbin free list,
         * BIN6_ADDR = bin_table_base 0x64D29C + slot 6*4) right here,
         * before this function's own Maps-enumeration loop (and its
         * allocations) can touch it, to quantify how much headroom
         * already existed at this point in the run -- the loop's own
         * iteration count is proven correct (see [SCDA-MAPSLOOP]), so
         * if bin[6] is already exhausted/near-exhausted on entry, the
         * true divergence is upstream of this whole function. */
        {
            const uint32_t bin6_addr = 0x0064D2B4u;
            uint32_t head = MEM32(bin6_addr);
            uint32_t cur = head;
            unsigned int chain_len = 0;
            unsigned int seen_cap = 0;
            while (cur != 0 && chain_len < 64u) {
                chain_len++;
                cur = MEM32(cur + 8u);
                if (++seen_cap >= 64u) break;
            }
            fprintf(stderr,
                    "[SCDA-BIN6-PRE-MAPSLOOP] head=0x%08X chain_len=%u\n",
                    head, chain_len);
            fflush(stderr);
        }
        g_scda_1d589d_entry_esp = g_esp;
    } else if (xbox_va == 0x001D5A5Eu && g_scda_1d589d_entry_esp != 0) {
        uint32_t ebp_local = g_scda_1d589d_entry_esp - 4u - 120u;
        fprintf(stderr,
                "[SCDA-MAPSLOOP] array=0x%08X count=%u(0x%X) capacity=%u(0x%X) "
                "ebp=0x%08X entry_esp=0x%08X\n",
                MEM32(ebp_local + 0x4Cu), MEM32(ebp_local + 0x50u),
                MEM32(ebp_local + 0x50u), MEM32(ebp_local + 0x54u),
                MEM32(ebp_local + 0x54u), ebp_local, g_scda_1d589d_entry_esp);
        fflush(stderr);
    }
    /* 2026-09-03 (continued 20): the EDI/sub_0004612B stack-leak
     * investigation (docs/SCDA_WORKLOG.md, "continued 13-19") is CLOSED --
     * root cause found and fixed (scda_460f0_earlyexit_0004612B). Do not
     * re-widen this gate or the CMakeLists.txt trace ranges to re-chase
     * it; the near-full-.text watchpoint that made every run take 8+
     * minutes wall-clock has been removed. This lean final-verification
     * gate only allows the three checkpoints requested for confirming the
     * fix end-to-end: sub_001D5208's own entry (call sequence + param
     * edi), the type-lookup result at 0x1D560E, and the registry
     * bounds-check at 0x4909F. Widen ONLY for whatever new first
     * divergence point is found after this fix, not for this closed bug. */
    if (!(xbox_va == 0x001D5208u || xbox_va == 0x001D560Eu ||
          xbox_va == 0x0004909Fu ||
          /* 2026-09-03 (continued 22): LaunchDataPage payload dump --
           * see [SCDA-LAUNCHDATA-COPY] below. */
          xbox_va == 0x0005573Cu ||
          /* 2026-09-03 (continued 23): reboot-gate check -- see
           * [SCDA-REBOOTGATE-LIVE] below. */
          xbox_va == 0x00055666u ||
          /* 2026-09-03 (continued 23): sub_00054F57 entry -- the
           * unconditional sub_000557ED caller reached from the dashboard
           * integrity check (sub_00055309, called from sub_0047D997).
           * See [SCDA-DASHCHECK-CALLER] below. */
          xbox_va == 0x00054F57u ||
          /* 2026-09-03 (continued 23): sub_000557ED entry -- read the
           * real return address off the stack to identify the actual
           * caller, since neither of the two direct `call 0x557ed`
           * sites (sub_00054F57, sub_00055380's esi==0 branch) has been
           * confirmed live yet. See [SCDA-557ED-CALLER] below. */
          xbox_va == 0x000557EDu ||
          /* 2026-09-03 (continued 23): sub_0047D997 entry -- the real
           * gate. arg0 must be 0 and arg1 (a struct ptr) must be NULL,
           * or *arg1 must fall in [6,0xFF]/[0x10000,inf) for the reboot
           * chain to fire; *arg1 in [0,5]/[0x100,0xFFFF] or arg0!=0
           * takes the normal path instead. See [SCDA-47D997-ARGS]. */
          xbox_va == 0x0047D997u ||
          /* 2026-09-03 (continued 24): sub_00055187 stack-depth bisection.
           * Every basic-block boundary inside this function, entry through
           * its single shared epilogue, to find exactly which early-exit
           * branch reaches loc_000552ED/loc_000552FD with an unbalanced
           * esp (which would corrupt the edi/esi/ebx pops there). See
           * [SCDA-55187-BLOCK] below and docs/SCDA_WORKLOG.md. */
          (xbox_va >= 0x00055187u && xbox_va <= 0x000552FDu)))
        return;

    if (xbox_va >= 0x00055187u && xbox_va <= 0x000552FDu) {
        fprintf(stderr, "[SCDA-55187-BLOCK] pc=0x%08X esp=0x%08X edi=0x%08X esi=0x%08X ebx=0x%08X\n",
                xbox_va, g_esp, g_edi, g_esi, g_ebx);
        fflush(stderr);
        return;
    }

    if (xbox_va == 0x0047D997u) {
        uint32_t arg0 = MEM32(g_esp + 4u);
        uint32_t arg1 = MEM32(g_esp + 8u);
        /* 2026-09-03 (continued 24): sub_00055187 showed zero trace hits
         * despite sub_0047D997 firing -- check whether this call arrived
         * via the icall dispatch table (0x4F5268-style indirect call,
         * bypassing sub_0047DCA4/sub_00055187 entirely) rather than the
         * direct call at recomp_0027.c:30663. */
        fprintf(stderr, "[SCDA-47D997-ICALLSRC] g_icall_source=0x%08X\n", g_icall_source);
        fprintf(stderr,
                "[SCDA-47D997-ARGS] arg0=0x%08X arg1=0x%08X *arg1=0x%08X\n",
                arg0, arg1, arg1 ? MEM32(arg1) : 0xFFFFFFFFu);
        fflush(stderr);
        return;
    }

    if (xbox_va == 0x000557EDu) {
        fprintf(stderr, "[SCDA-557ED-CALLER] return_addr=0x%08X\n",
                MEM32(g_esp));
        fflush(stderr);
        return;
    }

    if (xbox_va == 0x00054F57u) {
        /* args (per [ebp+8..]): edi=path-descriptor ptr (compared against
         * the dashupdate/xboxdash/xonlinedash strings), plus 2 more.
         * ESP here = [retaddr, arg1, arg2, arg3, ...]. */
        fprintf(stderr,
                "[SCDA-DASHCHECK-CALLER] arg1=0x%08X arg2=0x%08X arg3=0x%08X\n",
                MEM32(g_esp + 4u), MEM32(g_esp + 8u), MEM32(g_esp + 12u));
        fflush(stderr);
        return;
    }

    if (xbox_va == 0x00055666u) {
        /* 2026-09-03 (continued 24): sub_00055666 has 4 known callers
         * (sub_000579F2, sub_001D5FA5, sub_0026AB34, sub_0047DCA4).
         * sub_0047D997 turned out to be reached via sub_00055666's OWN
         * callback-table dispatch (icall_source=0x556A4), not via
         * sub_0047DCA4/sub_00055187 as first assumed -- read the real
         * return address here to identify which of the 4 callers this
         * actually is, and its own param1 (which becomes sub_0047D997's
         * arg0 through the callback dispatch). */
        fprintf(stderr,
                "[SCDA-55666-CALLER] return_addr=0x%08X param1=0x%08X param2=0x%08X\n",
                MEM32(g_esp), MEM32(g_esp + 4u), MEM32(g_esp + 8u));
        fflush(stderr);
    }
    if (xbox_va == 0x00055666u) {
        /* sub_00055666 entry -- reads the same 0x5B1D90/0x5B1D94 static
         * dwords sub_00055666's loop checks. Confirms live in-game state
         * at the exact moment of the reboot-gate decision, rather than
         * assuming it matches the post-load snapshot. */
        fprintf(stderr,
                "[SCDA-REBOOTGATE-LIVE] 0x005B1D90=0x%08X 0x005B1D94=0x%08X\n",
                MEM32(0x005B1D90u), MEM32(0x005B1D94u));
        fflush(stderr);
        return;
    }

    if (xbox_va == 0x0005573Cu) {
        /* 2026-09-03 (continued 22): sub_000556A8's `rep movsd` copying
         * 0x300 dwords (3072 bytes) from esi (caller-supplied buffer,
         * traced back to whatever ultimately calls sub_000557ED) into the
         * LaunchDataPage at [launch_page+0x400] -- this is the classic
         * Xbox self-relaunch pattern (XLaunchNewImage-style): write launch
         * data, then HalReturnToFirmware(2) triggers a quick-reboot that
         * reloads whatever this data specifies. Our xbox_HalReturnToFirmware
         * just does ExitProcess(0) for every routine value -- if this is
         * genuine SCDA behavior (a bootstrap-stage relaunch into the real
         * game XBE), that's a missing kernel feature, not a recompiler
         * bug. Dump the source buffer as a string to see what's actually
         * being launched. RECOMP_TRACE_PC fires before this block's own
         * `rep movsd` executes, so esi/edi here are the pre-copy source/
         * dest pointers -- exactly what we want to read raw from guest
         * memory before the copy overwrites nothing (source is untouched
         * either way). */
        char buf[256];
        uint32_t i;
        for (i = 0; i < sizeof(buf) - 1; i++) {
            uint8_t b = (uint8_t)MEM32(g_esi + (i & ~3u));
            b = (uint8_t)(MEM32(g_esi + (i & ~3u)) >> ((i & 3u) * 8));
            if (b == 0 && i > 0) break;
            buf[i] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        buf[i] = '\0';
        fprintf(stderr,
                "[SCDA-LAUNCHDATA-COPY] src=0x%08X dst=0x%08X len=0x300dw "
                "text=\"%s\"\n",
                g_esi, g_edi, buf);
        fflush(stderr);
    } else if (xbox_va == 0x0004909Fu) {
        /* 2026-09-03 (continued 20): lean final-verification hook for the
         * registry bounds-check (sub_00048F1D). eax=index, esi=count,
         * ecx=base -- read directly from the just-executed MEM32 loads at
         * this exact block start (see tools/recomp/output/scda_v060/gen/
         * recomp_0002.c:40108-40110). Reports the live register values
         * (which mirror MEM32(0x6513C8)/MEM32(0x6513A8)/MEM32(0x6513A4)
         * at this point) plus a fresh direct memory read of the same
         * three globals for a second, independent confirmation, and the
         * branch outcome (index>=count -> "jge" sentinel/invalid path,
         * else the normal in-bounds dereference). Returns immediately
         * after -- no fall-through into the old heavy diagnostic chain
         * below, which is now dead weight for this lean verification
         * build (kept in place rather than deleted, in case a future
         * divergence needs any of its still-compiled-but-unreachable
         * branches; none of it can fire while the whitelist gate above
         * stays this narrow). */
        fprintf(stderr,
                "[SCDA-BOUNDSCHECK] index(eax)=%u count(esi)=%u base(ecx)=0x%08X "
                "mem_index=%u mem_count=%u mem_base=0x%08X in_bounds=%d\n",
                g_eax, g_esi, g_ecx,
                MEM32(0x006513C8u), MEM32(0x006513A8u), MEM32(0x006513A4u),
                (int32_t)g_eax < (int32_t)g_esi);
        fflush(stderr);
        return;
    }

    /* The main game initializer is much larger than the earlier XAPI path.
     * Keep the diagnostic bounded so a live game loop cannot flood stderr. */
    if (xbox_va >= 0x00048B15u && xbox_va < 0x00049F9Bu) {
        initializer_reports++;
        if (initializer_reports > 600) {
            if (initializer_reports == 601)
                fprintf(stderr,
                        "[SCDA-PC] initializer trace suppressed after 600 blocks\n");
            return;
        }
    }

    if ((xbox_va >= 0x00042082u && xbox_va < 0x000421D7u) ||
        (xbox_va >= 0x00043455u && xbox_va < 0x00043582u) ||
        (xbox_va >= 0x00041F56u && xbox_va < 0x00042040u) ||
        (xbox_va >= 0x000511E7u && xbox_va < 0x00051280u) ||
        (xbox_va >= 0x00051673u && xbox_va < 0x00051740u) ||
        (xbox_va >= 0x00051AC0u && xbox_va < 0x00051BC0u) ||
        (xbox_va >= 0x000725A5u && xbox_va < 0x00072650u)) {
        nested_initializer_reports++;
        if (nested_initializer_reports > 2000) {
            if (nested_initializer_reports == 2001)
                fprintf(stderr,
                        "[SCDA-PC] nested initializer trace suppressed after 2000 blocks\n");
            return;
        }
    }

    if (xbox_va == 0x00052A57u) {
        uint32_t tls_index = MEM32(0x005EA9ECu);
        uint32_t tls_vector = xbox_fs_read32(0x04u);
        uint32_t tls_slot_va = tls_vector + tls_index * 4u;
        uint32_t tls_block = tls_slot_va <= 0x03FFFFFCu
                                 ? MEM32(tls_slot_va) : 0xFFFFFFFFu;
        fprintf(stderr,
                "[SCDA-TLS] index=%u vector=0x%08X slot=0x%08X "
                "block=0x%08X value=0x%08X esp=0x%08X\n",
                tls_index, tls_vector, tls_slot_va, tls_block,
                MEM32(g_esp + 4), g_esp);
    } else if (xbox_va == 0x000562CDu) {
        fprintf(stderr,
                "[SCDA-HEAP-CALL] heap=0x%08X size=0x%08X entry_esp=0x%08X\n",
                MEM32(g_esp + 4), MEM32(g_esp + 8), g_esp);
    } else if (xbox_va == 0x0005644Du) {
        fprintf(stderr,
                "[SCDA-HEAP-RETURN] esp=0x%08X pop_edi=0x%08X "
                "pop_esi=0x%08X pop_ebx=0x%08X\n",
                g_esp, MEM32(g_esp), MEM32(g_esp + 4), MEM32(g_esp + 8));
    } else if (xbox_va == 0x00036116u) {
        fprintf(stderr,
                "[SCDA-VECTOR-DTOR] previous_pc=0x%08X vector=0x%08X "
                "data=0x%08X size=%u capacity=%u esp=0x%08X\n",
                previous_pc, g_ecx, MEM32(g_ecx), MEM32(g_ecx + 4),
                MEM32(g_ecx + 8), g_esp);
    } else if (xbox_va == 0x001E460Fu) {
        uint32_t allocator = MEM32(0x005B57F8u);
        uint32_t vtable = allocator ? MEM32(allocator) : 0;
        uint32_t target = vtable ? MEM32(vtable + 4) : 0;
        static unsigned int vecfree_seq = 0;
        vecfree_seq++;
        fprintf(stderr,
                "[SCDA-VECTOR-FREE] n=%u previous_pc=0x%08X vector=0x%08X data=0x%08X "
                "size=%u capacity=%u stride=%u byte_count=%u allocator=0x%08X "
                "vtable=0x%08X target=0x%08X bin6_len_before=%u\n",
                vecfree_seq, previous_pc, g_ecx, MEM32(g_ecx), MEM32(g_ecx + 4),
                MEM32(g_ecx + 8), MEM32(g_esp + 4),
                MEM32(g_ecx + 8) * MEM32(g_esp + 4),
                allocator, vtable, target, scda_bin6_chain_len());
    } else if (xbox_va == 0x000360A3u) {
        fprintf(stderr,
                "[SCDA-VECTOR-RESET] vector=0x%08X data=0x%08X count=%u "
                "capacity=%u entry_esp=0x%08X\n",
                g_ecx, MEM32(g_ecx), MEM32(g_ecx + 4), MEM32(g_ecx + 8),
                g_esp);
    } else if (xbox_va == 0x001C445Du) {
        fprintf(stderr,
                "[SCDA-VECTOR-ERASE] vector=0x%08X first=%u count=%u "
                "stride=%u entry_esp=0x%08X\n",
                g_ecx, MEM32(g_esp + 4), MEM32(g_esp + 8),
                MEM32(g_esp + 12), g_esp);
    } else if (xbox_va == 0x00045D0Bu) {
        uint32_t backend = MEM32(g_ecx + 4);
        uint32_t vtable = backend && backend < 0x03FFFFFCu
                            ? MEM32(backend) : 0;
        fprintf(stderr,
                "[SCDA-BACKEND-INIT] object=0x%08X backend=0x%08X "
                "arg=0x%08X vtable=0x%08X method0=0x%08X method8=0x%08X\n",
                g_ecx, backend, MEM32(g_esp + 4), vtable,
                vtable && vtable < 0x03FFFFF4u ? MEM32(vtable) : 0,
                vtable && vtable < 0x03FFFFF4u ? MEM32(vtable + 8) : 0);
    } else if (xbox_va == 0x00045DDEu || xbox_va == 0x00045DEDu ||
               xbox_va == 0x00045DF4u) {
        fprintf(stderr,
                "[SCDA-RESOURCE-IO] pc=0x%08X object=0x%08X resource=0x%08X "
                "out0c=%08X out20=%08X size10=%08X esp=%08X\n",
                xbox_va, g_esi, MEM32(g_esi + 0x2C), MEM32(g_esi + 0x0C),
                MEM32(g_esi + 0x20), MEM32(g_esi + 0x10), g_esp);
    } else if (xbox_va == 0x00045E42u || xbox_va == 0x00045E6Bu ||
               xbox_va == 0x00045E71u || xbox_va == 0x00045E7Bu) {
        fprintf(stderr,
                "[SCDA-RESOURCE-CALLSTATE] pc=0x%08X eax=%08X ebx=%08X "
                "ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X\n",
                xbox_va, g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp);
    } else if (xbox_va == 0x00045CD1u) {
        fprintf(stderr,
                "[SCDA-RESOURCE-VECTOR] pc=0x%08X ecx=%08X arg0=%08X arg1=%08X "
                "data=%08X count=%08X cap=%08X edi=%08X esp=%08X\n",
                xbox_va, g_ecx, MEM32(g_esp + 4), MEM32(g_esp + 8),
                MEM32(g_ecx), MEM32(g_ecx + 4), MEM32(g_ecx + 8), g_edi, g_esp);
    } else if (xbox_va == 0x000775A2u) {
        /* sub_000774E6 (dlmalloc-shaped small-block allocator), the
         * instruction immediately after the smallbin head load at
         * 0x00077590: `ecx` is the fixed, link-time-static smallbin table
         * slot (`(size>>3)*4 + 0x64D29C`), and `eax` (RECOMP_TRACE_PC fires
         * at the TOP of this block, before this block's own `eax = eax+8`
         * runs) is still the freshly-read, already-known-nonzero bin head
         * from the previous block -- reading `MEM32(g_ecx)` at the
         * PREVIOUS block's own trace point instead would only see this
         * block's *own* not-yet-executed computation and read stale ecx
         * left over from the unrelated top-chunk path above, which is the
         * mistake the first version of this diagnostic made. Every
         * legitimate bin head observed this session is either NULL (empty
         * bin, branches away before reaching here) or a real guest heap
         * pointer in the 0x005Bxxxx-0x01xxxxxx / 0x00F7xxxx (stack-relative
         * small vector storage) range. A head below this floor cannot be a
         * real chunk pointer -- it is corrupted smallbin-table state, most
         * likely a wild write elsewhere landing on this fixed global
         * table. Kept to a single fprintf on first detection (not a ring)
         * per the instruction to keep instrumentation narrow: this table
         * slot's guest VA is link-time-fixed, so one hit is enough to
         * name the exact address for a second run's hardware watchpoint.
         * See docs/SCDA_WORKLOG.md, 2026-09-02 (sub_000774E6 AV). */
        static int reported = 0;
        if (!reported && g_eax != 0 && g_eax < 0x00080000u) {
            reported = 1;
            fprintf(stderr,
                    "[SCDA-SMALLBIN-CORRUPT] bin_slot=0x%08X size_class=0x%08X "
                    "bin_head(garbage)=0x%08X esp=0x%08X previous_pc=0x%08X\n",
                    g_ecx, g_esi, g_eax, g_esp, previous_pc);
            fflush(stderr);
        }
    } else if (xbox_va == 0x0007708Fu || xbox_va == 0x00077098u ||
               xbox_va == 0x000770CAu) {
        fprintf(stderr,
                "[SCDA-ALLOC-NODE] pc=0x%08X eax=%08X ecx=%08X edx=%08X "
                "esi=%08X edi=%08X esp=%08X\n",
                xbox_va, g_eax, g_ecx, g_edx, g_esi, g_edi, g_esp);
    } else if (xbox_va == 0x00045DB1u) {
        uint32_t resource = MEM32(g_esi + 0x2C);
        uint32_t vtable = resource && resource < 0x03FFFFFCu
                            ? MEM32(resource) : 0;
        fprintf(stderr,
                "[SCDA-BACKEND-RESOURCE] object=0x%08X resource=0x%08X "
                "vtable=0x%08X method50=0x%08X\n",
                g_esi, resource, vtable,
                vtable && vtable < 0x03FFFFACu ? MEM32(vtable + 0x50) : 0);
    } else if (xbox_va == 0x00045D24u || xbox_va == 0x00045D33u) {
        uint32_t backend = MEM32(g_esi + 4);
        uint32_t vtable = backend && backend < 0x03FFFFFCu
                            ? MEM32(backend) : 0;
        fprintf(stderr,
                "[SCDA-BACKEND-DISPATCH] pc=0x%08X backend=0x%08X "
                "vtable=0x%08X slot0=0x%08X slot8=0x%08X eax=0x%08X "
                "arg0=0x%08X arg1=0x%08X esp=0x%08X\\n",
                xbox_va, backend, vtable,
                vtable && vtable < 0x03FFFFFCu ? MEM32(vtable) : 0,
                vtable && vtable < 0x03FFFFF4u ? MEM32(vtable + 8) : 0,
                g_eax, MEM32(g_esp + 4), MEM32(g_esp + 8), g_esp);
    } else if (xbox_va == 0x0004023Au || xbox_va == 0x00040264u ||
               xbox_va == 0x00040290u || xbox_va == 0x000406F7u ||
               xbox_va == 0x00040717u || xbox_va == 0x00040730u ||
               xbox_va == 0x0004075Au || xbox_va == 0x0004075Cu ||
               xbox_va == 0x00040766u || xbox_va == 0x000407D4u ||
               xbox_va == 0x000407EDu || xbox_va == 0x00045D36u) {
        fprintf(stderr,
                "[SCDA-RESOURCE-TRACE] pc=0x%08X previous_pc=0x%08X "
                "eax=0x%08X ecx=0x%08X edx=0x%08X ebx=0x%08X "
                "esi=0x%08X edi=0x%08X esp=0x%08X\\n",
                xbox_va, previous_pc, g_eax, g_ecx, g_edx, g_ebx,
                g_esi, g_edi, g_esp);
    }

    sequence++;
    fprintf(stderr,
            "[SCDA-PC] #%u pc=0x%08X eax=0x%08X ecx=0x%08X "
            "edx=0x%08X esp=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X "
            "status140=0x%08X status144=0x%08X fs4=0x%08X\n",
            sequence,
            xbox_va,
            g_eax,
            g_ecx,
            g_edx,
            g_esp,
            g_ebx,
            g_esi,
            g_edi,
            MEM32(0x005ED140u),
            MEM32(0x005ED144u),
            MEM32(0x00000004u));
    /* sub_0004721E sets edi = 0x005A570C once in its prologue and expects
     * it to survive, untouched, all the way to its sub_00046CE5 call at
     * 0x00047488 (confirmed live: that call's "this" pointer arrives as
     * 0x00045974 instead, a .text code address -- garbage for a hash-
     * table object -- causing the chain-walk hang investigated
     * 2026-09-02). Same watch technique as
     * [SCDA-CRT-ITERATOR-CLOBBER]/file-manager-write above: report the
     * first PC inside that range where edi has already gone wrong, so
     * the exact clobbering call site can be identified without guessing
     * further from static analysis. */
    if (xbox_va == 0x00048C9A || xbox_va == 0x00048CAAu ||
        xbox_va == 0x00048CB2u) {
        fprintf(stderr,
                "[SCDA-5B5800-BRANCH] pc=0x%08X status140=0x%08X "
                "status144=0x%08X edi=0x%08X esp=0x%08X\n",
                xbox_va, MEM32(0x005ED140u), MEM32(0x005ED144u),
                g_edi, g_esp);
        fflush(stderr);
    }
    /* Three fixed measurement points, matched byte-for-byte against xemu
     * (same tag names, same register set) to find exactly where our
     * recomp build first diverges from real hardware, rather than
     * guessing further from static analysis alone:
     *   A: immediately before MEM32(0x5B5800) is written (loc_00048CCB,
     *      the fallback path -- confirmed via xemu 2026-09-02 that real
     *      hardware also takes this path, so it's not itself the bug)
     *   B: immediately after that write (loc_00048CD0)
     *   C: immediately before sub_0004721E reads it back (loc_00047440)
     * Also dumps the first 28 bytes of the object MEM32(0x5B5800) points
     * to, when it looks like a plausible guest pointer, since the
     * *contents* diverging (not just the address) is equally telling. */
    if (xbox_va == 0x00048CCBu || xbox_va == 0x00048CD0u ||
        xbox_va == 0x00047440u) {
        uint32_t slot = MEM32(0x005B5800u);
        char objdump[16 * 6 + 1] = {0};
        if (slot && slot < 0x03FFFFE4u) {
            int i;
            char *p = objdump;
            for (i = 0; i < 7; i++) {
                p += sprintf_s(p, sizeof(objdump) - (p - objdump),
                               "%08X ", MEM32(slot + (uint32_t)i * 4));
            }
        }
        fprintf(stderr,
                "[SCDA-ABC] tag=%s pc=0x%08X eax=0x%08X ebx=0x%08X "
                "ecx=0x%08X edx=0x%08X esi=0x%08X edi=0x%08X esp=0x%08X "
                "ebp=0x%08X slot5B5800=0x%08X obj=%s\n",
                xbox_va == 0x00048CCBu ? "A" :
                xbox_va == 0x00048CD0u ? "B" : "C",
                xbox_va, g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi,
                g_esp, g_seh_ebp, slot, objdump);
        fflush(stderr);
    }
    /* sub_00045ADE is the ACTUAL resolved vtable target for the failing
     * dispatch (this=the stack error-object from points A/B/C,
     * vtable=0x004F7108) -- confirmed via [SCDA-VCALL] logging, NOT
     * sub_000360A3 as first assumed (that address is a DIFFERENT vtable
     * slot's target, reached only for other "this" objects earlier in
     * the same run). sub_00045ADE itself doesn't touch edi; it either
     * returns directly after calling sub_000457AE (verified: single,
     * correctly edi-preserving exit), or -- when that call's result is
     * zero -- falls into an INDIRECT TAIL JUMP (RECOMP_ITAIL) to
     * MEM32(eax+0x10). Instrument that specific branch: which target it
     * resolves to, and whether it's even a *resolved* function at all. */
    /* sub_000457AE has a single, correctly-formed exit (loc_00045932:
     * POP32 edi/esi/ebx, esp=ebp, POP32 ebp, ret 4) -- but it's a long
     * (~330 line), heavily-branching function, and any ONE branch with
     * an unmatched PUSH32/POP32 would corrupt exactly the edi/esi/ebx
     * restore at that single exit without otherwise crashing (the
     * `esp = ebp` right after masks any deeper imbalance for the local
     * frame itself). Measure ESP right after the prologue's three
     * pushes (0x000457CC is the first post-prologue label -- fires
     * before any of the many branches run) against ESP right before
     * that final POP32(edi) (0x00045932): they must be identical if
     * every branch in between is balanced. */
    if (xbox_va == 0x000457CCu || xbox_va == 0x000457F8u) {
        fprintf(stderr, "[SCDA-457AE-DEPTH] tag=post-prologue pc=0x%08X esp=0x%08X\n",
                xbox_va, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00045932u) {
        fprintf(stderr, "[SCDA-457AE-DEPTH] tag=pre-pop pc=0x%08X esp=0x%08X\n",
                xbox_va, g_esp);
        fflush(stderr);
    }
    /* Bisection checkpoints between the confirmed-mismatched post-prologue
     * (0x00F7AF20) and pre-pop (0x00F7AF10) points, at every basic-block
     * convergence point along the way, to narrow down which branch
     * introduces the 16-byte (4-dword) leak. */
    if (xbox_va == 0x0004580Au || xbox_va == 0x00045813u ||
        xbox_va == 0x00045830u || xbox_va == 0x00045928u ||
        xbox_va == 0x0004592Au || xbox_va == 0x000458B6u ||
        xbox_va == 0x000458F8u || xbox_va == 0x00045913u ||
        xbox_va == 0x0004583Bu || xbox_va == 0x00045844u ||
        xbox_va == 0x00045851u || xbox_va == 0x00045949u ||
        xbox_va == 0x00045950u || xbox_va == 0x00045953u ||
        xbox_va == 0x00045974u || xbox_va == 0x0004597Bu ||
        xbox_va == 0x00045981u) {
        fprintf(stderr, "[SCDA-457AE-BISECT] pc=0x%08X esp=0x%08X\n",
                xbox_va, g_esp);
        fflush(stderr);
    }
    /* New blocker (2026-09-02, post hash-chain-hang fix): a crash freeing
     * a garbage pointer inside sub_00077049 (called via sub_00077176 <-
     * sub_0003EDDE, operator-delete style). Trace the pointer value at
     * each hop back toward its actual origin. */
    if (xbox_va == 0x0003EDDEu) {
        fprintf(stderr, "[SCDA-77049-TRACE] fn=sub_0003EDDE arg=0x%08X esp=0x%08X\n",
                MEM32(g_esp + 4u), g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x0029FA18u) {
        fprintf(stderr, "[SCDA-77049-TRACE] fn=sub_0029FA18 entry arg=0x%08X ecx=0x%08X esp=0x%08X retaddr=0x%08X\n",
                MEM32(g_esp + 4u), g_ecx, g_esp, MEM32(g_esp));
        fflush(stderr);
    }
    if (xbox_va == 0x00077176u) {
        fprintf(stderr, "[SCDA-77049-TRACE] fn=sub_00077176 arg=0x%08X esp=0x%08X\n",
                MEM32(g_esp + 4u), g_esp);
        fflush(stderr);
    }
    /* Object-construction differential for the sub_00077049 crash:
     * sub_00046CA9 is the container destructor -- it destructs
     * MEM32(container+4) elements of stride 0x1C starting at
     * MEM32(container), then frees the backing array. Log the
     * container's own base/count/capacity fields at entry, and every
     * element's +0x14 pointer field (what sub_00045626 eventually
     * frees), to see whether the LAST element's field was ever written
     * before this destructor runs. */
    if (xbox_va == 0x00046D7Au) {
        uint32_t source_arg = MEM32(g_esp + 4u);
        fprintf(stderr,
                "[SCDA-46D7A-SELFCOPY] pc=0x%08X dest(ecx)=0x%08X source=0x%08X "
                "equal=%d esp=0x%08X\n",
                xbox_va, g_ecx, source_arg, g_ecx == source_arg, g_esp);
        fflush(stderr);
    }
    /* D/E/F/G checkpoints around the actual node allocation inside
     * sub_00046E1E (the hash-insert path): D = before the allocator
     * call (0x00046E1E entry), E = right after it returns and the node
     * address is computed (0x00046E2B, before testing edi==0 -- the
     * allocation-failure fallback that SKIPS sub_00046EA2's
     * MEM32(node+0xC)=0 zeroing entirely). */
    if (xbox_va == 0x00046E1Eu) {
        fprintf(stderr,
                "[SCDA-ALLOC-DEFG] tag=D(pre-alloc) table(ecx)=0x%08X esp=0x%08X\n",
                g_ecx, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00046E2Bu) {
        fprintf(stderr,
                "[SCDA-ALLOC-DEFG] tag=E(post-alloc) alloc_index_or_raw(eax)=0x%08X esp=0x%08X\n",
                g_eax, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00046E37u) {
        fprintf(stderr,
                "[SCDA-ALLOC-DEFG] tag=E2(alloc-ok, node=edi) node=0x%08X data(edi+8)=0x%08X esp=0x%08X\n",
                g_edi, g_edi + 8u, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00046E62u) {
        fprintf(stderr,
                "[SCDA-ALLOC-DEFG] tag=E3(ALLOC-FAILED path, edi forced 0 -- sub_00046EA2 SKIPPED) esp=0x%08X\n",
                g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00046E64u) {
        fprintf(stderr,
                "[SCDA-ALLOC-DEFG] tag=F(pre-link) edi=0x%08X esi(table)=0x%08X esp=0x%08X\n",
                g_edi, g_esi, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00046E9Au) {
        uint32_t obj = g_edi + 8u;
        fprintf(stderr,
                "[SCDA-ALLOC-DEFG] tag=G(insert-return) node(edi)=0x%08X returned_object=0x%08X "
                "field0=0x%08X field4=0x%08X field8=0x%08X fieldC=0x%08X esp=0x%08X\n",
                g_edi, obj, MEM32(obj), MEM32(obj + 4u),
                MEM32(obj + 8u), MEM32(obj + 0xCu), g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00046473u) {
        uint32_t allocator = MEM32(0x005B57F8u);
        uint32_t vtable = allocator ? MEM32(allocator) : 0;
        uint32_t target = vtable ? MEM32(vtable) : 0;
        fprintf(stderr,
                "[SCDA-46473-ICALL] pc=0x%08X allocator_singleton=0x%08X vtable=0x%08X "
                "target(vtable+0)=0x%08X size_arg=0x%08X esp=0x%08X\n",
                xbox_va, allocator, vtable, target, MEM32(g_ecx + 0x10u) << 2, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00046496u) {
        fprintf(stderr,
                "[SCDA-46473-ICALL] pc=0x00046496(icall-return) eax=0x%08X esp=0x%08X\n",
                g_eax, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00046EA2u)
        g_scda_46ea2_saved_esi = g_ecx; /* incoming "this", before esi=ecx runs */
    if (xbox_va == 0x00046EC8u || xbox_va == 0x00046EE7u ||
        xbox_va == 0x00046EA2u || xbox_va == 0x00046EB0u) {
        fprintf(stderr,
                "[SCDA-46EC8-PATH] pc=0x%08X esi(obj)=0x%08X edi(oldtbl)=0x%08X "
                "eax=0x%08X ebx=0x%08X esp=0x%08X fieldC_now=0x%08X\n",
                xbox_va, g_esi, g_edi, g_eax, g_ebx, g_esp,
                g_scda_46ea2_saved_esi ? MEM32(g_scda_46ea2_saved_esi + 0xCu) : 0xFFFFFFFFu);
        fflush(stderr);
    }
    if (xbox_va == 0x00046EC1u) {
        fprintf(stderr,
                "[SCDA-46EC8-PATH] pc=0x00046EC1(46EA2-return) this=0x%08X fieldC_now=0x%08X esp=0x%08X\n",
                g_scda_46ea2_saved_esi,
                g_scda_46ea2_saved_esi ? MEM32(g_scda_46ea2_saved_esi + 0xCu) : 0xFFFFFFFFu,
                g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x000463E7u || xbox_va == 0x000474DEu || xbox_va == 0x00047488u) {
        fprintf(stderr,
                "[SCDA-464EC-THIS] callsite=0x%08X hash_result(eax)=0x%08X esp=0x%08X\n",
                xbox_va, g_eax, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00045F37u) {
        fprintf(stderr, "[SCDA-CONTAINER] fn=sub_00045F37 entry ecx=0x%08X retaddr=0x%08X esp=0x%08X\n",
                g_ecx, MEM32(g_esp), g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00046CA9u) {
        uint32_t base = g_ecx ? MEM32(g_ecx) : 0;
        uint32_t count = g_ecx ? MEM32(g_ecx + 4u) : 0;
        uint32_t cap = g_ecx ? MEM32(g_ecx + 8u) : 0;
        uint32_t i;
        fprintf(stderr,
                "[SCDA-CONTAINER] container=0x%08X base=0x%08X count=%u cap=0x%08X\n",
                g_ecx, base, count, cap);
        for (i = 0; i < count && i < 8u; i++) {
            uint32_t elem = base + i * 0x1Cu;
            fprintf(stderr,
                    "[SCDA-CONTAINER]   elem[%u]=0x%08X field+0x14=0x%08X\n",
                    i, elem, MEM32(elem + 0x14u));
        }
        fflush(stderr);
    }
    if (xbox_va == 0x00077049u) {
        fprintf(stderr, "[SCDA-77049-TRACE] fn=sub_00077049 eax(ptr)=0x%08X ecx=0x%08X esp=0x%08X\n",
                g_eax, g_ecx, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00045ADEu) {
        fprintf(stderr, "[SCDA-45ADE] edge=entry edi=0x%08X ecx=0x%08X esp=0x%08X\n",
                g_edi, g_ecx, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00045AF5u) {
        uint32_t itail_target = MEM32(g_eax + 0x10u);
        fprintf(stderr,
                "[SCDA-45ADE] edge=pre-itail edi=0x%08X eax=0x%08X "
                "itail_target=0x%08X esp=0x%08X\n",
                g_edi, g_eax, itail_target, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00047456u) {
        /* Already logged by [SCDA-VCALL] above -- also tag whether the
         * ITAIL path was the one taken this run, by checking if we ever
         * saw 45AF5 fire for the same invocation (external correlation
         * via timestamps/order in the log, not tracked here directly). */
    }
    /* Verified divergence (2026-09-02, xemu vs. scda_recomp): EDI is
     * identical entering the vtable dispatch at 0x0004744A on both real
     * hardware and our build, but only real hardware's EDI survives the
     * call. Systematic bisection per plan: capture the resolved vtable
     * target at the call site first (to rule out "we call a different
     * function entirely"), then EDI at entry/exit of each function in
     * the confirmed chain sub_000360A3 -> sub_001C445D -> sub_0024A50B
     * to find the FIRST one where it diverges. */
    if (xbox_va == 0x0004744Au) {
        uint32_t this_ptr = g_ecx;
        uint32_t vtable = this_ptr ? MEM32(this_ptr) : 0;
        uint32_t target = vtable ? MEM32(vtable + 0x10u) : 0;
        fprintf(stderr,
                "[SCDA-VCALL] pc=0x%08X this=0x%08X vtable=0x%08X "
                "slot=+0x10 target=0x%08X edi_before=0x%08X esp=0x%08X\n",
                xbox_va, this_ptr, vtable, target, g_edi, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x00047456u) {
        fprintf(stderr,
                "[SCDA-VCALL] pc=0x%08X edi_after=0x%08X eax_after=0x%08X\n",
                xbox_va, g_edi, g_eax);
        fflush(stderr);
    }
    if (xbox_va == 0x000360A3u) {
        fprintf(stderr, "[SCDA-BISECT] fn=sub_000360A3 edge=entry edi=0x%08X ecx=0x%08X esp=0x%08X\n",
                g_edi, g_ecx, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x000360B2u) {
        fprintf(stderr, "[SCDA-BISECT] fn=sub_000360A3 edge=post-1C445D-call edi=0x%08X esp=0x%08X\n",
                g_edi, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x001C445Du) {
        fprintf(stderr, "[SCDA-BISECT] fn=sub_001C445D edge=entry edi=0x%08X ecx=0x%08X esp=0x%08X\n",
                g_edi, g_ecx, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x001C44CFu) {
        fprintf(stderr, "[SCDA-BISECT] fn=sub_001C445D edge=exit edi=0x%08X esp=0x%08X\n",
                g_edi, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x0024A50Bu) {
        fprintf(stderr, "[SCDA-BISECT] fn=sub_0024A50B edge=entry edi=0x%08X ecx=0x%08X esp=0x%08X\n",
                g_edi, g_ecx, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x0024A525u) {
        fprintf(stderr, "[SCDA-BISECT] fn=sub_0024A50B edge=pre-pop edi=0x%08X esp=0x%08X\n",
                g_edi, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x0024A50Bu) {
        /* Fires before "eax = MEM32(esi); esi = ecx;" execute -- esi will
         * become ecx and eax will become MEM32(that), so recompute what
         * the branch decision (eax == 0 ? direct-exit : icall-path) will
         * actually see, rather than trusting eax read further downstream
         * after the function has already returned and other code may
         * have reset it. */
        uint32_t will_be_eax = g_ecx ? MEM32(g_ecx) : 0;
        fprintf(stderr,
                "[SCDA-0024A50B-ENTRY] pc=0x%08X ecx=0x%08X will_be_eax=0x%08X "
                "edi_in=0x%08X esp=0x%08X\n",
                xbox_va, g_ecx, will_be_eax, g_edi, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x0024A517u) {
        uint32_t vt_ecx = MEM32(0x005B57F8u);
        uint32_t vt = vt_ecx ? MEM32(vt_ecx) : 0;
        uint32_t target = vt ? MEM32(vt + 8) : 0;
        fprintf(stderr,
                "[SCDA-0024A517-ICALL] pc=0x%08X edi_before=0x%08X "
                "eax=0x%08X vt_ecx=0x%08X vt=0x%08X target=0x%08X esp=0x%08X\n",
                xbox_va, g_edi, g_eax, vt_ecx, vt, target, g_esp);
        fflush(stderr);
    }
    if (xbox_va == 0x0004721Eu) {
        edi57570c_watch_reported = 0; /* fresh invocation: rearm */
    } else if ((xbox_va > 0x0004721Eu && xbox_va <= 0x00047488u) ||
               xbox_va == 0x000360B2u) {
      if (g_edi != 0x005A570Cu && edi57570c_watch_reported < 20) {
        fprintf(stderr,
                "[SCDA-EDI57570C-CLOBBER] #%d pc=0x%08X previous_pc=0x%08X "
                "edi=0x%08X eax=0x%08X ecx=0x%08X esi=0x%08X esp=0x%08X\n",
                edi57570c_watch_reported, xbox_va, previous_pc, g_edi,
                g_eax, g_ecx, g_esi, g_esp);
        fflush(stderr);
        edi57570c_watch_reported++;
      }
    }

    previous_pc = xbox_va;
}

/*
 * sub_00040A88 -- hand-translated override (2026-09-05, continued 45).
 *
 * This is the UMOD stream reader's Seek(this=ecx, targetPos=[esp+4])
 * method. The generic disassembler badly fragmented its real body
 * (0x00040A88-0x00040AE2, ending in a genuine `ret 4`): six addresses
 * inside it (0x40A8C, 0x40A90, 0x40A94, 0x40A98, 0x40A9C, 0x40AA0) got
 * misdetected as independent function starts by tools/disasm's
 * `_pass_imm_ref_targets` pass, each one truncating the real function
 * short. The auto-generated body for sub_00040A88 ended up only 4
 * instructions long (`push esi; push edi; mov edi,[esp+0xc]; push 0`) --
 * its real epilogue, and the `[esi+0x48]`/`[esi+0x4c]` position-field
 * writes that make this the reader's "Seek" method, never ran.
 *
 * Traced to this function via a long xemu-vs-scda_recomp investigation
 * (docs/SCDA_WORKLOG.md, continued 39-44): scda_recomp's UMOD reader for
 * `z:\Default\DynamicXbox.umd` was found positioned at byte offset 0x14
 * instead of the archive's real footer offset (`info_off`, 0x712FF0 for
 * the copy of dynamicxbox.umd this build ships) at the exact point
 * sub_00045D0B needs it correctly seeked -- which fed a wildly wrong
 * "FILES" compact-index decode (7068 instead of the real 795), which
 * fed a loop in sub_00045E42 with ~9x too many iterations, which is a
 * large part of why the whole chain (continued 38-40) looks like it
 * spins forever in sub_004A12B0 instead of finishing in a normal amount
 * of time and reaching sub_00048B15's device-creation tail.
 *
 * Hand-translated below from the real bytes (read via capstone directly
 * against default.xbe, not the broken generated output, and cross-
 * checked against sub_000518FE/sub_00072655/sub_00070854's own already-
 * correct generated bodies for their calling conventions). No behavior
 * change beyond what the real game code already does -- this is a
 * disassembly-completeness fix, not a workaround: the same class of fix
 * as the sub_002E7381 constructor gap found in continued 36.
 *
 * 2026-09-11 correction: the epilogue's original "esp += 4" for its
 * real ret-4 was off by one virtual stack slot -- this runtime's
 * convention (confirmed
 * against many auto-generated functions, e.g. any real `ret 4` shows up
 * as `esp += 8`) is that the return-address slot this model never
 * really pushes/pops natively still costs 4 bytes of the adjustment, so
 * a real `ret N` must translate to `esp += (4 + N)`, not `esp += N`.
 * This one instance used `esp += N` instead, silently eating the callee's
 * own single stack argument (the Seek target position) one call late --
 * traced live via a 5-level icall chain (sub_0007188A -> sub_00045738 ->
 * this function -> back into sub_00045738 -> sub_00040BB0/Serialize),
 * where the resulting 4-byte-short stack caused sub_00045738 to read a
 * stale heap pointer instead of the small length it should have passed
 * to Serialize -- the root cause of the ~48.5MB heap OOM chased across
 * Updates 92-93 in PROJECT_STATUS.md. Fixed to `esp += 8`.
 */
/* recomp_manual.c is hand-written code, not a RECOMP_GENERATED_CODE
 * translation unit, so the bare eax/ecx/esp/esi/edi register-alias
 * macros aren't active by default (see recomp_types.h) -- and
 * RECOMP_ICALL_SAFE's own expansion uses the bare names internally, so
 * a body that calls it needs them defined too, not just g_-prefixed
 * reads/writes. Scoped locally to this one function; undefined right
 * after so the rest of this hand-written file is unaffected. */
#define eax g_eax
#define ecx g_ecx
#define esp g_esp
#define esi g_esi
#define edi g_edi

void sub_00040A88(void)
{
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fas; (void)_fbs;

    RECOMP_TRACE_PC(0x00040A88u);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    edi = MEM32(esp + 0xCu);
    PUSH32(esp, 0);
    PUSH32(esp, 0);
    esi = ecx;
    PUSH32(esp, edi);
    PUSH32(esp, MEM32(esi + 0x3Cu));
    PUSH32(esp, 0x00040A9Du); RECOMP_ABI_CALL(0x000518FEu, sub_000518FE); /* call 0x518fe */

loc_00040A9D:
    RECOMP_TRACE_PC(0x00040A9Du);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(0xFFFFFFFFu) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, -1 (32-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_00040AD3; /* jne: seek succeeded, skip error path */

loc_00040AA2:
    RECOMP_TRACE_PC(0x00040AA2u);
    PUSH32(esp, 0);
    MEM32(esi + 0x34u) = 1;
    PUSH32(esp, 0x00040AB0u); RECOMP_ABI_CALL(0x00072655u, sub_00072655); /* call 0x72655 */

loc_00040AB0:
    RECOMP_TRACE_PC(0x00040AB0u);
    PUSH32(esp, eax);
    PUSH32(esp, MEM32(esi + 0x48u));
    PUSH32(esp, MEM32(esi + 0x44u));
    PUSH32(esp, edi);
    PUSH32(esp, 0x4F6A40u);
    PUSH32(esp, MEM32(esi + 0x40u));
    PUSH32(esp, 0x00040AC5u); RECOMP_ABI_CALL(0x00070854u, sub_00070854); /* call 0x70854 */

loc_00040AC5:
    RECOMP_TRACE_PC(0x00040AC5u);
    eax = MEM32(0x5E6E0Cu);
    esp = esp + 0x1Cu;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = 0u;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test eax, eax (32-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_00040AD3; /* je: no registered callback, skip */

loc_00040AD1:
    RECOMP_TRACE_PC(0x00040AD1u);
    { uint32_t _icall_esp = g_esp;
    g_icall_source = 0x00040AD1u; { uint32_t _icall_target = eax; PUSH32(esp, 0x00040AD3u); RECOMP_ICALL_SAFE(_icall_target, _icall_esp); } /* indirect call */
    }

loc_00040AD3:
    RECOMP_TRACE_PC(0x00040AD3u);
    MEM32(esi + 0x50u) = MEM32(esi + 0x50u) & 0;
    MEM32(esi + 0x48u) = edi;
    MEM32(esi + 0x4Cu) = edi;
    POP32(esp, edi);
    POP32(esp, esi);
    esp += 8; return; /* ret 4 */
}

#undef eax
#undef ecx
#undef esp
#undef esi
#undef edi

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

void sub_00040821(void)
{
    uint32_t ebp;
    ebp = g_ebp;  /* prologue saves caller's frame */
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;

loc_00040821: ;
    RECOMP_TRACE_PC(0x00040821u);
    PUSH32(esp, ebp);
    ebp = esp;
    g_ebp = ebp; /* publish frame for frameless callees */
    g_seh_ebp = ebp;
    esp = esp - 0x14;
    eax = MEM32(ebp + 0xC);
    PUSH32(esp, ebx);
    ebx = ecx;
    scda_ring_trace_boundary(ebx, 1, MEM32(ebp + 0xCu),
                             MEM32(ebp + 4u), 0);
    scda_ring_trace_callstack(ebx, ebp, MEM32(ebp + 8u),
                              MEM32(ebp + 0xCu));
    MEM32(ebx + 0x48) = MEM32(ebx + 0x48) + eax;
    edx = 0; /* xor self */
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(eax) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test eax, eax (32-bit) */
    MEM32(ebp + -8) = edx;
    if (TEST_Z(_fa, _fb)) goto loc_00040A10; /* je: equal / zero */

loc_0004083D: ;
    RECOMP_TRACE_PC(0x0004083Du);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    goto loc_00040847;

loc_00040841: ;
    RECOMP_TRACE_PC(0x00040841u);
    eax = MEM32(ebp + 0xC);
    edx = MEM32(ebp + -8);

loc_00040847: ;
    RECOMP_TRACE_PC(0x00040847u);
    ecx = MEM32(ebx + 0x50);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(ecx) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, ecx (32-bit) */
    if (CMP_LE(_fas, _fbs)) goto loc_000409DE; /* jle: less or equal (signed <=) */

loc_00040852: ;
    RECOMP_TRACE_PC(0x00040852u);
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fb = (uint32_t)(ecx) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test ecx, ecx (32-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_0004087B; /* je: equal / zero */

loc_00040856: ;
    RECOMP_TRACE_PC(0x00040856u);
    edi = MEM32(ebp + 8);
    esi = MEM32(ebx + 0x4C);
    edi = edi + edx;
    edx = ecx;
    ecx = ecx >> 2;
    esi = esi + ebx + 0x58;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4); esi += ecx * 4; edi += ecx * 4; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi - _i*4) = MEM32(esi - _i*4); esi -= ecx * 4; edi -= ecx * 4; }
    ecx = 0; /* rep movsd */
    ecx = edx;
    ecx = ecx & 3;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx); esi += ecx; edi += ecx; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM8(edi - _i) = MEM8(esi - _i); esi -= ecx; edi -= ecx; }
    ecx = 0; /* rep movsb */
    ecx = MEM32(ebx + 0x50);
    eax = eax - ecx;
    MEM32(ebp + -8) = MEM32(ebp + -8) + ecx;
    MEM32(ebp + 0xC) = eax;

loc_0004087B: ;
    RECOMP_TRACE_PC(0x0004087Bu);
    eax = MEM32(ebx + 0x4060);
    ecx = MEM32(ebx + 0x4058);
    MEM32(ebp + -4) = MEM32(ebp + -4) & 0;
    ecx = ecx - eax;
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fb = (uint32_t)(8) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp ecx, 8 (32-bit) */
    MEM32(ebp + -12) = ecx;
    edx = 0x4000;
    scda_ring_trace_state(ebx, 1, eax, eax, ecx,
                          ecx < 8u, 0, 0, ebp);
    if (CMP_AE(_fa, _fb)) goto loc_00040909; /* jae: above or equal (unsigned >=) */

loc_0004089A: ;
    RECOMP_TRACE_PC(0x0004089Au);
    edi = MEM32(ebx + 0x405C);
    esi = edi + eax;
    edi = edi - ecx;
    eax = ecx;
    ecx = ecx >> 2;
    edi = edi + edx;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4); esi += ecx * 4; edi += ecx * 4; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi - _i*4) = MEM32(esi - _i*4); esi -= ecx * 4; edi -= ecx * 4; }
    ecx = 0; /* rep movsd */
    ecx = eax;
    ecx = ecx & 3;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx); esi += ecx; edi += ecx; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM8(edi - _i) = MEM8(esi - _i); esi -= ecx; edi -= ecx; }
    ecx = 0; /* rep movsb */
    eax = MEM32(ebx + 0x44);
    eax = eax >> 0xB;
    eax++;
    eax = eax << 0xB;
    ecx = 0x20000;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(ecx) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, ecx (32-bit) */
    if (CMP_A(_fa, _fb)) eax = ecx; /* cmova */
    scda_ring_trace_refillstack(ebx, 0x000408E1u, ebp);
    scda_ring_trace_begin(ebx, 0x000408E1u, 1,
                          MEM32(ebx + 0x4060u),
                          MEM32(ebx + 0x4058u), MEM32(ebp + -12),
                          1, 0, 0);
    PUSH32(esp, 0);
    ecx = ebp + -4;
    PUSH32(esp, ecx);
    PUSH32(esp, eax);
    eax = MEM32(ebx + 0x405C);
    eax = eax + edx;
    PUSH32(esp, eax);
    PUSH32(esp, MEM32(ebx + 0x3C));
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x000408E1u); RECOMP_ABI_CALL(0x000516B7u, sub_000516B7); /* call 0x000516B7 */

loc_000408E1: ;
    RECOMP_TRACE_PC(0x000408E1u);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(eax) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test eax, eax (32-bit) */
    if (TEST_NZ(_fa, _fb)) goto loc_000408F0; /* jne: not equal / not zero */

loc_000408E5: ;
    RECOMP_TRACE_PC(0x000408E5u);
    eax = MEM32(0x5E6E0C);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(eax) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test eax, eax (32-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_000408F0; /* je: equal / zero */

loc_000408EE: ;
    RECOMP_TRACE_PC(0x000408EEu);
    { uint32_t _icall_esp = g_esp;
    g_icall_source = 0x000408EEu; { uint32_t _icall_target = eax; PUSH32(esp, 0x000408F0u); RECOMP_ICALL_SAFE(_icall_target, _icall_esp); } /* indirect call */
    }

loc_000408F0: ;
    RECOMP_TRACE_PC(0x000408F0u);
    eax = MEM32(ebp + -4);
    MEM32(ebx + 0x4064) = MEM32(ebx + 0x4064) + eax;
    /* SCDA-FIX-215 REVERTED FOR REAL, PERMANENTLY (docs Update 274/275):
       Capstone-verified against the original XBE bytes at 0x000408F0:
       `mov eax,[ebp-4]; add [ebx+4064],eax; mov edx,0x4000; mov eax,edx;
       sub eax,[ebp-0xc]; mov [ebx+4060],eax`. The Update 220/221 fix
       misread this: eax (`MEM32(ebp-4)`, the real transferred byte count)
       is used ONLY for the +0x4064 statistics accumulator on the line
       above -- the position formula on the next line explicitly
       OVERWRITES eax with the constant 0x4000 (`mov eax,edx`) before
       subtracting, discarding the real transfer count entirely. This is
       not "assumed transfer size", it is the buffer's fixed physical
       write offset (this ring buffer always fills starting at
       `fillbase+0x4000`, confirmed live in Update 273/274): the position
       must be measured relative to that same fixed mark, independent of
       how many bytes the OS actually transferred (which can be anywhere
       up to 0x20000). Using the real transferred count instead (the
       Update 220/221 fix) is only numerically harmless when transferred
       happens to be small (the short/EOF read from Updates 213-221); for
       a full, non-EOF transfer it places the header-parsing cursor deep
       inside the just-read payload instead of at its start, corrupting
       every downstream chunk header and permanently failing the decode
       call (`sub_000503EE`) that fills `ebx+0x58` -- byte-exact live
       proof in Update 274. Restored to the original, byte-verified
       formula. */
    edx = 0x4000;
    eax = edx - MEM32(ebp + -12);
    MEM32(ebx + 0x4060) = eax;
    scda_ring_trace_commit(ebx, 1);

loc_00040909: ;
    RECOMP_TRACE_PC(0x00040909u);
    eax = MEM32(ebx + 0x4060);
    edi = MEM32(ebx + 0x405C);
    ecx = MEM32(edi + eax);
    eax = eax + 4;
    MEM32(ebp + -20) = ecx;
    ecx = MEM32(ebx + 0x4058);
    MEM32(ebx + 0x4060) = eax;
    esi = MEM32(eax + edi);
    eax = eax + 4;
    ecx = ecx - eax;
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fb = (uint32_t)(esi) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp ecx, esi (32-bit) */
    MEM32(ebp + -16) = esi;
    MEM32(ebx + 0x4060) = eax;
    MEM32(ebp + -12) = ecx;
    scda_ring_trace_state(ebx, 2, eax - 8u, eax, ecx,
                          ecx < esi, MEM32(ebp + -20), esi, ebp);
    if (CMP_AE(_fa, _fb)) goto loc_000409A9; /* jae: above or equal (unsigned >=) */

loc_00040942: ;
    RECOMP_TRACE_PC(0x00040942u);
    scda_ring_trace_state(ebx, 3, MEM32(ebx + 0x4060u),
                          MEM32(ebx + 0x4060u), MEM32(ebp + -12), 1,
                          MEM32(ebp + -20), MEM32(ebp + -16), ebp);
    esi = eax + edi;
    edi = edi - ecx;
    eax = ecx;
    ecx = ecx >> 2;
    edi = edi + edx;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4); esi += ecx * 4; edi += ecx * 4; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi - _i*4) = MEM32(esi - _i*4); esi -= ecx * 4; edi -= ecx * 4; }
    ecx = 0; /* rep movsd */
    ecx = eax;
    ecx = ecx & 3;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx); esi += ecx; edi += ecx; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM8(edi - _i) = MEM8(esi - _i); esi -= ecx; edi -= ecx; }
    ecx = 0; /* rep movsb */
    eax = MEM32(ebx + 0x44);
    eax = eax >> 0xB;
    eax++;
    eax = eax << 0xB;
    ecx = 0x20000;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(ecx) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, ecx (32-bit) */
    if (CMP_A(_fa, _fb)) eax = ecx; /* cmova */
    scda_ring_trace_refillstack(ebx, 0x00040983u, ebp);
    scda_ring_trace_begin(ebx, 0x00040983u, 2,
                          MEM32(ebx + 0x4060u),
                          MEM32(ebx + 0x4058u), MEM32(ebp + -12),
                          1, MEM32(ebp + -20), MEM32(ebp + -16));
    PUSH32(esp, 0);
    ecx = ebp + -4;
    PUSH32(esp, ecx);
    PUSH32(esp, eax);
    eax = MEM32(ebx + 0x405C);
    eax = eax + edx;
    PUSH32(esp, eax);
    PUSH32(esp, MEM32(ebx + 0x3C));
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x00040983u); RECOMP_ABI_CALL(0x000516B7u, sub_000516B7); /* call 0x000516B7 */

loc_00040983: ;
    RECOMP_TRACE_PC(0x00040983u);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(eax) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test eax, eax (32-bit) */
    if (TEST_NZ(_fa, _fb)) goto loc_00040992; /* jne: not equal / not zero */

loc_00040987: ;
    RECOMP_TRACE_PC(0x00040987u);
    eax = MEM32(0x5E6E0C);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(eax) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test eax, eax (32-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_00040992; /* je: equal / zero */

loc_00040990: ;
    RECOMP_TRACE_PC(0x00040990u);
    { uint32_t _icall_esp = g_esp;
    g_icall_source = 0x00040990u; { uint32_t _icall_target = eax; PUSH32(esp, 0x00040992u); RECOMP_ICALL_SAFE(_icall_target, _icall_esp); } /* indirect call */
    }

loc_00040992: ;
    RECOMP_TRACE_PC(0x00040992u);
    eax = MEM32(ebp + -4);
    MEM32(ebx + 0x4064) = MEM32(ebx + 0x4064) + eax;
    /* SCDA-FIX-215 REVERTED FOR REAL, PERMANENTLY (docs Update 274/275):
       Capstone-verified against the original XBE bytes at 0x00040992:
       `mov eax,[ebp-4]; add [ebx+4064],eax; mov eax,0x4000;
       sub eax,[ebp-0xc]; mov [ebx+4060],eax`. Same defect and same root
       cause as loc_000408F0 -- see that site's comment for the full
       justification. eax is reloaded with the constant 0x4000 before the
       subtraction; the real transferred count (also in eax one line
       above) is used only for the +0x4064 accumulator, never for this
       formula. Restored to the original, byte-verified formula. */
    eax = 0x4000 - MEM32(ebp + -12);
    MEM32(ebx + 0x4060) = eax;
    scda_ring_trace_commit(ebx, 2);

loc_000409A9: ;
    RECOMP_TRACE_PC(0x000409A9u);
    eax = MEM32(ebx + 0x4060);
    eax = eax + MEM32(ebx + 0x405C);
    PUSH32(esp, MEM32(ebp + -16));
    PUSH32(esp, eax);
    eax = ebp + -20;
    PUSH32(esp, eax);
    eax = ebx + 0x58;
    PUSH32(esp, eax);
    g_ebp = ebp; /* frame stays current across calls */
    g_seh_ebp = ebp;
    PUSH32(esp, 0x000409C6u); RECOMP_ABI_CALL(0x000503EEu, sub_000503EE); /* call 0x000503EE */

loc_000409C6: ;
    RECOMP_TRACE_PC(0x000409C6u);
    eax = MEM32(ebp + -16);
    MEM32(ebx + 0x4060) = MEM32(ebx + 0x4060) + eax;
    eax = MEM32(ebp + -20);
    esp = esp + 0x10;
    MEM32(ebx + 0x4C) = MEM32(ebx + 0x4C) & 0;
    MEM32(ebx + 0x50) = eax;
    goto loc_00040A04;

loc_000409DE: ;
    RECOMP_TRACE_PC(0x000409DEu);
    esi = MEM32(ebx + 0x4C);
    edi = MEM32(ebp + 8);
    edi = edi + edx;
    ecx = eax;
    edx = ecx;
    ecx = ecx >> 2;
    esi = esi + ebx + 0x58;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4); esi += ecx * 4; edi += ecx * 4; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi - _i*4) = MEM32(esi - _i*4); esi -= ecx * 4; edi -= ecx * 4; }
    ecx = 0; /* rep movsd */
    ecx = edx;
    ecx = ecx & 3;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx); esi += ecx; edi += ecx; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM8(edi - _i) = MEM8(esi - _i); esi -= ecx; edi -= ecx; }
    ecx = 0; /* rep movsb */
    MEM32(ebx + 0x50) = MEM32(ebx + 0x50) - eax;
    MEM32(ebx + 0x4C) = MEM32(ebx + 0x4C) + eax;
    MEM32(ebp + 0xC) = MEM32(ebp + 0xC) & 0;

loc_00040A04: ;
    RECOMP_TRACE_PC(0x00040A04u);
    _fa = (uint32_t)(MEM32(ebp + 0xC)) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp MEM32(ebp + 0xC), 0 (32-bit) */
    if (CMP_NE(_fa, _fb)) goto loc_00040841; /* jne: not equal / not zero */

loc_00040A0E: ;
    RECOMP_TRACE_PC(0x00040A0Eu);
    POP32(esp, edi);
    POP32(esp, esi);

loc_00040A10: ;
    RECOMP_TRACE_PC(0x00040A10u);
    scda_ring_trace_output(ebx, MEM32(ebp + 8u));
    scda_ring_trace_boundary(ebx, 0, MEM32(ebp + 0xCu),
                             MEM32(ebp + 4u), MEM32(ebp + -8));
    POP32(esp, ebx);
    esp = ebp;
    POP32(esp, ebp); /* leave */
    esp += 12; return; /* ret 8 */

}

void sub_0004086E(void)
{
    uint32_t ebp;
    ebp = g_ebp;  /* frameless: caller's frame */
    int _flags = 0; /* fallback flag var */
    uint32_t _fa = 0, _fb = 0;
    int32_t _fas = 0, _fbs = 0;
    (void)_fa; (void)_fb; (void)_fas; (void)_fbs;
    ebp = g_seh_ebp; /* fpo_leaf: inherit caller's frame */

loc_0004086E: ;
    RECOMP_TRACE_PC(0x0004086Eu);
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx); esi += ecx; edi += ecx; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM8(edi - _i) = MEM8(esi - _i); esi -= ecx; edi -= ecx; }
    ecx = 0; /* rep movsb */
    ecx = MEM32(ebx + 0x50);
    eax = eax - ecx;
    MEM32(ebp + -8) = MEM32(ebp + -8) + ecx;
    MEM32(ebp + 0xC) = eax;
    eax = MEM32(ebx + 0x4060);
    ecx = MEM32(ebx + 0x4058);
    MEM32(ebp + -4) = MEM32(ebp + -4) & 0;
    ecx = ecx - eax;
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fb = (uint32_t)(8) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp ecx, 8 (32-bit) */
    MEM32(ebp + -12) = ecx;
    edx = 0x4000;
    scda_ring_trace_state(ebx, 1, eax, eax, ecx,
                          ecx < 8u, 0, 0, ebp);
    if (CMP_AE(_fa, _fb)) goto loc_00040909; /* jae: above or equal (unsigned >=) */

loc_0004089A: ;
    RECOMP_TRACE_PC(0x0004089Au);
    edi = MEM32(ebx + 0x405C);
    esi = edi + eax;
    edi = edi - ecx;
    eax = ecx;
    ecx = ecx >> 2;
    edi = edi + edx;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4); esi += ecx * 4; edi += ecx * 4; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi - _i*4) = MEM32(esi - _i*4); esi -= ecx * 4; edi -= ecx * 4; }
    ecx = 0; /* rep movsd */
    ecx = eax;
    ecx = ecx & 3;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx); esi += ecx; edi += ecx; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM8(edi - _i) = MEM8(esi - _i); esi -= ecx; edi -= ecx; }
    ecx = 0; /* rep movsb */
    eax = MEM32(ebx + 0x44);
    eax = eax >> 0xB;
    eax++;
    eax = eax << 0xB;
    ecx = 0x20000;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(ecx) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, ecx (32-bit) */
    if (CMP_A(_fa, _fb)) eax = ecx; /* cmova */
    scda_ring_trace_begin(ebx, 0x000408E1u, 3,
                          MEM32(ebx + 0x4060u),
                          MEM32(ebx + 0x4058u), MEM32(ebp + -12),
                          1, 0, 0);
    PUSH32(esp, 0);
    ecx = ebp + -4;
    PUSH32(esp, ecx);
    PUSH32(esp, eax);
    eax = MEM32(ebx + 0x405C);
    eax = eax + edx;
    PUSH32(esp, eax);
    PUSH32(esp, MEM32(ebx + 0x3C));
    PUSH32(esp, 0x000408E1u); RECOMP_ABI_CALL(0x000516B7u, sub_000516B7); /* call 0x000516B7 */

loc_000408E1: ;
    RECOMP_TRACE_PC(0x000408E1u);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(eax) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test eax, eax (32-bit) */
    if (TEST_NZ(_fa, _fb)) goto loc_000408F0; /* jne: not equal / not zero */

loc_000408E5: ;
    RECOMP_TRACE_PC(0x000408E5u);
    eax = MEM32(0x5E6E0C);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(eax) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test eax, eax (32-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_000408F0; /* je: equal / zero */

loc_000408EE: ;
    RECOMP_TRACE_PC(0x000408EEu);
    { uint32_t _icall_esp = g_esp;
    g_icall_source = 0x000408EEu; { uint32_t _icall_target = eax; PUSH32(esp, 0x000408F0u); RECOMP_ICALL_SAFE(_icall_target, _icall_esp); } /* indirect call */
    }

loc_000408F0: ;
    RECOMP_TRACE_PC(0x000408F0u);
    eax = MEM32(ebp + -4);
    MEM32(ebx + 0x4064) = MEM32(ebx + 0x4064) + eax;
    /* SCDA-FIX-215 REVERTED FOR REAL, PERMANENTLY (docs Update 274/275):
       Capstone-verified against the original XBE bytes at 0x000408F0:
       `mov eax,[ebp-4]; add [ebx+4064],eax; mov edx,0x4000; mov eax,edx;
       sub eax,[ebp-0xc]; mov [ebx+4060],eax`. The Update 220/221 fix
       misread this: eax (`MEM32(ebp-4)`, the real transferred byte count)
       is used ONLY for the +0x4064 statistics accumulator on the line
       above -- the position formula on the next line explicitly
       OVERWRITES eax with the constant 0x4000 (`mov eax,edx`) before
       subtracting, discarding the real transfer count entirely. This is
       not "assumed transfer size", it is the buffer's fixed physical
       write offset (this ring buffer always fills starting at
       `fillbase+0x4000`, confirmed live in Update 273/274): the position
       must be measured relative to that same fixed mark, independent of
       how many bytes the OS actually transferred (which can be anywhere
       up to 0x20000). Using the real transferred count instead (the
       Update 220/221 fix) is only numerically harmless when transferred
       happens to be small (the short/EOF read from Updates 213-221); for
       a full, non-EOF transfer it places the header-parsing cursor deep
       inside the just-read payload instead of at its start, corrupting
       every downstream chunk header and permanently failing the decode
       call (`sub_000503EE`) that fills `ebx+0x58` -- byte-exact live
       proof in Update 274. Restored to the original, byte-verified
       formula. */
    edx = 0x4000;
    eax = edx - MEM32(ebp + -12);
    MEM32(ebx + 0x4060) = eax;
    scda_ring_trace_commit(ebx, 3);

loc_00040909: ;
    RECOMP_TRACE_PC(0x00040909u);
    eax = MEM32(ebx + 0x4060);
    edi = MEM32(ebx + 0x405C);
    ecx = MEM32(edi + eax);
    eax = eax + 4;
    MEM32(ebp + -20) = ecx;
    ecx = MEM32(ebx + 0x4058);
    MEM32(ebx + 0x4060) = eax;
    esi = MEM32(eax + edi);
    eax = eax + 4;
    ecx = ecx - eax;
    _fa = (uint32_t)(ecx) & 0xFFFFFFFFu; _fb = (uint32_t)(esi) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp ecx, esi (32-bit) */
    MEM32(ebp + -16) = esi;
    MEM32(ebx + 0x4060) = eax;
    MEM32(ebp + -12) = ecx;
    scda_ring_trace_state(ebx, 2, eax - 8u, eax, ecx,
                          ecx < esi, MEM32(ebp + -20), esi, ebp);
    if (CMP_AE(_fa, _fb)) goto loc_000409A9; /* jae: above or equal (unsigned >=) */

loc_00040942: ;
    RECOMP_TRACE_PC(0x00040942u);
    scda_ring_trace_state(ebx, 3, MEM32(ebx + 0x4060u),
                          MEM32(ebx + 0x4060u), MEM32(ebp + -12), 1,
                          MEM32(ebp + -20), MEM32(ebp + -16), ebp);
    esi = eax + edi;
    edi = edi - ecx;
    eax = ecx;
    ecx = ecx >> 2;
    edi = edi + edx;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4); esi += ecx * 4; edi += ecx * 4; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi - _i*4) = MEM32(esi - _i*4); esi -= ecx * 4; edi -= ecx * 4; }
    ecx = 0; /* rep movsd */
    ecx = eax;
    ecx = ecx & 3;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx); esi += ecx; edi += ecx; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM8(edi - _i) = MEM8(esi - _i); esi -= ecx; edi -= ecx; }
    ecx = 0; /* rep movsb */
    eax = MEM32(ebx + 0x44);
    eax = eax >> 0xB;
    eax++;
    eax = eax << 0xB;
    ecx = 0x20000;
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(ecx) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp eax, ecx (32-bit) */
    if (CMP_A(_fa, _fb)) eax = ecx; /* cmova */
    scda_ring_trace_begin(ebx, 0x00040983u, 4,
                          MEM32(ebx + 0x4060u),
                          MEM32(ebx + 0x4058u), MEM32(ebp + -12),
                          1, MEM32(ebp + -20), MEM32(ebp + -16));
    PUSH32(esp, 0);
    ecx = ebp + -4;
    PUSH32(esp, ecx);
    PUSH32(esp, eax);
    eax = MEM32(ebx + 0x405C);
    eax = eax + edx;
    PUSH32(esp, eax);
    PUSH32(esp, MEM32(ebx + 0x3C));
    PUSH32(esp, 0x00040983u); RECOMP_ABI_CALL(0x000516B7u, sub_000516B7); /* call 0x000516B7 */

loc_00040983: ;
    RECOMP_TRACE_PC(0x00040983u);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(eax) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test eax, eax (32-bit) */
    if (TEST_NZ(_fa, _fb)) goto loc_00040992; /* jne: not equal / not zero */

loc_00040987: ;
    RECOMP_TRACE_PC(0x00040987u);
    eax = MEM32(0x5E6E0C);
    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fb = (uint32_t)(eax) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* test eax, eax (32-bit) */
    if (TEST_Z(_fa, _fb)) goto loc_00040992; /* je: equal / zero */

loc_00040990: ;
    RECOMP_TRACE_PC(0x00040990u);
    { uint32_t _icall_esp = g_esp;
    g_icall_source = 0x00040990u; { uint32_t _icall_target = eax; PUSH32(esp, 0x00040992u); RECOMP_ICALL_SAFE(_icall_target, _icall_esp); } /* indirect call */
    }

loc_00040992: ;
    RECOMP_TRACE_PC(0x00040992u);
    eax = MEM32(ebp + -4);
    MEM32(ebx + 0x4064) = MEM32(ebx + 0x4064) + eax;
    /* SCDA-FIX-215 REVERTED FOR REAL, PERMANENTLY (docs Update 274/275):
       Capstone-verified against the original XBE bytes at 0x00040992:
       `mov eax,[ebp-4]; add [ebx+4064],eax; mov eax,0x4000;
       sub eax,[ebp-0xc]; mov [ebx+4060],eax`. Same defect and same root
       cause as loc_000408F0 -- see that site's comment for the full
       justification. eax is reloaded with the constant 0x4000 before the
       subtraction; the real transferred count (also in eax one line
       above) is used only for the +0x4064 accumulator, never for this
       formula. Restored to the original, byte-verified formula. */
    eax = 0x4000 - MEM32(ebp + -12);
    MEM32(ebx + 0x4060) = eax;
    scda_ring_trace_commit(ebx, 4);

loc_000409A9: ;
    RECOMP_TRACE_PC(0x000409A9u);
    eax = MEM32(ebx + 0x4060);
    eax = eax + MEM32(ebx + 0x405C);
    PUSH32(esp, MEM32(ebp + -16));
    PUSH32(esp, eax);
    eax = ebp + -20;
    PUSH32(esp, eax);
    eax = ebx + 0x58;
    PUSH32(esp, eax);
    PUSH32(esp, 0x000409C6u); RECOMP_ABI_CALL(0x000503EEu, sub_000503EE); /* call 0x000503EE */

loc_000409C6: ;
    RECOMP_TRACE_PC(0x000409C6u);
    eax = MEM32(ebp + -16);
    MEM32(ebx + 0x4060) = MEM32(ebx + 0x4060) + eax;
    eax = MEM32(ebp + -20);
    esp = esp + 0x10;
    MEM32(ebx + 0x4C) = MEM32(ebx + 0x4C) & 0;
    MEM32(ebx + 0x50) = eax;
    goto loc_00040A04;

    RECOMP_TRACE_PC(0x000409DEu);
    esi = MEM32(ebx + 0x4C);
    edi = MEM32(ebp + 8);
    edi = edi + edx;
    ecx = eax;
    edx = ecx;
    ecx = ecx >> 2;
    esi = esi + ebx + 0x58;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx * 4); esi += ecx * 4; edi += ecx * 4; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM32(edi - _i*4) = MEM32(esi - _i*4); esi -= ecx * 4; edi -= ecx * 4; }
    ecx = 0; /* rep movsd */
    ecx = edx;
    ecx = ecx & 3;
    if (!g_df) { memcpy((void*)XBOX_PTR(edi), (void*)XBOX_PTR(esi), ecx); esi += ecx; edi += ecx; }
    else { uint32_t _i; for (_i = 0; _i < ecx; _i++) MEM8(edi - _i) = MEM8(esi - _i); esi -= ecx; edi -= ecx; }
    ecx = 0; /* rep movsb */
    MEM32(ebx + 0x50) = MEM32(ebx + 0x50) - eax;
    MEM32(ebx + 0x4C) = MEM32(ebx + 0x4C) + eax;
    MEM32(ebp + 0xC) = MEM32(ebp + 0xC) & 0;

loc_00040A04: ;
    RECOMP_TRACE_PC(0x00040A04u);
    _fa = (uint32_t)(MEM32(ebp + 0xC)) & 0xFFFFFFFFu; _fb = (uint32_t)(0) & 0xFFFFFFFFu;
    _fas = (int32_t)(int32_t)(_fa); _fbs = (int32_t)(int32_t)(_fb); /* cmp MEM32(ebp + 0xC), 0 (32-bit) */
    if (CMP_NE(_fa, _fb)) { g_seh_ebp = ebp; sub_00040841(); return; } /* jne: not equal / not zero */

loc_00040A0E: ;
    RECOMP_TRACE_PC(0x00040A0Eu);
    POP32(esp, edi);
    POP32(esp, esi);
    POP32(esp, ebx);
    esp = ebp;
    POP32(esp, ebp); /* leave */
    esp += 12; return; /* ret 8 */

}

#undef eax
#undef ebx
#undef ecx
#undef edx
#undef esp
#undef esi
#undef edi

/*
 * SCDA controller bridge -- XAPI input entry points (docs Update 347).
 *
 * SCDA links XAPI 5849's input stack statically into section XPP. Its OHCI /
 * XID driver never runs under the recomp, so XDEVICE_TYPE_GAMEPAD
 * (0x004EC4D4: CurrentConnected, ChangeConnected, PreviousConnected) stays
 * empty and the title never leaves "Press START". Identified byte-exactly
 * (Capstone, tools/debug/xbe_dis.py; the game's call sites push 0x004EC4D4):
 *   0x004EDA95 XGetDevices(type)                     ret 4
 *   0x004EDAB7 XGetDeviceChanges(type, &ins, &rem)   ret 0xC
 *   0x004EDB24 XInputOpen(type, port, slot, params)  ret 0x10
 *   0x004EDB7A XInputClose(handle)                   ret 4
 *   0x004EDB86 XInputGetState(handle, state)         ret 8
 *   0x004EDBF9 XInputSetState(handle, feedback)      ret 8
 *
 * XGetDevices/XGetDeviceChanges keep the original table arithmetic; before
 * it they apply the host's connection state to the gamepad table the way
 * the XID driver's insert/remove path does (Current updated, changed bits
 * set in ChangeConnected). The handle-based functions talk to the host pad
 * (projects/scda/src/scda_input.c) instead of an XID device node. Handles
 * are opaque cookies; nothing in the title dereferences them (it only
 * passes them back to these functions).
 */
#define SCDA_XDEVICE_TYPE_GAMEPAD 0x004EC4D4u
#define SCDA_XINPUT_HANDLE_BASE   0x58490000u  /* "XI" + port, never dereferenced */
#define SCDA_ERROR_DEVICE_NOT_CONNECTED 0x48Fu
#define SCDA_ERROR_IO_PENDING           0x3E5u

extern int scda_input_port_connected(unsigned port);
extern int scda_input_get_state(unsigned port, uint8_t *out18);
extern void scda_input_rumble(unsigned port, uint16_t left, uint16_t right);

static uint32_t g_scda_pad_packet[4];
static uint8_t  g_scda_pad_last[4][18];

static void scda_xpp_hotplug(uint32_t type)
{
    uint32_t cur, now = 0;
    unsigned p;

    if (type != SCDA_XDEVICE_TYPE_GAMEPAD)
        return;
    for (p = 0; p < 4; p++)
        if (scda_input_port_connected(p))
            now |= 1u << p;
    cur = MEM32(type);
    if (now != cur) {
        MEM32(type + 4) = MEM32(type + 4) | (now ^ cur);
        MEM32(type) = now;
        fprintf(stderr, "[SCDA-INPUT] gamepad ports 0x%X -> 0x%X\n", cur, now);
    }
}

static int scda_xinput_port(uint32_t handle)
{
    uint32_t p = handle - SCDA_XINPUT_HANDLE_BASE;
    return p < 4 ? (int)p : -1;
}

/* XGetDevices */
void sub_004EDA95(void)
{
    uint32_t type = MEM32(g_esp + 4u);
    uint32_t cur;

    scda_xpp_hotplug(type);
    cur = MEM32(type);
    MEM32(type + 4u) = 0;
    MEM32(type + 8u) = cur;
    g_eax = cur;
    g_esp += 8;
}

/* XGetDeviceChanges */
void sub_004EDAB7(void)
{
    uint32_t type = MEM32(g_esp + 4u);
    uint32_t pins = MEM32(g_esp + 8u);
    uint32_t prem = MEM32(g_esp + 12u);
    uint32_t cur, chg, prev, both, ins, rem;
    static uint32_t calls;

    /* Sparse: calls 1, 2, 4, 8, ... -- is the per-frame input update alive,
     * and what is in the first pad's handle slot (0x5E76D0, stride 0x2C)? */
    if (type == SCDA_XDEVICE_TYPE_GAMEPAD && (++calls & (calls - 1)) == 0)
        fprintf(stderr, "[SCDA-INPUT] XGetDeviceChanges call %u caller 0x%08X handle0=0x%08X\n",
                calls, MEM32(g_esp), MEM32(0x005E76D0u));
    scda_xpp_hotplug(type);
    if (MEM32(type + 4u) == 0) {
        MEM32(pins) = 0;
        MEM32(prem) = 0;
        g_eax = 0;
    } else {
        cur = MEM32(type);
        chg = MEM32(type + 4u);
        prev = MEM32(type + 8u);
        both = chg & prev & cur;  /* removed and re-inserted since last call */
        ins = (cur & ~prev) | both;
        rem = (prev & ~cur) | both;
        MEM32(pins) = ins;
        MEM32(prem) = rem;
        MEM32(type + 4u) = 0;
        MEM32(type + 8u) = cur;
        g_eax = (ins | rem) ? 1u : 0u;
    }
    g_esp += 16;
}

/* XInputOpen */
void sub_004EDB24(void)
{
    uint32_t type = MEM32(g_esp + 4u);
    uint32_t port = MEM32(g_esp + 8u);

    if (type == SCDA_XDEVICE_TYPE_GAMEPAD && port < 4 &&
        (MEM32(type) & (1u << port))) {
        fprintf(stderr, "[SCDA-INPUT] XInputOpen port %u\n", port);
        g_eax = SCDA_XINPUT_HANDLE_BASE + port;
        g_esp += 20;
        return;
    }
    fprintf(stderr, "[SCDA-INPUT] XInputOpen type=0x%08X port=%u -> not connected\n",
            type, port);
    /* SetLastError(ERROR_DEVICE_NOT_CONNECTED) through the title's own
     * helper, as the original failure path does, then return NULL. */
    PUSH32(g_esp, SCDA_ERROR_DEVICE_NOT_CONNECTED);
    PUSH32(g_esp, 0x004EDB3Fu);
    sub_00052A57();
    g_eax = 0;
    g_esp += 20;
}

/* XInputClose */
void sub_004EDB7A(void)
{
    g_esp += 8;
}

/* XInputGetState */
void sub_004EDB86(void)
{
    int port = scda_xinput_port(MEM32(g_esp + 4u));
    uint32_t state = MEM32(g_esp + 8u);
    uint8_t rep[18];
    unsigned i;

    if (port < 0 || !scda_input_get_state((unsigned)port, rep)) {
        g_eax = SCDA_ERROR_DEVICE_NOT_CONNECTED;
        g_esp += 12;
        return;
    }
    if (g_scda_pad_packet[port] == 0)
        fprintf(stderr, "[SCDA-INPUT] first XInputGetState port %d (caller 0x%08X)\n",
                port, MEM32(g_esp));
    if (memcmp(rep, g_scda_pad_last[port], sizeof rep) != 0 || g_scda_pad_packet[port] == 0) {
        memcpy(g_scda_pad_last[port], rep, sizeof rep);
        g_scda_pad_packet[port]++;
        fprintf(stderr, "[SCDA-INPUT] port %d packet %u buttons=0x%04X A=%u B=%u\n",
                port, g_scda_pad_packet[port], rep[0] | (rep[1] << 8), rep[2], rep[3]);
    }
    MEM32(state) = g_scda_pad_packet[port];
    for (i = 0; i < sizeof rep; i++)
        MEM8(state + 4u + i) = rep[i];
    g_eax = 0;
    g_esp += 12;
}

/* XInputSetState */
void sub_004EDBF9(void)
{
    int port = scda_xinput_port(MEM32(g_esp + 4u));
    uint32_t fb = MEM32(g_esp + 8u);
    static int warned_event;

    if (port < 0) {
        g_eax = SCDA_ERROR_DEVICE_NOT_CONNECTED;
        g_esp += 12;
        return;
    }
    /* XINPUT_FEEDBACK: Header {dwStatus, hEvent, Reserved[58]} = 0x42 bytes,
     * then XINPUT_RUMBLE {wLeftMotorSpeed, wRightMotorSpeed}. The request
     * completes at once: dwStatus goes straight to ERROR_SUCCESS. */
    scda_input_rumble((unsigned)port, MEM16(fb + 0x42u), MEM16(fb + 0x44u));
    MEM32(fb) = 0;
    if (MEM32(fb + 4u) && !warned_event) {
        warned_event = 1;
        fprintf(stderr, "[SCDA-INPUT] XInputSetState with hEvent=0x%08X (not signalled)\n",
                MEM32(fb + 4u));
    }
    g_eax = SCDA_ERROR_IO_PENDING;
    g_esp += 12;
}
