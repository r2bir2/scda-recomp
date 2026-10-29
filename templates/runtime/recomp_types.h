/**
 * Xbox Static Recompilation - Runtime Type Definitions
 *
 * Type definitions and helper macros used by mechanically translated
 * x86 -> C code. Each original x86 function is translated to a C
 * function that uses these types and macros.
 *
 * This is a reusable template for ANY Xbox game. Game-specific
 * customization should go in separate headers.
 *
 * Memory model:
 *   Xbox data sections are mapped to their original VAs via
 *   CreateFileMapping + MapViewOfFileEx (see xbox_memory.h).
 *   Recompiled code accesses globals via pointer casts, e.g.:
 *     *(uint32_t*)0x003B2360
 *
 * Register model:
 *   Volatile registers (eax, ecx, edx, esp) are global variables,
 *   matching real x86 behavior where these registers are shared
 *   across all code. This enables correct argument passing via the
 *   simulated stack and return value communication via eax.
 *
 *   Callee-saved registers (ebx, esi, edi) are also global because
 *   callers pass implicit parameters through them (e.g. 'this' via
 *   esi in thiscall). The callee-save contract is enforced by
 *   PUSH32/POP32 instructions in the generated code, not by C local
 *   variable scoping.
 *
 *   ebp is NOT global - it stays local in each function because many
 *   FPO (Frame Pointer Omission) functions use it as scratch without
 *   save/restore. For SEH functions, g_seh_ebp bridges the gap.
 *
 * Calling convention:
 *   All translated functions are void(void). Arguments are passed
 *   on the simulated Xbox stack (via push instructions before call).
 *   Return values are communicated through g_eax.
 *   The call instruction pushes the real guest return address (the VA of
 *   the instruction after the call); ret discards it with esp += 4.
 *   The value is never used to transfer control -- control flow is C
 *   call/return -- but it must be correct because guest code reads it
 *   (__SEH_prolog's scope table, _alloca probes, "mov eax, [esp]").
 */

#ifndef RECOMP_TYPES_H
#define RECOMP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* math.h is load-bearing, and its absence was invisible.
 *
 * The lifter emits sqrt() and fabs() for fsqrt/fabs -- 146 of them in one of
 * Halo's nine chunks alone -- and now sin/cos/tan/atan2/log2/exp2/fmod for the
 * x87 transcendentals. With no declaration in scope, C89 implicit declaration
 * makes every one of them return `int`: the caller reads EAX instead of XMM0 and
 * gets garbage, then converts that garbage to double. Vector normalisation is
 * 1/sqrt(x), so this corrupts every matrix the title builds.
 *
 * Nothing reported it because generated code is compiled with /w (see the game
 * CMakeLists) -- MSVC's C4013 was emitted and discarded. Same failure as the
 * missing stdlib.h in kernel_bridge.c, in a hotter path. */
#include <math.h>
/* SSE2 conversions / MXCSR access for the rounding helpers below. */
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <emmintrin.h>
#define RECOMP_HOST_X86 1
#endif

/* MSVC's __forceinline -> gcc/clang equivalent on POSIX. */
#if !defined(_MSC_VER) && !defined(__forceinline)
#define __forceinline inline __attribute__((always_inline))
#endif

/* Guest debug output: INT 2D, the Xbox kernel debug trap.
 *
 * eax selects the service, ecx carries its argument. Service 1 prints the
 * ANSI_STRING ecx points at -- what OutputDebugStringA compiles to. The int3
 * that always follows is the slide byte the kernel steps over, so the lifter
 * emits nothing for it; lifting it to __debugbreak() terminated the process
 * with STATUS_BREAKPOINT the first time a title tried to print. */
void recomp_debug_service(uint32_t service, uint32_t arg_va);

/* MSVC's __debugbreak() intrinsic -> gcc/clang equivalent.
 * The auto-generated code emits __debugbreak for x86 INT 3 instructions. */
#if !defined(_MSC_VER) && !defined(__debugbreak)
#define __debugbreak() __builtin_trap()
#endif

/* ================================================================
 * Memory offset
 * ================================================================ */

/**
 * Memory offset from Xbox VA to actual mapped address.
 * When Xbox memory is mapped at the original address (0x00010000),
 * this is 0 and the MEM macros are simple identity casts.
 * When mapped elsewhere, this adjusts all memory accesses.
 *
 * Set once during memory initialization, then read-only.
 */
extern ptrdiff_t g_xbox_mem_offset;

/* Bounds of the title's executable sections, filled in by
 * xbox_MemoryLayoutInit from the XBE's own section table. Zero means "not
 * loaded yet", which RECOMP_ICALL_IS_CODE treats as allow.
 *
 * These replace a hardcoded 0x00400000 cutoff that was only ever right for one
 * title -- see the comment where they are defined in xbox_memory_layout.c. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;

/* Is this indirect-call target plausibly code?
 *
 * Synthetic kernel thunks live at 0xFE0000xx and are dispatched by
 * recomp_lookup_kernel, so they are always allowed. Everything else has to lie
 * inside an executable section of the loaded title. A target that fails this is
 * a garbage pointer that reached a call site, and calling it would be worse
 * than dropping it. */
#define RECOMP_ICALL_IS_CODE(_va) \
    ((_va) >= 0xFE000000u || g_xbox_code_hi == 0u || \
     ((_va) >= g_xbox_code_lo && (_va) < g_xbox_code_hi))

/* ================================================================
 * Global registers
 * ================================================================ */

/**
 * Volatile x86 registers (caller-saved):
 *   eax - return values, general accumulator
 *   ecx - 'this' pointer for thiscall, loop counter
 *   edx - high dword of multiply/divide, general
 *   esp - stack pointer (initialized to top of Xbox stack)
 *
 * Callee-saved x86 registers (also global):
 *   ebx, esi, edi - global because callers pass implicit parameters
 *   through them. The callee-save contract is enforced by generated
 *   PUSH32/POP32 instructions.
 *
 * NOT global: ebp - stays local in each function because FPO
 * functions use it as scratch. For SEH, g_seh_ebp bridges the gap.
 */
/* Per-thread register state.
 *
 * These started as plain globals, which works exactly as long as one thread
 * runs recompiled code. Halo is the first title to create real workers (its
 * cache/file loader), and the runtime papered over that by running every worker
 * synchronously inside PsCreateSystemThreadEx -- so a worker that blocks
 * waiting for work never returns and startup deadlocks.
 *
 * On hardware each thread has its own register set, so model it that way.
 * Thread-local costs an indirection per access; a deadlock costs the title. */
#if defined(_MSC_VER)
#  define RECOMP_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#  define RECOMP_TLS __thread
#else
#  define RECOMP_TLS _Thread_local
#endif

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;

/* x87 stack. Per-thread for the same reason the integer registers are:
 * arguments are passed in st(0)/st(1) across call boundaries. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;

/**
 * SEH frame pointer bridge.
 *
 * __SEH_prolog sets up ebp for the caller, but since ebp is a local
 * variable in each function, the caller can't see the prolog's change.
 * The prolog writes g_seh_ebp, and the caller reads it after the call.
 * Similarly, __SEH_epilog reads g_seh_ebp at entry and writes it at exit.
 */
/* Linear base of the fs segment: where the fake TIB lives. Generated code
 * adds this to every fs-relative address, so page zero stays unmapped and a
 * null dereference faults instead of hitting the TIB. Must match
 * XBOX_FS_BASE in src/kernel/xbox_memory_layout.h.
 *
 * Per-thread, because a TIB is. fs:[0] is the SEH chain head and fs:[4]
 * reaches the CRT's per-thread data, so a single shared base makes every
 * guest thread the same thread as far as the CRT is concerned. */
extern RECOMP_TLS uint32_t g_fs_base;
#define XBOX_FS_BASE g_fs_base

extern RECOMP_TLS uint32_t g_seh_ebp;

/* ---- non-local jumps (setjmp / longjmp) --------------------------------
 *
 * A recompiled function is a real C function, so restoring the guest esp is
 * only half of a longjmp: the abandoned frames are still on the native stack,
 * and control returns into them once the resume point finishes. Pairing each
 * guest jmp_buf with a native one -- taken at the setjmp call site, the only
 * place a native setjmp is valid -- makes the guest jump a native jump, so the
 * frames actually unwind.
 *
 * recomp_setjmp_slot() returns the native buffer for a guest jmp_buf address,
 * allocating one on first use. recomp_guest_longjmp() restores the guest
 * registers the CRT's longjmp would and jumps; it returns 0, without jumping,
 * for a buffer that has no native counterpart, leaving the caller to fall back
 * to the guest's own longjmp.
 */
/* Atomic read-modify-write, for the lock-prefixed instructions.
 *
 * "lock xadd" and "lock cmpxchg" are what InterlockedIncrement and
 * InterlockedCompareExchange compile to, so they carry a title's reference
 * counts and its lock-free lists. They used to lift to a TODO comment, which
 * meant a refcount that never moved and a compare-and-swap that never swapped
 * -- harmless while every guest thread ran synchronously, and not once the
 * runtime began spawning real ones.
 *
 * Genuinely atomic, not merely correct in isolation: the guest's own threads
 * now run on real host threads, so a read-modify-write that races is exactly
 * the bug these instructions exist to prevent.
 */
#if defined(_MSC_VER)
#include <intrin.h>
#define RECOMP_ATOMIC_ADD32(p, v)     ((uint32_t)_InterlockedExchangeAdd((volatile long *)(p), (long)(v)))
#define RECOMP_ATOMIC_CAS32(p, cmp, val)     ((uint32_t)_InterlockedCompareExchange((volatile long *)(p),                                            (long)(val), (long)(cmp)))
#else
#define RECOMP_ATOMIC_ADD32(p, v)     ((uint32_t)__sync_fetch_and_add((volatile uint32_t *)(p), (uint32_t)(v)))
#define RECOMP_ATOMIC_CAS32(p, cmp, val)     ((uint32_t)__sync_val_compare_and_swap((volatile uint32_t *)(p),                                            (uint32_t)(cmp), (uint32_t)(val)))
#endif

#include <setjmp.h>
jmp_buf *recomp_setjmp_slot(uint32_t buf_va);
int recomp_guest_longjmp(uint32_t buf_va, uint32_t value);
extern RECOMP_TLS uint32_t g_ebp;

/* EFLAGS.DF, and the signed step the string instructions take because of it.
 *
 * `cld`/`std` used to lift to a comment and every string instruction stepped
 * forwards regardless. That is right almost everywhere -- DF is 0 by ABI and
 * the compiler restores it -- but MSVC's strrchr is `std; repne scasb` from
 * the terminator backwards, so it scanned forwards off the end of the string
 * and returned a pointer into whatever followed. The Xbox Dashboard uses it to
 * split "y:\default.xip" into a mount path; with strrchr wrong the archive
 * registered itself under its own full filename, no resource in it could ever
 * be found by name, and the dashboard rebooted rather than showing a UI.
 * memmove's overlapping case is the same instruction and the same bug.
 */
extern RECOMP_TLS int g_df;
#define RECOMP_DF_STEP(n) (g_df ? -(int32_t)(n) : (int32_t)(n))

/* A synthetic EFLAGS backing store for `pushfd`/`popfd`, plus a real `cpuid`.
 *
 * These three used to lift to a TODO placeholder comment -- fine for code
 * that only executes them for their side effect on real flag bits nothing
 * else reads, but Bink's own CPUID-availability probe is exactly `pushfd;
 * pop eax; xor eax, 0x200000; push eax; popfd; pushfd; pop eax; popfd; cmp
 * eax, ebx` (toggle EFLAGS.ID and see if it stuck). With the placeholders,
 * that compare is comparing stale, unrelated register contents -- not even
 * consistently wrong, just whatever eax/ebx happened to hold -- so CPUID
 * looks unavailable more often than not and every codepath gated on the
 * MMX feature bit (`cpuid` leaf 1, EDX bit 23) is skipped, including
 * whichever one installs a class's function-pointer table. Skipping
 * install leaves the pointer NULL, and a later virtual call through it
 * becomes exactly the `RECOMP_ICALL_SAFE` "target 0x00000000 is not code"
 * symptom -- silent, far from the real cause, and easy to mistake for a
 * disasm boundary bug again.
 *
 * `g_eflags` backs pushfd/popfd faithfully enough for a toggle-and-read-back
 * probe: ID (bit 21) starts set, so the very first probe already reports
 * CPUID available, matching every real x86 CPU built after 1998 (including
 * the one this recompiled binary actually runs on). `recomp_cpuid` executes
 * the host's own `cpuid` via the MSVC intrinsic and returns its real
 * results -- the host is x86-64, so MMX/SSE/SSE2 are unconditionally
 * present, which is a superset of the original Xbox's Coppermine-based CPU
 * and therefore never turns on a codepath the original game didn't already
 * expect on real hardware for the features it actually probes (MMX). */
extern RECOMP_TLS uint32_t g_eflags;
void recomp_cpuid(uint32_t leaf, uint32_t subleaf,
                   uint32_t *out_eax, uint32_t *out_ebx,
                   uint32_t *out_ecx, uint32_t *out_edx);

/* `rdtsc`: the guest time-stamp counter, ticking at the original Xbox CPU's
 * 733.333333 MHz rather than the host's TSC rate. Titles use it as a
 * high-resolution clock against that frequency as a constant -- RAD's timer
 * in Bink implements QueryPerformanceFrequency as the literal 733333333
 * and QueryPerformanceCounter as a bare rdtsc. Monotonic, derived from the
 * host QueryPerformanceCounter, starting near 0 at first use. */
uint64_t recomp_rdtsc(void);

/* A conditional whose flags the lifter could not resolve (no flag state at
 * that point). Returns `flags` -- the never-assigned _flags, i.e. always
 * false, exactly as before -- and reports each site's first execution with
 * [RECOMP-FLAGMISS] so the ones that run can be fixed (docs Update 357). */
int recomp_flag_miss(uint32_t site, int flags);

/* x87 control and status. Thread-local for the same reason the x87 stack
   above is: one guest routine can lift to several C functions, so a compare
   and the FNSTSW that reads it can land in different bodies, and the control
   word has to survive a call. (g_fp_stack/g_fp_top are declared above.) */
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
/* Raw x87 condition codes (C3 0x4000, C2 0x0400, C1 0x0200, C0 0x0100),
 * valid while g_fp_cmp == RECOMP_FP_CMP_RAW. fxam sets them: its result is a
 * class, not an ordering, so it does not fit the -1/0/1/2 encoding. The
 * MSVC CRT's _trandisp1/2 (fmod, pow, ...) classify their operands with
 * fxam; lifted as a no-op, fnstsw reported the last compare instead and
 * fmod took an arbitrary path (docs Update 406). */
#define RECOMP_FP_CMP_RAW 3
extern RECOMP_TLS uint16_t g_fp_cc;

static inline uint16_t recomp_fxam(double v)
{
    uint16_t sign = signbit(v) ? 0x0200u : 0u;
    switch (fpclassify(v)) {
    case FP_NAN:       return (uint16_t)(sign | 0x0100u);           /* C0 */
    case FP_INFINITE:  return (uint16_t)(sign | 0x0500u);           /* C2|C0 */
    case FP_ZERO:      return (uint16_t)(sign | 0x4000u);           /* C3 */
    case FP_SUBNORMAL: return (uint16_t)(sign | 0x4400u);           /* C3|C2 */
    default:           return (uint16_t)(sign | 0x0400u);           /* C2 */
    }
}

/* x87 fxtract (docs Update 427): split st0 into exponent (left in st1)
 * and significand in [1,2) with the sign (pushed as st0). Lifted as a
 * comment it left st0 untouched, so SCDA's fast log2 sub_00397510
 * (fld; fxtract; faddp; fld1; fsubp = exponent + significand - 1)
 * returned x + garbage - 1. Zero gives -inf, inf gives +inf, NaN both. */
static inline void recomp_fxtract(double v, double *sig, double *exponent)
{
    int e;
    switch (fpclassify(v)) {
    case FP_NAN:      *sig = v; *exponent = v;         return;
    case FP_INFINITE: *sig = v; *exponent = INFINITY;  return;
    case FP_ZERO:     *sig = v; *exponent = -INFINITY; return;
    default:
        *sig = frexp(v, &e) * 2.0;
        *exponent = (double)(e - 1);
        return;
    }
}

/* Rounding (docs Update 355). The guest selects it per unit and the
 * conversions must honour it, not C's truncating cast:
 *  - x87 fist/fistp/frndint: RC = control word bits 10-11 (0 nearest-even,
 *    1 down, 2 up, 3 toward zero). The CRT's _ftol sets RC=3 via fldcw.
 *  - SSE cvtss2si/cvtsd2si: MXCSR RC. The guest's MXCSR is the host
 *    thread's MXCSR (ldmxcsr/stmxcsr below), so the host conversion
 *    instruction rounds exactly as the guest's would. cvtt* truncate.
 * ldmxcsr keeps rounding, FTZ and DAZ from the guest value and keeps every
 * exception masked: the recompiled code has no #XM handler. */
static inline double recomp_x87_round(double v) {
    switch ((g_fp_control_word >> 10) & 3u) {
    case 1: return floor(v);
    case 2: return ceil(v);
    case 3: return trunc(v);
    default: {
        /* Round half to even independently of the host MXCSR, which follows
         * the guest's SSE rounding mode, not its x87 one. */
        double r = floor(v + 0.5);
        if (r - v == 0.5 && fmod(r, 2.0) != 0.0)
            r -= 1.0;
        return r;
    }
    }
}
#if defined(RECOMP_HOST_X86)
static inline int32_t recomp_cvtss2si(float v) {
    return _mm_cvtss_si32(_mm_set_ss(v));
}
static inline int32_t recomp_cvtsd2si(double v) {
    return _mm_cvtsd_si32(_mm_set_sd(v));
}
static inline int32_t recomp_cvttss2si(float v) {
    return _mm_cvttss_si32(_mm_set_ss(v));
}
static inline int32_t recomp_cvttsd2si(double v) {
    return _mm_cvttsd_si32(_mm_set_sd(v));
}
static inline void recomp_ldmxcsr(uint32_t v) {
    _mm_setcsr((v & 0xE040u) | 0x1F80u);
}
static inline uint32_t recomp_stmxcsr(void) {
    return _mm_getcsr();
}
#else
/* Non-x86 hosts (ARM port, docs Update 677): the conversions follow the
 * guest MXCSR kept here, with x86 semantics -- out of range or NaN gives the
 * "integer indefinite" 0x80000000 (ARM would saturate instead). */
extern RECOMP_TLS uint32_t g_recomp_mxcsr;
static inline int32_t recomp_cvt_rc(double v, unsigned rc) {
    double r;
    if (v != v) return (int32_t)0x80000000u;
    switch (rc & 3u) {
    case 1: r = floor(v); break;
    case 2: r = ceil(v); break;
    case 3: r = trunc(v); break;
    default: r = nearbyint(v); break;   /* host default mode: to nearest even */
    }
    if (r < -2147483648.0 || r > 2147483647.0) return (int32_t)0x80000000u;
    return (int32_t)r;
}
static inline int32_t recomp_cvtss2si(float v) {
    return recomp_cvt_rc(v, g_recomp_mxcsr >> 13);
}
static inline int32_t recomp_cvtsd2si(double v) {
    return recomp_cvt_rc(v, g_recomp_mxcsr >> 13);
}
static inline int32_t recomp_cvttss2si(float v) {
    return recomp_cvt_rc(v, 3);
}
static inline int32_t recomp_cvttsd2si(double v) {
    return recomp_cvt_rc(v, 3);
}
static inline void recomp_ldmxcsr(uint32_t v) {
    g_recomp_mxcsr = (v & 0xE040u) | 0x1F80u;
}
static inline uint32_t recomp_stmxcsr(void) {
    return g_recomp_mxcsr;
}
#endif

/* Result of an x87 compare, in the shape the status word wants:
 *   -1 less, 0 equal, 1 greater, 2 unordered (either operand is NaN).
 * The unordered case is not a curiosity: `fucompp` of a value with itself
 * followed by `test ah, 0x44; jp` is how this era's CRT asks "is this a NaN",
 * and collapsing it to "equal" answers no every time. */
#define RECOMP_FCMP(a, b)     (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)

/* The 32-bit Xbox FS segment is the per-thread TIB/KPCR view, not flat guest
 * address zero.  Keep it physically separate from mapped Xbox RAM: treating
 * fs:[offset] as MEM*(offset) lets ordinary low-address writes corrupt TLS. */
uint8_t xbox_fs_read8(uint32_t offset);
uint16_t xbox_fs_read16(uint32_t offset);
uint32_t xbox_fs_read32(uint32_t offset);
void xbox_fs_write8(uint32_t offset, uint8_t value);
void xbox_fs_write16(uint32_t offset, uint16_t value);
void xbox_fs_write32(uint32_t offset, uint32_t value);

/* ================================================================
 * ICALL trace ring buffer (for debugging indirect calls)
 * ================================================================ */

/** Size of the ring buffer (must be power of 2). */
#define ICALL_TRACE_SIZE 16

/** Ring buffer of recent indirect call target VAs. */
extern volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE];

/** Current write index into the ring buffer. */
extern volatile uint32_t g_icall_trace_idx;

/** Total count of indirect calls executed. */
extern volatile uint64_t g_icall_count;

/* Xbox instruction address of this thread's most recent indirect call/tail
 * jump.  Guest threads execute concurrently, so keeping this process-global
 * makes source-gated diagnostics attribute one thread's call to another. */
extern RECOMP_TLS volatile uint32_t g_icall_source;

/**
 * Called when an indirect call target cannot be resolved.
 * Implement this in your game-specific code to log diagnostics.
 * The va parameter is the Xbox VA that failed to resolve.
 */
void recomp_icall_fail_log(uint32_t va);

/**
 * Xbox kernel debug-print trap: `int 0x2D` immediately followed by a
 * literal `int3` byte (service selector in eax, argument pointer in ecx).
 * The lifter fuses that exact two-instruction pattern (see
 * try_match_xbox_dbgprint in tools/recomp/lifter.py) into a single call
 * here instead of letting the trailing int3 fall to the generic
 * __debugbreak() rule. Implement this in your game-specific code -- eax
 * selects the service (1 = DbgPrint with ecx pointing to an in-place
 * ANSI_STRING-shaped {Length:WORD; MaximumLength:WORD; Buffer:PCHAR}
 * header); on a real Xbox with no debugger attached this is a no-op, so an
 * empty implementation is always a safe default.
 */
void xbox_dbgprint_service(uint32_t eax, uint32_t ecx);

/* Report an indirect call whose target is not code (a null or wild
 * function pointer). Rate-limited per address by the implementation,
 * because these usually arrive inside a loop -- which is exactly why
 * they must be reported: silently skipping one turns a diagnosable null
 * vtable call into an unexplained hang. */
void recomp_icall_not_code_log(uint32_t va);

/* Report a DIRECT call that reached an unresolved-target stub: an address the
 * translated code calls but the disassembler never made a function, so the
 * call is skipped. Such a skip is otherwise silent -- the same class of bug
 * as a failed icall (docs Update 450: six missed function starts silenced
 * the level music). Implemented by the game project; log once per address. */
void recomp_unresolved_stub_hit(uint32_t va);

/* Indirect-branch target feedback. The ring buffer above is crash forensics --
 * 16 entries, overwritten constantly. This is a durable, deduplicated record of
 * every target the title ever reached, for feeding back into the next codegen
 * run (tools/recomp/icall_feedback.py).
 *
 * The header is pulled in only when the feature is on, so a default build needs
 * neither the file nor src/kernel on its include path. Disabled,
 * RECOMP_ICALL_OBSERVE discards its arguments without expanding them, so the
 * RECOMP_ICALL_SEEN_* constants need not exist either. */
#ifdef RECOMP_ICALL_FEEDBACK
#include "recomp_icall_feedback.h"
#else
#define RECOMP_ICALL_OBSERVE(va, flags) ((void)0)
#endif

/**
 * Function entry trace, emitted only for addresses passed to
 * tools.recomp --trace-functions. Bring-up is largely "which of these
 * init calls does it not come back from", and answering that by
 * overriding a function loses the body you were trying to observe.
 */
void recomp_trace_enter(const char *name, uint32_t va);
#define RECOMP_TRACE_ENTER(name, va) recomp_trace_enter((name), (va))
void recomp_trace_exit(const char *name, uint32_t va);
#define RECOMP_TRACE_EXIT(name, va) recomp_trace_exit((name), (va))
void recomp_trace_esp(const char *name, const char *tag);
#define RECOMP_TRACE_ESP(name, tag) recomp_trace_esp((name), (tag))


/* ================================================================
 * Optional guest-PC trace hook
 * ================================================================ */

/*
 * Projects may define RECOMP_TRACE_HANDLER, RECOMP_TRACE_START, and
 * RECOMP_TRACE_END for a narrowly bounded diagnostic build.  All normal
 * builds compile this to a no-op.  Generated code passes a constant basic-
 * block start, allowing the compiler to remove calls outside the range.
 */
#if defined(RECOMP_TRACE_START_2) && defined(RECOMP_TRACE_END_2)
#define RECOMP_TRACE_IN_SECOND_RANGE(xbox_va) \
    ((xbox_va) >= (uint32_t)(RECOMP_TRACE_START_2) && \
     (xbox_va) < (uint32_t)(RECOMP_TRACE_END_2))
#else
#define RECOMP_TRACE_IN_SECOND_RANGE(xbox_va) 0
#endif

#if defined(RECOMP_TRACE_START_3) && defined(RECOMP_TRACE_END_3)
#define RECOMP_TRACE_IN_THIRD_RANGE(xbox_va) \
    ((xbox_va) >= (uint32_t)(RECOMP_TRACE_START_3) && \
     (xbox_va) < (uint32_t)(RECOMP_TRACE_END_3))
#else
#define RECOMP_TRACE_IN_THIRD_RANGE(xbox_va) 0
#endif

#if defined(RECOMP_TRACE_START_4) && defined(RECOMP_TRACE_END_4)
#define RECOMP_TRACE_IN_FOURTH_RANGE(xbox_va) \
    ((xbox_va) >= (uint32_t)(RECOMP_TRACE_START_4) && \
     (xbox_va) < (uint32_t)(RECOMP_TRACE_END_4))
#else
#define RECOMP_TRACE_IN_FOURTH_RANGE(xbox_va) 0
#endif

#if defined(RECOMP_TRACE_START_5) && defined(RECOMP_TRACE_END_5)
#define RECOMP_TRACE_IN_FIFTH_RANGE(xbox_va) \
    ((xbox_va) >= (uint32_t)(RECOMP_TRACE_START_5) && \
     (xbox_va) < (uint32_t)(RECOMP_TRACE_END_5))
#else
#define RECOMP_TRACE_IN_FIFTH_RANGE(xbox_va) 0
#endif

#if defined(RECOMP_TRACE_START_6) && defined(RECOMP_TRACE_END_6)
#define RECOMP_TRACE_IN_SIXTH_RANGE(xbox_va) \
    ((xbox_va) >= (uint32_t)(RECOMP_TRACE_START_6) && \
     (xbox_va) < (uint32_t)(RECOMP_TRACE_END_6))
#else
#define RECOMP_TRACE_IN_SIXTH_RANGE(xbox_va) 0
#endif

#if defined(RECOMP_TRACE_START_7) && defined(RECOMP_TRACE_END_7)
#define RECOMP_TRACE_IN_SEVENTH_RANGE(xbox_va) \
    ((xbox_va) >= (uint32_t)(RECOMP_TRACE_START_7) && \
     (xbox_va) < (uint32_t)(RECOMP_TRACE_END_7))
#else
#define RECOMP_TRACE_IN_SEVENTH_RANGE(xbox_va) 0
#endif

/* A statically recompiled CPU cannot be preempted by a host device thread in
 * the middle of generated C.  Every basic block already passes this macro, so
 * use that boundary as the architectural interrupt poll.  The normal cost is
 * one read of a process-wide flag; guest ISR code runs only after a device has
 * raised a new level assertion. */
extern volatile int32_t g_recomp_irq_pending;
void xbox_irq_safepoint(void);
#define RECOMP_IRQ_SAFEPOINT() do { \
    if (g_recomp_irq_pending) xbox_irq_safepoint(); \
} while (0)

/* Basic-block coverage recording (docs Update 460). Off unless a runtime
 * toggle (SCDA: F10) sets the flag; then every executed block's guest VA is
 * counted, and the stop writes coverage_<pid>_<n>.txt. Diffing a good and a
 * bad run of the same scene shows where the title's control flow first
 * diverges. Cost while off: one read of a process-wide flag per block, as
 * for the IRQ safepoint above. Implemented in src/kernel/recomp_coverage.c. */
extern volatile int32_t g_recomp_cov_on;
void recomp_cov_hit(uint32_t xbox_va);
#define RECOMP_COVERAGE_HIT(xbox_va) do { \
    if (g_recomp_cov_on) recomp_cov_hit((uint32_t)(xbox_va)); \
} while (0)

#if defined(RECOMP_TRACE_HANDLER) && \
    defined(RECOMP_TRACE_START) && defined(RECOMP_TRACE_END)
void RECOMP_TRACE_HANDLER(uint32_t xbox_va);
#define RECOMP_TRACE_PC(xbox_va) do { \
    RECOMP_IRQ_SAFEPOINT(); \
    RECOMP_COVERAGE_HIT(xbox_va); \
    const uint32_t _trace_va = (uint32_t)(xbox_va); \
    if ((_trace_va >= (uint32_t)(RECOMP_TRACE_START) && \
        _trace_va < (uint32_t)(RECOMP_TRACE_END)) || \
        RECOMP_TRACE_IN_SECOND_RANGE(_trace_va) || \
        RECOMP_TRACE_IN_THIRD_RANGE(_trace_va) || \
        RECOMP_TRACE_IN_FOURTH_RANGE(_trace_va) || \
        RECOMP_TRACE_IN_FIFTH_RANGE(_trace_va) || \
        RECOMP_TRACE_IN_SIXTH_RANGE(_trace_va) || \
        RECOMP_TRACE_IN_SEVENTH_RANGE(_trace_va)) \
        RECOMP_TRACE_HANDLER(_trace_va); \
} while (0)
#else
#define RECOMP_TRACE_PC(xbox_va) do { \
    RECOMP_IRQ_SAFEPOINT(); RECOMP_COVERAGE_HIT(xbox_va); \
} while (0)
#endif

/* ================================================================
 * Memory access helpers
 * ================================================================ */

/**
 * Translate an Xbox VA to an actual pointer.
 * Mask to 32-bit first: Xbox addresses are 32-bit and arithmetic
 * in the recompiled code can overflow. Without the mask, a 64-bit
 * uintptr_t cast preserves the overflow bits, landing us 4GB+ past
 * our mapping and causing access violations.
 */
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)

/** Read/write N bytes at a flat Xbox memory address. */
#define MEM8(addr)   (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)  (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)  (*(volatile uint32_t *)XBOX_PTR(addr))

/* Direct device-register access (docs Update 643).
 *
 * The NV2A (0xFD000000) and APU (0xFE800000) register windows are
 * PAGE_NOACCESS so that every guest access faults into a vectored handler,
 * which decodes the host instruction and calls the emulated device. That is
 * an exception round trip per register, and the game thread paid ~50,000 of
 * them per second (DirectSound's voice update, D3D's pushbuffer kick). The
 * lifter emits IOR/IOW for the memory accesses of device driver routines
 * (tools/recomp/lifter.py, io_function); in a device window they call
 * recomp_io_read/write, which reach the emulated device directly while its
 * trap is active and otherwise make the ordinary access -- and with it the
 * fault path. Everything else stays an ordinary access. */
extern int g_recomp_io_direct;
uint32_t recomp_io_read(uint32_t va, uint32_t size);
void recomp_io_write(uint32_t va, uint32_t val, uint32_t size);
#define RECOMP_IO_DIRECT(a) (g_recomp_io_direct && \
    (uint32_t)((uint32_t)(a) - 0xFD000000u) < 0x01880000u)
#define IOR8(addr)  ((uint8_t)(RECOMP_IO_DIRECT(addr) \
    ? recomp_io_read((uint32_t)(addr), 1) : MEM8(addr)))
#define IOR16(addr) ((uint16_t)(RECOMP_IO_DIRECT(addr) \
    ? recomp_io_read((uint32_t)(addr), 2) : MEM16(addr)))
#define IOR32(addr) ((uint32_t)(RECOMP_IO_DIRECT(addr) \
    ? recomp_io_read((uint32_t)(addr), 4) : MEM32(addr)))
#define RECOMP_IOW(T, n, addr, val) do { \
    uint32_t _io_a = (uint32_t)(addr); T _io_v = (T)(val); \
    if (RECOMP_IO_DIRECT(_io_a)) recomp_io_write(_io_a, _io_v, n); \
    else MEM##n##_W(_io_a) = _io_v; } while (0)
#define MEM1_W(a) MEM8(a)
#define MEM2_W(a) MEM16(a)
#define MEM4_W(a) MEM32(a)
#define IOW8(addr, val)  RECOMP_IOW(uint8_t, 1, addr, val)
#define IOW16(addr, val) RECOMP_IOW(uint16_t, 2, addr, val)
#define IOW32(addr, val) RECOMP_IOW(uint32_t, 4, addr, val)

/* Forward `rep movs` / `rep stosb` that honour device memory.
 *
 * The CPU executes `rep movsd` as a sequence of 4-byte moves in address
 * order. memcpy/memset may use 32-byte AVX and overlapping unaligned
 * accesses, which is the same thing for RAM but not for device registers:
 * DirectSound copies GP DSP memory out of the APU window with `rep movsd`
 * (SCDA sub_004D213E) and clears buffers there with `rep stosd/stosb`, and
 * the APU trap handler then saw an undecodable `vmovdqu` or a misaligned
 * 4-byte access (docs Update 345). Guest VAs from 0xFD000000 up are the
 * device apertures (NV2A, APU, flash); only those take the element loop. */
#define RECOMP_IS_DEVICE_VA(a) ((uint32_t)(a) >= 0xFD000000u)

static inline void recomp_rep_movs_fwd(uint32_t dst, uint32_t src,
                                       uint32_t count, uint32_t size)
{
    uint32_t i;
    if (!RECOMP_IS_DEVICE_VA(dst) && !RECOMP_IS_DEVICE_VA(src)) {
        memcpy((void *)XBOX_PTR(dst), (void *)XBOX_PTR(src),
               (size_t)count * size);
        return;
    }
    for (i = 0; i < count; i++) {
        uint32_t o = i * size;
        if (size == 4)      MEM32(dst + o) = MEM32(src + o);
        else if (size == 2) MEM16(dst + o) = MEM16(src + o);
        else                MEM8(dst + o)  = MEM8(src + o);
    }
}

static inline void recomp_rep_stosb_fwd(uint32_t dst, uint8_t value,
                                        uint32_t count)
{
    uint32_t i;
    if (!RECOMP_IS_DEVICE_VA(dst)) {
        memset((void *)XBOX_PTR(dst), value, count);
        return;
    }
    for (i = 0; i < count; i++)
        MEM8(dst + i) = value;
}

/** Signed memory reads. */
#define SMEM8(addr)  (*(volatile int8_t   *)XBOX_PTR(addr))
#define SMEM16(addr) (*(volatile int16_t  *)XBOX_PTR(addr))
#define SMEM32(addr) (*(volatile int32_t  *)XBOX_PTR(addr))
#define SMEM64(addr) (*(volatile int64_t  *)XBOX_PTR(addr))

/** Float/double memory access. */
#define MEMF(addr)   (*(volatile float    *)XBOX_PTR(addr))
#define MEMD(addr)   (*(volatile double   *)XBOX_PTR(addr))

/* x87 80-bit extended memory operands (`fld/fstp tbyte ptr`), docs Update
 * 459. The FPU stack is modelled as double, so these convert at the memory
 * boundary: 64-bit explicit mantissa, 15-bit biased exponent, sign bit.
 * The lifter used to read an m80 as a 32-bit float (the low mantissa bytes)
 * and to drop `fstp m80` entirely, pop included -- the retail CRT keeps its
 * transcendental limits (e.g. 32767.5 for exp) as m80 constants, so every
 * exp()/pow() took the overflow branch. */
static inline double recomp_ld80(uint32_t addr)
{
    uint64_t mant = (uint64_t)MEM32(addr) | ((uint64_t)MEM32(addr + 4u) << 32);
    uint32_t se = MEM16(addr + 8u);
    int neg = (se & 0x8000u) != 0;
    int exp = (int)(se & 0x7FFFu);
    double v;
    if (exp == 0x7FFF) {
        if ((mant << 1) == 0)
            return neg ? -INFINITY : INFINITY;
        return NAN;
    }
    if (mant == 0)
        return neg ? -0.0 : 0.0;
    /* value = mant * 2^(exp - 16383 - 63); denormals use exp 1 - 16383 */
    v = ldexp((double)mant, (exp ? exp : 1) - 16383 - 63);
    return neg ? -v : v;
}

static inline void recomp_st80(uint32_t addr, double v)
{
    uint64_t mant;
    uint32_t se = signbit(v) ? 0x8000u : 0u;
    if (isnan(v)) {
        mant = 0xC000000000000000ull;
        se |= 0x7FFFu;
    } else if (isinf(v)) {
        mant = 0x8000000000000000ull;
        se |= 0x7FFFu;
    } else if (v == 0.0) {
        mant = 0;
    } else {
        int e;
        double f = frexp(fabs(v), &e);          /* f in [0.5, 1) */
        mant = (uint64_t)ldexp(f, 64);          /* exact: f has 53 bits */
        se |= (uint32_t)(e - 1 + 16383) & 0x7FFFu;
    }
    MEM32(addr) = (uint32_t)mant;
    MEM32(addr + 4u) = (uint32_t)(mant >> 32);
    MEM16(addr + 8u) = (uint16_t)se;
}

/* ================================================================
 * SSE / XMM register state
 *
 * XMM is 128 bits of architectural state, not a scalar float. Modelling
 * it as a `float` made movaps/movups transfer 4 of 16 bytes and silently
 * drop the upper three lanes, and left the packed arithmetic with no
 * representation at all.
 *
 * The registers are global for the same reason the volatile GPRs are:
 * one guest routine can lift to several C functions, so a value produced
 * in one body and read in the next has to outlive the body that wrote it.
 * A function-local declaration would also shadow these, and the local
 * starts zeroed -- a returned float would silently read as 0.0.
 *
 * The helpers are lane-wise C rather than host intrinsics: the guest
 * semantics stay explicit (MINPS returning src on unordered, CMPNEQPS
 * being the unordered form) and the header stays portable.
 * ================================================================ */

#ifndef RECOMP_XMM_DEFINED
#define RECOMP_XMM_DEFINED
typedef union RecompXmm {
    float    f[4];
    double   d[2];
    uint32_t u[4];
    int32_t  i[4];
    uint64_t q[2];
} RecompXmm;
#endif

extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;

/* -- construction -- */

static inline RecompXmm XMM_ZERO(void) {
    RecompXmm r; r.q[0] = 0; r.q[1] = 0; return r;
}

/** movss from memory: lane 0 set, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR(float v) {
    RecompXmm r = XMM_ZERO(); r.f[0] = v; return r;
}

/** movsd from memory: low double set, high double zeroed. */
static inline RecompXmm XMM_SCALAR_DOUBLE(double v) {
    RecompXmm r = XMM_ZERO(); r.d[0] = v; return r;
}

/** movd: 32 raw bits into lane 0, upper lanes zeroed. */
static inline RecompXmm XMM_SCALAR_BITS(uint32_t bits) {
    RecompXmm r = XMM_ZERO(); r.u[0] = bits; return r;
}

/* -- guest memory --
 * Addresses are guest VAs, so they go through MEM32 like every other
 * access. Done lane-wise, which is also unaligned-safe for movups. */

static inline RecompXmm XMM_MEM(uint32_t addr) {
    RecompXmm r;
    r.u[0] = MEM32(addr);      r.u[1] = MEM32(addr + 4);
    r.u[2] = MEM32(addr + 8);  r.u[3] = MEM32(addr + 12);
    return r;
}

static inline void XMM_STORE(uint32_t addr, RecompXmm v) {
    MEM32(addr)      = v.u[0]; MEM32(addr + 4)  = v.u[1];
    MEM32(addr + 8)  = v.u[2]; MEM32(addr + 12) = v.u[3];
}

/* movlps/movhps move 8 bytes into or out of one half, leaving the
 * other half alone. */
#define XMM_LOAD_LOW(dst, addr)   recomp_xmm_load_half(&(dst), (addr), 0)
#define XMM_LOAD_HIGH(dst, addr)  recomp_xmm_load_half(&(dst), (addr), 1)
#define XMM_STORE_LOW(addr, src)  recomp_xmm_store_half((addr), (src), 0)
#define XMM_STORE_HIGH(addr, src) recomp_xmm_store_half((addr), (src), 1)

static inline void recomp_xmm_load_half(RecompXmm *dst, uint32_t addr,
                                        int high) {
    dst->u[high * 2]     = MEM32(addr);
    dst->u[high * 2 + 1] = MEM32(addr + 4);
}

static inline void recomp_xmm_store_half(uint32_t addr, RecompXmm src,
                                         int high) {
    MEM32(addr)     = src.u[high * 2];
    MEM32(addr + 4) = src.u[high * 2 + 1];
}

/** movlhps: dst high = src low. */
static inline RecompXmm XMM_MOVE_LOW_TO_HIGH(RecompXmm a, RecompXmm b) {
    RecompXmm r; r.q[0] = a.q[0]; r.q[1] = b.q[0]; return r;
}

/** movhlps: dst low = src high. */
static inline RecompXmm XMM_MOVE_HIGH_TO_LOW(RecompXmm a, RecompXmm b) {
    RecompXmm r; r.q[0] = b.q[1]; r.q[1] = a.q[1]; return r;
}

/* -- packed arithmetic -- */

#define RECOMP_XMM_LANEWISE(name, expr)                                   \
    static inline RecompXmm name(RecompXmm a, RecompXmm b) {              \
        RecompXmm r; int i;                                               \
        for (i = 0; i < 4; ++i) { (void)a; (void)b; r.f[i] = (expr); }    \
        return r;                                                         \
    }

RECOMP_XMM_LANEWISE(XMM_ADD, a.f[i] + b.f[i])
RECOMP_XMM_LANEWISE(XMM_SUB, a.f[i] - b.f[i])
RECOMP_XMM_LANEWISE(XMM_MUL, a.f[i] * b.f[i])
RECOMP_XMM_LANEWISE(XMM_DIV, a.f[i] / b.f[i])
/* MINPS/MAXPS return the second operand when the lanes are unordered or
 * equal -- that is the hardware's tie-break, not a C fmin/fmax. */
RECOMP_XMM_LANEWISE(XMM_MIN, (a.f[i] < b.f[i]) ? a.f[i] : b.f[i])
RECOMP_XMM_LANEWISE(XMM_MAX, (a.f[i] > b.f[i]) ? a.f[i] : b.f[i])

/* sqrtps / rsqrtps / rcpps: all four lanes. rsqrt/rcp are exact here where
 * the hardware approximates to 12 bits; titles refine them with a Newton
 * step, which an exact start value survives. */
#define RECOMP_XMM_UNARY(name, expr)                                      \
    static inline RecompXmm name(RecompXmm a) {                           \
        RecompXmm r; int i;                                               \
        for (i = 0; i < 4; ++i) { r.f[i] = (expr); }                      \
        return r;                                                         \
    }

RECOMP_XMM_UNARY(XMM_SQRT,  sqrtf(a.f[i]))
RECOMP_XMM_UNARY(XMM_RSQRT, 1.0f / sqrtf(a.f[i]))
RECOMP_XMM_UNARY(XMM_RCP,   1.0f / a.f[i])

#define RECOMP_XMM_BITWISE(name, expr)                                    \
    static inline RecompXmm name(RecompXmm a, RecompXmm b) {              \
        RecompXmm r; int i;                                               \
        for (i = 0; i < 4; ++i) { (void)a; (void)b; r.u[i] = (expr); }    \
        return r;                                                         \
    }

RECOMP_XMM_BITWISE(XMM_AND,  a.u[i] & b.u[i])
RECOMP_XMM_BITWISE(XMM_OR,   a.u[i] | b.u[i])
RECOMP_XMM_BITWISE(XMM_XOR,  a.u[i] ^ b.u[i])
/* ANDNPS is ~dst & src, not dst & ~src. */
RECOMP_XMM_BITWISE(XMM_ANDN, (~a.u[i]) & b.u[i])

/* Compares produce an all-ones or all-zero mask per lane. EQ/LT/LE are
 * the ordered forms (false when either lane is NaN); NEQ is the
 * unordered form, so it is true when a lane is NaN. */
RECOMP_XMM_BITWISE(XMM_CMP_EQ,  (a.f[i] == b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_LT,  (a.f[i] <  b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_LE,  (a.f[i] <= b.f[i]) ? 0xFFFFFFFFu : 0u)
RECOMP_XMM_BITWISE(XMM_CMP_NEQ, (a.f[i] == b.f[i]) ? 0u : 0xFFFFFFFFu)

/** movmskps: the four lane sign bits, packed into the low nibble. */
static inline uint32_t XMM_MOVEMASK(RecompXmm a) {
    return ((a.u[0] >> 31) & 1u) | (((a.u[1] >> 31) & 1u) << 1)
         | (((a.u[2] >> 31) & 1u) << 2) | (((a.u[3] >> 31) & 1u) << 3);
}

/** shufps: lanes 0-1 selected out of `a`, lanes 2-3 out of `b`. */
static inline RecompXmm XMM_SHUFFLE(RecompXmm a, RecompXmm b, uint32_t imm) {
    RecompXmm r;
    r.u[0] = a.u[(imm >> 0) & 3u]; r.u[1] = a.u[(imm >> 2) & 3u];
    r.u[2] = b.u[(imm >> 4) & 3u]; r.u[3] = b.u[(imm >> 6) & 3u];
    return r;
}

/** unpcklps / unpckhps: interleave the low or high halves. */
static inline RecompXmm XMM_UNPACK_LOW(RecompXmm a, RecompXmm b) {
    RecompXmm r;
    r.u[0] = a.u[0]; r.u[1] = b.u[0]; r.u[2] = a.u[1]; r.u[3] = b.u[1];
    return r;
}

static inline RecompXmm XMM_UNPACK_HIGH(RecompXmm a, RecompXmm b) {
    RecompXmm r;
    r.u[0] = a.u[2]; r.u[1] = b.u[2]; r.u[2] = a.u[3]; r.u[3] = b.u[3];
    return r;
}

/* ================================================================
 * Flag computation helpers
 *
 * These macros compute x86 flags for conditional branches.
 * Used by the lifter's pattern-matching output:
 *   cmp a, b; jcc target  ->  if (COND(a, b)) goto target;
 * ================================================================ */

/* Unsigned comparison conditions (from CMP a, b -> a - b) */
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b)  ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))   /* below (CF=1) */
#define CMP_AE(a, b)  ((uint32_t)(a) >= (uint32_t)(b))   /* above or equal */
#define CMP_BE(a, b)  ((uint32_t)(a) <= (uint32_t)(b))   /* below or equal */
#define CMP_A(a, b)   ((uint32_t)(a) >  (uint32_t)(b))   /* above */

/* Signed comparison conditions */
/* x86 evaluates the signed conditions at the operand width, not at 32 bits.
   The generated code passes LO8/HI8/LO16 sub-register reads straight in, and
   those zero-extend, so recover the width and sign-extend before comparing. */
#define RECOMP_FLAG_WIDTH(a, b) (sizeof(a) < sizeof(b) ? sizeof(a) : sizeof(b))
#define RECOMP_SIGNED(value, width) \
    ((width) == 1u ? (int32_t)(int8_t)(uint8_t)(uint32_t)(value) \
     : (width) == 2u ? (int32_t)(int16_t)(uint16_t)(uint32_t)(value) \
     : (int32_t)(uint32_t)(value))
#define CMP_L(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* less */
#define CMP_GE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* >= */
#define CMP_LE(a, b)  (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <= \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* <= */
#define CMP_G(a, b)   (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >  \
                       RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))  /* > */

/* TEST-based conditions (AND without storing result) */
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)  /* ZF=1 */
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0)  /* ZF=0 */
#define TEST_S(a, b)  (RECOMP_SIGNED((uint32_t)(a) & (uint32_t)(b), \
                                     RECOMP_FLAG_WIDTH(a, b)) < 0)  /* SF=1 */

/* ================================================================
 * Arithmetic with carry/overflow detection
 * ================================================================ */

/** Add with carry flag. Returns result, sets *cf. */
static inline uint32_t ADD32_CF(uint32_t a, uint32_t b, int *cf) {
    uint32_t r = a + b;
    *cf = (r < a);
    return r;
}

/** Sub with carry (borrow) flag. Returns result, sets *cf. */
static inline uint32_t SUB32_CF(uint32_t a, uint32_t b, int *cf) {
    *cf = (a < b);
    return a - b;
}

/* ================================================================
 * Rotation / shift helpers
 * ================================================================ */

static inline uint32_t ROL32(uint32_t val, int n) {
    n &= 31;
    return n ? (val << n) | (val >> (32 - n)) : val;
}

static inline uint32_t ROR32(uint32_t val, int n) {
    n &= 31;
    return n ? (val >> n) | (val << (32 - n)) : val;
}

/* Width-aware rotates (docs Update 357). x86 masks the count to 5 bits;
 * rol/ror then rotate modulo the operand width, rcl/rcr rotate through CF
 * modulo width+1. rcl/rcr were not lifted at all: the CRT's 64-bit
 * division helpers (_aulldiv/_allrem slow path: shr edx,1 / rcr eax,1)
 * returned wrong quotients for divisors above 2^32. */
static inline uint32_t recomp_rot_mask(int bits) {
    return bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
}
static inline uint32_t recomp_rol(uint32_t v, uint32_t n, int bits) {
    uint32_t m = recomp_rot_mask(bits);
    v &= m;
    n = (n & 31u) % (uint32_t)bits;
    return n ? ((v << n) | (v >> (bits - n))) & m : v;
}
static inline uint32_t recomp_ror(uint32_t v, uint32_t n, int bits) {
    uint32_t m = recomp_rot_mask(bits);
    v &= m;
    n = (n & 31u) % (uint32_t)bits;
    return n ? ((v >> n) | (v << (bits - n))) & m : v;
}
static inline uint32_t recomp_rcl(uint32_t v, uint32_t n, int bits, int *cf) {
    uint32_t m = recomp_rot_mask(bits);
    v &= m;
    n &= 31u;
    if (bits < 32) n %= (uint32_t)(bits + 1);
    while (n--) {
        uint32_t out = (v >> (bits - 1)) & 1u;
        v = ((v << 1) | (uint32_t)(*cf & 1)) & m;
        *cf = (int)out;
    }
    return v;
}
static inline uint32_t recomp_rcr(uint32_t v, uint32_t n, int bits, int *cf) {
    uint32_t m = recomp_rot_mask(bits);
    v &= m;
    n &= 31u;
    if (bits < 32) n %= (uint32_t)(bits + 1);
    while (n--) {
        uint32_t out = v & 1u;
        v = (v >> 1) | ((uint32_t)(*cf & 1) << (bits - 1));
        *cf = (int)out;
    }
    return v;
}

/* ================================================================
 * Sign/zero extension
 * ================================================================ */

#define ZX8(v)   ((uint32_t)(uint8_t)(v))
#define ZX16(v)  ((uint32_t)(uint16_t)(v))
#define SX8(v)   ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)  ((uint32_t)(int32_t)(int16_t)(v))

/* ================================================================
 * Byte/word register access
 *
 * These macros extract or set partial registers, matching x86
 * behavior where writing AL doesn't affect bits 8-31 of EAX.
 * ================================================================ */

/** Extract low byte (al, bl, cl, dl). */
#define LO8(r)  ((uint8_t)((r) & 0xFF))
/** Extract high byte of low word (ah, bh, ch, dh). */
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
/** Extract low word (ax, bx, cx, dx). */
#define LO16(r) ((uint16_t)((r) & 0xFFFF))

/** Set low byte, preserving upper 24 bits. */
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
/** Set high byte of low word, preserving other bits. */
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
/** Set low word, preserving upper 16 bits. */
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))

/* ================================================================
 * Stack simulation
 *
 * For push/pop heavy prologues in the generated code.
 * ================================================================ */

/**
 * Push a 32-bit value onto the simulated stack.
 * Evaluates val BEFORE decrementing sp, matching x86 semantics
 * where push [esp+N] reads the operand before adjusting ESP.
 */
#define PUSH32(sp, val) do { \
    uint32_t _pv = (uint32_t)(val); \
    (sp) -= 4; \
    MEM32(sp) = _pv; \
} while(0)

/**
 * x86 parity flag: 1 when the low byte of the result has an EVEN number of set
 * bits (that is what PF means). Used by the x87 float-compare idiom
 * `fnstsw ax; test ah, mask; jp/jnp`, which is how all pre-SSE code branches on
 * a float comparison. Without a real parity here the branch was hardcoded and
 * every such comparison went one fixed direction.
 */
static inline int recomp_parity8(uint32_t x) {
    x &= 0xFFu; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return (int)(~x & 1u);   /* 1 = even parity (PF set) */
}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))

/** Pop a 32-bit value from the simulated stack. */
#define POP32(sp, dst) do { \
    (dst) = MEM32(sp); \
    (sp) += 4; \
} while(0)

/* ================================================================
 * Byte swap (for endian conversion if needed)
 *
 * Xbox is little-endian like x86, so these are rarely needed,
 * but some games use bswap for network byte order or data parsing.
 * ================================================================ */

static inline uint32_t BSWAP32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

static inline uint16_t BSWAP16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/* ================================================================
 * Indirect call dispatch
 *
 * The dispatch system resolves Xbox virtual addresses to native
 * function pointers at runtime. Three lookup sources are checked:
 *   1. Manual overrides (hand-written reimplementations)
 *   2. Generated dispatch table (auto-recompiled functions)
 *   3. Kernel thunk bridge (Xbox kernel function replacements)
 * ================================================================ */

/**
 * Generic function pointer type for all recompiled functions.
 * All translated functions are void(void) - arguments and return
 * values are passed through global registers and the simulated stack.
 */
#ifndef RECOMP_DISPATCH_H  /* avoid conflict with recomp_dispatch.h */
typedef void (*recomp_func_t)(void);
#ifdef SCDA_ICALL_PROBE
void scda_icall_probe(uint32_t xbox_va, recomp_func_t fn, uint32_t entry_esp);
#define RECOMP_SCDA_ICALL_PROBE(va, fn, esp) \
    do { if (g_icall_source == 0x00071BE6u || g_icall_source == 0x001D560Bu || \
             g_icall_source == 0x000462D0u || g_icall_source == 0x001D55D7u || \
             g_icall_source == 0x001D53F9u || g_icall_source == 0x001D541Du || \
             g_icall_source == 0x001D5441u || g_icall_source == 0x001D5465u || \
             g_icall_source == 0x001D5489u) \
             scda_icall_probe((va), (fn), (esp)); } while (0)
#else
#define RECOMP_SCDA_ICALL_PROBE(va, fn, esp) ((void)0)
#endif

/**
 * Look up a recompiled function by its Xbox VA.
 * Returns NULL if the VA is not in the generated dispatch table.
 */
recomp_func_t recomp_lookup(uint32_t xbox_va);

/**
 * Build the flat, directly-indexed dispatch table.
 *
 * Turns recomp_lookup from a binary search over every translated function into
 * a bounds check and one indexed load -- the C form of Microsoft's
 * `jmp [table + guest_eip*8]`. Call it once at startup, before any recompiled
 * code runs.
 *
 * Entirely optional: if it is never called, or returns 0 because the allocation
 * failed, recomp_lookup keeps using the binary search and everything still
 * works. Costs 8 bytes per byte of guest code span (see
 * recomp_dispatch_flat_bytes), allocated with calloc so the untouched middle
 * stays uncommitted.
 *
 * Returns 1 if the flat table is in use, 0 if the search is.
 */
int recomp_dispatch_init(void);

/** Bytes held by the flat table, or 0 if it was never built. */
size_t recomp_dispatch_flat_bytes(void);

/**
 * Look up a kernel thunk function by its synthetic VA.
 * Kernel thunks live at 0xFE000000+ (synthetic addresses assigned
 * during kernel bridge initialization).
 * Returns NULL if the VA is not a kernel thunk.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/**
 * Look up a manually overridden function by its Xbox VA.
 * Manual overrides take priority over generated code.
 * Returns NULL if no manual override exists for this VA.
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
#endif

/**
 * RECOMP_ABI_CALL - call a resolved target, optionally checking the ABI.
 *
 * ebx, esi and edi are callee-saved on x86, and the recompiler keeps them in
 * globals rather than on the host's stack. So a lifted function that never
 * reaches its own epilogue -- the usual cause is a decode that lost sync on
 * embedded data -- does not merely lose its own state, it silently corrupts
 * every caller's.
 *
 * That failure is invisible at the crash site: the caller carries on with a
 * wrong loop cursor and simply does less work. Half-Life 2's C++ static
 * initialiser walks 5,305 constructors with esi as the cursor and edi as the
 * limit; a single callee returning with those changed ended the walk early and
 * left Source with no registered interfaces, with no error anywhere.
 *
 * Build with -DRECOMP_ABI_CHECK to have every indirect call verify them and
 * name the first offenders. esp is deliberately not checked: the convention
 * decides whether the callee pops arguments, so there is no single correct
 * value -- but there is one invariant that holds under every convention:
 * the callee at least pops its own return address, so esp must come back
 * at least 4 higher than it went in. Coming back lower means the epilogue
 * never ran, which is the failure this exists to catch.
 *
 * Covers direct calls as well as indirect: tools/recomp emits every direct
 * call through this macro, which expands to a plain call when the flag is
 * off. That matters because CRT and static-initialiser paths -- where these
 * clobbers actually bite -- are almost entirely direct calls.
 * Generated code emits a direct call as a plain C call to the symbol, with no
 * macro to hook, so a direct callee that clobbers these registers is invisible
 * here. That matters more than it sounds -- CRT and static-initialiser paths
 * are almost entirely direct calls, so this found nothing at all on Half-Life
 * 2's static init, where the clobber demonstrably exists. It is the right tool
 * for vtable-dispatch-heavy code and the wrong one for early boot.
 */
#ifdef RECOMP_ABI_CHECK
void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0);
#ifdef SCDA_GPU_EMIT_TRACE
/* SCDA Update 375: the former 0x0049EFC0 attribution used a stale build's
 * map and was refuted by the current build-specific trace.  Keep the
 * declaration for the retained diagnostic utility, but compile its callsite
 * hook out until a real producer hypothesis exists. */
void scda_gpu_emit_call_trace(uint32_t va);
#define RECOMP_GPU_EMIT_TRACE(va) ((void)0)
#else
#define RECOMP_GPU_EMIT_TRACE(va) ((void)0)
#endif
#define RECOMP_ABI_CALL(va, fn) do { \
    uint32_t _ab = g_ebx, _as = g_esi, _ad = g_edi, _ap = g_esp; \
    RECOMP_GPU_EMIT_TRACE(va); \
    (fn)(); \
    if (g_ebx != _ab || g_esi != _as || g_edi != _ad || g_esp < _ap + 4) \
        recomp_abi_violation_log((va), _ab, _as, _ad, _ap); \
} while(0)
#else
#define RECOMP_ABI_CALL(va, fn) (fn)()
#endif

/**
 * RECOMP_ICALL - Indirect call through the dispatch table.
 *
 * Looks up the Xbox VA and calls the translated function.
 * Falls back to kernel bridge for kernel thunk synthetic VAs.
 * The caller must PUSH32 the guest return address before this macro.
 * If not found, pops it back off to keep the stack balanced.
 *
 * RECOMP_ICALL_IS_CODE skips garbage VAs that come from uninitialized vtable
 * pointers. Kernel thunks at 0xFE000000+ are never blocked.
 *
 * Nothing to customize: the range comes from g_xbox_code_lo/g_xbox_code_hi,
 * which the memory layout fills in from the sections it actually mapped. This
 * used to say "change the VA range check to match your game's code range",
 * left over from a hardcoded 0x00400000 cutoff that was only ever right for
 * one title. Editing this header per project is no longer a thing.
 */
#define RECOMP_ICALL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    /* Skip garbage VAs outside code section + kernel thunk range */ \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp += 4; eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    /* SCDA's startup dispatcher bisection: compile out everywhere else. */ \
    RECOMP_SCDA_ICALL_PROBE(_va, _fn, g_esp); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; eax = 0; } \
} while(0)

/**
 * RECOMP_ICALL_SAFE - Stack-safe indirect call.
 *
 * Restores g_esp to saved_esp (pre-argument value) on lookup failure,
 * preventing stdcall argument leaks on failed vtable calls.
 * Use this when the caller pushes arguments that the callee would
 * normally clean up (stdcall convention).
 *
 * 2026-09-11: RECOMP_ABI_CALL's own check only catches esp coming back
 * too LOW (epilogue never ran) -- an esp that comes back too HIGH (the
 * callee's `ret N` popped more than this call site actually pushed)
 * silently passes it whenever ebx/esi/edi happen to still match. But
 * *this* macro alone knows the exact pre-push esp (saved_esp), and
 * `saved_esp` is a hard ceiling regardless of calling convention: every
 * `ret` pops at least its own return address, so a callee that cleans
 * its own stdcall-style arguments too brings esp back up to exactly
 * saved_esp, and a cdecl-style callee that leaves them for the caller
 * comes back lower still -- esp can never LEGITIMATELY end up above
 * saved_esp. An over-pop here leaves the emulated guest stack short by
 * however many bytes were over-cleaned, which corrupts whatever a later
 * PUSH32/POP32 pair in an outer frame reads back next -- e.g. a
 * caller's own push/pop-preserved register coming back as leftover
 * stack garbage instead of the value it pushed, with no register
 * mismatch at the call site that actually did the damage. Gated with
 * RECOMP_ABI_CHECK since it costs one more compare per resolved
 * indirect call.
 */
#ifdef RECOMP_ABI_CHECK
void recomp_icall_stack_overpop_log(uint32_t va, uint32_t saved_esp, uint32_t esp_after);
#define RECOMP_ICALL_SAFE_STACK_CHECK(_va, _saved_esp) do { \
    if (g_esp > (_saved_esp)) \
        recomp_icall_stack_overpop_log((_va), (_saved_esp), g_esp); \
} while(0)
#else
#define RECOMP_ICALL_SAFE_STACK_CHECK(_va, _saved_esp) do {} while(0)
#endif

#define RECOMP_ICALL_SAFE(xbox_va, saved_esp) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va; \
    g_icall_trace_idx++; \
    g_icall_count++; \
    if (!RECOMP_ICALL_IS_CODE(_va)) { \
        recomp_icall_not_code_log(_va); \
        g_esp = (saved_esp); eax = 0; break; \
    } \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    /* 2026-09-03: the 0x71BE6 bisection's probe was only wired into
     * RECOMP_ICALL (the non-safe variant) and never fired, because the
     * actual generated call site for this investigation uses
     * RECOMP_ICALL_SAFE -- proven via /P preprocessor output, not
     * guessed. See docs/SCDA_WORKLOG.md. */ \
    RECOMP_SCDA_ICALL_PROBE(_va, _fn, g_esp); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); \
               RECOMP_ICALL_SAFE_STACK_CHECK(_va, (saved_esp)); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp = (saved_esp); eax = 0; } \
} while(0)

/**
 * RECOMP_ITAIL - Indirect tail call (jmp through function pointer).
 *
 * No return address is pushed - reuses the current frame's return addr.
 * Used for tail-call optimization where the original code uses
 * jmp [reg] instead of call [reg].
 */
#define RECOMP_ITAIL(xbox_va) do { \
    uint32_t _va = (uint32_t)(xbox_va); \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    if (!_fn) _fn = recomp_lookup(_va); \
    if (!_fn) _fn = recomp_lookup_kernel(_va); \
    if (_fn) { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_RESOLVED); \
               RECOMP_ABI_CALL(_va, _fn); } \
    else { RECOMP_ICALL_OBSERVE(_va, RECOMP_ICALL_SEEN_UNRESOLVED); \
           recomp_icall_fail_log(_va); g_esp += 4; g_eax = 0; } \
} while(0)

/* ================================================================
 * Register name aliases for generated code
 *
 * Map x86 volatile register names to global variables.
 * These #defines allow the generated code to use natural register
 * names (eax, ecx, edx, esp) which the preprocessor maps to the
 * corresponding globals (g_eax, g_ecx, g_edx, g_esp).
 *
 * Only active when RECOMP_GENERATED_CODE is defined (in generated
 * .c files) to avoid polluting hand-written code.
 * ================================================================ */

#ifdef RECOMP_GENERATED_CODE
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi

/* ================================================================
 * MMX register file
 *
 * The Xbox is a Pentium III and every XDK codec leans on MMX: the WMV
 * decoder's IDCT and motion compensation are almost nothing else. Modelled the
 * same way as RecompXmm -- a union of lane views over one 64-bit register --
 * because that is what the instructions are: the same bits read as bytes,
 * words or dwords.
 *
 * mm0..mm7 alias the x87 stack on real hardware. Nothing here does, and
 * nothing needs to: a title that interleaves the two calls emms between, and
 * emms is a no-op for us. Modelling the aliasing would mean giving up the
 * separate x87 model that the FPU work depends on, to reproduce a hazard the
 * hardware exists to let software avoid.
 * ================================================================ */

#ifndef RECOMP_MMX_DEFINED
#define RECOMP_MMX_DEFINED
typedef union RecompMmx {
    int8_t   b[8];
    uint8_t  ub[8];
    int16_t  w[4];
    uint16_t uw[4];
    int32_t  d[2];
    uint32_t ud[2];
    uint64_t q;
} RecompMmx;
#endif

extern RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;

static inline RecompMmx MMX_ZERO(void) { RecompMmx r; r.q = 0; return r; }

/* cvtps2pi / cvttps2pi: the low two packed singles of an SSE register or of a
 * 64-bit memory operand become two signed dwords in an MMX register. The
 * rounding form follows the current rounding mode, round-to-nearest everywhere
 * these titles use it; the truncating form is what a C cast already does.
 *
 * An input that is NaN or outside int32 gives the "integer indefinite" value
 * on hardware, where the C cast is undefined -- and a video decoder pushing
 * coefficients through this reaches that edge often enough to matter. */
static inline int32_t MMX_CVT_F2I(float v, int truncate)
{
    if (!(v >= -2147483648.0f && v <= 2147483647.0f))
        return (int32_t)0x80000000u;       /* integer indefinite */
    if (truncate)
        return (int32_t)v;
    return (int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

static inline RecompMmx MMX_FROM_PS(float lo, float hi, int truncate)
{
    RecompMmx r;
    r.d[0] = MMX_CVT_F2I(lo, truncate);
    r.d[1] = MMX_CVT_F2I(hi, truncate);
    return r;
}

static inline RecompMmx MMX_MEM(uint32_t addr) {
    RecompMmx r;
    memcpy(&r, (const void *)XBOX_PTR(addr), 8);
    return r;
}

static inline void MMX_STORE(uint32_t addr, RecompMmx v) {
    memcpy((void *)XBOX_PTR(addr), &v, 8);
}

static inline RecompMmx MMX_FROM32(uint32_t v) {
    RecompMmx r; r.q = 0; r.ud[0] = v; return r;
}

/* -- saturation helpers ---------------------------------------- */
static inline int16_t recomp_sat_i16(int32_t v) {
    return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}
static inline int8_t recomp_sat_i8(int32_t v) {
    return (int8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
}
static inline uint8_t recomp_sat_u8(int32_t v) {
    return (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
}
static inline uint16_t recomp_sat_u16(int32_t v) {
    return (uint16_t)(v > 65535 ? 65535 : (v < 0 ? 0 : v));
}

/* -- integer arithmetic, lane-wise, wrapping -------------------- */
#define RECOMP_MMX_BINOP(NAME, LANES, FIELD, EXPR)                      \
    static inline RecompMmx NAME(RecompMmx a, RecompMmx b) {            \
        RecompMmx r; int i;                                             \
        for (i = 0; i < (LANES); i++) { (void)b; r.FIELD[i] = (EXPR); } \
        return r;                                                       \
    }

RECOMP_MMX_BINOP(MMX_PADDB, 8, b, (int8_t)((uint8_t)a.b[i] + (uint8_t)b.b[i]))
RECOMP_MMX_BINOP(MMX_PADDW, 4, w, (int16_t)((uint16_t)a.w[i] + (uint16_t)b.w[i]))
RECOMP_MMX_BINOP(MMX_PADDD, 2, d, (int32_t)((uint32_t)a.d[i] + (uint32_t)b.d[i]))
RECOMP_MMX_BINOP(MMX_PSUBB, 8, b, (int8_t)((uint8_t)a.b[i] - (uint8_t)b.b[i]))
RECOMP_MMX_BINOP(MMX_PSUBW, 4, w, (int16_t)((uint16_t)a.w[i] - (uint16_t)b.w[i]))
RECOMP_MMX_BINOP(MMX_PSUBD, 2, d, (int32_t)((uint32_t)a.d[i] - (uint32_t)b.d[i]))
RECOMP_MMX_BINOP(MMX_PADDSB, 8, b, recomp_sat_i8((int32_t)a.b[i] + b.b[i]))
RECOMP_MMX_BINOP(MMX_PADDSW, 4, w, recomp_sat_i16((int32_t)a.w[i] + b.w[i]))
RECOMP_MMX_BINOP(MMX_PSUBSB, 8, b, recomp_sat_i8((int32_t)a.b[i] - b.b[i]))
RECOMP_MMX_BINOP(MMX_PSUBSW, 4, w, recomp_sat_i16((int32_t)a.w[i] - b.w[i]))
RECOMP_MMX_BINOP(MMX_PADDUSB, 8, ub,
                 recomp_sat_u8((int32_t)a.ub[i] + b.ub[i]))
RECOMP_MMX_BINOP(MMX_PSUBUSB, 8, ub,
                 recomp_sat_u8((int32_t)a.ub[i] - b.ub[i]))
RECOMP_MMX_BINOP(MMX_PADDUSW, 4, uw,
                 recomp_sat_u16((int32_t)a.uw[i] + b.uw[i]))
RECOMP_MMX_BINOP(MMX_PSUBUSW, 4, uw,
                 recomp_sat_u16((int32_t)a.uw[i] - b.uw[i]))
RECOMP_MMX_BINOP(MMX_PMULLW, 4, w, (int16_t)((int32_t)a.w[i] * b.w[i]))
RECOMP_MMX_BINOP(MMX_PMULHW, 4, w, (int16_t)(((int32_t)a.w[i] * b.w[i]) >> 16))
RECOMP_MMX_BINOP(MMX_PAVGB, 8, ub, (uint8_t)(((int32_t)a.ub[i] + b.ub[i] + 1) >> 1))
RECOMP_MMX_BINOP(MMX_PAVGW, 4, uw, (uint16_t)(((int32_t)a.uw[i] + b.uw[i] + 1) >> 1))
RECOMP_MMX_BINOP(MMX_PMINSW, 4, w, (a.w[i] < b.w[i] ? a.w[i] : b.w[i]))
RECOMP_MMX_BINOP(MMX_PMAXSW, 4, w, (a.w[i] > b.w[i] ? a.w[i] : b.w[i]))
RECOMP_MMX_BINOP(MMX_PCMPEQB, 8, b, (int8_t)(a.b[i] == b.b[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPEQW, 4, w, (int16_t)(a.w[i] == b.w[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPEQD, 2, d, (int32_t)(a.d[i] == b.d[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTB, 8, b, (int8_t)(a.b[i] > b.b[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTW, 4, w, (int16_t)(a.w[i] > b.w[i] ? -1 : 0))
RECOMP_MMX_BINOP(MMX_PCMPGTD, 2, d, (int32_t)(a.d[i] > b.d[i] ? -1 : 0))

/* pmaddwd: multiply signed words, add adjacent pairs into dwords. */
static inline RecompMmx MMX_PMADDWD(RecompMmx a, RecompMmx b) {
    RecompMmx r;
    r.d[0] = (int32_t)a.w[0] * b.w[0] + (int32_t)a.w[1] * b.w[1];
    r.d[1] = (int32_t)a.w[2] * b.w[2] + (int32_t)a.w[3] * b.w[3];
    return r;
}

/* -- bitwise ---------------------------------------------------- */
static inline RecompMmx MMX_PAND(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q & b.q; return r; }
static inline RecompMmx MMX_PANDN(RecompMmx a, RecompMmx b) { RecompMmx r; r.q = ~a.q & b.q; return r; }
static inline RecompMmx MMX_POR(RecompMmx a, RecompMmx b)   { RecompMmx r; r.q = a.q | b.q; return r; }
static inline RecompMmx MMX_PXOR(RecompMmx a, RecompMmx b)  { RecompMmx r; r.q = a.q ^ b.q; return r; }

/* -- shifts -----------------------------------------------------
 * A count at or past the lane width gives zero for the logical shifts and a
 * full sign fill for the arithmetic ones. The count is the whole 64-bit
 * register, not one lane, and it is unsigned -- a negative-looking count is a
 * huge one, which saturates the same way.
 */
#define RECOMP_MMX_SHIFT(NAME, LANES, FIELD, WIDTH, OP)                 \
    static inline RecompMmx NAME(RecompMmx a, uint64_t cnt) {           \
        RecompMmx r; int i;                                             \
        for (i = 0; i < (LANES); i++)                                   \
            r.FIELD[i] = (cnt >= (WIDTH)) ? 0 : (OP);                   \
        return r;                                                       \
    }
RECOMP_MMX_SHIFT(MMX_PSLLW, 4, uw, 16, (uint16_t)(a.uw[i] << cnt))
RECOMP_MMX_SHIFT(MMX_PSRLW, 4, uw, 16, (uint16_t)(a.uw[i] >> cnt))
RECOMP_MMX_SHIFT(MMX_PSLLD, 2, ud, 32, (uint32_t)(a.ud[i] << cnt))
RECOMP_MMX_SHIFT(MMX_PSRLD, 2, ud, 32, (uint32_t)(a.ud[i] >> cnt))

static inline RecompMmx MMX_PSLLQ(RecompMmx a, uint64_t cnt) {
    RecompMmx r; r.q = (cnt >= 64) ? 0 : (a.q << cnt); return r;
}
static inline RecompMmx MMX_PSRLQ(RecompMmx a, uint64_t cnt) {
    RecompMmx r; r.q = (cnt >= 64) ? 0 : (a.q >> cnt); return r;
}
static inline RecompMmx MMX_PSRAW(RecompMmx a, uint64_t cnt) {
    RecompMmx r; int i; uint64_t c = (cnt >= 16) ? 15 : cnt;
    for (i = 0; i < 4; i++) r.w[i] = (int16_t)(a.w[i] >> c);
    return r;
}
static inline RecompMmx MMX_PSRAD(RecompMmx a, uint64_t cnt) {
    RecompMmx r; int i; uint64_t c = (cnt >= 32) ? 31 : cnt;
    for (i = 0; i < 2; i++) r.d[i] = (int32_t)(a.d[i] >> c);
    return r;
}

/* -- unpack / pack ---------------------------------------------- */
static inline RecompMmx MMX_PUNPCKLBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) { r.ub[i*2] = a.ub[i]; r.ub[i*2+1] = b.ub[i]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKHBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) { r.ub[i*2] = a.ub[i+4]; r.ub[i*2+1] = b.ub[i+4]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKLWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) { r.uw[i*2] = a.uw[i]; r.uw[i*2+1] = b.uw[i]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKHWD(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) { r.uw[i*2] = a.uw[i+2]; r.uw[i*2+1] = b.uw[i+2]; }
    return r;
}
static inline RecompMmx MMX_PUNPCKLDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r; r.ud[0] = a.ud[0]; r.ud[1] = b.ud[0]; return r;
}
static inline RecompMmx MMX_PUNPCKHDQ(RecompMmx a, RecompMmx b) {
    RecompMmx r; r.ud[0] = a.ud[1]; r.ud[1] = b.ud[1]; return r;
}
static inline RecompMmx MMX_PACKSSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.b[i]     = recomp_sat_i8(a.w[i]);
    for (i = 0; i < 4; i++) r.b[i + 4] = recomp_sat_i8(b.w[i]);
    return r;
}
static inline RecompMmx MMX_PACKUSWB(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.ub[i]     = recomp_sat_u8(a.w[i]);
    for (i = 0; i < 4; i++) r.ub[i + 4] = recomp_sat_u8(b.w[i]);
    return r;
}
static inline RecompMmx MMX_PACKSSDW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i;
    for (i = 0; i < 2; i++) r.w[i]     = recomp_sat_i16(a.d[i]);
    for (i = 0; i < 2; i++) r.w[i + 2] = recomp_sat_i16(b.d[i]);
    return r;
}

/* -- word insert / extract / shuffle ----------------------------- */
static inline RecompMmx MMX_PSHUFW(RecompMmx a, uint32_t imm) {
    RecompMmx r; int i;
    for (i = 0; i < 4; i++) r.uw[i] = a.uw[(imm >> (i * 2)) & 3];
    return r;
}
static inline RecompMmx MMX_PINSRW(RecompMmx a, uint32_t v, uint32_t imm) {
    RecompMmx r = a; r.uw[imm & 3] = (uint16_t)v; return r;
}
static inline uint32_t MMX_PEXTRW(RecompMmx a, uint32_t imm) {
    return (uint32_t)a.uw[imm & 3];
}
static inline uint32_t MMX_PMOVMSKB(RecompMmx a) {
    uint32_t m = 0; int i;
    for (i = 0; i < 8; i++) if (a.ub[i] & 0x80) m |= (1u << i);
    return m;
}
static inline RecompMmx MMX_PSADBW(RecompMmx a, RecompMmx b) {
    RecompMmx r; int i; uint32_t s = 0;
    for (i = 0; i < 8; i++)
        s += (uint32_t)(a.ub[i] > b.ub[i] ? a.ub[i] - b.ub[i] : b.ub[i] - a.ub[i]);
    r.q = 0; r.uw[0] = (uint16_t)s;
    return r;
}

#define xmm0 g_xmm0
#define xmm1 g_xmm1
#define xmm2 g_xmm2
#define xmm3 g_xmm3
#define xmm4 g_xmm4
#define xmm5 g_xmm5
#define xmm6 g_xmm6
#define xmm7 g_xmm7
#define mm0 g_mm0
#define mm1 g_mm1
#define mm2 g_mm2
#define mm3 g_mm3
#define mm4 g_mm4
#define mm5 g_mm5
#define mm6 g_mm6
#define mm7 g_mm7
/* ebp is NOT global - it's local in each function.
 * For __SEH_prolog/epilog, use g_seh_ebp to bridge. */
#endif

/* ================================================================
 * Forward declarations for translated functions
 *
 * These are generated by the recompiler and included per-file.
 * The recomp_funcs.h header (generated) declares all translated
 * function prototypes.
 * ================================================================ */

#endif /* RECOMP_TYPES_H */
