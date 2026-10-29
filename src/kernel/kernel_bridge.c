/**
 * kernel_bridge.c - Bridge between translated game code and kernel functions
 *
 * Problem:
 *   Translated game code calls kernel functions via indirect calls through
 *   the kernel thunk table at VA 0x0036B7C0. In the XBE file, these entries
 *   contain unresolved ordinals (0x80000000 | ordinal). On real Xbox hardware,
 *   the kernel loader replaces these with actual function pointers before the
 *   game runs.
 *
 * Solution:
 *   1. After xbox_MemoryLayoutInit copies .rdata, call xbox_kernel_bridge_init()
 *   2. Replace each ordinal entry in Xbox memory with a synthetic VA
 *   3. When RECOMP_ICALL encounters a synthetic VA, route it to a per-ordinal
 *      bridge function that reads args from the simulated Xbox stack, translates
 *      pointer arguments from Xbox VA→native, and calls the kernel function.
 *
 * Synthetic VA scheme:
 *   Each thunk slot i gets VA 0xFE000000 + i*4
 *   The lookup function checks this range and dispatches appropriately.
 *
 * Why per-ordinal bridges instead of a generic trampoline:
 *   Kernel functions receive Xbox pointers (32-bit VAs) that must be translated
 *   to native pointers by adding g_xbox_mem_offset. Different functions have
 *   different parameter layouts (pointer vs value), so each needs its own bridge.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "recomp_icall_feedback.h"
#include <stdio.h>
/* stdlib.h is load-bearing, not tidiness. Without it C89 implicit declaration
 * makes malloc return `int`, so bridge_spawn_thread truncated its heap pointer
 * to 32 bits and sign-extended it into a `struct bridge_thread_start *`. Every
 * subsequent s->field wrote to an address that had nothing to do with the
 * allocation. MSVC says so (C4013 + C4047 "differs in levels of indirection")
 * but only as warnings, and this file is compiled with /W4 /WX-. */
#include <stdlib.h>
#include <float.h>

/* Access to recompiled code registers. Per-thread: RECOMP_TLS comes from
 * xbox_memory_layout.h and must match the definitions there -- a plain extern
 * here binds to the TLS template rather than the calling thread's copy, which
 * reads as every register being zero. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* SCDA-side: last indirect-call source VA, set by generated code just before
 * RECOMP_ICALL_SAFE, read by recomp_icall_fail_log for diagnostics. */
extern RECOMP_TLS volatile uint32_t g_icall_source;
extern ptrdiff_t g_xbox_mem_offset;
/* The calling thread's TIB/KPCR (xbox_memory_layout.c); fs:[0x24] is Irql. */
uint8_t xbox_fs_read8(uint32_t offset);
void xbox_fs_write8(uint32_t offset, uint8_t value);

/* Dispatch table lookup (for function pointer args) */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);

/* Memory access - same as recomp_types.h MEM32 but without the #define guard */
#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))

/* Translate Xbox VA to native pointer (NULL-safe: 0 → NULL) */
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)

/* ── Synthetic VA range (for function exports) ─────────── */

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)

/* ── Kernel data exports ──────────────────────────────────
 *
 * Some kernel ordinals are DATA exports (structs/variables), not functions.
 * The game reads their thunk entries and dereferences the result to access
 * the data. These cannot use synthetic VAs — they must point to real,
 * dereferenceable addresses in the Xbox VA space.
 *
 * We allocate a "kernel data area" at XBOX_KERNEL_DATA_BASE and populate
 * it with the expected structures.
 */

#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)(addr) + g_xbox_mem_offset))

/**
 * Get the Xbox VA of data for a kernel DATA export ordinal.
 * Returns 0 if the ordinal is not a data export (i.e., it's a function).
 */
static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{
    /* Ordinals here are checked BEFORE function routing (see the thunk build
     * loop), so an ordinal listed by mistake turns a real kernel function into
     * a data address -- the title then calls it and jumps into kernel data.
     *
     * This table had a whole block shifted. 17 (ExFreePool), 65
     * (IoCreateDevice), 327 (XeLoadSection) and 328 (XeUnloadSection) are all
     * functions and were all being handed data addresses; Crimson Skies imports
     * every one of them. In the other direction, the genuine exports at 16, 353,
     * 354, 355, 356 and 357 got no thunk at all, so a title reading
     * XboxLANKey or KeTimeIncrement read whatever the function fallback left.
     *
     * test_bridge_ordinals.py now checks every entry below against the export
     * table, which is why the block cannot drift again unnoticed. */
    switch (ordinal) {
    case  16: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  22: return XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    case  30: return XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    case  31: return XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    case  40: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS;
    case  41: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_MODEL_STR;
    case  42: return XBOX_KERNEL_DATA_BASE + KDATA_DISK_SERIAL_STR;
    case  64: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  70: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case  71: return XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 157: return XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 353: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 354: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    case 356: return XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO;
    case 357: return XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
    default:  return 0;  /* Not a data export */
    }
}

/**
 * Initialize kernel data export values at the kernel data area.
 * Called during bridge init, after Xbox memory is mapped.
 */
static void bridge_load_launch_page(void);  /* title relaunch, below */

static void bridge_load_signature_keys(void)
{
    char path[MAX_PATH];
    const char *env = getenv("RECOMP_XBOX_KEYS");
    uint8_t keys[32];
    FILE *f;
    size_t got;
    if (env && env[0]) {
        snprintf(path, sizeof(path), "%s", env);
    } else {
        char *slash;
        DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
        if (n == 0 || n >= sizeof(path))
            return;
        slash = strrchr(path, 0x5C);  /* backslash */
        if (!slash)
            return;
        snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "xbox_keys.bin");
    }
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "  [KERNEL] no %s: save-signing keys stay zero "
                "(console/xemu saves will not verify)\n", path);
        return;
    }
    got = fread(keys, 1, sizeof(keys), f);
    fclose(f);
    if (got != sizeof(keys)) {
        fprintf(stderr, "  [KERNEL] %s has %u bytes, expected 32: keys stay zero\n",
                path, (unsigned)got);
        return;
    }
    memcpy((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset),
           keys, 16);
    memcpy((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset),
           keys + 16, 16);
    fprintf(stderr, "  [KERNEL] save-signing keys loaded from %s\n", path);
}

static void kernel_data_init(void)
{
    /* XboxHardwareInfo (ordinal 322) - XBOX_HARDWARE_INFO
     *   +0: ULONG Flags (0 = retail, 0x20 = devkit)
     *   +4: UCHAR GpuRevision
     *   +5: UCHAR McpRevision
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;   /* Retail */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1; /* NV2A A1 */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1; /* MCPX B1 */

    /* XboxKrnlVersion (ordinal 324) - XBOX_KRNL_VERSION
     *   +0: USHORT Major (1)
     *   +2: USHORT Minor (0)
     *   +4: USHORT Build (5849 = XDK version)
     *   +6: USHORT Qfe (0)
     */
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    /* KeTickCount (ordinal 156) - initialized to current tick count.
     * A background thread in main.c updates this every ~1ms. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = xbox_PreciseTickCount();

    /* LaunchDataPage (ordinal 164) - NULL (no launch data), unless this
     * process is the relaunch of a title that called XLaunchNewImage. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;
    bridge_load_launch_page();

    /* Object-type exports. Each gets a DISTINCT non-zero value rather than 0.
     *
     * They were all zero, which is wrong twice over: a title that null-checks
     * one sees "no such type", and a title that distinguishes two of them --
     * ObReferenceObjectByHandle takes an expected type and compares it -- sees
     * every type as equal, so a mutant handle passes a check meant for events.
     * The values are opaque to the game; only identity and non-nullness matter,
     * so they are the export ordinal offset into the kernel data area, which
     * also makes a stray one recognisable in a crash dump.
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE)    = XBOX_KERNEL_DATA_BASE + KDATA_MUTANT_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE) = XBOX_KERNEL_DATA_BASE + KDATA_SEMAPHORE_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE)     = XBOX_KERNEL_DATA_BASE + KDATA_TIMER_OBJ_TYPE;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE)      = XBOX_KERNEL_DATA_BASE + KDATA_FILE_OBJ_TYPE;

    /* KeTimeIncrement (ordinal 157) - 100ns units per clock tick. 0x2710 is
     * 1 ms, which is what KeTickCount above is updated at. A title dividing by
     * this to convert ticks to time gets a division by zero if it is left 0. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TIME_INCREMENT) = 0x2710;

    /* HalBootSMCVideoMode (ordinal 356) - SMC video mode word from boot. 0 is
     * "no video mode reported", which titles treat as auto-detect. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_BOOT_SMC_VIDEO) = 0;

    /* IdexChannelObject (ordinal 357) - IDE channel object. Not opaque: XAPI
     * CancelIo raises to DISPATCH_LEVEL and walks the channel's pending-IRP
     * queue, a LIST_ENTRY at +0x28, moving every IRP of the file being
     * cancelled onto a local list. File I/O here completes synchronously, so
     * nothing is ever queued: the correct state is an empty list (Flink =
     * Blink = the head). With the old 16-byte placeholder +0x28 landed in
     * XboxSignatureKey (zeros), the walk followed a null Flink and never got
     * back to the head -- SCDA's DirectSound stream Release -> CancelIo hung
     * the main thread in the menu (docs Update 451). */
    {
        const uint32_t chan = XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL;
        memset((void*)((uintptr_t)chan + g_xbox_mem_offset), 0, KDATA_IDEX_CHANNEL_SIZE);
        BRIDGE_MEM32(chan + 0x28) = chan + 0x28;  /* Flink */
        BRIDGE_MEM32(chan + 0x2C) = chan + 0x28;  /* Blink */
    }

    /* HalDiskCachePartitionCount (ordinal 40) - number of cache partitions.
     * Retail consoles report 3 (X, Y, Z). Titles size a partition array from
     * this, so 0 gives a zero-length array and 1 hides two drives. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_DISK_CACHE_PARTS) = 3;

    /* IoCompletionObjectType (ordinal 64) - type object */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;

    /* IoDeviceObjectType (ordinal 71) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;

    /* XboxHDKey (ordinal 323) and XboxSignatureKey (ordinal 325). Titles sign
     * their saves with XCalculateSignature, an HMAC-SHA1 keyed from
     * XboxSignatureKey (roamable, identical on every retail kernel) or
     * XboxHDKey (non-roamable, per console). With zeros here, saves from a
     * console or xemu fail the title's signature check and ours fail there.
     * The keys are console/Microsoft data and are not shipped: they are
     * loaded from a local 32-byte file (SignatureKey, then HDKey), read from
     * the user's own kernel with tools/debug/xemu_read_kernel_keys.py --
     * RECOMP_XBOX_KEYS=<path>, default xbox_keys.bin next to the EXE. No
     * file keeps the old all-zero keys (docs Update 453). */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);
    bridge_load_signature_keys();

    /* XboxLANKey (ordinals 326, 355) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxAlternateSignatureKeys (ordinals 327, 356) - 256 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    /* XePublicKeyData (ordinal 357) - 284 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    /* HAL disk identity strings (ordinals 41/42). The exported symbol is an
     * XBOX_ANSI_STRING whose Buffer must be an Xbox VA the title can deref --
     * HalRandGather reads the bytes for entropy. Build the struct and its text
     * inside the kernel data area so both are addressable. */
    {
        struct { uint32_t str_off, buf_off; const char *text; } d[] = {
            { KDATA_DISK_MODEL_STR,  KDATA_DISK_MODEL_BUF,  "XBOXRECOMP VIRTUAL HDD" },
            { KDATA_DISK_SERIAL_STR, KDATA_DISK_SERIAL_BUF, "XR0000000000" },
            /* XeImageFileName (ordinal 326). Declared but never filled in, so
             * its Buffer held whatever was in the page -- Half-Life 2's CRT
             * reads it while working out the running image's path, took the
             * uninitialised bytes as a char*, and dereferenced 0x68737572
             * (the ASCII "rush"). A disc-booted title's value looks like this. */
            { KDATA_XE_IMAGE_FILENAME, KDATA_XE_IMAGE_BUF,
              "\\Device\\CdRom0\\default.xbe" },
        };
        for (int k = 0; k < (int)(sizeof(d) / sizeof(d[0])); k++) {
            uint32_t str_va = XBOX_KERNEL_DATA_BASE + d[k].str_off;
            uint32_t buf_va = XBOX_KERNEL_DATA_BASE + d[k].buf_off;
            size_t len = strlen(d[k].text);
            memcpy(XBOX_TO_NATIVE(buf_va), d[k].text, len + 1);
            BRIDGE_MEM16(str_va + 0) = (uint16_t)len;        /* Length */
            BRIDGE_MEM16(str_va + 2) = (uint16_t)(len + 1);  /* MaximumLength */
            BRIDGE_MEM32(str_va + 4) = buf_va;               /* Buffer (Xbox VA) */
        }
    }

    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

/* SCDA-only, not upstream yet: refresh dynamic kernel data exports that guest
 * code reads directly off the kernel-data page (no thunk dispatch occurs at
 * the read site, so it must be kept current between reads instead). Called
 * from scda_trace_pc on every traced guest instruction. */
void xbox_KernelDataTickUpdate(void)
{
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = xbox_PreciseTickCount();
}

/* ── Per-slot ordinal and bridge function ────────────────── */

/* Ordinal for each slot (read from Xbox memory during init) */
static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Log counter - limit output to avoid flooding */
static int g_kernel_call_count = 0;

/* How many kernel calls get logged before the log goes quiet.
 *
 * The cap keeps a title that makes thousands of calls from burying the
 * console, but a bring-up that gets past early init then has no visibility at
 * exactly the point it stops being obvious. Override with
 * RECOMP_KERNEL_LOG_BUDGET, the same way RECOMP_TRACE_BUDGET works for the
 * function tracer. 0 silences the log entirely.
 */
static long kernel_log_budget(void)
{
    static long budget = -1;

    if (budget < 0) {
        const char *env = getenv("RECOMP_KERNEL_LOG_BUDGET");
        budget = env ? strtol(env, NULL, 0) : 200;
        if (budget < 0)
            budget = 0;
    }
    return budget;
}

#define KERNEL_LOG_ON()      (g_kernel_call_count <= kernel_log_budget())
/* Some sites logged at a tighter cap than the rest; keep them proportional. */
#define KERNEL_LOG_ON_HALF() (g_kernel_call_count <= kernel_log_budget() / 2)

/* Kernel calls still to log after a directory open (RECOMP_KERNEL_FOLLOW_DIR). */
static RECOMP_TLS int g_kernel_follow_left;

/* Read Xbox stack arg as uint32_t.
 * After kernel_thunk_dispatch pops the dummy return address (g_esp += 4),
 * arg0 is at g_esp+0, arg1 at g_esp+4, etc. */
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

/* ── Per-ordinal bridge functions ─────────────────────────
 *
 * Each bridge reads args from the Xbox stack, translates pointer
 * args from Xbox VA→native, calls the kernel function, and stores
 * the result in g_eax.
 *
 * Xbox cdecl: args pushed right-to-left, caller cleans stack.
 * Xbox stdcall: args pushed right-to-left, callee cleans stack.
 * In our case the caller (translated code) does "PUSH32" for each arg
 * before calling, and the kernel function's ret-N is handled by the
 * translated code's own stack adjustment.
 */

/* ── PsCreateSystemThreadEx (ordinal 255) ────────────────
 * NTSTATUS PsCreateSystemThreadEx(
 *   PHANDLE ThreadHandle,      // arg0: Xbox VA → pointer
 *   ULONG ThreadExtraSize,     // arg1: value
 *   ULONG KernelStackSize,     // arg2: value
 *   ULONG TlsDataSize,         // arg3: value
 *   PULONG ThreadId,           // arg4: Xbox VA → pointer (can be NULL)
 *   PVOID StartContext1,       // arg5: Xbox VA → opaque
 *   PVOID StartContext2,       // arg6: Xbox VA → opaque
 *   BOOLEAN CreateSuspended,   // arg7: value
 *   BOOLEAN DebugStack,        // arg8: value
 *   PXBOX_SYSTEM_ROUTINE StartRoutine  // arg9: Xbox function pointer
 * )
 *
 * For static recompilation, we don't create a real thread.
 * Instead we call the StartRoutine synchronously via RECOMP_ICALL.
 * This is correct because on Xbox, the entry point creates a system
 * thread and returns, and the thread runs the actual game.
 */
static int g_thread_call_count = 0;

/* Thread entry shim. Sets up the new thread's own simulated stack, pushes the
 * two Xbox start-context arguments plus the dummy return address the callee's
 * `ret` consumes, and runs. */
/* Set on threads this bridge spawned; see PsTerminateSystemThread. */
static RECOMP_TLS int g_is_spawned_thread = 0;

/* This thread's simulated stack, so both exits can give it back. Thread-local
 * for the obvious reason, and needed at all because the common exit is
 * ExitThread from PsTerminateSystemThread -- bridge_thread_main's own return
 * path is the rare one. */
static RECOMP_TLS uint32_t g_thread_stack_top = 0;

/* The fake KTHREAD-shaped guest structure this worker should mark done on
 * exit (0 for the inline "first call" path, which never has one). See
 * bridge_ObReferenceObjectByHandle and s_handle_kthread_va below. */
static RECOMP_TLS uint32_t g_thread_kthread_va = 0;

#define KTHREAD_STRUCT_SIZE     0x124u
#define KTHREAD_DONE_OFFSET     4u
#define KTHREAD_EXITCODE_OFFSET 0x120u

/* Tagged-handle constants and the fake-KTHREAD lookup table, forward-declared
 * here (full handle table further down, see "Handle table") because
 * bridge_PsCreateSystemThreadEx registers into s_handle_kthread_va and
 * bridge_ObReferenceObjectByHandle reads it, and both sit earlier in the
 * file than that section. */
#define BRIDGE_HANDLE_TAG  0x48000000u
#define BRIDGE_HANDLE_MASK 0x00FFFFFFu
#define BRIDGE_HANDLE_MAX  16384
static uint32_t s_handle_kthread_va[BRIDGE_HANDLE_MAX];

/* Mark a worker's fake KTHREAD structure done, for whichever poll loop is
 * waiting on ObReferenceObjectByHandle's resolution of its thread handle. */
static void bridge_mark_thread_done(uint32_t kthread_va, uint32_t exit_status)
{
    if (!kthread_va)
        return;
    BRIDGE_MEM32(kthread_va + KTHREAD_EXITCODE_OFFSET) = exit_status;
    BRIDGE_MEM8(kthread_va + KTHREAD_DONE_OFFSET) = 1;
}

struct bridge_thread_start {
    recomp_func_t fn;
    uint32_t ctx1, ctx2, stack_top, kthread_va;
};

static void bridge_write_handle(uint32_t handle_va, HANDLE h);

static void bridge_run_thread_inline(recomp_func_t fn, uint32_t ctx1,
                                     uint32_t ctx2)
{
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = ctx1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    g_seh_ebp = g_esp;
    fn();
    g_esp += 12;
}

static DWORD WINAPI bridge_thread_main(LPVOID param)
{
    struct bridge_thread_start *s = (struct bridge_thread_start *)param;
    recomp_func_t fn = s->fn;
    uint32_t ctx1 = s->ctx1, ctx2 = s->ctx2, kthread_va = s->kthread_va;

    xbox_PinGuestThread();  /* runs guest code: the console's single core */

    /* Own register set (RECOMP_TLS), own simulated stack -- and own TIB.
     *
     * The TIB carries the SEH chain head and, through fs:[4], the CRT's
     * per-thread data. Sharing one made "which thread am I" a single answer
     * for every thread, which is how two of them ended up inside _lock() each
     * holding the lock the other wanted. */
    g_is_spawned_thread = 1;
    g_esp = s->stack_top;
    g_thread_stack_top = s->stack_top;
    g_thread_kthread_va = kthread_va;
    {
        uint32_t tib = xbox_AllocThreadTib();
        if (tib)
            g_fs_base = tib;
        else
            fprintf(stderr, "  [KERNEL] worker thread has no TIB of its own;"
                            " it shares the main thread's\n");
    }
    free(s);

    bridge_run_thread_inline(fn, ctx1, ctx2);

    fprintf(stderr, "  [KERNEL] worker thread returned (eax=0x%08X)\n", g_eax);
    fflush(stderr);
    /* The routine returned instead of calling PsTerminateSystemThread --
     * still a normal exit, so whoever polls ObReferenceObjectByHandle's
     * resolution of this thread's handle needs to see it as done too. */
    bridge_mark_thread_done(kthread_va, g_eax);
    xbox_FreeThreadStack(g_thread_stack_top);
    g_thread_stack_top = 0;
    return 0;
}

/* Guest thread priorities (docs Update 617). The title sets them through
 * KeSetBasePriorityThread on its KTHREAD pointer, which the bridge passed on
 * as if it were a host HANDLE -- SetThreadPriority failed and every guest
 * thread ran at the same priority on the one core they share, unlike the
 * console, where e.g. a streaming thread can outrank rendering. Here each
 * fake KTHREAD is tied to its host thread so the priority can be applied.
 * SCDA raises its audio mixer thread (start 0x45218D, which keeps a 48 ms
 * looping buffer ahead of voice 68) by +1; without it that thread lost the
 * core to rendering and 48 ms of music played twice (Update 621: 3 repeats
 * vs 0 in A/B). Applied by default; RECOMP_THREAD_PRIORITY=0 turns it off.
 * The first calls are logged either way. */
#define BRIDGE_KTHREAD_HOST_MAX 128
static struct { uint32_t va; HANDLE h; } s_kthread_host[BRIDGE_KTHREAD_HOST_MAX];
static volatile LONG s_kthread_host_count;

static int bridge_thread_priority_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_THREAD_PRIORITY");
        on = !(e && e[0] == '0');   /* default on since Update 621 */
        if (on)
            fprintf(stderr, "  [KERNEL] guest thread priorities applied "
                            "(RECOMP_THREAD_PRIORITY=0 turns this off)\n");
    }
    return on;
}

static void bridge_kthread_host_add(uint32_t va, HANDLE th)
{
    HANDLE dup = NULL;
    LONG i;
    if (!va || !th || !DuplicateHandle(GetCurrentProcess(), th,
            GetCurrentProcess(), &dup,
            THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    i = InterlockedIncrement(&s_kthread_host_count) - 1;
    if (i < BRIDGE_KTHREAD_HOST_MAX) {
        s_kthread_host[i].h = dup;
        MemoryBarrier();
        s_kthread_host[i].va = va;
    } else {
        CloseHandle(dup);
    }
}

static HANDLE bridge_kthread_host(uint32_t va)
{
    LONG n = s_kthread_host_count;
    if (!va)
        return NULL;
    if (va == g_thread_kthread_va)
        return GetCurrentThread();
    if (n > BRIDGE_KTHREAD_HOST_MAX)
        n = BRIDGE_KTHREAD_HOST_MAX;
    for (LONG i = 0; i < n; i++)
        if (s_kthread_host[i].va == va)
            return s_kthread_host[i].h;
    return NULL;
}

/* The calling thread's fake KTHREAD; the title's main thread gets one on
 * first use (spawned threads have theirs from PsCreateSystemThreadEx). */
static uint32_t bridge_current_kthread(void)
{
    if (!g_thread_kthread_va) {
        uint32_t va = xbox_HeapAlloc(KTHREAD_STRUCT_SIZE, 4);
        if (va) {
            g_thread_kthread_va = va;
            bridge_kthread_host_add(va, GetCurrentThread());
        }
    }
    return g_thread_kthread_va;
}

static HANDLE bridge_spawn_thread(recomp_func_t fn, uint32_t ctx1,
                                  uint32_t ctx2, uint32_t stack_top,
                                  uint32_t kthread_va,
                                  int create_suspended)
{
    struct bridge_thread_start *s = malloc(sizeof(*s));
    HANDLE th;

    if (!s) return NULL;
    s->fn = fn; s->ctx1 = ctx1; s->ctx2 = ctx2; s->stack_top = stack_top;
    s->kthread_va = kthread_va;

    /* PsCreateSystemThreadEx's CreateSuspended flag is observable title
     * semantics, not just a scheduling hint.  SCDA's Bink pool deliberately
     * creates a worker suspended, fills the context's event/mutant handles,
     * and resumes it afterwards.  Starting it here immediately lets the
     * worker consume stale heap dwords as handles before the owner finishes
     * the context. */
    th = CreateThread(NULL, 0, bridge_thread_main, s,
                      create_suspended ? CREATE_SUSPENDED : 0, NULL);
    if (!th) free(s);
    /* Record the game thread so a host-tick-driven title's watchdog can sample
     * it via xbox_thread_debug_handle. Harmless for default-model titles: they
     * spawn workers too, but never read it back. See kernel_thread.c. */
    else {
        xbox_set_game_thread(th);
        bridge_kthread_host_add(kthread_va, th);   /* priorities, Update 617 */
    }
    return th;
}

/* Two ways a title expects its first PsCreateSystemThreadEx to behave.
 *
 * INLINE (default): the first call IS the game starting -- run the routine
 * inline, inheriting register state, and it drives its own main loop forever.
 * This is what Halo and Crimson Skies need and the historical behavior.
 *
 * SPAWN: the title's entry spawns an init thread and RETURNS, expecting the host
 * to drive the per-frame tick afterwards (Burnout 3 is tick-driven). Here the
 * first call must spawn a real thread and return, so control comes back to the
 * host. Opt in with xbox_SetThreadMode before the game starts. See
 * docs/technical/burnout3-reunification.md. */
/* XBOX_THREAD_MODE_* and xbox_SetThreadMode are declared in xbox_memory_layout.h. */
static int g_thread_mode = XBOX_THREAD_MODE_INLINE;
void xbox_SetThreadMode(int mode) { g_thread_mode = mode; }

static void bridge_PsCreateSystemThreadEx(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    uint32_t create_suspended = STACK_ARG(7);
    uint32_t start_routine   = STACK_ARG(9);
    /* In SPAWN mode there is no privileged "first call": every thread is real,
     * so the entry can return. In INLINE mode the first call runs the game. */
    int is_first_call = (g_thread_mode == XBOX_THREAD_MODE_INLINE)
                        && (g_thread_call_count == 0);
    g_thread_call_count++;

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx #%d: routine=0x%08X ctx1=0x%08X ctx2=0x%08X source=0x%08X\n",
            g_thread_call_count, start_routine, start_context1, start_context2, (uint32_t)g_icall_source);
    fflush(stderr);

    /* Write a fake handle to the output pointer */
    if (xbox_handle_ptr) {
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0001;  /* fake handle */
    }

    /* Call the start routine synchronously through the recomp dispatch.
     * Xbox thread start routines receive two parameters:
     *   void ThreadRoutine(PVOID StartContext1, PVOID StartContext2)
     * We push both onto the simulated stack (right-to-left).
     *
     * First call: the game's main thread entry point. Must run synchronously
     * and inherit the current register state (this IS the game starting).
     *
     * Subsequent calls: worker threads. Must save/restore ALL global registers
     * because on real Xbox each thread has its own register set. Without this,
     * the worker clobbers the caller's g_esi, g_ebx, etc. */
    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                /* Main game thread: run directly, inheriting register state */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                fn();
                g_esp += 12;
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: main thread returned (g_eax=0x%08X)\n", g_eax);
                fflush(stderr);
            } else {
                /* Worker thread: a real one.
                 *
                 * This used to run the routine synchronously and restore the
                 * caller's registers afterwards, which is fine only for a
                 * worker that finishes. Halo's cache/file worker does not -- it
                 * blocks on an event waiting for requests, so CreateThread
                 * never returned and startup deadlocked before the main loop.
                 *
                 * Now that the register set is thread-local (RECOMP_TLS), a
                 * spawned thread gets its own, and the caller's is untouched by
                 * construction rather than by save/restore. */
                uint32_t stack_top = xbox_AllocThreadStackSized(STACK_ARG(2));

                if (!stack_top) {
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: out of "
                            "thread stacks, running worker 0x%08X inline\n",
                            start_routine);
                    fflush(stderr);
                    bridge_run_thread_inline(fn, start_context1, start_context2);
                } else {
                    /* Allocated before the thread starts so bridge_thread_main
                     * has it from its very first instruction -- the worker
                     * marks this done itself on exit (see
                     * bridge_mark_thread_done), whichever of its two exit
                     * paths it takes. */
                    uint32_t kthread_va = xbox_HeapAlloc(KTHREAD_STRUCT_SIZE, 4);
                    HANDLE th = bridge_spawn_thread(fn, start_context1,
                                                    start_context2, stack_top,
                                                    kthread_va,
                                                    create_suspended != 0);
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawned "
                            "worker 0x%08X (ctx=0x%08X, stack top 0x%08X, "
                            "requested stack 0x%X, suspended=%u)\n",
                            start_routine, start_context1, stack_top,
                            STACK_ARG(2), create_suspended != 0);
                    fflush(stderr);
                    fprintf(stderr, "SCDA-DIAG234O spawned tid=%lu routine=0x%08X ctx1=0x%08X ctx2=0x%08X\n",
                            th ? (unsigned long)GetThreadId(th) : 0, start_routine,
                            start_context1, start_context2);
                    fflush(stderr);
                    if (xbox_handle_ptr && th) {
                        bridge_write_handle(xbox_handle_ptr, th);
                        /* Register the fake KTHREAD so
                         * ObReferenceObjectByHandle can hand it back later --
                         * see bridge_ObReferenceObjectByHandle. Re-derives the
                         * table index from the token just written rather than
                         * threading it through bridge_write_handle's return
                         * value, which every other caller already discards. */
                        if (kthread_va) {
                            uint32_t token = BRIDGE_MEM32(xbox_handle_ptr);
                            if ((token & ~BRIDGE_HANDLE_MASK) == BRIDGE_HANDLE_TAG) {
                                uint32_t i = token & BRIDGE_HANDLE_MASK;
                                if (i > 0 && i < BRIDGE_HANDLE_MAX)
                                    s_handle_kthread_va[i] = kthread_va;
                            }
                            fprintf(stderr, "SCDA-DIAG234F thread token=0x%08X kthread_va=0x%08X "
                                    "ctx1=0x%08X ctx2=0x%08X\n",
                                    token, kthread_va, start_context1, start_context2);
                            fflush(stderr);
                        }
                    }
                }
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtClose (ordinal 187) ───────────────────────────────
 * NTSTATUS NtClose(HANDLE Handle)
 * Handle is a value (not a pointer), so safe for generic call.
 */
/* Handle-table helpers; defined further below. Xbox memory slots are 32-bit
 * but native HANDLEs are 64-bit pointers, so handles are kept in a table and
 * referenced by tagged 32-bit tokens. */
static void   bridge_write_handle(uint32_t handle_va, HANDLE h);
static HANDLE bridge_take_handle(uint32_t token);

static void bridge_NtClose(void)
{
    uint32_t raw_handle = STACK_ARG(0);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtClose: handle=0x%08X\n", raw_handle);
        fflush(stderr);
    }

    /* Close real handles but skip fake/synthetic ones */
    if (raw_handle && raw_handle != 0xDEAD0001u && raw_handle != 0xBEEF0010u) {
        HANDLE h = bridge_take_handle(raw_handle);
        if (h && h != INVALID_HANDLE_VALUE)
            xbox_NtClose(h);  /* also ends a directory enumeration on h */
    }
    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── MmAllocateContiguousMemory (ordinal 165) ─────────────
 * PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)
 */
static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    /* From the contiguous window, not the general heap: the caller is
     * entitled to assume (VA & 0x0FFFFFFF) | 0x80000000 == VA, because that
     * is how a driver converts between the address it holds and the one it
     * hands the hardware. See xbox_ContiguousAlloc. */
    uint32_t xbox_va = xbox_ContiguousAlloc(size, 4096);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmAllocateContiguousMemoryEx (ordinal 166) ───────────
 * PVOID MmAllocateContiguousMemoryEx(SIZE_T size, ULONG_PTR low, ULONG_PTR high,
 *                                     ULONG alignment, ULONG protect)
 */
/* Contiguous memory is addressed through the physical-memory mirror: physical
 * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
 * physical addresses check the returned pointer against that, so the address
 * has to be honoured rather than satisfied from the general heap. */
#define XBOX_PHYSICAL_MIRROR_BASE 0x80000000u

static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);
    uint32_t xbox_va;

    (void)prot;

    /*
     * A caller that constrains the range to exactly one allocation's worth is
     * demanding a specific physical address, not expressing a preference.
     * Halo does this for its two big pools and asserts on the result
     * (physical_memory_map.c:46) - XPhysicalAlloc passes lowest = the address
     * it wants and highest = lowest + size - 1, then requires
     * 0x80000000 | lowest back. Satisfying that from the heap fails the assert
     * and leaves its whole memory map wrong.
     */
    if (low && high >= low && (high - low + 1) <= size + 0x1000) {
        xbox_va = XBOX_PHYSICAL_MIRROR_BASE + low;

        /* The console hands out zeroed pages here, and titles rely on it:
         * pool headers and free-list roots are assumed clear, so whatever the
         * backing view happened to contain shows up later as structures that
         * are "allocated" but full of garbage. */
        memset((void *)((uintptr_t)xbox_va + g_xbox_mem_offset), 0, size);

        if (KERNEL_LOG_ON_HALF()) {
            fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u "
                    "pinned phys 0x%08X -> Xbox VA 0x%08X (zeroed)\n",
                    size, low, xbox_va);
            fflush(stderr);
        }
        g_eax = xbox_va;
        return;
    }

    if (align < 4096) align = 4096;
    xbox_va = xbox_ContiguousAlloc(size, align);

    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u → Xbox VA 0x%08X\n",
                size, align, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── MmFreeContiguousMemory (ordinal 171) ─────────────────
 * VOID MmFreeContiguousMemory(PVOID BaseAddress)
 */
static void bridge_MmFreeContiguousMemory(void)
{
    uint32_t addr = STACK_ARG(0);
    xbox_HeapFree(addr);
    g_eax = 0;
}

/* ── MmCreateKernelStack (ordinal 169) ────────────────────
 * PVOID MmCreateKernelStack(ULONG NumberOfBytes, BOOLEAN DebuggerThread)
 *
 * Returns the *top* of a fresh stack (it grows down), 0 on failure. SCDA's
 * XAPI fiber setup at 0x00375090 relies on exactly that: it stores the result
 * as the stack base and, if it loses the install race, frees it with
 * MmDeleteKernelStack(base, base - 0x6000); a 0 return means E_OUTOFMEMORY.
 * With no bridge the call returned 0 and the title went on to call through
 * a garbage pointer at 0x0003FAA0 forever (docs Update 338). */
static void bridge_MmCreateKernelStack(void)
{
    uint32_t size = (STACK_ARG(0) + 0xFFFu) & ~0xFFFu;
    uint32_t base = size ? xbox_HeapAlloc(size, 4096) : 0;

    if (base)
        memset((void *)((uintptr_t)base + g_xbox_mem_offset), 0, size);
    g_eax = base ? base + size : 0;
    if (KERNEL_LOG_ON_HALF()) {
        fprintf(stderr, "  [KERNEL] MmCreateKernelStack: size=%u -> top 0x%08X\n",
                size, g_eax);
        fflush(stderr);
    }
}

/* ── MmDeleteKernelStack (ordinal 170) ────────────────────
 * VOID MmDeleteKernelStack(PVOID StackBase, PVOID StackLimit)
 * StackBase is the top returned above; StackLimit is its low end, which is
 * the address the allocation started at. */
static void bridge_MmDeleteKernelStack(void)
{
    uint32_t limit = STACK_ARG(1);
    if (limit)
        xbox_HeapFree(limit);
    g_eax = 0;
}

/* ── NtAllocateVirtualMemory (ordinal 184) ────────────────
 * NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *     PULONG AllocationSize, ULONG AllocationType, ULONG Protect)
 */
static uint32_t vm_take(uint32_t size, uint32_t hint);
static void vm_record(uint32_t va, uint32_t size);
static int vm_release_enabled(void);
static int vm_query_free(uint32_t addr, uint32_t *end);

static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);  /* PVOID* in Xbox VA */
    uint32_t zero_bits = STACK_ARG(1);
    uint32_t size_ptr = STACK_ARG(2);  /* PULONG in Xbox VA */
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    /* Read the requested size from Xbox memory */
    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    /* Read the base address hint (0 = let kernel choose) */
    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    if (size == 0) {
        g_eax = 0xC0000045u; /* STATUS_INVALID_PAGE_PROTECTION */
        return;
    }

    /*
     * Xbox NtAllocateVirtualMemory supports two modes:
     * - MEM_RESERVE (0x2000): Reserve virtual address space
     * - MEM_COMMIT  (0x1000): Commit pages within a reserved region
     * - MEM_RESERVE|MEM_COMMIT (0x3000): Both in one call
     *
     * Our Xbox heap (bump allocator) always commits memory immediately,
     * so MEM_COMMIT on an already-reserved region is a no-op.
     * Only allocate new memory when MEM_RESERVE is requested.
     */
    /* An address above physical RAM is not free memory -- it aliases.
     *
     * The runtime maps 64 MB and then mirrors it at 64 MB intervals,
     * because real Xbox RAM wraps on a 26-bit address bus. So a guest that
     * sub-allocates past the top of RAM does not get fresh pages, it gets
     * low memory that something else already owns, and the two quietly
     * share storage. Half-Life 2 put a CUtlRBTree element array at
     * 0x0CB80000, which aliases 0x00B80000; other regions it took land
     * inside the live heap (0x05B80000 -> 0x01B80000).
     *
     * Real hardware wraps *physical* addresses while translating virtual
     * ones, so this never happens there. Modelling every guest address as
     * physical is the gap, and that is a bigger change than a bridge fix.
     * Until then, say so: silent aliasing surfaces as corrupted data
     * structures far from here, which is the worst way to find it.
     */
    if (base_hint >= (g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram)) {
        static unsigned warned;
        if (warned++ < 8)
            fprintf(stderr,
                    "  [KERNEL] WARNING: allocation at 0x%08X is above "
                    "%u MB mapped; it aliases 0x%08X\n",
                    base_hint,
                    (unsigned)((g_xbox_map_size ? g_xbox_map_size
                                                : g_xbox_total_ram)
                               / (1024 * 1024)),
                    (uint32_t)(base_hint % (g_xbox_map_size
                                            ? g_xbox_map_size
                                            : g_xbox_total_ram)));
        fflush(stderr);
    }

    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {
        /* MEM_COMMIT only, on an already-reserved region.
         * The memory is already committed by our bump allocator.
         * Don't change the base address - just return success. */
        if (KERNEL_LOG_ON()) {
            fprintf(stderr, "  [KERNEL] → MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* Allocate from Xbox heap (MEM_RESERVE or MEM_RESERVE|MEM_COMMIT).
     *
     * A pure MEM_RESERVE costs no RAM on real hardware -- it takes address
     * space out of a 4 GB range, not pages out of the 64 MB of memory -- so
     * titles reserve far more than the console physically has and commit a
     * fraction of it. Our heap is a bump allocator that commits everything it
     * hands out, so a large reserve asks for RAM that does not exist.
     *
     * Half-Life 2's XBE header sets PeHeapReserve to 128 MB, and its CRT
     * reserves exactly that during RtlCreateHeap. Failing it returned
     * STATUS_NO_MEMORY, RtlCreateHeap returned 0, and CRT init aborted before
     * main -- on a console with 64 MB, asking for 128 MB is normal, not an
     * error.
     *
     * So a reserve that does not fit is clamped to what the heap can actually
     * back, and the caller is told the real size through the IN/OUT RegionSize
     * parameter, which is where the API already reports the rounded figure.
     *
     * ponytail: the honest fix is a reserve that costs nothing and a commit
     * that backs pages on demand, which needs the allocator to separate the
     * two. This clamp is enough for a title that reserves generously and
     * commits little, and it fails loudly and later rather than silently and
     * at startup if one does not. */
    /* Released ranges first (Update 505): at the hint if it lies inside
     * one, else first fit -- else the heap as before. */
    uint32_t xbox_va = 0, from_vm = 0;
    if (vm_release_enabled())
        from_vm = xbox_va = vm_take(size, base_hint);
    /* MEM_TOP_DOWN (0x100000): from the top of the heap range, as the
     * console places it -- keeps the space after a growing region free. */
    if (!xbox_va && (alloc_type & 0x100000u) && vm_release_enabled())
        xbox_va = xbox_HeapAllocTopDown(size, 4096);
    if (!xbox_va)
        xbox_va = xbox_HeapAlloc(size, 4096);
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        /* A pure reservation too big for the heap. Take it from the mapped
         * space above RAM, where it costs no heap and the pages are distinct.
         *
         * Clamping instead -- handing back a fraction of what was asked for --
         * is what broke Half-Life 2. It reserves 128 MB and then 200 MB, got
         * 32 MB and 12.5 MB, and then sub-allocated across the range it
         * believed it owned. That walks past the top of RAM, where the mirrors
         * alias low memory, so its containers quietly shared storage with the
         * live heap. Granting the full range is both more honest and less
         * damaging. Returns 0 unless the title asked for a mapping larger than
         * RAM, so nothing changes for titles that did not. */
        xbox_va = xbox_ReserveAlloc(size, 4096);
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u"
                            " granted at 0x%08X above RAM\n", size, xbox_va);
            fflush(stderr);
        }
    }
    if (!xbox_va && (alloc_type & 0x2000) && !(alloc_type & 0x1000)) {
        uint32_t want = size;
        while (want > 0x10000 && !xbox_va) {
            want /= 2;
            xbox_va = xbox_HeapAlloc(want, 4096);
        }
        if (xbox_va) {
            fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: reserve of %u "
                            "clamped to %u (heap cannot back the full range)\n",
                    size, want);
            fflush(stderr);
            size = want;
        }
    }
    if (!xbox_va) {
        g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
        return;
    }

    {
        static volatile LONG logged;
        if (getenv("RECOMP_NTVM_TRACE") && InterlockedIncrement(&logged) <= 400)
            fprintf(stderr, "[NTVM] alloc hint=0x%08X size=%u type=0x%X -> 0x%08X ret=0x%08X\n",
                    base_hint, size, alloc_type, xbox_va, (uint32_t)g_icall_source);
    }
    if (!from_vm && vm_release_enabled())
        vm_record(xbox_va, size);
    /* Write back the allocated address and actual size */
    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0; /* STATUS_SUCCESS */
}

/* ── NtFreeVirtualMemory (ordinal 199) ────────────────────
 * NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize,
 *     ULONG FreeType)
 */
/* Guest virtual-memory bookkeeping for NtAllocate/NtFree/NtQueryVirtualMemory
 * (docs Update 505).
 *
 * Every reservation bridge_NtAllocateVirtualMemory hands out is recorded as a
 * region. MEM_RELEASE of a region's base turns it into a FREE range that
 * NtQueryVirtualMemory reports as MEM_FREE and that only later reservations
 * reuse -- at their hint address if it lies inside, else first fit. It never
 * goes back to the general heap (xbox_HeapAlloc), which also serves kernel
 * pool allocations.
 *
 * Why this shape: SCDA's sbrk-style CRT heap (0x768E0..0x76C02) probes the
 * address after its region with VirtualQuery, reserves exactly there
 * (result must equal the hint), and releases its tail again; its streaming
 * buffers (528384 bytes) are reserved and released per video/stream.
 * Before Update 503 no release ever succeeded (the bridge called the host's
 * VirtualFree on a guest VA): the heap ran out after a level plus its end
 * cutscene ("disc may be dirty"). Update 503 returned released blocks to the
 * general heap: a 64 KB tail went to a 184-byte pool allocation at once.
 * Keeping them allocated but reported as committed (505a) still hung "Neues
 * Spiel": the sbrk heap could not grow back into its own released tail.
 * Real hardware frees the range for the next reservation at that address,
 * which is what this models. */
#define VM_MAX 2048
typedef struct { uint32_t va, size; } VmRange;
static VmRange s_vm_used[VM_MAX], s_vm_free[VM_MAX];
static int s_vm_used_n, s_vm_free_n;
static SRWLOCK s_vm_lock = SRWLOCK_INIT;

static int vm_release_enabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_NTFREE_OLD") == NULL;
    return on;
}

static void vm_record(uint32_t va, uint32_t size)
{
    AcquireSRWLockExclusive(&s_vm_lock);
    if (s_vm_used_n < VM_MAX) {
        s_vm_used[s_vm_used_n].va = va;
        s_vm_used[s_vm_used_n].size = size;
        s_vm_used_n++;
    }
    ReleaseSRWLockExclusive(&s_vm_lock);
}

/* A reservation out of the released ranges: at hint if hint != 0 (the whole
 * [hint, hint+size) must be free), else first fit. 0 if none. */
static uint32_t vm_take(uint32_t size, uint32_t hint)
{
    uint32_t va = 0;
    int i;

    size = (size + 0xFFFu) & ~0xFFFu;
    AcquireSRWLockExclusive(&s_vm_lock);
    for (i = 0; i < s_vm_free_n && !va; i++) {
        VmRange r = s_vm_free[i];
        uint32_t at = hint ? hint : r.va;
        if (at < r.va || (uint64_t)at + size > (uint64_t)r.va + r.size)
            continue;
        va = at;
        /* Split: [r.va, at) and [at+size, end) stay free. */
        s_vm_free[i] = s_vm_free[--s_vm_free_n];
        if (at > r.va && s_vm_free_n < VM_MAX) {
            s_vm_free[s_vm_free_n].va = r.va;
            s_vm_free[s_vm_free_n].size = at - r.va;
            s_vm_free_n++;
        }
        if (at + size < r.va + r.size && s_vm_free_n < VM_MAX) {
            s_vm_free[s_vm_free_n].va = at + size;
            s_vm_free[s_vm_free_n].size = r.va + r.size - (at + size);
            s_vm_free_n++;
        }
    }
    if (va && s_vm_used_n < VM_MAX) {
        s_vm_used[s_vm_used_n].va = va;
        s_vm_used[s_vm_used_n].size = size;
        s_vm_used_n++;
    }
    ReleaseSRWLockExclusive(&s_vm_lock);
    if (va)   /* fresh pages read as zero */
        memset(XBOX_TO_NATIVE(va), 0, size);
    return va;
}

/* MEM_RELEASE: size of the region based at va (now free), or 0. */
static uint32_t vm_release(uint32_t va)
{
    uint32_t size = 0;
    int i;

    AcquireSRWLockExclusive(&s_vm_lock);
    for (i = 0; i < s_vm_used_n; i++) {
        if (s_vm_used[i].va != va)
            continue;
        size = (s_vm_used[i].size + 0xFFFu) & ~0xFFFu;
        s_vm_used[i] = s_vm_used[--s_vm_used_n];
        break;
    }
    if (size) {
        uint32_t lo = va, hi = va + size;
        /* Merge with touching free ranges. */
        for (i = 0; i < s_vm_free_n; ) {
            if (s_vm_free[i].va + s_vm_free[i].size == lo ||
                s_vm_free[i].va == hi) {
                if (s_vm_free[i].va < lo) lo = s_vm_free[i].va;
                if (s_vm_free[i].va + s_vm_free[i].size > hi)
                    hi = s_vm_free[i].va + s_vm_free[i].size;
                s_vm_free[i] = s_vm_free[--s_vm_free_n];
                i = 0;
                continue;
            }
            i++;
        }
        if (s_vm_free_n < VM_MAX) {
            s_vm_free[s_vm_free_n].va = lo;
            s_vm_free[s_vm_free_n].size = hi - lo;
            s_vm_free_n++;
        }
    }
    ReleaseSRWLockExclusive(&s_vm_lock);
    return size;
}

/* NtQueryVirtualMemory: is addr inside a released range? Sets *end. */
static int vm_query_free(uint32_t addr, uint32_t *end)
{
    int i, hit = 0;
    AcquireSRWLockShared(&s_vm_lock);
    for (i = 0; i < s_vm_free_n; i++) {
        if (addr >= s_vm_free[i].va && addr < s_vm_free[i].va + s_vm_free[i].size) {
            *end = s_vm_free[i].va + s_vm_free[i].size;
            hit = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&s_vm_lock);
    return hit;
}

static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t free_type = STACK_ARG(2);
    uint32_t va = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;
    uint32_t size, req = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;

    /* This used to pass the guest's PVOID* to the host's NtFreeVirtualMemory,
     * which read the 32-bit guest VA as a 64-bit host pointer: every call
     * failed (err 487) and nothing was ever released. */
    if (!va) {
        g_eax = 0xC000000Du; /* STATUS_INVALID_PARAMETER */
        return;
    }
    /* MEM_DECOMMIT keeps its long-standing result (pages stay committed).
     * RECOMP_NTFREE_OLD=1: no release either (A/B). MEM_RELEASE takes size
     * 0 (XAPI's VirtualFree enforces that). */
    if (!vm_release_enabled() || !(free_type & 0x8000) || req) {
        g_eax = 0xC0000001u; /* STATUS_UNSUCCESSFUL */
        return;
    }
    size = vm_release(va);
    {
        static volatile LONG logged;
        if (getenv("RECOMP_NTVM_TRACE") && InterlockedIncrement(&logged) <= 400)
            fprintf(stderr, "[NTVM] free va=0x%08X type=0x%X size=%u src=0x%08X\n",
                    va, free_type, size, (uint32_t)g_icall_source);
    }
    if (!size) {
        g_eax = 0xC0000001u;
        return;
    }
    if (size_ptr)
        BRIDGE_MEM32(size_ptr) = size;
    g_eax = 0;
}

/* NtQueryVirtualMemory -- ordinal 217, 2 args = 8 bytes.
 *
 * This has to be a guest-side answer, and it has to exist at all: without a
 * bridge entry the thunk is left unbridged, and a title's CRT heap creation
 * calls this to probe its heap region (RtlCreateHeap: `call
 * NtQueryVirtualMemory; test eax,eax; jl fail; cmp mbi.BaseAddress,
 * requested; jne fail; cmp mbi.State, MEM_FREE; je fail`, returning 0 on any
 * of those) -- on Half-Life 2 an unbridged call here let a null heap
 * propagate silently through the rest of CRT init.
 *
 * Not routed at all before this fix: bridge_for_ordinal(217) fell through
 * to the default NULL case, so kernel_thunk_dispatch's "no bridge" path
 * unconditionally set g_eax = 0 (STATUS_SUCCESS) without ever calling
 * xbox_NtQueryVirtualMemory. A title using this to poll "is there still
 * room in my memory pool" (SCDA's sub_00371C2D, reached through
 * sub_00076849's allocation loop) saw permanent, unconditional success
 * and never observed real exhaustion, so the retry loop around it spun
 * forever -- this is the root cause of the v0.6.0-merge regression hang
 * investigated across this session's later entries (see
 * docs/SCDA_WORKLOG.md, 2026-09-01).
 *
 * Real Xbox signature (confirmed against Cxbx-Reloaded's own
 * xboxkrnl.NtQueryVirtualMemory, which every other Xbox emulation project
 * had to reverse the same way): only TWO parameters,
 *   NtQueryVirtualMemory(PVOID BaseAddress, PMEMORY_BASIC_INFORMATION Buffer)
 * matching SCDA's own call site (sub_00371C2D pushes exactly 2 stack
 * dwords) -- a first attempt using the generic 4-argument Win32 NT shape
 * crashed reading garbage stack slots the caller never populated.
 *
 * BaseAddress IS passed by value, Buffer by reference -- a false start
 * mid-session (documented in docs/SCDA_WORKLOG.md, 2026-09-01/02)
 * mis-read sub_00371C2D's "push [esp+8]" TWICE as pushing the exact same
 * value twice: esp moves by -4 between the two PUSH32s, so they actually
 * read two DIFFERENT source slots -- the caller's stack has [esp+4]=the
 * raw BaseAddress value, [esp+8]=the address of its local BaseAddress/
 * Buffer variable (sub_00076849 pushes `lea eax,[ebp-0x2c]` then
 * `push [ebp-0x2c]` right before the call). Working through the actual
 * push order confirms STACK_ARG(0) is the plain value and STACK_ARG(1)
 * is the buffer pointer, exactly like the very first pass of this fix
 * had it -- treating STACK_ARG(0) as a pointer to dereference (the
 * false start) read the wrong memory and froze the pool-population
 * loop's probe address at whatever garbage happened to be there.
 *
 * Xbox's MEMORY_BASIC_INFORMATION is NOT the same struct as Win32's:
 * the FIELD ORDER differs (Xbox: AllocationBase, AllocationProtect,
 * BaseAddress, RegionSize, State, Protect, Type -- Win32 puts BaseAddress
 * first, AllocationBase second). Raw-copying the host VirtualQuery()
 * result into the guest buffer -- what an earlier pass of this fix did
 * via xbox_NtQueryVirtualMemory's generic implementation -- hands the
 * game every field one slot off from where it expects it, so whatever
 * check it does against "State" or "RegionSize" reads a different field
 * entirely and never correctly observes pool exhaustion. Build the
 * Xbox-ordered struct by hand instead of delegating to the Win32-shaped
 * helper. */
static void bridge_NtQueryVirtualMemory(void)
{
    uint32_t base_address = STACK_ARG(0);  /* PVOID BaseAddress -- by value */
    uint32_t info_va      = STACK_ARG(1);  /* PMEMORY_BASIC_INFORMATION Buffer */

    NTSTATUS status = STATUS_SUCCESS;

    /* Our Xbox RAM mapping is one flat MEM_COMMIT VirtualAlloc/MapViewOfFileEx
     * view [0, g_xbox_total_ram) -- host VirtualQuery() on it just reports
     * that whole 64MB as a single committed region, none of the
     * FREE/RESERVED sub-region boundaries real Xbox hardware's memory
     * manager has already carved out by this point (kernel structures,
     * XBE image, thread stacks, ...). SCDA's memory-pool loop
     * (sub_00076849/sub_00371C2D) walks VirtualQuery results forward
     * hoping to eventually land on a MEM_FREE region to claim -- against
     * a raw VirtualQuery() of our flat mapping that never happens, so
     * the loop walks straight past the end of our mapping into unrelated
     * host memory (wildly inconsistent results, an infinite
     * back-and-forth between g_xbox_total_ram and an unrelated lower
     * address was observed) instead of terminating around 15-16MB in
     * the way real hardware's loop does (confirmed via xemu tracing
     * against the real disc, see docs/SCDA_WORKLOG.md, 2026-09-01).
     *
     * Rather than trying to reverse-engineer Windows' own region
     * coalescing to approximate this, synthesize the two-region shape
     * the game's algorithm actually needs directly from our own
     * allocator bookkeeping: everything below xbox_GetHeapEnd() (our
     * bump allocator's high-water mark -- already-handed-out memory:
     * kernel data, the loaded XBE image, and every allocation made so
     * far) is MEM_COMMIT, and everything from there up to
     * g_xbox_total_ram is one MEM_FREE region the game can claim. */
    {
        uint32_t heap_end  = xbox_GetHeapEnd();
        uint32_t total_ram = (uint32_t)g_xbox_total_ram;
        uint32_t alloc_base = 0, base_out = 0, region_size = 0, state = 0, protect = 0, type = 0;

        uint32_t free_end = 0;
        if (base_address >= total_ram) {
            status = STATUS_INVALID_PARAMETER;
        } else if (vm_query_free(base_address, &free_end)) {
            /* Released by NtFreeVirtualMemory (Update 505). */
            alloc_base  = 0;
            base_out    = base_address;
            region_size = free_end - base_address;
            state       = MEM_FREE;
            protect     = PAGE_NOACCESS;
            type        = 0;
        } else if (base_address >= xbox_GetHeapTopDownLow()) {
            /* MEM_TOP_DOWN blocks (Update 505). */
            alloc_base  = 0;
            base_out    = base_address;
            region_size = total_ram - base_address;
            state       = MEM_COMMIT;
            protect     = PAGE_READWRITE;
            type        = MEM_PRIVATE;
        } else if (base_address < heap_end) {
            alloc_base  = 0;
            base_out    = base_address;
            region_size = heap_end - base_address;
            state       = MEM_COMMIT;
            protect     = PAGE_READWRITE;
            type        = MEM_PRIVATE;
        } else {
            alloc_base  = 0;   /* Windows reports 0 for MEM_FREE regions */
            base_out    = base_address;
            region_size = xbox_GetHeapTopDownLow() - base_address;
            state       = MEM_FREE;
            protect     = PAGE_NOACCESS;
            type        = 0;
        }

        if (status == STATUS_SUCCESS) {
            if (info_va) {
                /* Standard MEMORY_BASIC_INFORMATION field order (BaseAddress
                 * first, matching every Win32/XDK reference and confirmed
                 * directly against SCDA's own disassembly): sub_00076849's
                 * memory-pool-claiming loop treats info_va+0x00 as
                 * BaseAddress -- it is both the queried address (pushed as
                 * the call's first argument from this same buffer slot) and
                 * the accumulator the loop advances by adding RegionSize
                 * (info_va+0x0C) each iteration. The previous field order
                 * here (AllocationBase, AllocationProtect, BaseAddress --
                 * apparently transcribed from a source that lists
                 * AllocationBase before BaseAddress) wrote a constant 0
                 * (alloc_base) into that slot every call instead of the
                 * real queried address, so the loop's accumulator was reset
                 * to 0 and then incremented by RegionSize alone every
                 * iteration -- an address that depends only on RegionSize,
                 * not on where the walk actually was. With heap_end/total_ram
                 * from xbox_GetHeapEnd()/g_xbox_total_ram this produces a
                 * stable 2-value oscillation (addr -> heap_end-addr ->
                 * addr -> ...) that never reaches a MEM_FREE region --
                 * confirmed as the root cause of an observed indefinite
                 * spin (hundreds of millions of NtQueryVirtualMemory calls,
                 * constant guest ESP, no forward progress). See
                 * docs/SCDA_WORKLOG.md, 2026-09-04. */
                BRIDGE_MEM32(info_va + 0x00) = base_out;      /* BaseAddress */
                BRIDGE_MEM32(info_va + 0x04) = alloc_base;    /* AllocationBase */
                BRIDGE_MEM32(info_va + 0x08) = protect;       /* AllocationProtect */
                BRIDGE_MEM32(info_va + 0x0C) = region_size;   /* RegionSize */
                BRIDGE_MEM32(info_va + 0x10) = state;         /* State */
                BRIDGE_MEM32(info_va + 0x14) = protect;       /* Protect */
                BRIDGE_MEM32(info_va + 0x18) = type;          /* Type */
            }
        }
    }

    g_eax = (uint32_t)status;
}

/* ── ExAllocatePool / ExAllocatePoolWithTag (ordinals 15, 16) ─
 * Must allocate from Xbox heap so the returned pointer is an Xbox VA
 * that can be accessed via MEM32(). Native HeapAlloc returns 64-bit
 * pointers that get truncated and produce garbage Xbox VAs.
 */
/* ExQueryNonVolatileSetting(ValueIndex, Type, Value, ValueLength, ResultLength)
 *
 * Titles read region, language and AV settings from EEPROM through this very
 * early in boot. Ordinal 24 was previously routed to bridge_ExQueryPoolBlockSize,
 * so the call returned a pool size where the game expected a settings blob. */
static void bridge_ExQueryNonVolatileSetting(void)
{
    uint32_t value_index  = STACK_ARG(0);
    uint32_t type_va      = STACK_ARG(1);
    uint32_t value_va     = STACK_ARG(2);
    uint32_t value_length = STACK_ARG(3);
    uint32_t result_va    = STACK_ARG(4);

    NTSTATUS st = xbox_ExQueryNonVolatileSetting(
        value_index,
        type_va   ? (PULONG)&BRIDGE_MEM32(type_va)   : NULL,
        value_va  ? (PVOID)((uintptr_t)value_va + g_xbox_mem_offset) : NULL,
        value_length,
        result_va ? (PULONG)&BRIDGE_MEM32(result_va) : NULL);

    g_eax = (uint32_t)st;
}

/* ── Title relaunch (XLaunchNewImage of the same XBE) ─────
 *
 * On a console XLaunchNewImage writes the target path and the title's launch
 * data into a persisted page (LaunchDataPage, ordinal 164) and quick-reboots
 * (HalReturnToFirmware(2)); the kernel boots the image and hands it the page,
 * which XGetLaunchInfo then reads. Here the "image" is this executable: save
 * the page next to it, start a new instance (same command line, same
 * environment) and let this one exit. At startup the new instance puts the
 * page back into guest memory before the title runs (bridge_load_launch_page).
 * Only a relaunch of default.xbe is handled -- anything else (the dashboard)
 * still exits. */
#define BRIDGE_LAUNCH_FILE L"launch_data.bin"

void xbox_coverage_stop_if_running(void);  /* recomp_coverage.c */
void xbox_log_close_file(void);            /* kernel_thunks.c */
/* The host's part of a relaunch (xbox_SetRelaunchHostHook): runs before the
 * new instance starts, e.g. to hand over log files and locks. */
static void (*s_relaunch_host_hook)(void);

static void bridge_relaunch_same_title(void)
{
    uint32_t page = BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);
    char path[520];
    uint32_t i;
    HANDLE f;
    DWORD done = 0;

    /* A level-section change is a relaunch; write a running coverage
     * recording before this process goes away (docs Update 465). */
    xbox_coverage_stop_if_running();
    WCHAR exe[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (!page)
        return;
    for (i = 0; i < sizeof(path) - 1; i++) {
        uint8_t c = BRIDGE_MEM8(page + 8 + i);
        if (!c) break;
        path[i] = (char)c;
    }
    path[i] = 0;
    _strlwr_s(path, sizeof path);
    if (!strstr(path, "default.xbe")) {
        fprintf(stderr, "  [KERNEL] launch target '%s' is not this title -- exiting\n", path);
        return;
    }
    f = CreateFileW(BRIDGE_LAUNCH_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    WriteFile(f, XBOX_TO_NATIVE(page), 4096, &done, NULL);
    CloseHandle(f);
    if (done != 4096 || !GetModuleFileNameW(NULL, exe, MAX_PATH))
        return;
    /* Keep a copy: copying it back to launch_data.bin replays this exact
     * transition (e.g. a level change) without playing up to it (docs
     * Update 504). The new instance deletes launch_data.bin itself. */
    CopyFileW(BRIDGE_LAUNCH_FILE, L"launch_data_last.bin", FALSE);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;

    /* Window handover (docs Update 502, fb_present.c): the new instance takes
     * over our window's place once it has drawn, and until then this one keeps
     * showing its last frame, as the console's screen does across the reboot.
     * RECOMP_HANDOVER=0 restores "exit at once". */
    HANDLE handover = NULL;
    {
        extern void *xbox_FramebufferWindowHandle(void);
        const char *ho = getenv("RECOMP_HANDOVER");
        HWND w = (HWND)xbox_FramebufferWindowHandle();
        if (w && IsWindowVisible(w) && !(ho && ho[0] == '0')) {
            char name[64], hex[32];
            snprintf(name, sizeof name, "Local\\scda_recomp_handover_%lu",
                     (unsigned long)GetCurrentProcessId());
            handover = CreateEventA(NULL, TRUE, FALSE, name);
            if (handover) {
                snprintf(hex, sizeof hex, "%llX", (unsigned long long)(uintptr_t)w);
                SetEnvironmentVariableA("RECOMP_HANDOVER_HWND", hex);
                SetEnvironmentVariableA("RECOMP_HANDOVER_EVENT", name);
            }
        }
    }
    /* Release what the new instance opens at startup (log files, the
     * single-instance lock) before it starts, since this one now lives on
     * until the handover. */
    if (s_relaunch_host_hook)
        s_relaunch_host_hook();
    xbox_log_close_file();

    if (CreateProcessW(exe, GetCommandLineW(), NULL, NULL, FALSE, 0, NULL, NULL,
                       &si, &pi)) {
        fprintf(stderr, "  [KERNEL] relaunching this title (pid %lu) with the launch "
                "data page\n", (unsigned long)pi.dwProcessId);
        fflush(stderr);
        CloseHandle(pi.hThread);
        if (handover) {
            HANDLE wait[2] = { handover, pi.hProcess };
            DWORD r;
            AllowSetForegroundWindow(pi.dwProcessId);
            r = WaitForMultipleObjects(2, wait, FALSE, 60000);
            fprintf(stderr, "  [KERNEL] window handover: %s\n",
                    r == WAIT_OBJECT_0 ? "done" :
                    r == WAIT_OBJECT_0 + 1 ? "new instance exited" : "timed out");
            fflush(stderr);
        }
        CloseHandle(pi.hProcess);
    } else {
        fprintf(stderr, "  [KERNEL] relaunch failed (%lu)\n", GetLastError());
        DeleteFileW(BRIDGE_LAUNCH_FILE);
    }
    if (handover)
        CloseHandle(handover);
}

void xbox_SetRelaunchHostHook(void (*fn)(void))
{
    s_relaunch_host_hook = fn;
}

/* Startup half of the relaunch: consume launch_data.bin if present. */
static void bridge_load_launch_page(void)
{
    HANDLE f = CreateFileW(BRIDGE_LAUNCH_FILE, GENERIC_READ, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD done = 0;
    uint32_t va;

    if (f == INVALID_HANDLE_VALUE)
        return;
    va = xbox_ContiguousAlloc(4096, 4096);
    if (va && ReadFile(f, XBOX_TO_NATIVE(va), 4096, &done, NULL) && done == 4096) {
        BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = va;
        fprintf(stderr, "  [KERNEL] launch data page restored at 0x%08X (type=%u)\n",
                va, BRIDGE_MEM32(va));
        {
            /* Update 457: RECOMP_GUEST_WATCH=<hex addr>[:<hex len>] arms a
             * guest write watch in the relaunched (in-level) process only --
             * the heap address of a corrupted in-level object is stable
             * there, while the menu process reuses that memory. */
            const char *w = getenv("RECOMP_GUEST_WATCH");
            if (w && w[0]) {
                char *end;
                unsigned long a = strtoul(w, &end, 16);
                unsigned long n = (*end == ':') ? strtoul(end + 1, NULL, 16) : 4;
                xbox_DebugWatchGuestRangeEx((uint32_t)a, (uint32_t)n, 400, 200000000);
            }
        }
    } else {
        fprintf(stderr, "  [KERNEL] launch_data.bin present but not restored\n");
    }
    CloseHandle(f);
    DeleteFileW(BRIDGE_LAUNCH_FILE);
}

/* HalReturnToFirmware(Routine) - the title asking to reboot or quit.
 *
 * It never returns on hardware. Returning here would let the game run on past
 * a decision to quit, which reads as a hang rather than an exit. */
static void bridge_HalReturnToFirmware(void)
{
    uint32_t routine = STACK_ARG(0);
    uint32_t caller_return = BRIDGE_MEM32(g_esp);

    /* g_icall_source is set immediately before every icall dispatch to the
     * guest VA of the call SITE itself -- kept from our own instrumentation
     * (2026-09-03, SCDA_WORKLOG.md "continued 22") alongside upstream's
     * launch-data-page + call-chain dump below, since it doesn't depend on
     * this export being reached via the icall path at all. */
    fprintf(stderr,
            "  [KERNEL] HalReturnToFirmware: routine=%u icall_source=0x%08X "
            "caller_return=0x%08X esp=0x%08X\n",
            routine, (uint32_t)g_icall_source, caller_return, g_esp);

    /* Routine 2 is a quick reboot, which on Xbox is how a title hands off to
     * another image: XLaunchNewImage fills the launch data page and reboots.
     * So "the title is exiting" and "the title is launching something" look
     * identical here, and the launch page is what tells them apart. */
    {
        uint32_t page = BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);

        if (page) {
            char path[64];
            uint32_t i;

            for (i = 0; i < sizeof(path) - 1; i++) {
                uint8_t c = BRIDGE_MEM8(page + 8 + i);
                if (!c) break;
                path[i] = (char)c;
            }
            path[i] = 0;
            fprintf(stderr, "  [KERNEL] launch data page 0x%08X:"
                            " type=%u titleid=0x%08X path='%s'\n",
                    page, BRIDGE_MEM32(page), BRIDGE_MEM32(page + 4), path);
            /* XapiBootToDash packs its reason and two parameters into the
             * front of the launch data, so this says why the title asked to
             * leave rather than merely that it did. */
            fprintf(stderr, "  [KERNEL]   launch data:");
            for (i = 0; i < 8; i++)
                fprintf(stderr, " %08X", BRIDGE_MEM32(page + 1024 + i * 4));
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, "  [KERNEL] no launch data page set\n");
        }
    }

    /* Who asked to quit.
     *
     * A title exiting looks identical whether it finished cleanly, hit an
     * error path, or was told to reboot -- and the routine number does not say
     * which. The guest call chain does. Same GS format tools/stackwalk.py
     * reads. */
    {
        const uint8_t *mem = (const uint8_t *)g_xbox_mem_offset;
        uint32_t i;

        fprintf(stderr, "  [KERNEL] exit requested, guest esp=0x%08X:\n", g_esp);
        for (i = 0; i < 200; i++) {
            uint32_t a = g_esp + i * 4;
            if (a < 0x00010000u || a >= 0x04000000u) break;
            fprintf(stderr, "    GS %08X %08X\n", a,
                    *(const uint32_t *)(mem + a));
        }
        fflush(stderr);
    }

    fprintf(stderr, "  [KERNEL] HalReturnToFirmware: routine=%u - title is exiting\n",
            routine);
    fflush(stderr);

    /* Write the indirect-branch targets before the process goes away. This
     * path ends in ExitProcess, which does not run atexit handlers, so the
     * host's registered dump never fires -- and a title that gives up during
     * boot is exactly the one whose targets are worth having. No-op unless
     * RECOMP_ICALL_FEEDBACK is on. */
    RECOMP_ICALL_FEEDBACK_DUMP();

    /* Let a host-played FMV finish before the process goes away.
     *
     * The title is not the one presenting it, so it has no reason to wait --
     * it opens the file, carries on, and quits, which would kill the video
     * thread part-way through a five-second clip. Waiting here is what makes
     * the clip actually watchable, and it costs nothing when no video is
     * playing. Bounded, so a stuck player cannot stop the process exiting. */
    {
        extern int xbox_VideoIsPlaying(void);
        int waited = 0;

        while (xbox_VideoIsPlaying() && waited < 60000) {
            Sleep(50);
            waited += 50;
        }
        if (waited)
            fprintf(stderr, "  [KERNEL] waited %dms for the video to finish\n",
                    waited);
    }

    /* XLaunchNewImage of this same title: relaunch the process with the
     * launch data page carried over (docs Update 349). SCDA reboots itself
     * with a mission in the launch data when a new game starts; treating
     * that as an exit closed the window at the mission loading screen. */
    if (routine == 2)
        bridge_relaunch_same_title();

    xbox_HalReturnToFirmware(routine);
}

static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u → Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' → Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* ── KfRaiseIrql / KfLowerIrql (ordinals 160, 161) ────── */
/* Both are __fastcall: the new IRQL is in cl, not on the stack (these read
 * STACK_ARG(0) -- the caller's next stack slot -- until docs Update 347).
 * The current IRQL is the KPCR's Irql byte at fs:[0x24]: titles read it
 * directly (DirectSound's lock helper sub_004CD790 raises only when it is
 * below DISPATCH_LEVEL), and interrupt/DPC delivery gates on it.
 *
 * The Xbox has one CPU: while a thread runs at DISPATCH_LEVEL or above, no
 * other thread and no DPC runs. Titles rely on that instead of locks --
 * DirectSound raises to DISPATCH_LEVEL to edit the voice/packet lists its
 * DPC sub_004D2B2C walks. Here guest threads are host threads and the IRQL
 * is per thread, so the DPC ran on another thread in the middle of such an
 * edit (docs Update 412: divide by a zero format byte in sub_004D45CB under
 * that DPC). g_dispatch_cs restores the exclusion: a thread holds it from
 * its raise to DISPATCH_LEVEL until it lowers below, and DPCs run only while
 * holding it (irq_run_dpcs); a DPC that cannot get it stays queued, as on
 * the console, and runs at the next delivery point -- at the latest at the
 * end of the KfLowerIrql call that releases it. ISRs do not take it; on the
 * console they preempt DISPATCH_LEVEL code. */
static CRITICAL_SECTION g_dispatch_cs;
static INIT_ONCE g_dispatch_cs_once = INIT_ONCE_STATIC_INIT;
static RECOMP_TLS int t_dispatch_held;

static BOOL CALLBACK dispatch_cs_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_dispatch_cs);
    return TRUE;
}

static void irql_cross(uint8_t old_irql, uint8_t new_irql)
{
    InitOnceExecuteOnce(&g_dispatch_cs_once, dispatch_cs_init, NULL, NULL);
    if (old_irql < 2 && new_irql >= 2 && !t_dispatch_held) {
        EnterCriticalSection(&g_dispatch_cs);
        t_dispatch_held = 1;
    } else if (old_irql >= 2 && new_irql < 2 && t_dispatch_held) {
        t_dispatch_held = 0;
        LeaveCriticalSection(&g_dispatch_cs);
    }
}

static void bridge_KfRaiseIrql(void)
{
    uint8_t old = xbox_fs_read8(0x24);
    irql_cross(old, (uint8_t)g_ecx);
    xbox_fs_write8(0x24, (uint8_t)g_ecx);
    xbox_KfRaiseIrql((UCHAR)g_ecx);
    g_eax = old;
}

static void bridge_KfLowerIrql(void)
{
    uint8_t old = xbox_fs_read8(0x24);
    xbox_fs_write8(0x24, (uint8_t)g_ecx);
    irql_cross(old, (uint8_t)g_ecx);
    xbox_KfLowerIrql((UCHAR)g_ecx);
    g_eax = 0;
}

/* ── KeRaiseIrqlToDpcLevel (ordinal 129) ─────────────────── */
static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    uint8_t old = xbox_fs_read8(0x24);
    irql_cross(old, 2);
    xbox_fs_write8(0x24, 2);
    xbox_KeRaiseIrqlToDpcLevel();
    g_eax = old;
}

/* ── RtlInitializeCriticalSection / Enter / Leave (ordinals 291, 277, 294) ─ */
static void bridge_RtlInitializeCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* ── KeQueryPerformanceCounter / Frequency (ordinals 126, 127) ─ */
static void bridge_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceFrequency();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

/* ── KeQuerySystemTime (ordinal 128) ─────────────────────── */
static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}

/* ── MmQueryStatistics (ordinal 181) ─────────────────────── */
static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

/* ── NtCreateEvent (ordinal 189) ─────────────────────────── */
static void bridge_NtCreateEvent(void)
{
    uint32_t handle_ptr = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    uint32_t event_type = STACK_ARG(2);
    uint32_t initial_state = STACK_ARG(3);

    /* Use local HANDLE to avoid 8-byte write to 4-byte Xbox memory slot.
     * On x64, HANDLE is 8 bytes but Xbox expects 4-byte handles. */
    HANDLE local_handle = NULL;
    NTSTATUS status = xbox_NtCreateEvent(
        &local_handle,
        XBOX_TO_NATIVE(obj_attr_ptr),
        event_type, initial_state);

    if (handle_ptr) {
        bridge_write_handle(handle_ptr, local_handle);
    }

    fprintf(stderr, "  [BRIDGE] NtCreateEvent: handle_ptr=0x%08X type=%u init=%u → status=0x%08X handle=0x%08X\n",
            handle_ptr, event_type, initial_state, (uint32_t)status,
            (uint32_t)(uintptr_t)local_handle);

    g_eax = (uint32_t)status;
}

/* ── Guest-memory dispatcher events (docs Update 355) ───────
 *
 * Xbox titles own their KEVENTs: D3D keeps one in its device block, clears
 * its SignalState inline, queues a "NOP 5" software interrupt and waits
 * `while (KeWaitForSingleObject(&ev, 6, 1, 0, NULL));` until its DPC calls
 * KeSetEvent (SCDA 0x4A15D8 / 0x4A342E). The Win32-handle model below treats
 * such a pointer as a HANDLE: the wait fails at once, the set is lost, and
 * D3D spun forever once the real GPU made it take that path.
 *
 * A guest KEVENT is recognised by its NT header as KeInitializeEvent leaves
 * it: Type 0 (notification) or 1 (synchronization), Size 4 (dwords) and an
 * empty wait list (Flink == Blink == &WaitListHead) -- this runtime never
 * links wait blocks into it. SignalState lives in guest memory, as on the
 * console, so inline guest code that reads or clears it stays coherent.
 * Waiters sleep on a generation counter bumped by every KeSetEvent and keep
 * delivering interrupts/DPCs between slices (the setter is usually a DPC). */
#pragma comment(lib, "Synchronization.lib")
static volatile LONG g_guest_event_gen;
/* Which path delivered the current IRQ (docs Update 617, [IRQ-LAT]):
 * 1 wait, 2 wait-slice, 3 mmio, 4 hijack, 5 safepoint, 6 kernel call. */
static RECOMP_TLS int g_irq_via;
static int xbox_irq_deliver(void);   /* with the interrupt code below */

/* Only with the emulated NV2A attached. The legacy CPU-rasterizer path
 * (RECOMP_PB_EXEC) raises no GPU interrupts, so D3D's vblank/NOP events are
 * never set there; it only ever ran because these waits failed at once and
 * D3D carried on (run356g: stuck in sub_0049A610 on the vblank KEVENT). */
extern int xbox_nv2a_xemu_active(void);

static int guest_kevent(uint32_t va)
{
    if (!xbox_nv2a_xemu_active())
        return 0;
    if (va < 0x10000u || va > 0xFFFFFFF0u)
        return 0;
    return BRIDGE_MEM8(va) <= 1 && BRIDGE_MEM8(va + 2) == 4 &&
           BRIDGE_MEM32(va + 8) == va + 8 && BRIDGE_MEM32(va + 12) == va + 8;
}

static LONG guest_kevent_set(uint32_t va)
{
    LONG prev = InterlockedExchange((volatile LONG *)XBOX_TO_NATIVE(va + 4), 1);
    InterlockedIncrement(&g_guest_event_gen);
    WakeByAddressAll((PVOID)&g_guest_event_gen);
    return prev;
}

/* Consume the signal if present: a synchronization event auto-resets. */
static int guest_kevent_try(uint32_t va)
{
    volatile LONG *state = (volatile LONG *)XBOX_TO_NATIVE(va + 4);
    if (BRIDGE_MEM8(va) == 0)
        return *state > 0;
    return InterlockedCompareExchange(state, 0, 1) > 0;
}

/* Interrupt-delivery priority for waiting guest threads (docs Update 617).
 * A guest thread that waits checks for pending interrupts every slice
 * (4 ms) and runs the ISR/DPC itself (Update 349). All guest threads share
 * one host core (xbox_PinGuestThread), so a waiter that woke for a pending
 * APU interrupt could not run before the busy render thread used up its
 * Windows quantum -- 15-28 ms late, DirectSound refilled its short stream
 * buffers too late and music streams went silent for a few ms (stutter).
 * On the console the interrupt preempts the running code at once. Waiting
 * threads therefore wait at a raised priority, so a waking one preempts the
 * runner, and drop back before they return to guest code.
 * Opt-in RECOMP_IRQ_WAIT_BOOST=1: user test (Update 619) heard more music stutter
 * with it -- every waiter preempting the render thread every slice on the one
 * guest core starves the title's own threads. Off by default. */
#define WAIT_BOOST_NONE (-1000)
static int wait_boost_begin(void)
{
    static int on = -1;
    int prio;
    if (on < 0) {
        const char *e = getenv("RECOMP_IRQ_WAIT_BOOST");
        on = e && *e && *e != '0';   /* opt-in: made stutter worse (Update 619) */
    }
    if (!on)
        return WAIT_BOOST_NONE;
    prio = GetThreadPriority(GetCurrentThread());
    if (prio == THREAD_PRIORITY_ERROR_RETURN || prio >= THREAD_PRIORITY_HIGHEST)
        return WAIT_BOOST_NONE;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    return prio;
}

static void wait_boost_end(int prio)
{
    if (prio != WAIT_BOOST_NONE)
        SetThreadPriority(GetCurrentThread(), prio);
}

static NTSTATUS guest_kevent_wait_slices(uint32_t va, uint32_t timeout_va);

static NTSTATUS guest_kevent_wait(uint32_t va, uint32_t timeout_va)
{
    int prio;
    NTSTATUS st;
    if (guest_kevent_try(va))
        return STATUS_SUCCESS;
    prio = wait_boost_begin();
    st = guest_kevent_wait_slices(va, timeout_va);
    wait_boost_end(prio);
    return st;
}

static NTSTATUS guest_kevent_wait_slices(uint32_t va, uint32_t timeout_va)
{
    ULONGLONG deadline = 0;   /* 0 = infinite */
    if (timeout_va) {
        LONGLONG t = (LONGLONG)((uint64_t)BRIDGE_MEM32(timeout_va) |
                                ((uint64_t)BRIDGE_MEM32(timeout_va + 4) << 32));
        LONGLONG ms;
        if (t < 0) {
            ms = -t / 10000;
        } else {
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            ms = (t - (LONGLONG)(((uint64_t)ft.dwHighDateTime << 32) |
                                 ft.dwLowDateTime)) / 10000;
        }
        if (t == 0 || ms <= 0)
            return guest_kevent_try(va) ? STATUS_SUCCESS : STATUS_TIMEOUT;
        deadline = GetTickCount64() + (ULONGLONG)ms;
    }
    for (;;) {
        LONG gen = g_guest_event_gen;
        DWORD slice = 4;
        if (guest_kevent_try(va))
            return STATUS_SUCCESS;
        if (deadline) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                return STATUS_TIMEOUT;
            if (deadline - now < slice)
                slice = (DWORD)(deadline - now);
        }
        WaitOnAddress((volatile VOID *)&g_guest_event_gen, &gen, sizeof(gen),
                      slice);
        g_irq_via = 1;
        xbox_irq_deliver();
    }
}

/* ── KeSetEvent (ordinal 145) ────────────────────────────── */
static void bridge_KeSetEvent(void)
{
    uint32_t event_ptr = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);

    if (guest_kevent(event_ptr)) {
        g_eax = (uint32_t)guest_kevent_set(event_ptr);
        return;
    }
    g_eax = (uint32_t)xbox_KeSetEvent(XBOX_TO_NATIVE(event_ptr), increment, (BOOLEAN)wait);
}

/* ── Interrupt-aware waits (docs Update 349) ─────────────
 *
 * Device interrupts and DPCs are delivered at the end of guest kernel calls
 * (xbox_irq_deliver). When every guest thread is blocked in a wait, nobody
 * makes a kernel call, so a pending APU interrupt or DirectSound DPC never
 * runs -- and what the threads wait for (a stream packet completing, set by
 * that DPC) never happens. SCDA froze exactly so after skipping the mission
 * intro video. On hardware the interrupt reaches the idle CPU regardless.
 * So a blocking wait waits in slices and delivers pending work in between,
 * on the waiting thread, which is inside the kernel at that point. The wait
 * still ends only when the object is signalled or the caller's own timeout
 * expires. Zero and absolute timeouts pass through unchanged. */
typedef NTSTATUS (*irq_wait_fn)(void *ctx, PLARGE_INTEGER timeout);
static int irq_work_possible(void);   /* with the interrupt code below */
static int xbox_irq_deliver(void);

static NTSTATUS irq_aware_wait_slices(irq_wait_fn fn, void *ctx,
                                      PLARGE_INTEGER timeout);

static NTSTATUS irq_aware_wait(irq_wait_fn fn, void *ctx, PLARGE_INTEGER timeout)
{
    int prio;
    NTSTATUS st;

    if (!irq_work_possible() || (timeout && timeout->QuadPart >= 0))
        return fn(ctx, timeout);
    prio = wait_boost_begin();   /* see guest_kevent_wait */
    st = irq_aware_wait_slices(fn, ctx, timeout);
    wait_boost_end(prio);
    return st;
}

static NTSTATUS irq_aware_wait_slices(irq_wait_fn fn, void *ctx,
                                      PLARGE_INTEGER timeout)
{
    LARGE_INTEGER freq, start, now, slice;
    LONGLONG total_100ns;

    total_100ns = timeout ? -timeout->QuadPart : -1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (;;) {
        LONGLONG s = 40000;  /* 4 ms */
        NTSTATUS st;
        if (total_100ns >= 0) {
            LONGLONG elapsed, left;
            QueryPerformanceCounter(&now);
            elapsed = (now.QuadPart - start.QuadPart) * 10000000LL / freq.QuadPart;
            left = total_100ns - elapsed;
            if (left <= 0)
                return STATUS_TIMEOUT;
            if (left < s)
                s = left;
        }
        slice.QuadPart = -s;
        st = fn(ctx, &slice);
        if (st != STATUS_TIMEOUT)
            return st;
        g_irq_via = 2;
        xbox_irq_deliver();
    }
}

struct irq_wait_ke_single { void *object; uint32_t reason, mode, alertable; };
static NTSTATUS irq_wait_ke_single_fn(void *c, PLARGE_INTEGER t)
{
    struct irq_wait_ke_single *w = c;
    return xbox_KeWaitForSingleObject(w->object, w->reason, w->mode,
                                      (BOOLEAN)w->alertable, t);
}

/* ── KeWaitForSingleObject (ordinal 159) ─────────────────── */
static void bridge_KeWaitForSingleObject(void)
{
    struct irq_wait_ke_single w;
    if (guest_kevent(STACK_ARG(0))) {
        /* RECOMP_KEVENT_WAIT_PROFILE=1: wall time spent in guest-event waits
         * per (guest return address, event), every 5 s -- which guest wait
         * holds the game thread back (docs Update 650). */
        static int prof = -1;
        if (prof < 0)
            prof = getenv("RECOMP_KEVENT_WAIT_PROFILE") != NULL;
        if (prof) {
            static struct { uint32_t ret, ev; DWORD tid; double ms; unsigned n; } tab[16];
            static ULONGLONG t0;
            static SRWLOCK lock = SRWLOCK_INIT;
            uint32_t ret = g_icall_source, ev = STACK_ARG(0);
            LARGE_INTEGER a, b, f;
            double ms;
            int k;
            QueryPerformanceCounter(&a);
            g_eax = (uint32_t)guest_kevent_wait(ev, STACK_ARG(4));
            QueryPerformanceCounter(&b);
            QueryPerformanceFrequency(&f);
            ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
            AcquireSRWLockExclusive(&lock);
            for (k = 0; k < 16 && tab[k].n && (tab[k].ret != ret || tab[k].ev != ev); k++)
                ;
            if (k < 16) {
                tab[k].ret = ret;
                tab[k].ev = ev;
                tab[k].tid = GetCurrentThreadId();
                tab[k].ms += ms;
                tab[k].n++;
            }
            if (!t0)
                t0 = GetTickCount64();
            if (GetTickCount64() - t0 >= 5000) {
                for (k = 0; k < 16 && tab[k].n; k++)
                    fprintf(stderr, "[KEVENT-WAIT] 5 s: site 0x%08X event 0x%08X tid %lu: "
                            "%u waits, %.0f ms\n", tab[k].ret, tab[k].ev,
                            (unsigned long)tab[k].tid, tab[k].n, tab[k].ms);
                memset(tab, 0, sizeof tab);
                t0 = GetTickCount64();
            }
            ReleaseSRWLockExclusive(&lock);
            return;
        }
        g_eax = (uint32_t)guest_kevent_wait(STACK_ARG(0), STACK_ARG(4));
        return;
    }
    w.object = XBOX_TO_NATIVE(STACK_ARG(0));
    w.reason = STACK_ARG(1);
    w.mode = STACK_ARG(2);
    w.alertable = STACK_ARG(3);

    g_eax = (uint32_t)irq_aware_wait(irq_wait_ke_single_fn, &w,
                                     XBOX_TO_NATIVE(STACK_ARG(4)));
}

static HANDLE bridge_resolve_handle(uint32_t token);

struct irq_wait_nt_single { HANDLE handle; uint32_t mode, alertable, ex; };
static NTSTATUS irq_wait_nt_single_fn(void *c, PLARGE_INTEGER t)
{
    struct irq_wait_nt_single *w = c;
    if (w->ex)
        return xbox_NtWaitForSingleObjectEx(w->handle, (KPROCESSOR_MODE)w->mode,
                                            (BOOLEAN)w->alertable, t);
    return xbox_NtWaitForSingleObject(w->handle, (BOOLEAN)w->alertable, t);
}

struct irq_wait_nt_multi { uint32_t count; HANDLE *handles; uint32_t type, alertable; };
static NTSTATUS irq_wait_nt_multi_fn(void *c, PLARGE_INTEGER t)
{
    struct irq_wait_nt_multi *w = c;
    return xbox_NtWaitForMultipleObjectsEx(w->count, w->handles, w->type,
                                           (BOOLEAN)w->alertable, t);
}

struct irq_wait_delay { uint32_t mode, alertable; };
static NTSTATUS irq_wait_delay_fn(void *c, PLARGE_INTEGER t)
{
    struct irq_wait_delay *w = c;
    NTSTATUS st = xbox_KeDelayExecutionThread((KPROCESSOR_MODE)w->mode,
                                              (BOOLEAN)w->alertable, t);
    /* A finished slice of a delay is a timeout for irq_aware_wait. */
    return st == STATUS_SUCCESS ? STATUS_TIMEOUT : st;
}

/* ── NtWaitForSingleObject (ordinal 233) ─────────────────── */
/*
 * The synchronous sibling of ...Ex. Halo's synchronous ReadFile issues the read
 * and then waits on its completion event through this; unbridged it fell to the
 * "return 0" default (STATUS_SUCCESS = "already signalled"), so the read handshake
 * completed before the data arrived and the UI-map precache never made progress.
 */
static void bridge_NtWaitForSingleObject(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t alertable   = STACK_ARG(1);
    uint32_t timeout_ptr = STACK_ARG(2);

    struct irq_wait_nt_single w;
    w.handle = handle;
    w.mode = 0;
    w.alertable = alertable;
    w.ex = 0;
    g_eax = (uint32_t)irq_aware_wait(irq_wait_nt_single_fn, &w,
                                     XBOX_TO_NATIVE(timeout_ptr));
}

/* ── NtClearEvent (ordinal 186) ──────────────────────────── */
/* Resets an event to non-signalled. Halo clears the read-completion event
 * before each async map read; a no-op here left the event stuck signalled. */
static void bridge_NtClearEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    g_eax = (uint32_t)xbox_NtClearEvent(handle);
}

/* ── NtSetEvent (ordinal 225) ────────────────────────────── */
/* Signals an event and optionally returns its previous state. Unbridged it
 * no-op'd, so a producer's "work ready" signal never landed -- Halo's map-copy
 * worker thread then slept forever in WaitForSingleObject on the decompress
 * context's go-event and only the first 14 KB of the map ever loaded. */
static void bridge_NtSetEvent(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t prev   = STACK_ARG(1);
    g_eax = (uint32_t)xbox_NtSetEvent(handle, XBOX_TO_NATIVE(prev));
}

/* ── NtPulseEvent (ordinal 205) ──────────────────────────── */
/* Signal-then-reset: releases threads currently waiting, then leaves the event
 * non-signalled. Same unbridged-no-op hazard as NtSetEvent in the map-load
 * handoff chain. PulseEvent carries the (deprecated, lossy) Xbox semantics
 * faithfully -- a waiter not yet blocked misses it, exactly as on hardware. */
static void bridge_NtPulseEvent(void)
{
    HANDLE handle = bridge_resolve_handle(STACK_ARG(0));
    if (handle) PulseEvent(handle);
    g_eax = 0;
}

/* ── NtWaitForSingleObjectEx (ordinal 234) ───────────────── */
/*
 * Unbridged, this fell through to the "no bridge, returning 0" default -- and 0
 * is STATUS_SUCCESS, so every wait returned instantly as though the object were
 * already signalled. Halo's main loop then spun: 91 million calls in 100
 * seconds, no blocking, no progress. A wait that always succeeds is worse than
 * one that always fails, because it looks like the game is running.
 */
static HANDLE bridge_resolve_handle(uint32_t token);

/* RECOMP_WAIT_TRACE=1: per-call SCDA-DIAG234M/N wait and read traces. */
static int bridge_wait_trace_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_WAIT_TRACE") != NULL;
    return on;
}

static void bridge_NtWaitForSingleObjectEx(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t wait_mode   = STACK_ARG(1);
    uint32_t alertable   = STACK_ARG(2);
    uint32_t timeout_ptr = STACK_ARG(3);

    static int logged = 0;
    if (logged++ < 20) {
        fprintf(stderr, "  [KERNEL] NtWaitForSingleObjectEx: token=0x%08X "
                "handle=%p timeout=%s\n",
                STACK_ARG(0), handle, timeout_ptr ? "finite" : "INFINITE");
        fflush(stderr);
    }

    /* Per-wait trace from the boot hunt (Update 234). Unconditional, it
     * wrote two flushed lines per wait -- ~1,000 WriteFile calls a second
     * in a level, and the sampler found the game thread in NtWriteFile 68 %
     * of the time (docs Update 475). Now only with RECOMP_WAIT_TRACE=1. */
    if (bridge_wait_trace_on()) {
        fprintf(stderr, "SCDA-DIAG234M tid=%lu enter token=0x%08X handle=%p tick=%lu esp=0x%08X ret=0x%08X\n",
                (unsigned long)GetCurrentThreadId(), STACK_ARG(0), handle,
                (unsigned long)GetTickCount(), g_esp, g_esp ? BRIDGE_MEM32(g_esp) : 0);
        fflush(stderr);
    }

    {
        struct irq_wait_nt_single w;
        w.handle = handle;
        w.mode = wait_mode;
        w.alertable = alertable;
        w.ex = 1;
        g_eax = (uint32_t)irq_aware_wait(irq_wait_nt_single_fn, &w,
                                         XBOX_TO_NATIVE(timeout_ptr));
    }

    if (bridge_wait_trace_on()) {
        fprintf(stderr, "SCDA-DIAG234M tid=%lu leave token=0x%08X status=0x%08X tick=%lu\n",
                (unsigned long)GetCurrentThreadId(), STACK_ARG(0), g_eax,
                (unsigned long)GetTickCount());
        fflush(stderr);
    }
}

/* ── MmQueryAddressProtect (ordinal 179) ─────────────────── */
/* NtWaitForMultipleObjectsEx (ordinal 235, 6 args = 24 bytes; see the
 * corrected signature and its history further down, right above the
 * function body)
 *
 * xbox_NtWaitForMultipleObjectsEx has been in kernel_sync.c all along; only
 * the bridge wrapper was missing, so the thunk fell through to the fallback
 * and returned 0 -- STATUS_SUCCESS, meaning 'object 0 is signalled'. A wait
 * that always reports signalled turns a blocking wait into a busy loop, which
 * is exactly what Half-Life 2 does after spawning its first worker: the main
 * thread spins in a CUtlLinkedList walk making no indirect calls at all.
 *
 * Handles is a guest array of tokens, so each has to be resolved
 * individually -- the array cannot just be pointed at. Bounded because a
 * bogus Count would otherwise read arbitrary guest memory onto the stack;
 * MAXIMUM_WAIT_OBJECTS is the real kernel's own limit.
 */
/* 2026-09-11: this is a 6-argument call, not 5 -- proven via capstone
 * against the real XBE bytes at sub_00052C5C's call site (0x00052CB3),
 * which pushes six dwords (edi, [ebp+0x18], 1, ebx, eax, esi) before
 * `call [0x4F52F0]` (the ordinal-235 import thunk). With only 5 args
 * declared here, `stdcall_args_for_ordinal` popped 20 of the 24 bytes
 * the caller actually pushed, leaking 4 bytes of stack on every call.
 * That 4-byte leak is exactly what corrupted the callee-saved
 * ebx/esi/edi that sub_00052C5C's own epilogue restores from the stack
 * (POP32 reading one slot short/shifted) -- traced live via a chain of
 * tightly-gated hooks from the Bink worker thread's ebx-sentinel
 * corruption in sub_004BBD90 back through sub_00052D40 ->
 * sub_00052C5C to this exact call. See PROJECT_STATUS.md and
 * docs/SCDA_WORKLOG.md.
 *
 * 2026-09-11 (continued, upstream comparison): the FIRST fix above
 * guessed the sixth argument was WaitBlockArray sitting between
 * Alertable and Timeout, by analogy with the kernel-internal
 * KeWaitForMultipleObjects. That analogy was wrong -- the real,
 * documented ntoskrnl NTAPI prototype (matching upstream
 * sp00nznet/xboxrecomp commit 87fb4bcf8d, independently arrived at
 * from Half-Life 2's own call site) is:
 *
 *   NTSTATUS NtWaitForMultipleObjectsEx(ULONG Count, HANDLE *Handles,
 *       WAIT_TYPE WaitType, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
 *       PLARGE_INTEGER Timeout);
 *
 * WaitMode sits between WaitType and Alertable, not between Alertable
 * and Timeout. Both orderings happen to fix the stack-corruption bug
 * (24 bytes popped either way), but the first version read Alertable
 * and the extra slot swapped relative to the real ABI. We don't
 * implement real kernel-mode-vs-user-mode wait bookkeeping, so
 * WaitMode is read (to keep the stack accounting honest) and
 * otherwise ignored. */
static void bridge_NtWaitForMultipleObjectsEx(void)
{
    uint32_t count       = STACK_ARG(0);
    uint32_t handles_va  = STACK_ARG(1);
    uint32_t wait_type   = STACK_ARG(2);
    uint32_t wait_mode   = STACK_ARG(3);  /* unused: no user/kernel-mode wait distinction */
    uint32_t alertable   = STACK_ARG(4);
    uint32_t timeout_ptr = STACK_ARG(5);
    HANDLE   handles[MAXIMUM_WAIT_OBJECTS];
    uint32_t i;
    (void)wait_mode;

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || !handles_va) {
        g_eax = 0xC000000Du;             /* STATUS_INVALID_PARAMETER */
        return;
    }
    for (i = 0; i < count; i++)
        handles[i] = bridge_resolve_handle(BRIDGE_MEM32(handles_va + i * 4));

    {
        static int logged;
        if (logged++ < 20) {
            fprintf(stderr, "  [KERNEL] NtWaitForMultipleObjectsEx: count=%u type=%u timeout=%s\n",
                    count, wait_type, timeout_ptr ? "finite" : "INFINITE");
            for (i = 0; i < count; i++)
                fprintf(stderr, "      [%u] token=0x%08X host=%p\n", i,
                        BRIDGE_MEM32(handles_va + i * 4), handles[i]);
            fflush(stderr);
        }
    }

    {
        struct irq_wait_nt_multi w;
        w.count = count;
        w.handles = handles;
        w.type = wait_type;
        w.alertable = alertable;
        g_eax = (uint32_t)irq_aware_wait(irq_wait_nt_multi_fn, &w,
                                         XBOX_TO_NATIVE(timeout_ptr));
    }
}

/*
 * Takes an Xbox VA, so the native pointer has to be formed before the query --
 * an unbridged 0 return reads as PAGE_NOACCESS. Halo walks all 22 MB of its
 * physical memory map asserting every page is PAGE_READWRITE
 * (physical_memory_map.c:77), so a zero here stops startup on the first page.
 */
static void bridge_MmQueryAddressProtect(void)
{
    uint32_t address = STACK_ARG(0);

    g_eax = address ? (uint32_t)xbox_MmQueryAddressProtect(XBOX_TO_NATIVE(address))
                    : 0;
}

/* ── NtUserIoApcDispatcher (ordinal 232) ─────────────────── */
/*
 * The kernel side of XAPI's ReadFileEx/WriteFileEx. XAPI passes *this* as the
 * ApcRoutine to NtReadFile and puts the title's completion routine in
 * ApcContext, so the dispatcher's only job is to call it with Win32 argument
 * shape:
 *
 *   VOID CALLBACK Completion(DWORD dwErrorCode,
 *                            DWORD dwNumberOfBytesTransfered,
 *                            LPOVERLAPPED lpOverlapped)   // __stdcall, ret 12
 *
 * lpOverlapped is the IO_STATUS_BLOCK pointer: an NT OVERLAPPED begins with
 * Internal/InternalHigh, which is exactly a IO_STATUS_BLOCK, so the title's
 * OVERLAPPED and the block it handed to NtReadFile are the same address.
 * Halo's cache_files_windows completion relies on that -- it reads its own
 * field at lpOverlapped+0x10 and sets the flag the setup loop polls.
 */
static void bridge_NtUserIoApcDispatcher(void)
{
    uint32_t apc_context = STACK_ARG(0);
    uint32_t iostatus    = STACK_ARG(1);
    uint32_t status      = iostatus ? BRIDGE_MEM32(iostatus) : 0;
    uint32_t information = iostatus ? BRIDGE_MEM32(iostatus + 4) : 0;
    recomp_func_t fn;

    fn = recomp_lookup(apc_context);
    if (!fn) fn = recomp_lookup_manual(apc_context);
    if (!fn) {
        fprintf(stderr, "  [KERNEL] NtUserIoApcDispatcher: completion routine "
                "0x%08X not in dispatch\n", apc_context);
        fflush(stderr);
        g_eax = 0;
        return;
    }

    /* __stdcall, right-to-left. The callee's `ret 12` consumes the dummy
     * return address and all three arguments, so g_esp needs no fixup here. */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = information;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = (status == 0) ? 0 : status;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
    fn();

    g_eax = 0;
}

/* ── KeDelayExecutionThread (ordinal 99) ─────────────────── */
/* Unbridged this returned instantly, turning every "sleep and retry" in the
 * title into a hot spin. Halo's cache-partition setup retries this way. */
static void bridge_KeDelayExecutionThread(void)
{
    uint32_t wait_mode    = STACK_ARG(0);
    uint32_t alertable    = STACK_ARG(1);
    uint32_t interval_ptr = STACK_ARG(2);
    struct irq_wait_delay w;
    NTSTATUS st;

    /* Sliced like the waits above (irq_aware_wait), so a thread sleeping
     * while everything else waits still serves pending interrupts/DPCs. */
    w.mode = wait_mode;
    w.alertable = alertable;
    st = irq_aware_wait(irq_wait_delay_fn, &w, XBOX_TO_NATIVE(interval_ptr));
    g_eax = (uint32_t)(st == STATUS_TIMEOUT ? STATUS_SUCCESS : st);
}

/* ── KeBugCheck (ordinal 95) / KeBugCheckEx (96) ─────────── */
/*
 * The title asking the kernel to die. Unbridged this returned 0 and execution
 * carried on into whatever the bug check was there to prevent, so the real
 * failure surfaced later somewhere unrelated. Report the code and stop
 * pretending the call succeeded.
 */
static void bridge_KeBugCheck(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheck: code=0x%08X ***\n",
            STACK_ARG(0));
    fflush(stderr);
    g_eax = 0;
}

static void bridge_KeBugCheckEx(void)
{
    fprintf(stderr, "  [KERNEL] *** KeBugCheckEx: code=0x%08X "
            "(0x%08X, 0x%08X, 0x%08X, 0x%08X) ***\n",
            STACK_ARG(0), STACK_ARG(1), STACK_ARG(2),
            STACK_ARG(3), STACK_ARG(4));
    fflush(stderr);
    g_eax = 0;
}

/* ── NtYieldExecution (ordinal 238) ──────────────────────── */
static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

/* ── MmGetPhysicalAddress (ordinal 173) ──────────────────── */
static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Xbox uses identity mapping (physical == virtual) for the lower 64MB.
     * Just return the Xbox VA as-is. Don't call xbox_MmGetPhysicalAddress
     * which would return a native pointer. */
    g_eax = addr;
}

/* ── MmSetAddressProtect (ordinal 182) ───────────────────── */
static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

/* ── AvSetDisplayMode (ordinal 3) ────────────────────────── */
static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    /* The framebuffer the display is meant to scan out, and the format it is
     * in. This is the only place the address is stated: the title never writes
     * PCRTC_START itself, so without this there is nothing that says where the
     * guest believes its picture is. */
    fprintf(stderr, "  [AV] SetDisplayMode mode=0x%08X format=0x%08X"
                    " pitch=%u fb=0x%08X\n", mode, format, pitch, fb);
    fflush(stderr);

    xbox_SetDisplayFramebuffer(fb, pitch);
    {
        /* Point the framebuffer window at whatever the title just set, and
         * start it on the first display mode -- before that there is nothing
         * to show and no pitch to interpret it with. */
        extern void xbox_FramebufferWindowSet(uint32_t, uint32_t);
        extern void xbox_FramebufferWindowStart(void);
        uint32_t fb_va = 0;

        if (xbox_ResolveGpuPhysicalRange(fb, pitch * 480u, &fb_va))
            xbox_FramebufferWindowSet(fb_va, pitch);
        xbox_FramebufferWindowStart();
    }
    /* With the emulated NV2A, leave the CRTC as the console kernel would: its
     * display size, line offset and depth (see nv2a_recomp_set_display_mode).
     * The kernel takes them from its mode tables; here they follow from the
     * framebuffer: depth from the display format, width = pitch / depth,
     * height from the standard mode of that width. */
    if (xbox_nv2a_xemu_active()) {
        extern void nv2a_recomp_set_display_mode(uint32_t, uint32_t,
                                                 uint32_t, uint32_t);
        uint32_t bpp, width, height;
        switch (format) {
        case 0x10: case 0x1C: bpp = 15; break;   /* (LIN_)X1R5G5B5 */
        case 0x11: bpp = 16; break;              /* LIN_R5G6B5 */
        default: bpp = 32; break;                /* LIN_A8R8G8B8 0x12, X8R8G8B8 0x1E */
        }
        width = pitch / (bpp == 32 ? 4u : 2u);
        height = width >= 1920 ? 1080 : width >= 1280 ? 720 : 480;
        nv2a_recomp_set_display_mode(width, height, pitch, bpp);
    }
    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    g_eax = 0;
}

/* ── PsTerminateSystemThread (ordinal 258) ───────────────
 * VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
 *
 * On real Xbox, this terminates the calling thread (never returns).
 * In our recompiled version, threads run synchronously, so we just
 * return. The caller (sub_001D1818) handles this gracefully.
 */
static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);

    fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X%s\n",
            exit_status, g_is_spawned_thread ? " (worker)" : " (main)");
    fflush(stderr);

    g_eax = exit_status;

    /*
     * This does not return on hardware. Returning was survivable while every
     * thread ran on the host's main thread, but a spawned worker that returns
     * here falls off the end of its start routine and into whatever bytes
     * follow -- Halo's input worker landed on an int 3, and the resulting
     * breakpoint took down the whole process while the main thread was still
     * inside input_initialize.
     *
     * The main thread still returns: it is the host's thread and unwinding
     * back to main() is how the process shuts down cleanly.
     */
    if (g_is_spawned_thread) {
        /* The normal exit for a worker, and therefore the one that has to
         * return the stack -- ExitThread never comes back to bridge_thread_main
         * to do it. Same reason it has to mark the fake KTHREAD done here
         * too: bridge_thread_main's own completion write is never reached
         * from this path. */
        bridge_mark_thread_done(g_thread_kthread_va, exit_status);
        xbox_FreeThreadStack(g_thread_stack_top);
        g_thread_stack_top = 0;
        ExitThread(exit_status);
    }
}

/* ── HalReadSMCTrayState (ordinal 47) ─────────────────────
 * VOID HalReadSMCTrayState(PDWORD TrayState, PDWORD TrayStateChangeCount)
 *
 * Returns DVD tray state. 0x10 = no disc, 0x14 = tray closed with disc.
 */
static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x10;  /* No disc */
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

/* ── KeInitializeDpc (ordinal 107) ────────────────────────
 * VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
 *                       PVOID DeferredContext)
 *
 * Initializes a DPC object. The Xbox KDPC structure is 32 bytes.
 * We zero it and set the routine and context pointers.
 */
static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    /* Zero the structure (32 bytes) */
    memset(XBOX_TO_NATIVE(dpc_va), 0, 32);

    /* Set Type (0x13 = DpcObject) and fields */
    BRIDGE_MEM16(dpc_va + 0) = 0x13;   /* Type */
    BRIDGE_MEM32(dpc_va + 12) = routine; /* DeferredRoutine */
    BRIDGE_MEM32(dpc_va + 16) = context; /* DeferredContext */
    g_eax = 0;
}

/* ── NV2A interrupt plumbing (ordinals 44, 98, 109) ───────
 *
 * The D3D8 library linked into a title installs an ISR for the GPU's vblank /
 * command-completion interrupt. There is no NV2A here and nothing ever raises
 * that interrupt, so these exist to let initialisation complete rather than to
 * deliver anything.
 *
 * KeConnectInterrupt reports success: reporting failure sends Halo's
 * rasterizer down an error path during preinitialize, and the goal is to get
 * past setup, not to pretend the hardware is broken.
 *
 * ponytail: nothing raises the NV2A line, so D3D8's ISR never runs. Code that
 * *waits* on it rather than polling will hang here, and the fix for that is to
 * bridge the D3D8 entry point that owns the wait, not to synthesise NV2A
 * interrupts. Lines a device does raise (the APU, IRQ 5) are delivered --
 * see "Device interrupts and DPCs" below.
 */

/* ULONG HalGetInterruptVector(ULONG BusInterruptLevel, PKIRQL Irql) */
static void bridge_HalGetInterruptVector(void)
{
    uint32_t level   = STACK_ARG(0);
    uint32_t irql_va = STACK_ARG(1);

    if (irql_va) {
        /* IRQL is conventionally the vector for device interrupts. */
        BRIDGE_MEM8(irql_va) = (uint8_t)level;
    }
    g_eax = level;
}

/* VOID KeInitializeInterrupt(PKINTERRUPT, ServiceRoutine, ServiceContext,
 *                            Vector, Irql, InterruptMode, ShareVector) */
static void bridge_KeInitializeInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t routine      = STACK_ARG(1);
    uint32_t context      = STACK_ARG(2);
    uint32_t vector       = STACK_ARG(3);

    /* Xbox KINTERRUPT is 44 bytes: ServiceRoutine, ServiceContext,
     * BusInterruptLevel, Irql, Connected, ShareVector, Mode, ... */
    memset(XBOX_TO_NATIVE(interrupt_va), 0, 44);
    BRIDGE_MEM32(interrupt_va + 0)  = routine;
    BRIDGE_MEM32(interrupt_va + 4)  = context;
    BRIDGE_MEM32(interrupt_va + 8)  = vector;
    BRIDGE_MEM32(interrupt_va + 12) = (uint8_t)STACK_ARG(4); /* Irql (KIRQL) */
    BRIDGE_MEM8(interrupt_va + 17)  = (uint8_t)STACK_ARG(6); /* ShareVector */
    BRIDGE_MEM8(interrupt_va + 18)  = (uint8_t)STACK_ARG(5); /* Mode */
    g_eax = 0;
}

/* ── Device interrupts and DPCs (docs Update 347) ────────
 *
 * The note above held while nothing needed an interrupt. DirectSound does:
 * IDirectSoundStream_Flush marks the voice "stopping" (flag 0x8000) and only
 * its ISR clears that, when the APU reports the voice idle through a front-
 * end trap (SE2FE_IDLE_VOICE). Without delivery SCDA's Bink sound stop spun in
 * GetStatus forever. Xemu shows the ISR running on the main thread in the
 * middle of RtlEnterCriticalSection.
 *
 * Model: a device raises its line (xbox_irq_set_line; the APU is IRQ 5). The
 * next kernel call made by any guest thread whose IRQL (KPCR Irql, fs:[0x24])
 * is below the interrupt's Irql runs the connected ServiceRoutine(Interrupt,
 * ServiceContext) on that thread's guest stack, at the interrupt's Irql, with
 * the volatile guest registers saved around it. A kernel-call boundary is an
 * instruction boundary, so this is a subset of where hardware could interrupt;
 * the volatile x87/XMM state is dead there by the calling convention. One
 * ISR or DPC at a time (the console has one CPU). DPCs queued by the ISR (or
 * anyone) run the same way once the thread's IRQL is below DISPATCH_LEVEL. */
#define XBOX_IRQ_LINES 16
static volatile LONG g_irq_line[XBOX_IRQ_LINES];
static volatile LONG g_irq_line_generation[XBOX_IRQ_LINES];
static struct { uint32_t kint, routine, context, irql; } g_irq_isr[XBOX_IRQ_LINES];
static volatile LONG g_irq_busy;
volatile int32_t g_recomp_irq_pending;

#define XBOX_DPC_QUEUE 32
static uint32_t g_dpc_queue[XBOX_DPC_QUEUE];
/* DPC objects the APU ISR queues (docs Update 617): DirectSound refills its
 * stream buffers there, and the APU fast path runs them too. */
static RECOMP_TLS int t_in_apu_isr;
static uint32_t g_apu_dpcs[8];
static volatile LONG g_apu_dpc_count;
static int g_dpc_count;
static volatile LONG g_dpc_pending;
static CRITICAL_SECTION g_dpc_lock;
static INIT_ONCE g_dpc_lock_once = INIT_ONCE_STATIC_INIT;

extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS double g_fp_stack[8];

static BOOL CALLBACK dpc_lock_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_dpc_lock);
    return TRUE;
}

/* APU interrupt latency (docs Update 617): from the line going high to its
 * ISR starting, by the path that delivered it. DirectSound refills stream
 * buffers from that ISR/DPC; a late one starves a music stream. */
static volatile int64_t s_apu_irq_raised_qpc;
/* g_irq_via: declared before the wait helpers (Update 617) */
static struct {
    unsigned n, over5, over10, max_via;
    double sum_ms, max_ms;
    int64_t last_report_qpc;
} s_irq_lat;

static int64_t irq_qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static double irq_qpc_ms(int64_t d)
{
    static LARGE_INTEGER f;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    return d * 1000.0 / (double)f.QuadPart;
}

/* While the APU line waits: who tried to deliver and what stopped them. */
static volatile LONG s_apu_try_busy, s_apu_try_irql, s_apu_try_calls;
static volatile int64_t s_busy_since_qpc;
static volatile uint32_t s_busy_what;   /* ISR routine or DPC routine running */
static double s_apu_busy_max_ms;
static uint32_t s_apu_busy_max_what;

static void irq_lat_note_apu(void)
{
    int64_t raised = s_apu_irq_raised_qpc, now = irq_qpc();
    double ms;
    if (!raised)
        return;
    s_apu_irq_raised_qpc = 0;
    ms = irq_qpc_ms(now - raised);
    if (ms > 10.0) {
        static unsigned logged;
        static const char *const via[] = { "?", "wait", "wait-slice", "mmio",
                                           "hijack", "safepoint", "kernel-call" };
        if (logged++ < 100 || (logged & (logged - 1)) == 0)
            fprintf(stderr, "[IRQ-LAT] APU ISR %.1f ms late, via %s: delivery "
                    "attempts %ld, blocked by a running ISR/DPC %ld (longest "
                    "%.1f ms, routine 0x%08X), by IRQL %ld\n", ms,
                    via[g_irq_via >= 0 && g_irq_via < 7 ? g_irq_via : 0],
                    s_apu_try_calls, s_apu_try_busy, s_apu_busy_max_ms,
                    s_apu_busy_max_what, s_apu_try_irql);
    }
    s_apu_try_busy = s_apu_try_irql = s_apu_try_calls = 0;
    s_apu_busy_max_ms = 0;
    s_apu_busy_max_what = 0;
    s_irq_lat.n++;
    s_irq_lat.sum_ms += ms;
    s_irq_lat.over5 += ms > 5.0;
    s_irq_lat.over10 += ms > 10.0;
    if (ms > s_irq_lat.max_ms) {
        s_irq_lat.max_ms = ms;
        s_irq_lat.max_via = (unsigned)g_irq_via;
    }
    if (!s_irq_lat.last_report_qpc)
        s_irq_lat.last_report_qpc = now;
    if (irq_qpc_ms(now - s_irq_lat.last_report_qpc) >= 10000.0) {
        static const char *const via[] = { "?", "wait", "wait-slice", "mmio",
                                           "hijack", "safepoint", "kernel-call" };
        fprintf(stderr, "[IRQ-LAT] APU 10 s: %u ISRs, delay avg %.2f max %.1f ms "
                "(via %s), >5 ms %u, >10 ms %u\n", s_irq_lat.n,
                s_irq_lat.sum_ms / s_irq_lat.n, s_irq_lat.max_ms,
                via[s_irq_lat.max_via < 7 ? s_irq_lat.max_via : 0],
                s_irq_lat.over5, s_irq_lat.over10);
        memset(&s_irq_lat, 0, sizeof(s_irq_lat));
        s_irq_lat.last_report_qpc = now;
    }
}

void xbox_irq_set_line(unsigned line, int asserted)
{
    if (line < XBOX_IRQ_LINES) {
        LONG level = asserted ? 1 : 0;
        LONG old = InterlockedExchange(&g_irq_line[line], level);
        if (old != level) {
            if (level) {
                if (line == 5 && !s_apu_irq_raised_qpc)
                    s_apu_irq_raised_qpc = irq_qpc();
                InterlockedIncrement(&g_irq_line_generation[line]);
                InterlockedExchange((volatile LONG *)&g_recomp_irq_pending, 1);
                /* Wake guest-event waiters now (docs Update 650): they
                 * deliver interrupts between 4 ms slices, so D3D's wait for
                 * the GPU fence (sub_004A1490 on device+0x1DCC, set by its
                 * ISR) otherwise noticed the NV2A interrupt up to a slice
                 * late, every frame. RECOMP_IRQ_WAKE_WAITERS=0 = old way. */
                {
                    static int wake = -1;
                    if (wake < 0) {
                        const char *e = getenv("RECOMP_IRQ_WAKE_WAITERS");
                        wake = !(e && e[0] == '0');
                    }
                    if (wake) {
                        InterlockedIncrement(&g_guest_event_gen);
                        WakeByAddressAll((PVOID)&g_guest_event_gen);
                    }
                }
            }
        }
    }
}

/* mcpx_apu_set_irq_handler target (xbox_memory_layout.c). */
void xbox_irq_apu_line(int asserted)
{
    xbox_irq_set_line(5, asserted);
}

/* Run guest code `routine(args...)` (stdcall) on this thread's guest stack at
 * the given IRQL, preserving the interrupted state. */
static uint32_t irq_run_guest(uint32_t routine, const uint32_t *args, int nargs,
                              uint8_t irql)
{
    uint32_t result;
    recomp_func_t fn = recomp_lookup(routine);
    uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_esp = g_esp;
    uint32_t s_ebx = g_ebx, s_esi = g_esi, s_edi = g_edi, s_ebp = g_ebp;
    uint32_t s_seh = g_seh_ebp, s_src = g_icall_source;
    int s_top = g_fp_top;
    double s_fp[8];
    uint8_t s_irql = xbox_fs_read8(0x24);
    int i;

    if (!fn) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "  [KERNEL] interrupt/DPC routine 0x%08X has no "
                    "recompiled function\n", routine);
        return 0;
    }
    memcpy(s_fp, g_fp_stack, sizeof(s_fp));
    xbox_fs_write8(0x24, irql);
    for (i = nargs - 1; i >= 0; i--) {
        g_esp -= 4;
        BRIDGE_MEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    BRIDGE_MEM32(g_esp) = 0;  /* return address */
    g_seh_ebp = g_esp;
    fn();
    result = g_eax;
    xbox_fs_write8(0x24, s_irql);
    memcpy(g_fp_stack, s_fp, sizeof(s_fp));
    g_fp_top = s_top;
    g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_esp = s_esp;
    g_ebx = s_ebx; g_esi = s_esi; g_edi = s_edi; g_ebp = s_ebp;
    g_seh_ebp = s_seh; g_icall_source = s_src;
    return result;
}

/* Runs the queued DPCs; 0 when another thread is at DISPATCH_LEVEL or
 * above (g_dispatch_cs, see bridge_KfRaiseIrql), the DPCs then stay queued. */
static int irq_run_dpcs(void)
{
    static unsigned dpc_runs;
    InitOnceExecuteOnce(&g_dispatch_cs_once, dispatch_cs_init, NULL, NULL);
    if (!TryEnterCriticalSection(&g_dispatch_cs))
        return 0;
    for (;;) {
        uint32_t dpc = 0;
        uint32_t args[4];
        InitOnceExecuteOnce(&g_dpc_lock_once, dpc_lock_init, NULL, NULL);
        EnterCriticalSection(&g_dpc_lock);
        if (g_dpc_count) {
            dpc = g_dpc_queue[0];
            memmove(g_dpc_queue, g_dpc_queue + 1,
                    (size_t)(--g_dpc_count) * sizeof(g_dpc_queue[0]));
            BRIDGE_MEM8(dpc + 2) = 0;  /* Inserted */
        }
        InterlockedExchange(&g_dpc_pending, g_dpc_count ? 1 : 0);
        LeaveCriticalSection(&g_dpc_lock);
        if (!dpc) {
            LeaveCriticalSection(&g_dispatch_cs);
            return 1;
        }
        args[0] = dpc;
        args[1] = BRIDGE_MEM32(dpc + 16);  /* DeferredContext */
        args[2] = BRIDGE_MEM32(dpc + 20);  /* SystemArgument1 */
        args[3] = BRIDGE_MEM32(dpc + 24);  /* SystemArgument2 */
        {
            uint32_t routine = BRIDGE_MEM32(dpc + 12);
            unsigned run = ++dpc_runs;
            int report = run <= 256 || (run & (run - 1)) == 0;
            if (report)
                fprintf(stderr,
                        "  [KERNEL] DPC %u begin object=0x%08X "
                        "routine=0x%08X context=0x%08X\n",
                        run, dpc, routine, args[1]);
            s_busy_what = routine;   /* [IRQ-LAT], Update 617 */
            irq_run_guest(routine, args, 4, 2 /* DISPATCH_LEVEL */);
            if (report)
                fprintf(stderr, "  [KERNEL] DPC %u end\n", run);
        }
    }
}

/* Is there any interrupt or DPC machinery a blocking wait must keep serving
 * (see irq_aware_wait)? */
static int irq_work_possible(void)
{
    unsigned line;
    if (g_dpc_pending)
        return 1;
    for (line = 0; line < XBOX_IRQ_LINES; line++)
        if (g_irq_isr[line].routine)
            return 1;
    return 0;
}

/* Called at the end of every guest kernel call. */
/* APU interrupt fast path (docs Update 617). Interrupt handlers are
 * serialized by g_irq_busy (one CPU). The GPU's D3D ISR (IRQ 3) can however
 * sit for 20-60 ms in emulated-GPU register accesses waiting for the GPU
 * thread -- microseconds on the console -- and every APU interrupt in that
 * time waited with it, so DirectSound refilled its short stream buffers late
 * and music streams went silent for a few ms (the stutter that grows with
 * camera movement). The APU ISR touches only DirectSound's data, so while
 * another handler holds g_irq_busy it may run on this thread anyway; its own
 * flag keeps it to one at a time. Its DPCs run with the normal delivery.
 * Opt-in since Update 621 (RECOMP_APU_IRQ_FASTPATH=1): the stutter it was
 * built for turned out to be the thread priority above. */
#define APU_IRQ_LINE 5
static volatile LONG g_irq_apu_busy;
/* What the g_irq_busy holder is doing: the line whose ISR it runs (-1 =
 * anything else, e.g. DPCs or the pass bookkeeping) and its thread. The fast
 * path only overtakes another line's ISR -- never a DPC, which may expect the
 * ISR it pairs with not to run meanwhile, and never its own thread. */
static volatile LONG g_irq_busy_isr_line = -1;
static volatile DWORD g_irq_busy_tid;

static int irq_apu_fastpath_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_IRQ_FASTPATH");
        on = e && *e && *e != '0';   /* opt-in since Update 621 */
    }
    return on;
}

/* Runs the APU ISR while its line is asserted, at most once per assertion
 * within one delivery (*delivered_gen, as the level-triggered loop in
 * xbox_irq_deliver). The caller holds g_irq_apu_busy. Returns 1 if it ran. */
static int irq_run_apu_isr(uint8_t irql, LONG *delivered_gen)
{
    LONG gen = g_irq_line_generation[APU_IRQ_LINE];
    uint32_t args[2];
    if (!g_irq_line[APU_IRQ_LINE] || !g_irq_isr[APU_IRQ_LINE].routine ||
        gen == *delivered_gen || irql >= g_irq_isr[APU_IRQ_LINE].irql)
        return 0;
    *delivered_gen = gen;
    args[0] = g_irq_isr[APU_IRQ_LINE].kint;
    args[1] = g_irq_isr[APU_IRQ_LINE].context;
    irq_lat_note_apu();
    t_in_apu_isr = 1;
    irq_run_guest(g_irq_isr[APU_IRQ_LINE].routine, args, 2,
                  (uint8_t)g_irq_isr[APU_IRQ_LINE].irql);
    t_in_apu_isr = 0;
    return 1;
}

static int irq_is_apu_dpc(uint32_t dpc)
{
    LONG n = g_apu_dpc_count;
    for (LONG i = 0; i < n && i < 8; i++)
        if (g_apu_dpcs[i] == dpc)
            return 1;
    return 0;
}

/* Runs queued DPCs that the APU ISR queued, at DISPATCH_LEVEL, if no DPC or
 * DISPATCH-level section holds g_dispatch_cs (the same exclusion as
 * irq_run_dpcs and KfRaiseIrql). Other DPCs stay queued for the normal
 * delivery. */
static void irq_run_apu_dpcs_fast(void)
{
    InitOnceExecuteOnce(&g_dispatch_cs_once, dispatch_cs_init, NULL, NULL);
    if (!g_dpc_pending || !TryEnterCriticalSection(&g_dispatch_cs))
        return;
    for (;;) {
        uint32_t dpc = 0, args[4];
        int i;
        InitOnceExecuteOnce(&g_dpc_lock_once, dpc_lock_init, NULL, NULL);
        EnterCriticalSection(&g_dpc_lock);
        for (i = 0; i < g_dpc_count; i++) {
            if (irq_is_apu_dpc(g_dpc_queue[i])) {
                dpc = g_dpc_queue[i];
                memmove(g_dpc_queue + i, g_dpc_queue + i + 1,
                        (size_t)(g_dpc_count - i - 1) * sizeof(g_dpc_queue[0]));
                g_dpc_count--;
                BRIDGE_MEM8(dpc + 2) = 0;  /* Inserted */
                break;
            }
        }
        InterlockedExchange(&g_dpc_pending, g_dpc_count ? 1 : 0);
        LeaveCriticalSection(&g_dpc_lock);
        if (!dpc)
            break;
        args[0] = dpc;
        args[1] = BRIDGE_MEM32(dpc + 16);
        args[2] = BRIDGE_MEM32(dpc + 20);
        args[3] = BRIDGE_MEM32(dpc + 24);
        irq_run_guest(BRIDGE_MEM32(dpc + 12), args, 4, 2 /* DISPATCH_LEVEL */);
    }
    LeaveCriticalSection(&g_dispatch_cs);
}

static void irq_deliver_apu_fast(void)
{
    LONG busy_line = g_irq_busy_isr_line;
    if (busy_line < 0 || busy_line == APU_IRQ_LINE ||
        g_irq_busy_tid == GetCurrentThreadId())
        return;
    if (!irq_apu_fastpath_on() || !g_irq_line[APU_IRQ_LINE] ||
        InterlockedCompareExchange(&g_irq_apu_busy, 1, 0) != 0)
        return;
    {
        LONG delivered = 0;
        irq_run_apu_isr(xbox_fs_read8(0x24), &delivered);
    }
    InterlockedExchange(&g_irq_apu_busy, 0);
    if (xbox_fs_read8(0x24) < 2)
        irq_run_apu_dpcs_fast();
}

static int xbox_irq_deliver(void)
{
    unsigned line;
    uint8_t irql;
    unsigned pass;
    LONG delivered_generation[XBOX_IRQ_LINES] = { 0 };

    if (!g_dpc_pending) {
        for (line = 0; line < XBOX_IRQ_LINES; line++)
            if (g_irq_line[line] && g_irq_isr[line].routine)
                break;
        if (line == XBOX_IRQ_LINES)
            return 1;
    }
    if (!g_fs_base)
        return 1;
    if (s_apu_irq_raised_qpc)
        InterlockedIncrement(&s_apu_try_calls);
    if (InterlockedCompareExchange(&g_irq_busy, 1, 0) != 0) {
        if (s_apu_irq_raised_qpc) {
            int64_t since = s_busy_since_qpc;
            InterlockedIncrement(&s_apu_try_busy);
            if (since) {
                double held = irq_qpc_ms(irq_qpc() - since);
                if (held > s_apu_busy_max_ms) {
                    s_apu_busy_max_ms = held;
                    s_apu_busy_max_what = s_busy_what;
                }
            }
        }
        irq_deliver_apu_fast();
        return 0;
    }
    s_busy_since_qpc = irq_qpc();
    g_irq_busy_tid = GetCurrentThreadId();
    irql = xbox_fs_read8(0x24);
    if (s_apu_irq_raised_qpc && g_irq_isr[5].routine && irql >= g_irq_isr[5].irql)
        InterlockedIncrement(&s_apu_try_irql);
    /* Device IRQs are level-triggered.  An ISR/DPC may acknowledge one GPU
     * condition and thereby let PFIFO expose the next condition before the
     * handler returns.  The old single pass then left the still-asserted line
     * pending until some unrelated later kernel/MMIO safe point.  SCDA's first
     * xemu-PGRAPH submission is exactly such a burst (successive nonzero NOP
     * notifications), and no later safe point exists because the title waits
     * for the GPU: PFIFO stopped forever after the second condition.
     *
     * Re-sample the lines after the ISR and all queued DPCs, as level hardware
     * does.  The bound protects the host from a genuinely broken ISR that
     * never acknowledges its source; a later safe point can resume delivery. */
    for (pass = 0; pass < 1024; pass++) {
        int did_work = 0;
        for (line = 0; line < XBOX_IRQ_LINES; line++) {
            LONG line_generation = g_irq_line_generation[line];
            if (line == APU_IRQ_LINE && irq_apu_fastpath_on()) {
                /* Shared with the fast path above: one APU ISR at a time,
                 * each assertion once. A busy flag means the fast path is
                 * running it right now. */
                if (g_irq_line[line] && line_generation != delivered_generation[line] &&
                    InterlockedCompareExchange(&g_irq_apu_busy, 1, 0) == 0) {
                    s_busy_what = g_irq_isr[line].routine;
                    InterlockedExchange(&g_irq_busy_isr_line, (LONG)line);
                    if (irq_run_apu_isr(irql, &delivered_generation[line]))
                        did_work = 1;
                    InterlockedExchange(&g_irq_busy_isr_line, -1);
                    InterlockedExchange(&g_irq_apu_busy, 0);
                }
                continue;
            }
            if (g_irq_line[line] && g_irq_isr[line].routine &&
                line_generation != delivered_generation[line] &&
                irql < g_irq_isr[line].irql) {
                static unsigned delivered;
                uint32_t args[2], r;
                args[0] = g_irq_isr[line].kint;
                args[1] = g_irq_isr[line].context;
                /* Mark this assertion before entering guest code.  If the ISR
                 * lowers and PFIFO raises the line again, its generation
                 * changes and the next pass handles it.  A source that stays
                 * high is not called 1024 times in this one safe point. */
                delivered_generation[line] = line_generation;
                if (delivered < 256)
                    fprintf(stderr, "  [KERNEL] IRQ %u begin -> ISR 0x%08X "
                            "(delivery %u, generation %ld, irql %u)\n", line,
                            g_irq_isr[line].routine, delivered + 1,
                            line_generation, (unsigned)irql);
                if (line == 5)
                    irq_lat_note_apu();
                s_busy_what = g_irq_isr[line].routine;
                InterlockedExchange(&g_irq_busy_isr_line, (LONG)line);
                r = irq_run_guest(g_irq_isr[line].routine, args, 2,
                                  (uint8_t)g_irq_isr[line].irql);
                InterlockedExchange(&g_irq_busy_isr_line, -1);
                did_work = 1;
                if (++delivered <= 256 || (delivered & (delivered - 1)) == 0)
                    fprintf(stderr, "  [KERNEL] IRQ %u -> ISR 0x%08X (delivery %u) "
                            "returned %u, line now %ld\n", line,
                            g_irq_isr[line].routine, delivered, r & 0xFF,
                            g_irq_line[line]);
            }
        }
        if (g_dpc_pending && irql < 2 && irq_run_dpcs())
            did_work = 1;
        if (!did_work)
            break;
    }
    if (pass == 1024) {
        static unsigned saturated;
        if (++saturated <= 8 || (saturated & (saturated - 1)) == 0)
            fprintf(stderr, "  [KERNEL] IRQ delivery saturated at 1024 "
                    "level-triggered passes (occurrence %u)\n", saturated);
    }
    s_busy_since_qpc = 0;
    s_busy_what = 0;
    g_irq_busy_tid = 0;
    InterlockedExchange(&g_irq_busy, 0);
    return 1;
}

/* Safe point inside a trapped device-register access (src/gpu/nv2a_recomp.c,
 * docs Update 355). Xbox D3D spins in `while (IsGpuBusy());` -- register
 * reads, no kernel call -- while the GPU waits for the D3D ISR to service a
 * context-switch interrupt; on hardware the interrupt preempts the loop. The
 * access has completed when this runs, so it is an instruction boundary like
 * a kernel-call end. The ISR gets a 1 KiB gap below the stored guest ESP in
 * case the interrupted code still holds a newer ESP in a host register;
 * everything below ESP is dead on x86, exactly what a hardware interrupt
 * assumes too. */
void xbox_irq_deliver_from_mmio(void)
{
    uint32_t saved_esp;

    if (!g_fs_base)
        return;
    saved_esp = g_esp;
    g_esp = (g_esp - 0x400u) & ~0xFu;
    if (g_irq_via != 4)
        g_irq_via = 3;
    xbox_irq_deliver();
    g_esp = saved_esp;
}

/* Cooperative interrupt boundary used by every generated basic block (see
 * templates/runtime/recomp_types.h).  Device threads only publish the flag;
 * guest code and its TLS register set remain owned by this thread. */
/* Guest timeslice (docs Update 620). All guest threads share one host core
 * (the console's single CPU, xbox_PinGuestThread), but Windows lets the
 * foreground process's running thread keep that core for a long quantum
 * (tens of ms) unless it waits. A busy render thread then held off SCDA's
 * music-streaming thread past its 48 ms packet deadline and DirectSound
 * played the previous packet again -- the music stutter. The async IRQ thread
 * ticks a slice generation every RECOMP_GUEST_TIMESLICE_MS (default 0 = off;
 * 4 hung SCDA early in boot, Update 620 -- not yet understood; 0 =
 * off) and arms the safe point; the guest thread that reaches it yields the
 * core to a ready guest thread (SwitchToThread does nothing if none is). */
static volatile LONG s_slice_gen;
static RECOMP_TLS LONG t_slice_seen;

void xbox_irq_safepoint(void)
{
    uint32_t saved_esp;

    if (!g_fs_base ||
        InterlockedExchange((volatile LONG *)&g_recomp_irq_pending, 0) == 0)
        return;
    saved_esp = g_esp;
    g_esp = (g_esp - 0x400u) & ~0xFu;
    /* Multiple recompiled guest threads run on host threads, although the
     * Xbox has one CPU.  A safe point can therefore race an ISR/DPC already
     * running on another host thread.  Do not lose the edge merely because
     * that thread currently owns g_irq_busy: leave it armed for the first
     * safe point after the owner releases the guest CPU. */
    g_irq_via = 5;
    if (!xbox_irq_deliver())
        InterlockedExchange((volatile LONG *)&g_recomp_irq_pending, 1);
    g_esp = saved_esp;
    if (t_slice_seen != s_slice_gen) {
        t_slice_seen = s_slice_gen;
        SwitchToThread();
    }
}

/* ── Asynchronous interrupt delivery (docs Update 355) ──────────────────
 *
 * Kernel-call ends and trapped register accesses are not the only places a
 * console CPU takes an interrupt: it takes it between any two instructions.
 * Xbox D3D relies on that. Its pushbuffer-space wait (SCDA sub_004A1490)
 * spins on a GET shadow in RAM that the GPU only advances once the D3D ISR
 * has acknowledged the NOP/notify interrupt the GPU is stalled on -- no
 * kernel call, no register access, so neither safe point above ever comes.
 *
 * A watchdog notices a line that has stayed asserted for ~2 ms, suspends a
 * guest thread that is executing translated guest code (its RIP inside the
 * generated functions -- never runtime code, which may hold locks), and
 * redirects it to a trampoline that runs the ISR through
 * xbox_irq_deliver_from_mmio and then restores the exact interrupted host
 * context. That is a hardware interrupt, modelled at host-instruction
 * granularity: guest registers live in host registers/locals there and are
 * restored bit for bit; the ISR gets its stack gap below the guest ESP. */
#define XBOX_MAX_GUEST_THREADS 64
static HANDLE g_guest_threads[XBOX_MAX_GUEST_THREADS];
static volatile LONG g_guest_thread_count;
static uintptr_t g_gen_lo, g_gen_hi;           /* translated-code host range */
/* The thread hijack is Windows/x86-64 only (SuspendThread, Get/SetThread-
 * Context, RtlRestoreContext); POSIX hosts deliver IRQs synchronously, as
 * RECOMP_IRQ_ASYNC=0 does on Windows (ARM port, docs Update 677). */
#if defined(_WIN32)
static __declspec(align(16)) CONTEXT g_hijack_ctx;
static volatile LONG g_hijack_active;
static volatile LONG g_async_hijacks;

void xbox_irq_register_guest_thread(void)
{
    HANDLE h = NULL;
    LONG n;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                         | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0))
        return;
    n = InterlockedIncrement(&g_guest_thread_count) - 1;
    if (n < XBOX_MAX_GUEST_THREADS)
        g_guest_threads[n] = h;
    else
        CloseHandle(h);
}

static void irq_hijack_trampoline(void)
{
    __declspec(align(16)) CONTEXT resume = g_hijack_ctx;
    InterlockedExchange(&g_hijack_active, 0);
    g_irq_via = 4;
    xbox_irq_deliver_from_mmio();
    RtlRestoreContext(&resume, NULL);
}

/* An access fault can already be in flight when the watchdog suspends a
 * guest thread: GetThreadContext still shows the faulting instruction (in
 * translated code), the thread resumes into the trampoline, and the fault is
 * then dispatched with the trampoline's RIP and the original fault address.
 * The MMIO VEHs (APU, NV2A, AC'97) decode the instruction at RIP and fail on
 * the trampoline's -- a crash at irq_hijack_trampoline with an APU register
 * as fault address (docs Update 611). The access itself has not happened, so
 * continue into the trampoline: its RtlRestoreContext returns to the
 * original instruction, which faults again and is handled as usual. */
static volatile LONG g_hijack_fault_races;

static LONG CALLBACK irq_hijack_fault_veh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ContextRecord->Rip == (DWORD64)(uintptr_t)irq_hijack_trampoline) {
        LONG n = InterlockedIncrement(&g_hijack_fault_races);
        if (n <= 5 || (n & (n - 1)) == 0)
            fprintf(stderr, "  [KERNEL] async IRQ: fault in flight at hijack "
                    "(#%ld, address %p) -- retried after the ISR\n", n,
                    (void *)ep->ExceptionRecord->ExceptionInformation[1]);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Host address range of the translated functions: every guest VA the
 * dispatcher knows, probed once. Generated objects link as one block. */
static void irq_find_generated_range(void)
{
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (uint32_t va = 0x00010000u; va < 0x00800000u; va++) {
        recomp_func_t f = recomp_lookup(va);
        if (f) {
            if ((uintptr_t)f < lo) lo = (uintptr_t)f;
            if ((uintptr_t)f > hi) hi = (uintptr_t)f;
        }
    }
    g_gen_lo = lo;
    g_gen_hi = hi + 0x10000;     /* the last function's body */
}

static int irq_line_pending(void)
{
    for (unsigned line = 0; line < XBOX_IRQ_LINES; line++)
        if (g_irq_line[line] && g_irq_isr[line].routine)
            return 1;
    return 0;
}

static DWORD WINAPI irq_async_thread(LPVOID arg)
{
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    unsigned pending_ticks = 0, rr = 0, slice_ms = 0, slice_ticks = 0;  /* opt-in, Update 620 */
    (void)arg;
    {
        const char *e = getenv("RECOMP_GUEST_TIMESLICE_MS");
        if (e && *e)
            slice_ms = (unsigned)atoi(e);
        fprintf(stderr, "  [KERNEL] guest timeslice %u ms%s\n", slice_ms,
                slice_ms ? "" : " (off)");
    }
    irq_find_generated_range();
    fprintf(stderr, "  [KERNEL] async IRQ delivery armed (translated code "
            "%p..%p)\n", (void *)g_gen_lo, (void *)g_gen_hi);
    for (;;) {
        LARGE_INTEGER due;
        due.QuadPart = -10000;   /* 1 ms */
        if (timer) {
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
        if (slice_ms && ++slice_ticks >= slice_ms) {
            slice_ticks = 0;
            InterlockedIncrement(&s_slice_gen);
            InterlockedExchange((volatile LONG *)&g_recomp_irq_pending, 1);
        }
        if (!irq_line_pending() && !g_dpc_pending) {
            pending_ticks = 0;
            continue;
        }
        if (++pending_ticks < 2 || g_irq_busy || g_hijack_active)
            continue;
        LONG n = g_guest_thread_count;
        if (n > XBOX_MAX_GUEST_THREADS) n = XBOX_MAX_GUEST_THREADS;
        for (LONG k = 0; k < n; k++) {
            HANDLE t = g_guest_threads[(rr + k) % n];
            CONTEXT ctx;
            if (!t || SuspendThread(t) == (DWORD)-1)
                continue;
            ctx.ContextFlags = CONTEXT_FULL | CONTEXT_FLOATING_POINT;
            if (GetThreadContext(t, &ctx) && ctx.Rip >= g_gen_lo
                    && ctx.Rip < g_gen_hi) {
                g_hijack_ctx = ctx;
                InterlockedExchange(&g_hijack_active, 1);
                ctx.Rsp = ((ctx.Rsp - 512) & ~(DWORD64)0xF) - 8;
                ctx.Rip = (DWORD64)(uintptr_t)irq_hijack_trampoline;
                if (SetThreadContext(t, &ctx)) {
                    InterlockedIncrement(&g_async_hijacks);
                    ResumeThread(t);
                    rr = (unsigned)((rr + k + 1) % n);
                    pending_ticks = 0;
                    break;
                }
                InterlockedExchange(&g_hijack_active, 0);
            }
            ResumeThread(t);
        }
    }
    return 0;
}

static void irq_async_start_once(void)
{
    static volatile LONG started;
    const char *a = getenv("RECOMP_IRQ_ASYNC");
    const char *x = getenv("RECOMP_NV2A_XEMU");
    int on = (a && *a && *a != '0') || (x && *x && *x != '0');
    /* RECOMP_IRQ_ASYNC=0 opts out even on the xemu GPU path (docs Update
     * 675): under Box64 a suspended thread's context holds stale registers
     * the dynarec keeps in ARM registers, so the hijack corrupted state. */
    if (a && a[0] == '0')
        on = 0;
    if (!on || InterlockedExchange(&started, 1))
        return;
    /* First in the chain, ahead of the MMIO VEHs registered at init. */
    AddVectoredExceptionHandler(1, irq_hijack_fault_veh);
    CloseHandle(CreateThread(NULL, 0, irq_async_thread, NULL, 0, NULL));
}
#else
static volatile LONG g_hijack_active;   /* never set: no hijack here */
static void irq_async_start_once(void) {}
void xbox_irq_register_guest_thread(void) {}
#endif /* _WIN32: async IRQ hijack */

/* BOOLEAN KeConnectInterrupt(PKINTERRUPT Interrupt)
 * FALSE when the object is already connected or the line is taken, as the
 * real kernel answers (no shared vectors here). DirectSound connects the APU
 * ISR once per DirectSoundCreate and disconnects it on release; SCDA creates
 * it three times, so the line must follow KeDisconnectInterrupt -- keeping
 * the first registration ran the ISR against a freed APU object, whose
 * idle-voice handler then never matched (docs Update 347). */
static void bridge_KeConnectInterrupt(void)
{
    uint32_t kint = STACK_ARG(0);
    uint32_t level = BRIDGE_MEM32(kint + 8);

    g_eax = 0;
    if (BRIDGE_MEM8(kint + 16))
        return;
    if (level < XBOX_IRQ_LINES && !g_irq_isr[level].routine) {
        g_irq_isr[level].kint = kint;
        g_irq_isr[level].context = BRIDGE_MEM32(kint + 4);
        g_irq_isr[level].irql = BRIDGE_MEM32(kint + 12);
        g_irq_isr[level].routine = BRIDGE_MEM32(kint + 0);
        BRIDGE_MEM8(kint + 16) = 1;  /* Connected */
        irq_async_start_once();
    } else if (level < XBOX_IRQ_LINES) {
        fprintf(stderr, "  [KERNEL] KeConnectInterrupt: level %u already taken by "
                "0x%08X -> FALSE\n", level, g_irq_isr[level].kint);
        return;
    }
    fprintf(stderr, "  [KERNEL] KeConnectInterrupt: level %u irql %u routine 0x%08X "
            "context 0x%08X%s\n", level, BRIDGE_MEM32(kint + 12), BRIDGE_MEM32(kint),
            BRIDGE_MEM32(kint + 4),
            level == 5 ? " (APU: delivered)" : " (no device raises this line)");
    g_eax = 1;
}

/* BOOLEAN KeDisconnectInterrupt(PKINTERRUPT Interrupt) */
static void bridge_KeDisconnectInterrupt(void)
{
    uint32_t kint = STACK_ARG(0);
    unsigned line;

    g_eax = 0;
    if (!kint || !BRIDGE_MEM8(kint + 16))
        return;
    for (line = 0; line < XBOX_IRQ_LINES; line++) {
        if (g_irq_isr[line].kint == kint) {
            g_irq_isr[line].routine = 0;
            g_irq_isr[line].kint = 0;
        }
    }
    BRIDGE_MEM8(kint + 16) = 0;
    fprintf(stderr, "  [KERNEL] KeDisconnectInterrupt: 0x%08X (level %u)\n", kint,
            BRIDGE_MEM32(kint + 8));
    g_eax = 1;
}

/* BOOLEAN KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2) */
static void bridge_KeInsertQueueDpc(void)
{
    static unsigned dpc_inserts;
    uint32_t dpc = STACK_ARG(0);

    g_eax = 0;
    if (!dpc)
        return;
    InitOnceExecuteOnce(&g_dpc_lock_once, dpc_lock_init, NULL, NULL);
    EnterCriticalSection(&g_dpc_lock);
    if (!BRIDGE_MEM8(dpc + 2) && g_dpc_count < XBOX_DPC_QUEUE) {
        BRIDGE_MEM8(dpc + 2) = 1;  /* Inserted */
        BRIDGE_MEM32(dpc + 20) = STACK_ARG(1);
        BRIDGE_MEM32(dpc + 24) = STACK_ARG(2);
        g_dpc_queue[g_dpc_count++] = dpc;
        if (t_in_apu_isr && !irq_is_apu_dpc(dpc) && g_apu_dpc_count < 8) {
            g_apu_dpcs[g_apu_dpc_count] = dpc;
            InterlockedIncrement(&g_apu_dpc_count);
        }
        InterlockedExchange(&g_dpc_pending, 1);
        InterlockedExchange((volatile LONG *)&g_recomp_irq_pending, 1);
        g_eax = 1;
        if (++dpc_inserts <= 256)
            fprintf(stderr, "  [KERNEL] DPC insert %u object=0x%08X "
                    "queue=%d source=0x%08X\n", dpc_inserts, dpc,
                    g_dpc_count, g_icall_source);
    }
    LeaveCriticalSection(&g_dpc_lock);
}

/* BOOLEAN KeSynchronizeExecution(PKINTERRUPT Interrupt,
 *                                PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
 *                                PVOID SynchronizeContext)
 * Runs the routine at the interrupt's Irql, i.e. with that ISR held off, and
 * returns its result. DirectSound calls it right after connecting the APU
 * interrupt; unbridged it returned 0 without running the routine. The ISR
 * cannot run meanwhile: g_irq_busy is held and the Irql is raised. */
static void bridge_KeSynchronizeExecution(void)
{
    uint32_t kint = STACK_ARG(0), routine = STACK_ARG(1);
    uint32_t arg = STACK_ARG(2);
    uint8_t irql = kint ? (uint8_t)BRIDGE_MEM32(kint + 12) : 0;
    LONG was_busy = InterlockedExchange(&g_irq_busy, 1);
    uint8_t cur = xbox_fs_read8(0x24);
    /* The APU fast path runs that ISR outside g_irq_busy (Update 617):
     * synchronizing with it means holding its own flag as well. */
    int apu = kint && kint == g_irq_isr[APU_IRQ_LINE].kint && irq_apu_fastpath_on();
    while (apu && InterlockedCompareExchange(&g_irq_apu_busy, 1, 0) != 0)
        YieldProcessor();

    /* Above DISPATCH_LEVEL: no DPC or other thread runs meanwhile. */
    irql_cross(cur, irql);
    g_eax = irq_run_guest(routine, &arg, 1, irql) & 0xFF;
    irql_cross(irql, cur);
    if (apu)
        InterlockedExchange(&g_irq_apu_busy, 0);
    if (!was_busy)
        InterlockedExchange(&g_irq_busy, 0);
}

/* BOOLEAN KeRemoveQueueDpc(PKDPC Dpc) */
static void bridge_KeRemoveQueueDpc(void)
{
    uint32_t dpc = STACK_ARG(0);
    int i;

    g_eax = 0;
    InitOnceExecuteOnce(&g_dpc_lock_once, dpc_lock_init, NULL, NULL);
    EnterCriticalSection(&g_dpc_lock);
    for (i = 0; i < g_dpc_count; i++) {
        if (g_dpc_queue[i] == dpc) {
            memmove(g_dpc_queue + i, g_dpc_queue + i + 1,
                    (size_t)(g_dpc_count - i - 1) * sizeof(g_dpc_queue[0]));
            g_dpc_count--;
            BRIDGE_MEM8(dpc + 2) = 0;
            g_eax = 1;
            break;
        }
    }
    InterlockedExchange(&g_dpc_pending, g_dpc_count ? 1 : 0);
    LeaveCriticalSection(&g_dpc_lock);
}

/* ── MmClaimGpuInstanceMemory (ordinal 168) ───────────────
 * PVOID MmClaimGpuInstanceMemory(SIZE_T NumberOfBytes, SIZE_T *Padding)
 *
 * Reserves the GPU instance memory the NV2A keeps its object context in. On
 * hardware it sits at the very top of physical RAM, so the returned address is
 * the end of the contiguous window minus the request. D3D8 stores this and
 * indexes off it, so returning 0 (the unbridged default) had it building
 * pointers from a null base.
 *
 * MAXULONG_PTR means "claim everything left"; the console answers with the
 * default instance size rather than the whole of RAM.
 */
static void bridge_MmClaimGpuInstanceMemory(void)
{
    uint32_t bytes      = STACK_ARG(0);
    uint32_t padding_va = STACK_ARG(1);

    if (bytes == 0xFFFFFFFFu) {
        bytes = XBOX_GPU_INSTANCE_DEFAULT;
    }
    if (padding_va) {
        BRIDGE_MEM32(padding_va) = 0;
    }
    g_eax = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - bytes;
}

/* VOID HalRegisterShutdownNotification(PHAL_SHUTDOWN_REGISTRATION, BOOLEAN)
 * Records a callback for console shutdown. Nothing here ever shuts down that
 * way, so registration is accepted and dropped. */
static void bridge_HalRegisterShutdownNotification(void)
{
    g_eax = 0;
}

/* ── KeInitializeTimerEx (ordinal 113) ────────────────────
 * VOID KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type)
 *
 * Initializes a timer object. Xbox KTIMER is 40 bytes.
 */
static void bridge_KeInitializeTimerEx(void)
{
    uint32_t timer_va = STACK_ARG(0);
    uint32_t type = STACK_ARG(1);

    /* Zero the structure (40 bytes) */
    memset(XBOX_TO_NATIVE(timer_va), 0, 40);

    /* Set Type (0x08 = TimerNotificationObject, 0x09 = TimerSynchronizationObject) */
    BRIDGE_MEM16(timer_va + 0) = (uint16_t)(0x08 + (type & 1));
    g_eax = 0;
}

/* ── KeSetTimer / KeSetTimerEx (ordinal 149/150) ──────────
 * BOOLEAN KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
 *
 * Sets a timer. We don't actually start timers - just record the state.
 * Returns FALSE (timer was not already set).
 */
static void bridge_KeSetTimer(void)
{
    /* Timer functionality is not needed for basic execution.
     * Return FALSE = timer was not previously set. */
    g_eax = 0;
}

/* ── ExQueryPoolBlockSize (ordinal 24) ────────────────────
 * ULONG ExQueryPoolBlockSize(PVOID PoolBlock)
 *
 * Returns the size of a pool memory block.
 * Since we use HeapAlloc, we can query the Windows heap.
 */
static void bridge_ExQueryPoolBlockSize(void)
{
    /* Pool blocks come from xbox_HeapAlloc, so the block table has the real
     * answer. It used to return a literal 0 on the theory that this is only
     * ever used for stats -- which is a guess about the caller, and a title
     * that sizes a copy from it copies nothing. */
    g_eax = xbox_HeapBlockSize(STACK_ARG(0));
}

/* ── RtlNtStatusToDosError (ordinal 301) ─────────────────
 * ULONG RtlNtStatusToDosError(NTSTATUS Status)
 *
 * Converts an NTSTATUS to a Win32 error code.
 *
 * The Xbox kernel's table is the NT one, which the host's ntdll carries in
 * full. A seven-entry switch mapped everything else to 317
 * (ERROR_MR_MID_NOT_FOUND): XAPI's FindFirstFile turned STATUS_NO_MORE_FILES
 * for a not-yet-existing "single1.sav" into error 317 instead of 18, and
 * SCDA's in-level save treated the missing slot file as a hard failure and
 * silently gave up (docs Update 355).
 */
static void bridge_RtlNtStatusToDosError(void)
{
    typedef ULONG (WINAPI *rtl_status_fn)(LONG);
    static rtl_status_fn host_fn;
    static int looked_up;
    uint32_t status = STACK_ARG(0);

    if (!looked_up) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        host_fn = ntdll ? (rtl_status_fn)GetProcAddress(ntdll, "RtlNtStatusToDosError")
                        : NULL;
        looked_up = 1;
    }
    if (host_fn) {
        g_eax = host_fn((LONG)status);
        return;
    }

    /* Fallback without ntdll: the common status codes */
    switch (status) {
    case 0x00000000: g_eax = 0; break;          /* STATUS_SUCCESS → ERROR_SUCCESS */
    case 0x80000006: g_eax = 18; break;         /* STATUS_NO_MORE_FILES → ERROR_NO_MORE_FILES */
    case 0xC0000034: g_eax = 2; break;          /* STATUS_OBJECT_NAME_NOT_FOUND → ERROR_FILE_NOT_FOUND */
    case 0xC000003A: g_eax = 3; break;          /* STATUS_OBJECT_PATH_NOT_FOUND → ERROR_PATH_NOT_FOUND */
    case 0xC0000022: g_eax = 5; break;          /* STATUS_ACCESS_DENIED → ERROR_ACCESS_DENIED */
    case 0xC0000008: g_eax = 6; break;          /* STATUS_INVALID_HANDLE → ERROR_INVALID_HANDLE */
    case 0xC0000017: g_eax = 8; break;          /* STATUS_NO_MEMORY → ERROR_NOT_ENOUGH_MEMORY */
    case 0xC000000D: g_eax = 87; break;         /* STATUS_INVALID_PARAMETER → ERROR_INVALID_PARAMETER */
    default:         g_eax = 317; break;         /* ERROR_MR_MID_NOT_FOUND (generic) */
    }
}

/* ── File I/O bridge helpers ─────────────────────────────── */

/*
 * Xbox structures use 32-bit pointers. On Win64, the C structs
 * (XBOX_OBJECT_ATTRIBUTES, etc.) have 64-bit pointers, so we can't
 * cast Xbox memory to them directly. Instead, parse the 32-bit
 * Xbox layout manually:
 *
 * XBOX_OBJECT_ATTRIBUTES (12 bytes):
 *   offset 0: RootDirectory  (uint32_t)
 *   offset 4: ObjectName     (uint32_t, Xbox VA to ANSI_STRING)
 *   offset 8: Attributes     (uint32_t)
 *
 * XBOX_ANSI_STRING (8 bytes):
 *   offset 0: Length          (uint16_t)
 *   offset 2: MaximumLength   (uint16_t)
 *   offset 4: Buffer          (uint32_t, Xbox VA to char[])
 *
 * XBOX_IO_STATUS_BLOCK (8 bytes):
 *   offset 0: Status          (uint32_t)
 *   offset 4: Information     (uint32_t)
 */

/* Extract the ANSI path string from an Xbox OBJECT_ATTRIBUTES */
/* The object name is a counted ANSI_STRING: only Length bytes belong to it,
 * and the buffer need not be terminated there. XAPI's FindFirstFile passes
 * "U:\*" with Length covering just "U:\" (the mask goes to
 * NtQueryDirectoryFile separately); reading up to the NUL opened "U:\*" and
 * failed, so no save game was ever found (docs Update 348). The name is
 * copied, terminated, into a small per-thread ring so callers may hold two. */
static const char* bridge_get_xbox_path(uint32_t obj_attrs_va)
{
    static RECOMP_TLS char ring[4][MAX_PATH];
    static RECOMP_TLS unsigned next;
    uint32_t ansi_str_va, buf_va, len;
    char *out;

    if (!obj_attrs_va) return NULL;
    ansi_str_va = BRIDGE_MEM32(obj_attrs_va + 4);
    if (!ansi_str_va) return NULL;
    buf_va = BRIDGE_MEM32(ansi_str_va + 4);
    if (!buf_va) return NULL;
    len = BRIDGE_MEM16(ansi_str_va);
    if (len >= MAX_PATH)
        len = MAX_PATH - 1;
    out = ring[next++ & 3];
    memcpy(out, XBOX_TO_NATIVE(buf_va), len);
    out[len] = 0;
    return out;
}

/* Write NTSTATUS + Information into Xbox IO_STATUS_BLOCK */
static void bridge_write_iostatus(uint32_t ios_va, NTSTATUS status, uint32_t info)
{
    if (ios_va) {
        BRIDGE_MEM32(ios_va + 0) = (uint32_t)status;
        BRIDGE_MEM32(ios_va + 4) = info;
    }
}

/*
 * Handle table.
 *
 * Xbox memory only has 32-bit handle slots, but native HANDLEs are 64-bit
 * pointers (win32_compat objects, or real Win32 handles on Windows). Map
 * 32-bit tokens <-> native HANDLEs so a handle survives a round-trip through
 * Xbox memory. Tokens carry a tag in the high byte so they never collide
 * with the synthetic handles (0xDEAD0001 / 0xBEEF0010) used elsewhere.
 */
/* BRIDGE_HANDLE_TAG/MASK/MAX and s_handle_kthread_va are declared earlier in
 * this file (near bridge_mark_thread_done) so bridge_PsCreateSystemThreadEx
 * and bridge_ObReferenceObjectByHandle can use them; only the native-handle
 * side of the table lives here. */
static HANDLE s_handle_table[BRIDGE_HANDLE_MAX];
static char s_handle_paths[BRIDGE_HANDLE_MAX][MAX_PATH];

/* SCDA Update 302: compact, logical common.lin ring-buffer trace. The
 * recompiled override arms a per-thread event immediately before its real
 * NtReadFile call. This bridge owns the stable guest-token/path/file-position
 * mapping and completes the event here. Emulated state and I/O are unchanged. */
typedef struct scda_ring_trace_pending {
    uint32_t armed, matched, event, read_index;
    uint32_t object, token, callsite, writer;
    uint32_t cursor_before, buffer_size, remaining, wrap;
    uint32_t record_tag, record_length;
    uint64_t file_offset;
    uint32_t requested, actual, status;
} scda_ring_trace_pending;

static RECOMP_TLS scda_ring_trace_pending s_scda_ring_pending;
static RECOMP_TLS uint32_t s_scda_ring_common_token;
static RECOMP_TLS uint32_t s_scda_ring_read_index;
static RECOMP_TLS uint32_t s_scda_ring_invocation_index;
static RECOMP_TLS uint32_t s_scda_ring_boundary_count;
static RECOMP_TLS uint32_t s_scda_ring_after_short;
typedef struct scda_ring_boundary_record {
    uint32_t entering, requested_or_remaining, caller, copied;
    uint32_t cursor, invocation;
} scda_ring_boundary_record;
#define SCDA_RING_BOUNDARY_HISTORY 64u
static RECOMP_TLS scda_ring_boundary_record
    s_scda_ring_boundary_history[SCDA_RING_BOUNDARY_HISTORY];
static RECOMP_TLS uint32_t s_scda_ring_boundary_history_pos;
static RECOMP_TLS uint32_t s_scda_ring_boundary_history_count;
static RECOMP_TLS uint32_t s_scda_ring_callstack_count;
static RECOMP_TLS uint32_t s_scda_ring_refillstack_count;
static RECOMP_TLS uint32_t s_scda_ring_after_first_read;
static RECOMP_TLS uint32_t s_scda_ring_post_first_count;
static RECOMP_TLS uint32_t s_scda_ring_pre_second_count;
static RECOMP_TLS uint32_t s_scda_ring_state_stack_count;
static volatile LONG s_scda_ring_event_counter;

static int scda_ring_is_common_lin(const char *path)
{
    static const char suffix[] = "\\DataXb\\LMaps\\common.lin";
    size_t path_len, suffix_len;
    if (!path)
        return 0;
    path_len = strlen(path);
    suffix_len = sizeof(suffix) - 1;
    return path_len >= suffix_len &&
           _stricmp(path + path_len - suffix_len, suffix) == 0;
}

static const char *scda_ring_path_for_token(uint32_t token)
{
    uint32_t i;
    if ((token & ~BRIDGE_HANDLE_MASK) != BRIDGE_HANDLE_TAG)
        return NULL;
    i = token & BRIDGE_HANDLE_MASK;
    if (i == 0 || i >= BRIDGE_HANDLE_MAX || !s_handle_paths[i][0])
        return NULL;
    return s_handle_paths[i];
}

/* The ring-buffer investigation's .tsv files (Updates 302-311, closed) are
 * written only on request: RECOMP_RING_TRACE_FILES=1. They cost ~3.5 MB per
 * start in the player's folder otherwise. */
static int scda_ring_files_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_RING_TRACE_FILES");
        on = e && e[0] == '1';
    }
    return on;
}

static void scda_ring_trace_write(const char *type, uint32_t object,
                                  uint32_t cursor_before,
                                  uint32_t cursor_after,
                                  uint32_t buffer_size,
                                  uint32_t remaining,
                                  uint32_t wrap,
                                  uint32_t record_tag,
                                  uint32_t record_length,
                                  uint32_t writer)
{
    char filename[96];
    FILE *f = NULL;
    LONG event = InterlockedIncrement(&s_scda_ring_event_counter);
    _snprintf_s(filename, sizeof(filename), _TRUNCATE,
                "scda_ring_sequence_302_%lu.tsv",
                (unsigned long)GetCurrentProcessId());
    if (!scda_ring_files_enabled() ||
        fopen_s(&f, filename, "a") != 0 || !f)
        return;
    fprintf(f,
            "%ld\t%s\t%u\t0x%08X\t%lu\t0x%llX\t0x%X\t0x%X\t"
            "0x%08X\t0x%08X\t0x%08X\t0x%08X\t0x%08X\t0x%08X\t%u\t"
            "0x%08X\t0x%08X\t%u\t0x%08X\n",
            event, type, s_scda_ring_read_index, s_scda_ring_common_token,
            (unsigned long)GetCurrentThreadId(),
            (unsigned long long)(s_scda_ring_pending.matched ?
                                 s_scda_ring_pending.file_offset : 0),
            s_scda_ring_pending.matched ? s_scda_ring_pending.requested : 0,
            s_scda_ring_pending.matched ? s_scda_ring_pending.actual : 0,
            s_scda_ring_pending.matched ? s_scda_ring_pending.callsite : 0,
            object, cursor_before, cursor_after, buffer_size, remaining,
            wrap, record_tag, record_length, writer,
            s_scda_ring_pending.matched ? s_scda_ring_pending.status : 0);
    fclose(f);
}

void scda_ring_trace_begin(uint32_t object, uint32_t callsite,
                           uint32_t writer, uint32_t cursor_before,
                           uint32_t buffer_size, uint32_t remaining,
                           uint32_t wrap, uint32_t record_tag,
                           uint32_t record_length)
{
    memset(&s_scda_ring_pending, 0, sizeof(s_scda_ring_pending));
    s_scda_ring_pending.armed = 1;
    s_scda_ring_pending.object = object;
    s_scda_ring_pending.token = BRIDGE_MEM32(object + 0x3Cu);
    s_scda_ring_pending.callsite = callsite;
    s_scda_ring_pending.writer = writer;
    s_scda_ring_pending.cursor_before = cursor_before;
    s_scda_ring_pending.buffer_size = buffer_size;
    s_scda_ring_pending.remaining = remaining;
    s_scda_ring_pending.wrap = wrap;
    s_scda_ring_pending.record_tag = record_tag;
    s_scda_ring_pending.record_length = record_length;
}

void scda_ring_trace_state(uint32_t object, uint32_t state,
                           uint32_t cursor_before, uint32_t cursor_after,
                           uint32_t remaining, uint32_t wrap,
                           uint32_t record_tag, uint32_t record_length,
                           uint32_t stack)
{
    uint32_t token = BRIDGE_MEM32(object + 0x3Cu);
    const char *type;
    char filename[96];
    FILE *f = NULL;
    uint32_t i;
    if (!s_scda_ring_common_token || token != s_scda_ring_common_token)
        return;
    type = state == 1 ? "CHECK" : state == 2 ? "PARSE" : "WRAP";
    if (s_scda_ring_read_index == 1u &&
        s_scda_ring_state_stack_count < 128u) {
        s_scda_ring_state_stack_count++;
        _snprintf_s(filename, sizeof(filename), _TRUNCATE,
                    "scda_ring_state_stack_311_%lu.tsv",
                    (unsigned long)GetCurrentProcessId());
        if (scda_ring_files_enabled() &&
            fopen_s(&f, filename, "a") == 0 && f) {
            fprintf(f, "%u\t%s\t0x%08X\t0x%08X\t0x%08X",
                    s_scda_ring_state_stack_count, type, cursor_before,
                    cursor_after, stack);
            /* Update 311: 24 DWORDs reached the immediate decoder method but
             * not its virtual-dispatch caller.  Keep the same 128-event cap
             * and extend only this bounded diagnostic stack window. */
            for (i = 0; i < 96u; i++)
                fprintf(f, "\t0x%08X", BRIDGE_MEM32(stack + i * 4u));
            fputc('\n', f);
            fclose(f);
        }
    }
    memset(&s_scda_ring_pending, 0, sizeof(s_scda_ring_pending));
    scda_ring_trace_write(type, object, cursor_before, cursor_after,
                          BRIDGE_MEM32(object + 0x4058u), remaining, wrap,
                          record_tag, record_length, 0);
}

void scda_ring_trace_boundary(uint32_t object, uint32_t entering,
                              uint32_t requested_or_remaining,
                              uint32_t caller, uint32_t copied)
{
    uint32_t token = BRIDGE_MEM32(object + 0x3Cu);
    uint32_t invocation;
    scda_ring_boundary_record *record;
    if (!s_scda_ring_common_token || token != s_scda_ring_common_token)
        return;
    if (entering)
        s_scda_ring_invocation_index++;
    invocation = s_scda_ring_invocation_index;
    if (!s_scda_ring_after_short) {
        record = &s_scda_ring_boundary_history[
            s_scda_ring_boundary_history_pos % SCDA_RING_BOUNDARY_HISTORY];
        record->entering = entering;
        record->requested_or_remaining = requested_or_remaining;
        record->caller = caller;
        record->copied = copied;
        record->cursor = BRIDGE_MEM32(object + 0x4060u);
        record->invocation = invocation;
        s_scda_ring_boundary_history_pos++;
        if (s_scda_ring_boundary_history_count < SCDA_RING_BOUNDARY_HISTORY)
            s_scda_ring_boundary_history_count++;
        return;
    }
    if (s_scda_ring_boundary_count >= 512u)
        return;
    s_scda_ring_boundary_count++;
    memset(&s_scda_ring_pending, 0, sizeof(s_scda_ring_pending));
    scda_ring_trace_write(entering ? "CALL" : "RETURN", object,
                          BRIDGE_MEM32(object + 0x4060u),
                          BRIDGE_MEM32(object + 0x4060u),
                          BRIDGE_MEM32(object + 0x4058u),
                          requested_or_remaining, 0, caller, copied,
                          invocation);
}

void scda_ring_trace_output(uint32_t object, uint32_t output)
{
    uint32_t token = BRIDGE_MEM32(object + 0x3Cu);
    if (!s_scda_ring_after_short || !s_scda_ring_common_token ||
        token != s_scda_ring_common_token || !output ||
        s_scda_ring_boundary_count >= 32u)
        return;
    memset(&s_scda_ring_pending, 0, sizeof(s_scda_ring_pending));
    scda_ring_trace_write("OUTPUT", object,
                          BRIDGE_MEM32(object + 0x4060u),
                          BRIDGE_MEM32(object + 0x4060u),
                          BRIDGE_MEM32(object + 0x4058u), 0, 0,
                          output, BRIDGE_MEM32(output),
                          s_scda_ring_invocation_index);
}

void scda_ring_trace_callstack(uint32_t object, uint32_t stack,
                               uint32_t output, uint32_t requested)
{
    char filename[96];
    FILE *f = NULL;
    uint32_t token = BRIDGE_MEM32(object + 0x3Cu);
    uint32_t i;
    if (s_scda_ring_after_first_read && s_scda_ring_read_index == 1u &&
        s_scda_ring_pre_second_count < 8192u && s_scda_ring_common_token &&
        token == s_scda_ring_common_token) {
        s_scda_ring_pre_second_count++;
        _snprintf_s(filename, sizeof(filename), _TRUNCATE,
                    "scda_ring_read1_to_read2_307_%lu.tsv",
                    (unsigned long)GetCurrentProcessId());
        if (scda_ring_files_enabled() &&
            fopen_s(&f, filename, "a") == 0 && f) {
            uint32_t cursor = BRIDGE_MEM32(object + 0x4060u);
            uint32_t buffer_size = BRIDGE_MEM32(object + 0x4058u);
            uint32_t cache_offset = BRIDGE_MEM32(object + 0x4Cu);
            uint32_t cache_remaining = BRIDGE_MEM32(object + 0x50u);
            uint32_t source_byte = cache_remaining ?
                BRIDGE_MEM8(object + 0x58u + cache_offset) : 0xFFFFFFFFu;
            fprintf(f,
                    "%u\t%u\t%lu\t0x%08X\t0x%08X\t0x%08X\t"
                    "0x%08X\t0x%08X\t0x%08X\t0x%08X\t0x%08X\t"
                    "0x%08X\t0x%08X",
                    s_scda_ring_pre_second_count,
                    s_scda_ring_invocation_index,
                    (unsigned long)GetCurrentThreadId(), requested, output,
                    cursor, buffer_size, buffer_size - cursor,
                    BRIDGE_MEM32(object + 0x48u), cache_offset,
                    cache_remaining, source_byte, stack);
            for (i = 0; i < 24u; i++)
                fprintf(f, "\t0x%08X", BRIDGE_MEM32(stack + i * 4u));
            fputc('\n', f);
            fclose(f);
            f = NULL;
        }
    }
    if (s_scda_ring_after_first_read && s_scda_ring_post_first_count < 64u &&
        s_scda_ring_common_token && token == s_scda_ring_common_token) {
        s_scda_ring_post_first_count++;
        _snprintf_s(filename, sizeof(filename), _TRUNCATE,
                    "scda_ring_postread1_306_%lu.tsv",
                    (unsigned long)GetCurrentProcessId());
        if (scda_ring_files_enabled() &&
            fopen_s(&f, filename, "a") == 0 && f) {
            fprintf(f, "%u\t0x%08X\t0x%08X\t0x%08X",
                    s_scda_ring_post_first_count, requested, output, stack);
            for (i = 0; i < 24u; i++)
                fprintf(f, "\t0x%08X", BRIDGE_MEM32(stack + i * 4u));
            fputc('\n', f);
            fclose(f);
            f = NULL;
        }
    }
    if (!s_scda_ring_after_short || !s_scda_ring_common_token ||
        token != s_scda_ring_common_token ||
        s_scda_ring_callstack_count >= 16u)
        return;
    s_scda_ring_callstack_count++;
    _snprintf_s(filename, sizeof(filename), _TRUNCATE,
                "scda_ring_callstack_304_%lu.tsv",
                (unsigned long)GetCurrentProcessId());
    if (!scda_ring_files_enabled() ||
        fopen_s(&f, filename, "a") != 0 || !f)
        return;
    fprintf(f, "%u\t%u\t0x%08X\t0x%08X\t0x%08X",
            s_scda_ring_callstack_count, s_scda_ring_invocation_index,
            requested, output, stack);
    for (i = 0; i < 20u; i++)
        fprintf(f, "\t0x%08X", BRIDGE_MEM32(stack + i * 4u));
    fputc('\n', f);
    fclose(f);
}

void scda_ring_trace_refillstack(uint32_t object, uint32_t callsite,
                                 uint32_t stack)
{
    char filename[96];
    FILE *f = NULL;
    uint32_t token = BRIDGE_MEM32(object + 0x3Cu);
    const char *path = scda_ring_path_for_token(token);
    uint32_t i;
    if (!scda_ring_is_common_lin(path) ||
        s_scda_ring_refillstack_count >= 32u)
        return;
    s_scda_ring_refillstack_count++;
    _snprintf_s(filename, sizeof(filename), _TRUNCATE,
                "scda_ring_refillstack_305_%lu.tsv",
                (unsigned long)GetCurrentProcessId());
    if (!scda_ring_files_enabled() ||
        fopen_s(&f, filename, "a") != 0 || !f)
        return;
    fprintf(f, "%u\t%u\t0x%08X\t0x%08X\t0x%08X",
            s_scda_ring_refillstack_count, s_scda_ring_read_index + 1u,
            callsite, object, stack);
    for (i = 0; i < 24u; i++)
        fprintf(f, "\t0x%08X", BRIDGE_MEM32(stack + i * 4u));
    fputc('\n', f);
    fclose(f);
}

static void scda_ring_trace_flush_boundary_history(uint32_t object)
{
    scda_ring_trace_pending saved = s_scda_ring_pending;
    uint32_t first, i;
    memset(&s_scda_ring_pending, 0, sizeof(s_scda_ring_pending));
    first = s_scda_ring_boundary_history_pos -
            s_scda_ring_boundary_history_count;
    for (i = 0; i < s_scda_ring_boundary_history_count; i++) {
        scda_ring_boundary_record *record =
            &s_scda_ring_boundary_history[(first + i) %
                                          SCDA_RING_BOUNDARY_HISTORY];
        scda_ring_trace_write(record->entering ? "PRECALL" : "PRERETURN",
                              object, record->cursor, record->cursor,
                              BRIDGE_MEM32(object + 0x4058u),
                              record->requested_or_remaining, 0,
                              record->caller, record->copied,
                              record->invocation);
    }
    s_scda_ring_pending = saved;
    s_scda_ring_boundary_history_count = 0;
    s_scda_ring_after_short = 1;
    s_scda_ring_boundary_count = 0;
    s_scda_ring_callstack_count = 0;
}

void scda_ring_trace_commit(uint32_t object, uint32_t writer)
{
    if (!s_scda_ring_pending.matched)
        return;
    scda_ring_trace_write("READ", object,
                          s_scda_ring_pending.cursor_before,
                          BRIDGE_MEM32(object + 0x4060u),
                          s_scda_ring_pending.buffer_size,
                          s_scda_ring_pending.remaining,
                          s_scda_ring_pending.wrap,
                          s_scda_ring_pending.record_tag,
                          s_scda_ring_pending.record_length,
                          writer);
    if (s_scda_ring_read_index == 1u) {
        s_scda_ring_after_first_read = 1;
        s_scda_ring_post_first_count = 0;
        s_scda_ring_pre_second_count = 0;
        s_scda_ring_state_stack_count = 0;
    }
    memset(&s_scda_ring_pending, 0, sizeof(s_scda_ring_pending));
}

static uint32_t bridge_handle_token(HANDLE h)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == h) return BRIDGE_HANDLE_TAG | (uint32_t)i;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == NULL) {
            s_handle_table[i] = h;
            return BRIDGE_HANDLE_TAG | (uint32_t)i;
        }
    fprintf(stderr, "  [BRIDGE] handle table full\n");
    return 0;
}

/* Store a native HANDLE into a 32-bit Xbox memory slot (as a token). */
static void bridge_write_handle(uint32_t handle_va, HANDLE h)
{
    if (handle_va)
        BRIDGE_MEM32(handle_va) = bridge_handle_token(h);
}

/* Resolve a 32-bit Xbox handle slot back to a native HANDLE. */
static HANDLE bridge_read_handle(uint32_t va)
{
    uint32_t token = BRIDGE_MEM32(va);
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged value: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

/* Resolve a token to a HANDLE and release its table slot (for NtClose). */
/* Resolve a handle token passed BY VALUE, without consuming it.
 *
 * Three accessors, easily confused, and confusing two of them broke all file
 * I/O: bridge_read_handle(va) reads a token *from memory* and suits a PHANDLE
 * out-parameter; bridge_take_handle(token) resolves and CLEARS the table slot,
 * which is NtClose semantics; this one resolves and leaves the slot alone,
 * which is what every by-value HANDLE argument needs.
 *
 * NtSetInformationFile and friends take the handle by value, but were calling
 * bridge_read_handle on it -- dereferencing the token as if it were an address.
 * Halo created its save file successfully and then failed the very next call,
 * which surfaced as "couldn't open or create saved game file". */
static HANDLE bridge_resolve_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

static HANDLE bridge_take_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX) {
            HANDLE h = s_handle_table[i];
            s_handle_table[i] = NULL;
            s_handle_paths[i][0] = 0;
            return h;
        }
    }
    return NULL;   /* untagged -> not a table handle, do not close */
}

/* Build a native OBJECT_ATTRIBUTES wrapping the translated Xbox path. */
static void bridge_build_oa(uint32_t obj_attrs_va,
                            XBOX_OBJECT_ATTRIBUTES* oa, XBOX_ANSI_STRING* name)
{
    const char* path = bridge_get_xbox_path(obj_attrs_va);
    name->Buffer        = (PCHAR)path;
    name->Length        = path ? (USHORT)strlen(path) : 0;
    name->MaximumLength = (USHORT)(name->Length + 1);
    oa->RootDirectory = NULL;
    oa->ObjectName    = name;
    oa->Attributes    = 0;
}

/* Open a file by delegating to the ported xbox_NtCreateFile kernel HLE. */
static NTSTATUS bridge_create_file_impl(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;
    XBOX_IO_STATUS_BLOCK   ios;
    HANDLE   h  = NULL;
    NTSTATUS st;

    bridge_build_oa(obj_attrs_va, &oa, &name);
    if (!name.Buffer) {
        bridge_write_iostatus(iostatus_va, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    memset(&ios, 0, sizeof(ios));

    st = xbox_NtCreateFile(&h, access, &oa, &ios, NULL,
                           file_attrs, share, disposition, options);

    if (NT_SUCCESS(st)) {
        bridge_write_handle(handle_va, h);
        if (handle_va) {
            uint32_t token = BRIDGE_MEM32(handle_va);
            uint32_t index = token & BRIDGE_HANDLE_MASK;
            if ((token & ~BRIDGE_HANDLE_MASK) == BRIDGE_HANDLE_TAG &&
                    index > 0 && index < BRIDGE_HANDLE_MAX) {
                strncpy_s(s_handle_paths[index], sizeof(s_handle_paths[index]),
                          name.Buffer, _TRUNCATE);
            }
        }
        bridge_write_iostatus(iostatus_va, ios.Status, (uint32_t)ios.Information);
    } else {
        bridge_write_iostatus(iostatus_va, st, 0);
    }

    /* Bink opens mark the current boot boundary very precisely. Keep this
     * diagnostic in the shared helper so it also covers IoCreateFile (SCDA
     * uses that route for the post-logo probes, while the older [FILE] line
     * below only covered NtCreateFile). The host path is the exact path the
     * kernel translator last attempted; logging it is observational only. */
    {
        size_t path_len = name.Buffer ? strlen(name.Buffer) : 0;
        if (path_len >= 4 && _stricmp(name.Buffer + path_len - 4, ".bik") == 0) {
            uint32_t token = (NT_SUCCESS(st) && handle_va)
                ? BRIDGE_MEM32(handle_va) : 0;
            const wchar_t *host_path = xbox_LastHostPath();
            fprintf(stderr,
                    "  [BIK-OPEN] source=0x%08X guest=\"%s\" host=\"%ls\" "
                    "status=0x%08X ios=0x%08X info=%llu token=0x%08X\n",
                    (unsigned)g_icall_source, name.Buffer,
                    host_path ? host_path : L"",
                    (unsigned)st, (unsigned)ios.Status,
                    (unsigned long long)ios.Information, (unsigned)token);
            fflush(stderr);
        }
    }
    return st;
}

/* ── RtlInitAnsiString (ordinal 289) ──────────────────────
 * VOID RtlInitAnsiString(PANSI_STRING Destination, PCSZ Source)
 *
 * Fills an ANSI_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; }.
 * Unbridged this returned 0 and wrote nothing, so every path a title built
 * this way arrived at NtCreateFile as a null Buffer and failed with
 * STATUS_OBJECT_PATH_NOT_FOUND -- which looks like a missing file rather than
 * a missing bridge. Halo builds its map paths exactly this way.
 */
/* -- RtlEqualString (ordinal 279, 3 args) ----------------
 * BOOLEAN RtlEqualString(PSTRING String1, PSTRING String2, BOOLEAN CaseInSens)
 *
 * The fields are read out by hand rather than casting the guest struct. A
 * guest ANSI_STRING is {USHORT Length, USHORT MaximumLength, 32-bit Buffer},
 * eight bytes; the native one has a 64-bit PCHAR, so a cast would read
 * MaximumLength and Buffer from the wrong offsets and then dereference a guest
 * VA as a host address. RtlInitAnsiString stores a guest VA in that field --
 * see the bridge below -- so it has to be translated, not passed through.
 *
 * Stubbed, this returned 0: "never equal". Wreckless initialises a string and
 * compares it in a critical-section-protected lookup, so every comparison
 * missing turned that lookup into unbounded recursion and the process died of
 * a host stack overflow 200 kernel calls in.
 */
static void bridge_RtlEqualString(void)
{
    uint32_t s1_va  = STACK_ARG(0);
    uint32_t s2_va  = STACK_ARG(1);
    uint32_t nocase = STACK_ARG(2);
    XBOX_ANSI_STRING a, b;

    if (!s1_va || !s2_va) {
        g_eax = 0;
        return;
    }
    a.Length        = BRIDGE_MEM16(s1_va + 0);
    a.MaximumLength = BRIDGE_MEM16(s1_va + 2);
    a.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s1_va + 4));
    b.Length        = BRIDGE_MEM16(s2_va + 0);
    b.MaximumLength = BRIDGE_MEM16(s2_va + 2);
    b.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(s2_va + 4));

    if (!a.Buffer || !b.Buffer) {
        g_eax = 0;
        return;
    }
    g_eax = xbox_RtlEqualString(&a, &b, (BOOLEAN)nocase) ? 1 : 0;
}

/* -- RtlUpperChar (ordinal 316, 1 arg) -------------------
 * CHAR RtlUpperChar(CHAR Character)
 *
 * SCDA's UI loader calls it while parsing its first .mgb screen (after the
 * Core.int localisation probe). With no bridge and no argument count the
 * generic stub returned 0 and left the pushed character on the guest stack,
 * so sub_002A158E came back with ebx/esi/edi rotated and the parser ran on
 * character data ('m', 'e') as object pointers (docs Update 347).
 *
 * The Xbox kernel's ANSI code page is 1252: a-z plus the Latin-1 lowercase
 * letters 0xE0-0xFE (except 0xF7, the division sign) map down by 0x20, and
 * the three 1252-only letters s/oe/z-caron and y-diaeresis map to their
 * uppercase slots. Everything else is returned unchanged.
 */
static void bridge_RtlUpperChar(void)
{
    uint8_t c = (uint8_t)STACK_ARG(0);

    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
        c = (uint8_t)(c - 0x20);
    else if (c == 0x9A || c == 0x9C || c == 0x9E)
        c = (uint8_t)(c - 0x10);
    else if (c == 0xFF)
        c = 0x9F;
    g_eax = (g_eax & 0xFFFFFF00u) | c;
}

static void bridge_RtlInitAnsiString(void)
{
    uint32_t dest_va = STACK_ARG(0);
    uint32_t src_va  = STACK_ARG(1);

    if (!dest_va) {
        g_eax = 0;
        return;
    }
    if (src_va) {
        const char *src = (const char *)XBOX_TO_NATIVE(src_va);
        size_t len = strlen(src);
        if (len > 0xFFFE) {
            len = 0xFFFE;
        }
        BRIDGE_MEM16(dest_va + 0) = (uint16_t)len;
        BRIDGE_MEM16(dest_va + 2) = (uint16_t)(len + 1);
        BRIDGE_MEM32(dest_va + 4) = src_va;
    } else {
        BRIDGE_MEM16(dest_va + 0) = 0;
        BRIDGE_MEM16(dest_va + 2) = 0;
        BRIDGE_MEM32(dest_va + 4) = 0;
    }
    g_eax = 0;
}

/* ── NtCreateFile (ordinal 190, 9 args = 36 bytes) ─────── */
static void bridge_NtCreateFile(void)
{
    uint32_t handle_va   = STACK_ARG(0);  /* PHANDLE */
    uint32_t access      = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs   = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus    = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    /* arg4: AllocationSize - ignored */
    uint32_t file_attrs  = STACK_ARG(5);  /* FileAttributes */
    uint32_t share       = STACK_ARG(6);  /* ShareAccess */
    uint32_t disposition = STACK_ARG(7);  /* CreateDisposition */
    uint32_t options     = STACK_ARG(8);  /* CreateOptions */

    /* The out-parameter addresses matter as much as the result: this bridge
     * hands them to a real Win32 call, so a bogus one has Windows itself write
     * into Xbox memory. That is how a wild write ends up with a stack inside
     * ntdll and no recompiled frame to blame. */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);

    /* An FMV the host can decode itself.
     *
     * The title's own decoder is emulated like everything else, but it only
     * produces pixels once there is something to execute its GPU work -- so on
     * a bring-up where that does not exist yet, the video the game just asked
     * for can still be shown. The trigger is the title opening the file, so
     * this plays when the game decides to play it, not on a timer, and it
     * plays the file the game chose.
     *
     * Off unless RECOMP_FMV_HOST is set: it is a substitute for the title's
     * own output, and that should be a decision rather than a default. */
    if (g_eax == 0 && getenv("RECOMP_FMV_HOST")) {
        /* Declared here rather than included: the player lives in xbox_video,
         * which links xbox_d3d8, and having the kernel include its header
         * would make the dependency circular for no gain. Both land in the
         * same executable. */
        extern int xbox_VideoPlayFile(const char *host_path);
        extern int xbox_VideoIsPlaying(void);

        char host[MAX_PATH * 2];
        size_t n = 0;

        const wchar_t *w = xbox_LastHostPath();

        while (n < sizeof(host) - 1 && w[n]) {
            host[n] = (char)w[n];
            n++;
        }
        host[n] = 0;
        if (n > 4 && _stricmp(host + n - 4, ".wmv") == 0
                && !xbox_VideoIsPlaying())
            xbox_VideoPlayFile(host);
    }

    /* Paired with the [PATH] line the translation just printed: that says what
     * was asked for, this says whether it opened. A failed open is not itself
     * a bug -- a title probing the cache partition before the disc expects one
     * -- so the status is what separates a probe from a real miss. */
    fprintf(stderr, "  [FILE] -> 0x%08X%s (access=0x%08X disp=%u opts=0x%08X)\n",
            g_eax, g_eax ? " FAILED" : "", access, disposition, options);
    fflush(stderr);
}

/* ── NtOpenFile (ordinal 202, 6 args = 24 bytes) ──────── */
static void bridge_NtOpenFile(void)
{
    uint32_t handle_va = STACK_ARG(0);  /* PHANDLE */
    uint32_t access    = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus  = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    uint32_t share     = STACK_ARG(4);  /* ShareAccess */
    uint32_t options   = STACK_ARG(5);  /* OpenOptions */

    /* NtOpenFile = NtCreateFile with FILE_OPEN disposition */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        0, share, 1 /* FILE_OPEN */, options);
    /* Same pairing as NtCreateFile's [FILE] line: directory opens for
     * FindFirstFile / free-space queries come through here. */
    fprintf(stderr, "  [FILE] -> 0x%08X%s (NtOpenFile access=0x%08X opts=0x%08X)\n",
            g_eax, g_eax ? " FAILED" : "", access, options);
    /* RECOMP_KERNEL_FOLLOW_DIR=N: log the next N kernel calls after a
     * directory open (ordinal, return site, result). Save-game code opens
     * the container and then gives up without another logged file call
     * (docs Update 355); this names what it asked for instead. */
    if (options & 1u /* FILE_DIRECTORY_FILE */) {
        static int follow = -1;
        if (follow < 0) {
            const char *e = getenv("RECOMP_KERNEL_FOLLOW_DIR");
            follow = e ? atoi(e) : 0;
        }
        g_kernel_follow_left = follow;
    }
}

/*
 * Completion for a file request that carried an Event or an APC routine.
 *
 * Both bridges below do the I/O synchronously, and used to drop args 1-3
 * (Event, ApcRoutine, ApcContext) on the floor. A title that issues an async
 * request and waits alertably for the completion then waits forever: Halo's
 * cache-partition setup does exactly that, gives up after its 5-second SleepEx,
 * and asserts "setup for new cache file failed (#0)".
 *
 * ponytail: the APC runs inline here rather than at the next alertable wait.
 * The data really is ready by then, so the observable result matches; a title
 * that depends on the APC *not* having run yet would notice. A per-thread
 * deferred queue drained at alertable waits was tried for Halo's map streamer
 * and made no difference (it still issues one 14 KB batch and stops), so it was
 * dropped rather than risk changing this shared path for the other titles.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

static void deliver_one_apc(uint32_t apc_routine, uint32_t apc_context,
                            uint32_t iostatus)
{
    /* The APC can be game code or a kernel export. Halo's XAPI passes the
     * latter -- 0xFE0000FC, one of our own synthetic thunk VAs -- so the recomp
     * dispatch correctly fails to find it and the kernel fallback is the one
     * that matters. Checking only recomp_lookup left it undelivered. */
    recomp_func_t fn = recomp_lookup(apc_routine);
    if (!fn) fn = recomp_lookup_manual(apc_routine);
    if (!fn) fn = recomp_lookup_kernel(apc_routine);
    if (fn) {
        /* VOID ApcRoutine(PVOID ApcContext, PIO_STATUS_BLOCK, ULONG) */
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = apc_context;
        g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;   /* dummy return address */
        fn();
        g_esp += 12;
    } else {
        uint32_t ord = 0;
        if (apc_routine >= KERNEL_VA_BASE && apc_routine < KERNEL_VA_END) {
            ord = g_slot_ordinals[(apc_routine - KERNEL_VA_BASE) / 4];
        }
        fprintf(stderr, "  [KERNEL] file I/O APC 0x%08X unresolved"
                " (kernel ordinal %u)\n", apc_routine, ord);
        fflush(stderr);
    }
}

/* Per-thread pending-APC ring. An APC is delivered on the thread that issued
 * the request, which is also the thread that waits, so thread-local is right. */
static void bridge_complete_file_io(uint32_t event_token, uint32_t apc_routine,
                                    uint32_t apc_context, uint32_t iostatus)
{
    if (event_token) {
        HANDLE ev = bridge_resolve_handle(event_token);
        if (ev) SetEvent(ev);
    }
    if (apc_routine) {
        deliver_one_apc(apc_routine, apc_context, iostatus);
    }
}

/* -- XeLoadSection / XeUnloadSection (ordinals 327/328, 1 arg = 4 bytes) --
 *
 * NTSTATUS XeLoadSection(PXBE_SECTION_HEADER Section);
 *
 * On hardware a section marked non-preload is paged in from disc on demand,
 * and a title that keeps its video decoder in one -- Wreckless keeps WMVDEC
 * there -- calls this before touching it. Every section is already resident
 * here, so the work is the bookkeeping: hand back success and keep the
 * reference count the title can read.
 *
 * Done against guest memory rather than the PXBE_SECTION_HEADER struct: the
 * on-disc header is nine 32-bit fields and a digest, and the native struct
 * declares some of them as pointers, so on x64 its layout is not the 56 bytes
 * actually there.
 *
 *   +0x14  section name address      +0x18  section reference count
 */
#define XBE_SECTION_REFCOUNT_OFFSET 0x18

static void bridge_XeSection(int load)
{
    uint32_t section = STACK_ARG(0);
    uint32_t count;

    if (!section) {
        g_eax = 0xC000000Du;              /* STATUS_INVALID_PARAMETER */
        return;
    }
    count = BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET);
    if (load)
        count++;
    else if (count)
        count--;
    BRIDGE_MEM32(section + XBE_SECTION_REFCOUNT_OFFSET) = count;

    if (KERNEL_LOG_ON())
        fprintf(stderr, "  [XBE] Xe%sSection(0x%08X) refcount=%u\n",
                load ? "Load" : "Unload", section, count);
    g_eax = 0;
}

static void bridge_XeLoadSection(void)   { bridge_XeSection(1); }
static void bridge_XeUnloadSection(void) { bridge_XeSection(0); }

/* -- RtlUnwind (ordinal 312, 4 args = 16 bytes) -------------------------
 *
 * VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
 *                PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
 *
 * Discards the SEH registration frames between the current one and
 * TargetFrame, letting each handler run its __finally blocks on the way past,
 * and leaves fs:[0] pointing at TargetFrame. fs:[0] is guest address 0 here,
 * because the runtime models the TIB at the bottom of guest memory.
 *
 * Left unbridged this returned 0 without touching anything, which is not a
 * harmless stub: MSVC's _global_unwind2 calls it and then carries on as if the
 * frames were gone, so the chain kept pointing into stack that had already
 * been reused and the next dispatch walked records built out of live locals.
 *
 * The walk is bounded and checked rather than trusting the chain, since it
 * lives in guest stack memory that a title can corrupt: records must climb
 * toward the stack top, stay inside the stack, and stay 4-byte aligned. A
 * chain that breaks any of those is truncated instead of followed.
 */
#define XBOX_SEH_END_OF_CHAIN 0xFFFFFFFFu
#define XBOX_EXCEPTION_UNWINDING  0x02u
#define XBOX_EXCEPTION_EXIT_UNWIND 0x04u
#define XBOX_SEH_MAX_FRAMES 64

static void bridge_RtlUnwind(void)
{
    uint32_t target_frame = STACK_ARG(0);
    uint32_t exc_record   = STACK_ARG(2);
    uint32_t reg          = BRIDGE_MEM32(XBOX_FS_BASE);
    uint32_t prev_reg     = 0;
    uint32_t scratch      = 0;
    int      guard;

    /* An unwind with no record of its own still has to tell the handlers it
     * is an unwind, so synthesise one below the stack pointer. */
    if (!exc_record) {
        g_esp -= 0x50;
        scratch = g_esp;
        memset((uint8_t *)XBOX_TO_NATIVE(scratch), 0, 0x50);
        BRIDGE_MEM32(scratch) = 0xC0000027u;   /* STATUS_UNWIND */
        exc_record = scratch;
    }
    BRIDGE_MEM32(exc_record + 4) |= XBOX_EXCEPTION_UNWINDING
        | (target_frame ? 0u : XBOX_EXCEPTION_EXIT_UNWIND);

    for (guard = 0; guard < XBOX_SEH_MAX_FRAMES; guard++) {
        uint32_t next, handler;

        if (reg == XBOX_SEH_END_OF_CHAIN || reg == 0 || reg == target_frame)
            break;
        if ((reg & 3u) || reg < XBOX_STACK_BASE || reg >= XBOX_STACK_TOP)
            break;                       /* not a stack frame: chain is broken */
        if (prev_reg && reg <= prev_reg)
            break;                       /* not climbing: cycle or corruption */

        next    = BRIDGE_MEM32(reg);
        handler = BRIDGE_MEM32(reg + 4);

        /* Pop before dispatching. The handler may raise, and it must not see
         * its own frame still on the chain. */
        BRIDGE_MEM32(XBOX_FS_BASE) = next;

        if (handler) {
            recomp_func_t fn = recomp_lookup(handler);
            if (!fn) fn = recomp_lookup_manual(handler);
            if (!fn) fn = recomp_lookup_kernel(handler);
            if (fn) {
                /* EXCEPTION_DISPOSITION handler(record, frame, context,
                 * dispatcher) -- cdecl, so the caller pops. */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = reg;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = exc_record;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;  /* return address */
                fn();
                /* 16, not 20: the handler's own `ret` has already taken the
                 * return address off, leaving just the four arguments for the
                 * caller to drop. Cleaning 20 leaves esp four bytes high, and
                 * every argument the unwound-into frame reads after that comes
                 * from one slot over. */
                g_esp += 16;
            }
        }

        prev_reg = reg;
        reg      = next;
    }

    /* Land on the target even if the walk stopped early: leaving fs:[0] on a
     * discarded frame is worse than losing a __finally. */
    if (target_frame && target_frame != XBOX_SEH_END_OF_CHAIN)
        BRIDGE_MEM32(XBOX_FS_BASE) = target_frame;

    if (scratch)
        g_esp += 0x50;
}

/* ── NtReadFile (ordinal 219, 8 args = 32 bytes) ──────── */
static void bridge_NtReadFile(void)
{
    uint32_t handle_token = STACK_ARG(0);
    HANDLE   handle    = bridge_resolve_handle(handle_token);
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    LARGE_INTEGER  current;
    LARGE_INTEGER  zero;
    uint64_t trace_offset = UINT64_MAX;
    const char *trace_path = scda_ring_path_for_token(handle_token);
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
        trace_offset = (uint64_t)off.QuadPart;
    } else if (handle && handle != INVALID_HANDLE_VALUE) {
        zero.QuadPart = 0;
        if (SetFilePointerEx(handle, zero, &current, FILE_CURRENT))
            trace_offset = (uint64_t)current.QuadPart;
    }
    LARGE_INTEGER mr_t0, mr_t1, mr_t2;
    QueryPerformanceCounter(&mr_t0);
    g_eax = (uint32_t)xbox_NtReadFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    QueryPerformanceCounter(&mr_t1);

    if (s_scda_ring_pending.armed && scda_ring_is_common_lin(trace_path)) {
        s_scda_ring_common_token = handle_token;
        s_scda_ring_read_index++;
        s_scda_ring_pending.matched = 1;
        s_scda_ring_pending.read_index = s_scda_ring_read_index;
        s_scda_ring_pending.file_offset = trace_offset;
        s_scda_ring_pending.requested = length;
        s_scda_ring_pending.actual = (uint32_t)ios.Information;
        s_scda_ring_pending.status = (uint32_t)ios.Status;
        if (!s_scda_ring_after_short && ios.Information < length)
            scda_ring_trace_flush_boundary_history(
                s_scda_ring_pending.object);
    }

    /* What a read actually delivered. A decoder that rejects its input cannot
     * say whether the bytes were wrong or the read was, and the two look
     * identical from inside the title -- the first bytes settle it. */
    {
        const uint8_t *p = (const uint8_t *)XBOX_TO_NATIVE(buffer_va);
        uint32_t got = (uint32_t)ios.Information;
        fprintf(stderr, "  [READ] token=0x%08X handle=%p want=%u got=%u st=0x%08X %02X %02X %02X %02X\n",
                handle_token, handle, length, got, (uint32_t)ios.Status,
                got > 0 ? p[0] : 0, got > 1 ? p[1] : 0,
                got > 2 ? p[2] : 0, got > 3 ? p[3] : 0);
        fflush(stderr);
        QueryPerformanceCounter(&mr_t2);
        /* [MUSIC-READ] (docs Update 621): SCDA streams its music as 4608-byte
         * packets (48 ms of 16-bit mono each); a late packet is played twice
         * (the stutter). Per handle: the gap since the previous packet read,
         * the host read time and the time spent logging it, and the thread. */
        if (length == 4608) {
            static struct { uint32_t token; int64_t last; } mr[8];
            static LARGE_INTEGER f;
            static unsigned logged;
            int k;
            if (!f.QuadPart)
                QueryPerformanceFrequency(&f);
            for (k = 0; k < 8 && mr[k].token && mr[k].token != handle_token; k++)
                ;
            if (k < 8) {
                if (mr[k].token == handle_token && mr[k].last) {
                    double gap = (mr_t0.QuadPart - mr[k].last) * 1000.0 / f.QuadPart;
                    double rd = (mr_t1.QuadPart - mr_t0.QuadPart) * 1000.0 / f.QuadPart;
                    double lg = (mr_t2.QuadPart - mr_t1.QuadPart) * 1000.0 / f.QuadPart;
                    if ((gap > 60.0 || rd > 5.0 || lg > 5.0) &&
                        (logged++ < 300 || (logged & (logged - 1)) == 0))
                        fprintf(stderr, "[MUSIC-READ] token 0x%08X tid %lu: %.1f ms "
                                "since last packet, read %.2f ms, log %.2f ms, "
                                "tick %lu\n", handle_token, GetCurrentThreadId(),
                                gap, rd, lg, (unsigned long)GetTickCount());
                }
                mr[k].token = handle_token;
                mr[k].last = mr_t0.QuadPart;
            }
        }
        /* RECOMP_CACHE_TRACE=1 (docs Update 446): sound-stream reads with
         * file and offset, so the offsets can be matched to the stream
         * descriptors ([SCDA-STREAM], +0x10 = file offset) -- which streams
         * actually play when the action music should start. */
        {
            static int st = -1;
            if (st < 0)
                st = getenv("RECOMP_CACHE_TRACE") != NULL;
            if (st && trace_path) {
                size_t pl = strlen(trace_path);
                if (pl > 4 && (_stricmp(trace_path + pl - 4, ".SS2") == 0 ||
                               _stricmp(trace_path + pl - 4, ".LS2") == 0)) {
                    const char *bn = strrchr(trace_path, '\\');
                    fprintf(stderr, "  [SREAD] file=%s off=0x%llX len=0x%X got=0x%X "
                            "tick=%lu\n", bn ? bn + 1 : trace_path,
                            (unsigned long long)trace_offset, length, got,
                            (unsigned long)GetTickCount());
                }
            }
        }
        if (bridge_wait_trace_on()) {
            fprintf(stderr, "SCDA-DIAG234N tid=%lu read token=0x%08X tick=%lu source=0x%08X "
                    "offset_va=0x%08X offset=%lld\n",
                    (unsigned long)GetCurrentThreadId(), handle_token,
                    (unsigned long)GetTickCount(), (uint32_t)g_icall_source,
                    offset_va, poff ? (long long)poff->QuadPart : -1LL);
            fflush(stderr);
        }
        /* Diagnostic only (2026-09-18): stderr redirection from a
         * background-launched console app has repeatedly proven unreliable
         * in this environment, unlike xbox_log's own file sink -- mirror
         * the failing-handle case there too so it survives that flakiness. */
        if (!handle)
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                     "bridge_NtReadFile: token=0x%08X resolved to NULL handle "
                     "(want=%u)", handle_token, length);
    }
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);
}

/* ── NtWriteFile (ordinal 236, 8 args = 32 bytes) ─────── */
static void bridge_NtWriteFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    bridge_complete_file_io(STACK_ARG(1), STACK_ARG(2), STACK_ARG(3),
                            iostatus);
}

/* ── NtQueryInformationFile (ordinal 211, 5 args = 20 bytes) */
static void bridge_NtQueryInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtSetInformationFile (ordinal 226, 5 args = 20 bytes) ─ */
static void bridge_NtSetInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    if (STACK_ARG(0) == 0x48000007u) {
        fprintf(stderr, "SCDA-DIAG234P tid=%lu setinfo token=0x%08X class=%u pos=0x%08X%08X tick=%lu\n",
                (unsigned long)GetCurrentThreadId(), STACK_ARG(0), infoclass,
                info_va ? BRIDGE_MEM32(info_va+4) : 0, info_va ? BRIDGE_MEM32(info_va) : 0,
                (unsigned long)GetTickCount());
        fflush(stderr);
    }
    g_eax = (uint32_t)xbox_NtSetInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryVolumeInformationFile (ordinal 218, 5 args = 20 bytes) */
static void bridge_NtQueryVolumeInformationFile(void)
{
    HANDLE   handle    = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryVolumeInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FS_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtQueryFullAttributesFile (ordinal 210, 2 args = 8 bytes) */
static void bridge_NtQueryFullAttributesFile(void)
{
    uint32_t obj_attrs = STACK_ARG(0);
    uint32_t info_va   = STACK_ARG(1);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(obj_attrs, &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtQueryFullAttributesFile(&oa,
                (PXBOX_FILE_NETWORK_OPEN_INFORMATION)XBOX_TO_NATIVE(info_va));
}

/* ── NtFlushBuffersFile (ordinal 198, 2 args = 8 bytes) ─── */
static void bridge_NtFlushBuffersFile(void)
{
    HANDLE   handle = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(1);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFlushBuffersFile(handle, &ios);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtDeleteFile (ordinal 195, 1 arg = 4 bytes) ─────── */
static void bridge_NtDeleteFile(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(STACK_ARG(0), &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtDeleteFile(&oa);
}

/* ── NtQueryDirectoryFile (ordinal 207, 9 args = 36 bytes) ─ */
/* The Xbox signature has ten arguments -- FileInformationClass sits between
 * Length and FileMask (XAPI's FindFirstFile pushes 1 there). Reading nine
 * took the class as the mask pointer and the mask as RestartScan, and
 * cleaning 36 bytes instead of 40 left FindFirstFile popping edi/esi four
 * bytes low: XFindFirstSaveGame never saw a save directory, so a freshly
 * created profile could not be loaded ("profile load failed", docs Update
 * 348). Only FileDirectoryInformation (1) is implemented below. */
static void bridge_NtQueryDirectoryFile(void)
{
    HANDLE   handle      = bridge_resolve_handle(STACK_ARG(0));
    uint32_t ios_va      = STACK_ARG(4);
    uint32_t info_va     = STACK_ARG(5);
    uint32_t length      = STACK_ARG(6);
    uint32_t infoclass   = STACK_ARG(7);
    uint32_t filename_va = STACK_ARG(8);  /* PXBOX_ANSI_STRING */
    uint32_t restart     = STACK_ARG(9);  /* BOOLEAN */
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING     fn;
    PXBOX_ANSI_STRING    pfn = NULL;

    memset(&ios, 0, sizeof(ios));
    if (filename_va) {
        /* Xbox ANSI_STRING: 0=Length(u16), 2=MaximumLength(u16), 4=Buffer(u32) */
        uint32_t fn_buf  = BRIDGE_MEM32(filename_va + 4);
        fn.Length        = BRIDGE_MEM16(filename_va);
        fn.MaximumLength = BRIDGE_MEM16(filename_va + 2);
        fn.Buffer        = fn_buf ? (PCHAR)XBOX_TO_NATIVE(fn_buf) : NULL;
        if (fn.Buffer) pfn = &fn;
    }
    if (infoclass != 1) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "  [KERNEL] NtQueryDirectoryFile: class %u not implemented\n",
                    infoclass);
    }
    g_eax = (uint32_t)xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(info_va), length, pfn, (BOOLEAN)(restart & 0xFF));
    {
        static int logged;
        if (logged < 60 || g_kernel_follow_left > 0) {
            logged++;
            fprintf(stderr, "  [DIR] handle=%p mask=%.*s restart=%u -> 0x%08X attr=0x%08X "
                    "name=%.*s\n", handle, pfn ? (int)pfn->Length : 1, pfn ? pfn->Buffer : "-",
                    restart & 0xFF, g_eax,
                    g_eax ? 0u : BRIDGE_MEM32(info_va + 0x38),
                    g_eax ? 0 : (int)BRIDGE_MEM32(info_va + 0x3C),
                    g_eax ? "" : (const char *)XBOX_TO_NATIVE(info_va + 0x40));
        }
    }
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* ── NtOpenSymbolicLinkObject (ordinal 203, 2 args = 8 bytes) */
static void bridge_NtOpenSymbolicLinkObject(void)
{
    uint32_t handle_va = STACK_ARG(0);
    /* arg1: POBJECT_ATTRIBUTES - ignored, we return a synthetic handle.
     * Written raw (untagged) so NtClose recognises it and skips it. */
    if (handle_va) BRIDGE_MEM32(handle_va) = 0xDEAD0001u;
    g_eax = STATUS_SUCCESS;
}

/* ── NtQuerySymbolicLinkObject (ordinal 215, 3 args = 12 bytes) */
static void bridge_NtQuerySymbolicLinkObject(void)
{
    /* uint32_t handle = STACK_ARG(0); */
    uint32_t target_va = STACK_ARG(1);
    uint32_t retlen_va = STACK_ARG(2);
    const char* target = "\\Device\\CdRom0";
    USHORT len = (USHORT)strlen(target);

    if (retlen_va) BRIDGE_MEM32(retlen_va) = (uint32_t)len;

    /* Say so when the buffer could not be filled.
     *
     * Reporting STATUS_SUCCESS with an untouched output buffer is the same
     * defect that left ordinal 215 unrouted: the caller believes it has a
     * device path and parses whatever was already in that memory. Half-Life 2
     * does exactly that -- it walked uninitialised bytes and dereferenced
     * 0x68737572, the ASCII "rush", as a pointer. That only looked survivable
     * because the RAM mirrors happened to back the address; with a mapping
     * that does not alias, it faults immediately.
     *
     * STATUS_BUFFER_TOO_SMALL is the honest answer, and it is one the caller
     * already has to handle -- it is what a real kernel returns when the
     * ANSI_STRING it was handed has no room. */
    if (!target_va) {
        g_eax = 0xC0000023u;             /* STATUS_BUFFER_TOO_SMALL */
        return;
    }
    {
        uint16_t max_len = BRIDGE_MEM16(target_va + 2);
        uint32_t buf_va  = BRIDGE_MEM32(target_va + 4);

        if (!buf_va || len >= max_len) {
            static unsigned warned;
            if (warned++ < 4) {
                fprintf(stderr,
                        "  [KERNEL] NtQuerySymbolicLinkObject: buffer 0x%08X "
                        "max=%u cannot hold %u bytes; returning "
                        "STATUS_BUFFER_TOO_SMALL\n",
                        buf_va, (unsigned)max_len, (unsigned)len + 1);
                fflush(stderr);
            }
            g_eax = 0xC0000023u;         /* STATUS_BUFFER_TOO_SMALL */
            return;
        }
        memcpy(XBOX_TO_NATIVE(buf_va), target, len + 1);
        BRIDGE_MEM16(target_va) = len;
    }
    g_eax = STATUS_SUCCESS;
}

/* ── IoCreateFile (ordinal 66, 10 args = 40 bytes) ────── */
static void bridge_IoCreateFile(void)
{
    /* Same as NtCreateFile with an extra Options arg at the end */
    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);
    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* -- NtDeviceIoControlFile (ordinal 196, 10 args = 40 bytes) ----
 *
 * NTSTATUS NtDeviceIoControlFile(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID,
 *                                PIO_STATUS_BLOCK, ULONG IoControlCode,
 *                                PVOID In, ULONG InLen,
 *                                PVOID Out, ULONG OutLen);
 */
#define IOCTL_DISK_GET_DRIVE_GEOMETRY 0x00070000u
#define IOCTL_DISK_GET_PARTITION_INFO 0x00074004u

/* The retail hard disk, in the units DISK_GEOMETRY reports. Deliberately the
 * same geometry kernel_path.c writes into the partition table it synthesises,
 * so a title that reads both sees one disk rather than two. */
#define XBOX_DISK_BYTES_PER_SECTOR   512u
#define XBOX_DISK_SECTORS_PER_TRACK  63u
#define XBOX_DISK_TRACKS_PER_CYL     255u
#define XBOX_DISK_CYLINDERS          1216u    /* ~10 GB, a retail 8 GB drive */

static void bridge_NtDeviceIoControlFile(void)
{
    uint32_t ios_va  = STACK_ARG(4);
    uint32_t ioctl   = STACK_ARG(5);
    uint32_t out_va  = STACK_ARG(8);
    uint32_t out_len = STACK_ARG(9);

    if (ioctl == IOCTL_DISK_GET_DRIVE_GEOMETRY) {
        /* DISK_GEOMETRY: Cylinders (LARGE_INTEGER), MediaType,
         * TracksPerCylinder, SectorsPerTrack, BytesPerSector -- 24 bytes. */
        if (!out_va || out_len < 24) {
            bridge_write_iostatus(ios_va, 0xC0000023u, 0); /* BUFFER_TOO_SMALL */
            g_eax = 0xC0000023u;
            return;
        }
        BRIDGE_MEM32(out_va +  0) = XBOX_DISK_CYLINDERS;
        BRIDGE_MEM32(out_va +  4) = 0;
        BRIDGE_MEM32(out_va +  8) = 0x0B;      /* FixedMedia */
        BRIDGE_MEM32(out_va + 12) = XBOX_DISK_TRACKS_PER_CYL;
        BRIDGE_MEM32(out_va + 16) = XBOX_DISK_SECTORS_PER_TRACK;
        BRIDGE_MEM32(out_va + 20) = XBOX_DISK_BYTES_PER_SECTOR;
        bridge_write_iostatus(ios_va, 0, 24);
        g_eax = 0;
        return;
    }

    if (ioctl == IOCTL_DISK_GET_PARTITION_INFO) {
        /* PARTITION_INFORMATION: StartingOffset and PartitionLength as
         * LARGE_INTEGERs, then HiddenSectors, PartitionNumber, and three
         * bytes of type/flags -- 32 bytes. The length is the image's own
         * size, which kernel_path.c set from the partition table. */
        HANDLE h = bridge_resolve_handle(STACK_ARG(0));
        LARGE_INTEGER size;

        if (!out_va || out_len < 32) {
            bridge_write_iostatus(ios_va, 0xC0000023u, 0);
            g_eax = 0xC0000023u;
            return;
        }
        size.QuadPart = 0;
        if (h && h != INVALID_HANDLE_VALUE)
            GetFileSizeEx(h, &size);
        BRIDGE_MEM32(out_va +  0) = 0;                       /* StartingOffset */
        BRIDGE_MEM32(out_va +  4) = 0;
        BRIDGE_MEM32(out_va +  8) = (uint32_t)size.LowPart;  /* PartitionLength */
        BRIDGE_MEM32(out_va + 12) = (uint32_t)size.HighPart;
        BRIDGE_MEM32(out_va + 16) = 0;                       /* HiddenSectors  */
        BRIDGE_MEM32(out_va + 20) = 1;                       /* PartitionNumber*/
        BRIDGE_MEM32(out_va + 24) = 0x00010106u;             /* type/boot/recog */
        BRIDGE_MEM32(out_va + 28) = 0;
        bridge_write_iostatus(ios_va, 0, 32);
        g_eax = 0;
        return;
    }

    fprintf(stderr, "  [FILE] NtDeviceIoControlFile(0x%X) - unhandled\n", ioctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu; /* STATUS_NOT_SUPPORTED */
}

/* ── NtFsControlFile (ordinal 200, 10 args = 40 bytes) ──── */
static void bridge_NtFsControlFile(void)
{
    uint32_t fsctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);
    fprintf(stderr, "  [FILE] NtFsControlFile(0x%X) - stub\n", fsctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu;
}

/* ── NtCreateDirectoryObject (ordinal 188) ──────────────── */
static void bridge_NtCreateDirectoryObject(void)
{
    /* Return STATUS_SUCCESS with a fake handle */
    uint32_t handle_ptr = STACK_ARG(0);
    if (handle_ptr) BRIDGE_MEM32(handle_ptr) = 0xBEEF0010;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* IoCreateSymbolicLink (ordinal 67, 2 args)
 *
 * Was a bare "return STATUS_SUCCESS": the title was told its link existed and
 * nothing recorded it. Titles mount their own drive letters this way --
 * Wreckless links \??\Z: to \Device\Harddisk0\Partition1\ and then loads
 * every asset through z:\ -- so dropping the link left xbox_translate_path
 * applying the generic "Z: is the cache partition" rule, and every asset open
 * failed with ERROR_FILE_NOT_FOUND a whole boot later.
 *
 * Both arguments are guest ANSI_STRINGs whose Buffer field holds a guest VA,
 * so passing the structs straight through would have xbox_copy_ansi read a
 * 32-bit guest address as a 64-bit host pointer. Rebuild them by hand, the way
 * bridge_RtlEqualString does.
 */
static void bridge_IoCreateSymbolicLink(void)
{
    uint32_t link_va   = STACK_ARG(0);
    uint32_t target_va = STACK_ARG(1);
    XBOX_ANSI_STRING link, target;

    if (!link_va) {
        g_eax = 0xC000000Du;  /* STATUS_INVALID_PARAMETER */
        return;
    }
    link.Length        = BRIDGE_MEM16(link_va + 0);
    link.MaximumLength = BRIDGE_MEM16(link_va + 2);
    link.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(link_va + 4));

    if (target_va) {
        target.Length        = BRIDGE_MEM16(target_va + 0);
        target.MaximumLength = BRIDGE_MEM16(target_va + 2);
        target.Buffer        = (PCHAR)XBOX_TO_NATIVE(BRIDGE_MEM32(target_va + 4));
    } else {
        target.Length = target.MaximumLength = 0;
        target.Buffer = NULL;
    }

    g_eax = (uint32_t)xbox_IoCreateSymbolicLink(&link,
                                                target_va ? &target : NULL);
}

/* ── ObReferenceObjectByHandle (ordinal 246) ─────────────── */
static void bridge_ObReferenceObjectByHandle(void)
{
    /* Xbox: NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object)
     * 3 args (not 6 like Windows NT).
     *
     * Always writing a null object here (previously the whole body) is a
     * stub, not an implementation: for a tagged thread handle (see
     * bridge_write_handle/bridge_handle_token), a title that resolves the
     * handle and then polls the object's own fields to learn "is this
     * thread still running" -- exactly the fs:[0x24]-independent completion
     * check sub_000523F7 does, see Update 169/170 -- reads a null object
     * forever and never sees anything but STILL_ACTIVE. Hand back the fake
     * KTHREAD-shaped structure bridge_mark_thread_done writes into instead,
     * when the handle is one of ours; fall back to null only for a handle
     * this bridge never tagged (a different object type, or a value we
     * genuinely don't track), same as before. */
    uint32_t handle = STACK_ARG(0);
    uint32_t obj_type = STACK_ARG(1);
    uint32_t object_ptr = STACK_ARG(2);
    uint32_t resolved = 0;
    (void)obj_type;

    if ((handle & ~BRIDGE_HANDLE_MASK) == BRIDGE_HANDLE_TAG) {
        uint32_t i = handle & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX)
            resolved = s_handle_kthread_va[i];
    } else if (handle == 0xFFFFFFFEu && bridge_thread_priority_on()) {
        /* NtCurrentThread(): XAPI's SetThreadPriority(GetCurrentThread())
         * resolves the pseudo handle to the caller's KTHREAD (Update 617). */
        resolved = bridge_current_kthread();
    }

    if (object_ptr) BRIDGE_MEM32(object_ptr) = resolved;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* ── RtlRaiseException (ordinal 302) ─────────────────────
 * VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
 *
 * Called by CRT / SEH code to raise structured exceptions.
 * On Xbox this triggers the kernel exception dispatcher.
 * For recompilation, we log and continue (no real SEH dispatch yet).
 */
static void bridge_RtlRaiseException(void)
{
    uint32_t record_ptr = STACK_ARG(0);
    uint32_t code = record_ptr ? BRIDGE_MEM32(record_ptr) : 0;

    static int raise_count = 0;
    raise_count++;
    if (raise_count <= 10) {
        fprintf(stderr, "  [KERNEL] RtlRaiseException: record=0x%08X code=0x%08X (#%d)\n",
                record_ptr, code, raise_count);
        fflush(stderr);
    }

    /* Handle float exceptions by clearing the FPU status.
     *
     * On the real Xbox, RtlRaiseException dispatches through the SEH chain.
     * For float exceptions (0xC0000090-0xC0000096), the CRT exception handler
     * clears the x87/SSE status word and continues execution. Without clearing,
     * the caller re-checks the FPU status, sees the exception still pending,
     * and re-raises in an infinite loop.
     *
     * _clearfp() clears both x87 and SSE exception flags on Windows x64.
     */
    if (code >= 0xC0000090u && code <= 0xC0000096u) {
        _clearfp();
    }

    g_eax = 0;
}

/* ── MmMapIoSpace (ordinal 177) ──────────────────────────
 * PVOID MmMapIoSpace(ULONG_PTR PhysicalAddress, ULONG NumberOfBytes, ULONG Protect)
 *
 * Maps physical I/O memory (GPU registers, etc.) into virtual address space.
 * Allocate from Xbox heap so the returned pointer is a valid Xbox VA.
 */
static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t xbox_va = xbox_HeapAlloc(num_bytes, 4096);

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u → Xbox VA 0x%08X\n",
            phys_addr, num_bytes, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

/* ── MmPersistContiguousMemory (ordinal 178) ─────────────
 * VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
 *
 * Marks contiguous memory as persistent across reboots (for save data).
 * No-op for recompilation.
 */
static void bridge_MmPersistContiguousMemory(void)
{
    /* No-op stub */
    g_eax = 0;
}

/* ── Generic fallback for simple value-only functions ────── */
static void bridge_generic_stub(void)
{
    /* Success-returning stub for functions whose callers only check for 0.
     * Deliberately silent: the caller (kernel_thunk_dispatch) warns for
     * ordinals with no bridge at all, which is the case worth hearing about. */
    g_eax = 0;
}


/* ══════════════════════════════════════════════════════════════════════════
 * Wrappers for the ordinals Halo 2276's thunk table binds but the bridge did
 * not route. Every one of these already had a working xbox_* implementation in
 * src/kernel/*.c; only the wrapper that moves arguments off the simulated stack
 * was missing, so each call was silently a no-op returning 0.
 *
 * Guest pointers go through XBOX_TO_NATIVE, which maps NULL to NULL. Scalars
 * pass straight through. Handles are tokens, not host HANDLEs, so they go
 * through bridge_resolve_handle / bridge_write_handle.
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── AvGetSavedDataAddress (ordinal 1, void) */
static void bridge_AvGetSavedDataAddress(void)
{
    g_eax = (uint32_t)xbox_AvGetSavedDataAddress();
}

/* ── AvSendTVEncoderOption (ordinal 2, 4 args) */
static void bridge_AvSendTVEncoderOption(void)
{
    xbox_AvSendTVEncoderOption(XBOX_TO_NATIVE(STACK_ARG(0)),
                               STACK_ARG(1), STACK_ARG(2),
                               (PULONG)XBOX_TO_NATIVE(STACK_ARG(3)));
    g_eax = 0;
}

/* ── ExFreePool (ordinal 17, 1 arg)
 * Was resolving to a DATA address before the kernel_data_va_for_ordinal fix,
 * so the title was calling into kernel data. Even after that it was an
 * unbridged no-op, which leaks every pool block the title ever frees. */
static void bridge_ExFreePool(void)
{
    xbox_ExFreePool(XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

/* ── IoCreateDevice (ordinal 65, 6 args) ────────────────
 *
 * NTSTATUS IoCreateDevice(PDRIVER_OBJECT DriverObject,
 *                         ULONG DeviceExtensionSize,
 *                         PANSI_STRING DeviceName,
 *                         DEVICE_TYPE DeviceType,
 *                         BOOLEAN Exclusive,
 *                         PDEVICE_OBJECT *DeviceObject);
 *
 * Deliberately does NOT call xbox_IoCreateDevice. That one allocates from the
 * host heap and writes a 64-bit native pointer through this 4-byte guest
 * out-parameter -- the memory-model mismatch the NOT ROUTED note below
 * describes. The object has to live in guest memory because the caller
 * immediately walks it, so allocate it there.
 *
 * Leaving the ordinal unbridged was not the safe option it looked like: the
 * unresolved thunk returns 0, which is STATUS_SUCCESS, so the title proceeds
 * with a NULL device. Wreckless reads DeviceExtension from it and `rep stosd`s
 * DeviceExtensionSize bytes through the NULL, erasing the fake TIB at guest
 * VA 0. The crash surfaced in SetLastError, thousands of calls later.
 *
 * Layout is the Xbox DEVICE_OBJECT: Flags at 0x14, DeviceExtension at 0x18,
 * DeviceType at 0x1C, StackSize at 0x1D, header 0x38 bytes. Only the fields a
 * title actually reads are filled; the rest is zero, which is what a freshly
 * created device object holds anyway.
 */
#define XBOX_DEVICE_OBJECT_SIZE 0x38u

static void bridge_IoCreateDevice(void)
{
    uint32_t extension_size = STACK_ARG(1);
    uint32_t device_type    = STACK_ARG(3);
    uint32_t out_va         = STACK_ARG(5);
    uint32_t object_va;

    if (!out_va) {
        g_eax = 0xC000000Du;   /* STATUS_INVALID_PARAMETER */
        return;
    }

    object_va = xbox_HeapAlloc(XBOX_DEVICE_OBJECT_SIZE + extension_size, 16);
    if (!object_va) {
        g_eax = 0xC000009Au;   /* STATUS_INSUFFICIENT_RESOURCES */
        return;
    }

    memset(XBOX_TO_NATIVE(object_va), 0,
           XBOX_DEVICE_OBJECT_SIZE + extension_size);
    BRIDGE_MEM16(object_va + 0x02) = (uint16_t)XBOX_DEVICE_OBJECT_SIZE;
    BRIDGE_MEM32(object_va + 0x04) = 1;                 /* ReferenceCount   */
    BRIDGE_MEM32(object_va + 0x08) = STACK_ARG(0);      /* DriverObject     */
    BRIDGE_MEM32(object_va + 0x18) =                    /* DeviceExtension  */
        extension_size ? object_va + XBOX_DEVICE_OBJECT_SIZE : 0;
    BRIDGE_MEM8(object_va + 0x1C)  = (uint8_t)device_type;
    BRIDGE_MEM8(object_va + 0x1D)  = 1;                 /* StackSize        */

    BRIDGE_MEM32(out_va) = object_va;
    g_eax = 0;                                          /* STATUS_SUCCESS   */
}

/* ── KeCancelTimer (ordinal 97, 1 arg) */
static void bridge_KeCancelTimer(void)
{
    g_eax = (uint32_t)xbox_KeCancelTimer(
        (PXBOX_KTIMER)XBOX_TO_NATIVE(STACK_ARG(0)));
}

/* KeDisconnectInterrupt (ordinal 100) lives with the interrupt delivery
 * code ("Device interrupts and DPCs"), routed since docs Update 347. */

/* ── KeSetBasePriorityThread (ordinal 143, 2 args) */
/* KeQueryBasePriorityThread (ordinal 124, 1 arg). The implementation has
 * existed in kernel_thread.c all along; only the bridge wrapper was
 * missing, so the thunk fell through to the fallback and returned 0. */
static void bridge_KeQueryBasePriorityThread(void)
{
    if (bridge_thread_priority_on()) {
        HANDLE host = bridge_kthread_host(STACK_ARG(0));
        g_eax = host ? (uint32_t)xbox_KeQueryBasePriorityThread(host) : 0;
        return;
    }
    g_eax = (uint32_t)xbox_KeQueryBasePriorityThread(
        XBOX_TO_NATIVE(STACK_ARG(0)));
}

static void bridge_KeSetBasePriorityThread(void)
{
    uint32_t kthread = STACK_ARG(0);
    LONG increment = (LONG)STACK_ARG(1);
    static volatile LONG logged;
    HANDLE host;

    if (!bridge_thread_priority_on()) {
        if (InterlockedIncrement(&logged) <= 40)
            fprintf(stderr, "  [KERNEL] KeSetBasePriorityThread kthread=0x%08X "
                    "increment=%ld tid=%lu (not applied)\n", kthread, increment,
                    GetCurrentThreadId());
        g_eax = (uint32_t)xbox_KeSetBasePriorityThread(
            XBOX_TO_NATIVE(kthread), increment);
        return;
    }
    host = bridge_kthread_host(kthread);
    if (InterlockedIncrement(&logged) <= 40)
        fprintf(stderr, "  [KERNEL] KeSetBasePriorityThread kthread=0x%08X "
                "increment=%ld tid=%lu host=%s\n", kthread, increment,
                GetCurrentThreadId(), host ? "found" : "unknown");
    g_eax = host ? (uint32_t)xbox_KeSetBasePriorityThread(host, increment) : 0;
}

/* ── KeStallExecutionProcessor (ordinal 151, 1 arg) */
static void bridge_KeStallExecutionProcessor(void)
{
    xbox_KeStallExecutionProcessor(STACK_ARG(0));
    g_eax = 0;
}

/* ── MmLockUnlockBufferPages (ordinal 175, 3 args) */
static void bridge_MmLockUnlockBufferPages(void)
{
    xbox_MmLockUnlockBufferPages(XBOX_TO_NATIVE(STACK_ARG(0)),
                                 STACK_ARG(1), (BOOLEAN)STACK_ARG(2));
    g_eax = 0;
}

/* ── MmQueryAllocationSize (ordinal 180, 1 arg)
 *
 * Answered from the guest heap's block table, NOT from xbox_MmQueryAllocationSize.
 * That one calls VirtualQuery, which on a translated guest address reports the
 * size of the whole 64 MB guest mapping -- a confidently wrong answer where the
 * title expects the size of the block it allocated. This is the memory-model
 * check the parked-bridge list below asks for, done: the question is about
 * guest memory, so only the guest allocator can answer it.
 */
static void bridge_MmQueryAllocationSize(void)
{
    g_eax = xbox_HeapBlockSize(STACK_ARG(0));
}

/* ── NtCreateMutant (ordinal 192, 3 args) */
static void bridge_NtCreateMutant(void)
{
    uint32_t handle_va = STACK_ARG(0);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING name;
    HANDLE h = NULL;
    NTSTATUS st;

    bridge_build_oa(STACK_ARG(1), &oa, &name);
    st = xbox_NtCreateMutant(&h, STACK_ARG(1) ? &oa : NULL,
                             (BOOLEAN)STACK_ARG(2));
    if (st >= 0 && handle_va) bridge_write_handle(handle_va, h);
    g_eax = (uint32_t)st;
}

/* ── NtReleaseMutant (ordinal 221, 2 args)
 *
 * NTSTATUS NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);
 *
 * The partner of NtCreateMutant above, and routing one without the other is a
 * deadlock generator: the create succeeds, the release silently does nothing,
 * and the mutex stays held forever by a thread that has already exited.
 *
 * That is what the Xbox Dashboard's audio streaming did. Each ambient WAV gets
 * five events, a worker thread and a mutant; the worker finished, failed to
 * release, and terminated. The next attempt could not take the mutex, so the
 * dashboard reopened the same file and spawned another worker with another
 * 512 KB stack, forever -- visible only as a heap that climbed and a tick that
 * never returned.
 *
 * Memory model: a handle token in, an optional 4-byte LONG out through a guest
 * address. Nothing allocates, frees, or hands back a host pointer.
 */
static void bridge_NtReleaseMutant(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtReleaseMutant(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PLONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* -- NtSuspendThread (ordinal 231, 2 args) --------------------------------
 *
 * NTSTATUS NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
 *
 * The implementation was already here and only the dispatch entry was missing,
 * which is worse than an outright stub: the call returned 0, so a thread that
 * parked itself believed it had stopped and carried straight on. Wreckless
 * does that on a worker, and the "suspended" thread spun through 289 million
 * kernel calls while the title thought it was idle.
 */
static void bridge_NtSuspendThread(void)
{
    uint32_t count_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_NtSuspendThread(
        bridge_resolve_handle(STACK_ARG(0)),
        count_va ? (PULONG)XBOX_TO_NATIVE(count_va) : NULL);
}

/* ── NtResumeThread (ordinal 224, 2 args) */
static void bridge_NtResumeThread(void)
{
    g_eax = (uint32_t)xbox_NtResumeThread(
        bridge_resolve_handle(STACK_ARG(0)),
        (PULONG)XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── ObfDereferenceObject (ordinal 250, fastcall: object in ecx)
 * Not STACK_ARG(0). Xbox uses __fastcall here, so the argument never reaches
 * the stack and the arg-size entry is 0. Reading it off the stack would
 * dereference whatever the caller happened to leave there. */
static void bridge_ObfDereferenceObject(void)
{
    xbox_ObfDereferenceObject(XBOX_TO_NATIVE(g_ecx));
    g_eax = 0;
}

/* ── PhyGetLinkState (ordinal 252, 1 arg) */
static void bridge_PhyGetLinkState(void)
{
    g_eax = (uint32_t)xbox_PhyGetLinkState((BOOLEAN)STACK_ARG(0));
}

/* ── PhyInitialize (ordinal 253, 2 args) */
static void bridge_PhyInitialize(void)
{
    g_eax = (uint32_t)xbox_PhyInitialize((BOOLEAN)STACK_ARG(0),
                                         XBOX_TO_NATIVE(STACK_ARG(1)));
}

/* ── RtlTimeToTimeFields (ordinal 305, 2 args) */
static void bridge_RtlTimeToTimeFields(void)
{
    xbox_RtlTimeToTimeFields(
        (PLARGE_INTEGER)XBOX_TO_NATIVE(STACK_ARG(0)),
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcSHAInit / XcSHAUpdate / XcSHAFinal (ordinals 335-337) */
static void bridge_XcSHAInit(void)
{
    xbox_XcSHAInit((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)));
    g_eax = 0;
}

static void bridge_XcSHAUpdate(void)
{
    xbox_XcSHAUpdate((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                     (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)),
                     STACK_ARG(2));
    g_eax = 0;
}

static void bridge_XcSHAFinal(void)
{
    xbox_XcSHAFinal((PXBOX_SHA_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(1)));
    g_eax = 0;
}

/* ── XcRC4Key / XcRC4Crypt (ordinals 338-339) */
static void bridge_XcRC4Key(void)
{
    xbox_XcRC4Key((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                  STACK_ARG(1),
                  (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

static void bridge_XcRC4Crypt(void)
{
    xbox_XcRC4Crypt((PXBOX_RC4_CONTEXT)XBOX_TO_NATIVE(STACK_ARG(0)),
                    STACK_ARG(1),
                    (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)));
    g_eax = 0;
}

/* ── XcHMAC (ordinal 340, 7 args) */
static void bridge_XcHMAC(void)
{
    xbox_XcHMAC((const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(2)), STACK_ARG(3),
                (const UCHAR*)XBOX_TO_NATIVE(STACK_ARG(4)), STACK_ARG(5),
                (UCHAR*)XBOX_TO_NATIVE(STACK_ARG(6)));
    g_eax = 0;
}

/* ── XcDESKeyParity (ordinal 346, 2 args) */
static void bridge_XcDESKeyParity(void)
{
    xbox_XcDESKeyParity((PUCHAR)XBOX_TO_NATIVE(STACK_ARG(0)), STACK_ARG(1));
    g_eax = 0;
}

/* ── Dispatch table: ordinal → bridge function + stack arg bytes ── */

typedef void (*bridge_func_t)(void);

/**
 * stdcall arg byte count for each kernel ordinal.
 * On x86 stdcall, the callee cleans (ret N). Our bridges must do the same
 * via g_esp += N after execution so the simulated stack stays balanced.
 *
 * Special cases:
 *   - KfRaiseIrql/KfLowerIrql: fastcall (arg in ecx), 0 stack bytes
 *   - KeSetTimer: DueTime is LARGE_INTEGER (8 bytes on stack) + Timer + Dpc
 */
static int stdcall_args_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* ── Display / AV ── */
    case   1: return  0;  /* AvGetSavedDataAddress (void) */
    case   2: return 16;  /* AvSendTVEncoderOption (4) */
    case   3: return 24;  /* AvSetDisplayMode (6) */
    case   4: return  4;  /* AvSetSavedDataAddress (1) */
    case   5: return  0;  /* DbgBreakPoint (void) */
    case   8: return  0;  /* DbgPrint - __cdecl varargs, caller cleans */
    case   9: return  8;  /* HalReadSMCTrayState (2) */
    case  14: return  4;  /* ExAllocatePool (1) */
    case  15: return  8;  /* ExAllocatePoolWithTag (2) */
    case  17: return  4;  /* ExFreePool (1) */
    case  23: return  4;  /* ExQueryPoolBlockSize (1) */
    case  24: return 20;  /* ExQueryNonVolatileSetting (5) */
    case  35: return  0;  /* FscGetCacheSize (void) */
    case  37: return  4;  /* FscSetCacheSize (1) */
    case  38: return  4;  /* HalClearSoftwareInterrupt (1) */
    case  39: return  8;  /* HalDisableSystemInterrupt (2) */
    case  42: return  0;  /* HalDiskSerialNumber - data export */
    case  44: return  8;  /* HalGetInterruptVector (2) */
    case  47: return  8;  /* HalRegisterShutdownNotification (2) */
    case  46: return 24;  /* HalReadWritePCISpace (6) */
    case  48: return  4;  /* HalRequestSoftwareInterrupt (1) */
    case  49: return  4;  /* HalReturnToFirmware (1) */
    case  61: return 36;  /* IoBuildDeviceIoControlRequest (9) */
    case  62: return 28;  /* IoBuildSynchronousFsdRequest (7) */
    case  65: return 24;  /* IoCreateDevice (6) */
    case  66: return 40;  /* IoCreateFile (10) */
    case  67: return  8;  /* IoCreateSymbolicLink (2) */
    case  68: return  4;  /* IoDeleteDevice (1) */
    case  69: return  4;  /* IoDeleteSymbolicLink (1) */
    case  73: return 12;  /* IoInitializeIrp (3) */
    case  74: return  8;  /* IoInvalidDeviceRequest (2) */
    case  79: return 20;  /* IoSetIoCompletion (5) */
    case  81: return  8;  /* IoStartNextPacket (2) */
    case  82: return 12;  /* IoStartNextPacketByKey (3) */
    case  83: return 16;  /* IoStartPacket (4) */
    case  84: return 32;  /* IoSynchronousDeviceIoControlRequest (8) */
    case  85: return 20;  /* IoSynchronousFsdRequest (5) */
    case  86: return  0;  /* IofCallDriver (fastcall: args in ecx/edx) */
    case  87: return  0;  /* IofCompleteRequest (fastcall: args in ecx/edx) */
    /* Missing this entry cost the Xbox Dashboard its whole boot. Ordinal 91 has
     * no bridge, so the generic stub ran -- and with no arg count it left the
     * one pushed argument on the guest stack. The caller (sub_00032859) then
     * ran `pop edi; pop esi; pop ebx` four bytes low and came back with its
     * registers rotated, which destroyed the XApp `this` pointer two frames up.
     * Its scene manager was never created, its scene never loaded, and it
     * returned to firmware -- reported as nothing more than "returning 0". */
    case  90: return  4;  /* IoDismountVolume (1) */
    case  91: return  4;  /* IoDismountVolumeByName (1) */
    case  93: return  8;  /* KeAlertThread (2) */
    case  95: return  4;  /* KeBugCheck (1) */
    case  96: return 20;  /* KeBugCheckEx (5) */
    case  97: return  4;  /* KeCancelTimer (1) */
    case  98: return  4;  /* KeConnectInterrupt (1) */
    case  99: return 12;  /* KeDelayExecutionThread (3) */
    case 100: return  4;  /* KeDisconnectInterrupt (1) */
    case 107: return 12;  /* KeInitializeDpc (3) */
    case 109: return 28;  /* KeInitializeInterrupt (7) */
    case 113: return  8;  /* KeInitializeTimerEx (2) */
    case 119: return 12;  /* KeInsertQueueDpc (3) */
    case 124: return  4;  /* KeQueryBasePriorityThread (1) */
    case 125: return  0;  /* KeQueryInterruptTime (void) */
    case 126: return  0;  /* KeQueryPerformanceCounter (void) */
    case 127: return  0;  /* KeQueryPerformanceFrequency (void) */
    case 128: return  4;  /* KeQuerySystemTime (1) */
    case 129: return  0;  /* KeRaiseIrqlToDpcLevel (void) */
    case 137: return  4;  /* KeRemoveQueueDpc (1) */
    case 139: return  4;  /* KeRestoreFloatingPointState (1) */
    case 142: return  4;  /* KeSaveFloatingPointState (1) */
    case 143: return  8;  /* KeSetBasePriorityThread (2) */
    case 144: return  8;  /* KeSetDisableBoostThread (2) */
    case 145: return 12;  /* KeSetEvent (3) */
    case 149: return 16;  /* KeSetTimer (Timer+DueTime[8]+Dpc) */
    case 150: return 20;  /* KeSetTimerEx (Timer+DueTime[8]+Period+Dpc) */
    case 151: return  4;  /* KeStallExecutionProcessor (1) */
    case 153: return 12;  /* KeSynchronizeExecution (3) */
    case 158: return 32;  /* KeWaitForMultipleObjects (8) */
    case 159: return 20;  /* KeWaitForSingleObject (5) */
    case 160: return  0;  /* KfRaiseIrql (fastcall: arg in ecx) */
    case 161: return  0;  /* KfLowerIrql (fastcall: arg in ecx) */
    case 165: return  4;  /* MmAllocateContiguousMemory (1) */
    case 166: return 20;  /* MmAllocateContiguousMemoryEx (5) */
    case 167: return  8;  /* MmAllocateSystemMemory (2) */
    case 168: return  8;  /* MmClaimGpuInstanceMemory (2) */
    case 169: return  8;  /* MmCreateKernelStack (2) */
    case 170: return  8;  /* MmDeleteKernelStack (2) */
    case 171: return  4;  /* MmFreeContiguousMemory (1) */
    case 172: return  8;  /* MmFreeSystemMemory (2) */
    case 173: return  4;  /* MmGetPhysicalAddress (1) */
    case 175: return 12;  /* MmLockUnlockBufferPages (3) */
    case 176: return  8;  /* MmLockUnlockPhysicalPage (2) */
    case 177: return 12;  /* MmMapIoSpace (3) */
    case 178: return 12;  /* MmPersistContiguousMemory (3) */
    case 179: return  4;  /* MmQueryAddressProtect (1) */
    case 180: return  4;  /* MmQueryAllocationSize (1) */
    case 181: return  4;  /* MmQueryStatistics (1) */
    case 182: return 12;  /* MmSetAddressProtect (3) */
    case 184: return 20;  /* NtAllocateVirtualMemory (5) */
    case 185: return  8;  /* NtCancelTimer (2) */
    case 186: return  4;  /* NtClearEvent (1) */
    case 187: return  4;  /* NtClose (1) */
    case 188: return  8;  /* NtCreateDirectoryObject (2) */
    case 189: return 16;  /* NtCreateEvent (4) */
    case 190: return 36;  /* NtCreateFile (9) */
    case 191: return 16;  /* NtCreateIoCompletion (4) */
    case 192: return 12;  /* NtCreateMutant (3) */
    case 193: return 16;  /* NtCreateSemaphore (4) */
    case 194: return 12;  /* NtCreateTimer (3) */
    case 195: return  4;  /* NtDeleteFile (1) */
    case 196: return 40;  /* NtDeviceIoControlFile (10) */
    case 197: return 12;  /* NtDuplicateObject (3) */
    case 198: return  8;  /* NtFlushBuffersFile (2) */
    case 199: return 12;  /* NtFreeVirtualMemory (3) */
    case 200: return 40;  /* NtFsControlFile (10) */
    case 202: return 24;  /* NtOpenFile (6) */
    case 203: return  8;  /* NtOpenSymbolicLinkObject (2) */
    case 204: return 16;  /* NtProtectVirtualMemory (4) */
    case 205: return  8;  /* NtPulseEvent (2) */
    case 206: return 20;  /* NtQueueApcThread (5) */
    case 207: return 40;  /* NtQueryDirectoryFile (10: ..., Length,
                            * FileInformationClass, FileMask, RestartScan;
                            * SCDA XAPI FindFirstFile 0x52999, docs Update 348) */
    case 210: return  8;  /* NtQueryFullAttributesFile (2) */
    case 211: return 20;  /* NtQueryInformationFile (5) */
    case 215: return 12;  /* NtQuerySymbolicLinkObject (3) */
    case 217: return  8;  /* NtQueryVirtualMemory(2), Xbox-specific signature --
                            * takes BaseAddress and Info only, not NT's
                            * generic four-arg shape. Found independently
                            * both here (SCDA's own call site) and upstream;
                            * see bridge_NtQueryVirtualMemory. */
    case 218: return 20;  /* NtQueryVolumeInformationFile (5) */
    case 219: return 32;  /* NtReadFile (8) */
    case 220: return 32;  /* NtReadFileScatter (8) */
    case 221: return  8;  /* NtReleaseMutant (2) */
    case 222: return 12;  /* NtReleaseSemaphore (3) */
    case 223: return 20;  /* NtRemoveIoCompletion (5) */
    case 224: return  8;  /* NtResumeThread (2) */
    case 225: return  8;  /* NtSetEvent (2) */
    case 226: return 20;  /* NtSetInformationFile (5) */
    case 227: return 20;  /* NtSetIoCompletion (5) */
    case 228: return  8;  /* NtSetSystemTime (2) */
    case 229: return 32;  /* NtSetTimerEx (8) */
    case 230: return 20;  /* NtSignalAndWaitForSingleObjectEx (5) */
    case 231: return  8;  /* NtSuspendThread (2) */
    case 232: return 12;  /* NtUserIoApcDispatcher (3) */
    case 233: return 12;  /* NtWaitForSingleObject (3) */
    case 234: return 16;  /* NtWaitForSingleObjectEx (4) */
    case 235: return 24;  /* NtWaitForMultipleObjectsEx (6: Count, Handles,
                            * WaitType, WaitMode, Alertable, Timeout; see
                            * bridge_NtWaitForMultipleObjectsEx). */
    case 236: return 32;  /* NtWriteFile (8) */
    case 237: return 32;  /* NtWriteFileGather (8) */
    case 238: return  0;  /* NtYieldExecution (void) */
    case 243: return 16;  /* ObOpenObjectByName (4) */
    case 247: return 20;  /* ObReferenceObjectByName (5) */
    case 250: return  0;  /* ObfDereferenceObject (fastcall: arg in ecx) */
    case 252: return  4;  /* PhyGetLinkState (1) */
    case 253: return  8;  /* PhyInitialize (2) */
    case 255: return 40;  /* PsCreateSystemThreadEx (10) */
    case 258: return  4;  /* PsTerminateSystemThread (1) */
    case 260: return 12;  /* RtlAnsiStringToUnicodeString (3) */
    case 268: return 12;  /* RtlCompareMemory (3) */
    case 269: return 12;  /* RtlCompareMemoryUlong (3) */
    case 270: return 12;  /* RtlCompareString (3) */
    case 277: return  4;  /* RtlEnterCriticalSection (1) */
    case 279: return 12;  /* RtlEqualString (3) */
    case 285: return 12;  /* RtlFillMemoryUlong (3) */
    case 286: return  4;  /* RtlFreeAnsiString (1) */
    case 289: return  8;  /* RtlInitAnsiString (2) */
    case 290: return  8;  /* RtlInitUnicodeString (2) */
    case 291: return  4;  /* RtlInitializeCriticalSection (1) */
    case 294: return  4;  /* RtlLeaveCriticalSection (1) */
    case 301: return  4;  /* RtlNtStatusToDosError (1) */
    case 302: return  4;  /* RtlRaiseException (1) */
    case 304: return  8;  /* RtlTimeFieldsToTime (2) */
    case 305: return  8;  /* RtlTimeToTimeFields (2) */
    case 308: return 12;  /* RtlUnicodeStringToAnsiString (3) */
    case 312: return 16;  /* RtlUnwind (4) */
    case 316: return  4;  /* RtlUpperChar (1) */
    case 327: return  4;  /* XeLoadSection (1) */
    case 328: return  4;  /* XeUnloadSection (1) */
    case 333: return 12;  /* WRITE_PORT_BUFFER_USHORT (3) */
    case 334: return 12;  /* WRITE_PORT_BUFFER_ULONG (3) */
    case 335: return  4;  /* XcSHAInit (1) */
    case 336: return 12;  /* XcSHAUpdate (3) */
    case 337: return  8;  /* XcSHAFinal (2) */
    case 338: return 12;  /* XcRC4Key (3) */
    case 339: return 12;  /* XcRC4Crypt (3) */
    case 340: return 28;  /* XcHMAC (7) */
    case 342: return 12;  /* XcPKDecPrivate (3) */
    case 343: return  4;  /* XcPKGetKeyLen (1) */
    case 344: return 12;  /* XcVerifyPKCS1Signature (3) */
    case 345: return 20;  /* XcModExp (5) */
    case 346: return  8;  /* XcDESKeyParity (2) */
    case 347: return 12;  /* XcKeyTable (3) */
    case 349: return 28;  /* XcBlockCryptCBC (7) */
    case 351: return  8;  /* XcUpdateCrypto (2) */
    case 352: return 12;  /* RtlRip (3) */
    case 358: return  0;  /* HalIsResetOrShutdownPending (void) */
    case 359: return  4;  /* IoMarkIrpMustComplete (1) */

    /* ── Unknown stubs ── */

    /* ── Pool Allocator ── */
    /* ordinal 16 is the ExEventObjectType data export; see
     * kernel_thunks.c, which points its thunk at kernel data.
     * 17 is ExFreePool and is a real function. */

    /* ── HAL ── */

    /* ── I/O Manager ── */
    /* ordinal 64 is the IoCompletionObjectType data export; see
     * kernel_thunks.c. 65 is IoCreateDevice, a real function. */
    /* case  71: DATA export - IoDeviceObjectType */

    /* ── Kernel Synchronization ── */
    /* case 156: DATA export - KeTickCount */

    /* ── Launch Data ── */
    /* case 164: DATA export - LaunchDataPage */

    /* ── Memory Management ── */

    /* ── NT Virtual Memory ── */

    /* ── NT File I/O & Handle ── */

    /* ── Object Manager ── */
    case 246: return 12;  /* ObReferenceObjectByHandle(3) - Xbox: Handle,Type,Object* */
    case 360: return  0;  /* HalInitiateShutdown (void) */

    /* ── Network / PHY ── */

    /* ── Threading ── */
    case 256: return  4;  /* PsQueryStatistics(1) -- SCDA-only, not upstream yet */
    /* case 259: DATA export - PsThreadObjectType */

    /* ── Runtime Library ── */

    /* ── Xbox Identity (data exports) ── */
    /* cases 322-328, 355-357: DATA exports */

    /* ── Port I/O -- SCDA-only, not upstream yet ── */
    case 329: return 12;  /* READ_PORT_BUFFER_UCHAR(3) */
    case 330: return 12;  /* READ_PORT_BUFFER_USHORT(3) */
    case 331: return 12;  /* READ_PORT_BUFFER_ULONG(3) */
    case 332: return 12;  /* WRITE_PORT_BUFFER_UCHAR(3) */

    /* ── Crypto -- SCDA-only additions not upstream yet ── */
    case 341: return 12;  /* XcPKEncPublic(3) */
    case 348: return 20;  /* XcBlockCrypt(5) */
    case 350: return  8;  /* XcCryptService(2) */

    default:  return  0;  /* DATA exports or truly unknown */
    }
}

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    switch (ordinal) {
    /* Threading */
    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;

    /* File/Handle */
    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 279: return bridge_RtlEqualString;
    case 289: return bridge_RtlInitAnsiString;
    case 195: return bridge_NtDeleteFile;
    case 196: return bridge_NtDeviceIoControlFile;
    case 198: return bridge_NtFlushBuffersFile;
    case 200: return bridge_NtFsControlFile;
    case 202: return bridge_NtOpenFile;
    case 203: return bridge_NtOpenSymbolicLinkObject;
    case 207: return bridge_NtQueryDirectoryFile;
    case 210: return bridge_NtQueryFullAttributesFile;
    case 211: return bridge_NtQueryInformationFile;
    case 218: return bridge_NtQueryVolumeInformationFile;
    case 219: return bridge_NtReadFile;
    case 312: return bridge_RtlUnwind;
    case 327: return bridge_XeLoadSection;
    case 328: return bridge_XeUnloadSection;
    case 226: return bridge_NtSetInformationFile;
    case 236: return bridge_NtWriteFile;

    /* Memory - contiguous */
    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 169: return bridge_MmCreateKernelStack;
    case 170: return bridge_MmDeleteKernelStack;
    case 171: return bridge_MmFreeContiguousMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;

    /* Memory - virtual */
    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;
    case 215: return bridge_NtQuerySymbolicLinkObject;
    case 217: return bridge_NtQueryVirtualMemory;

    /* Pool */
    case  14: return bridge_ExAllocatePool;
    case  15: return bridge_ExAllocatePoolWithTag;
    case  23: return bridge_ExQueryPoolBlockSize;
    case  24: return bridge_ExQueryNonVolatileSetting;

    /* IRQL */
    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    /* Critical sections */
    case 291: return bridge_RtlInitializeCriticalSection;
    /* Wreckless asks for the AV pack during D3D device creation; unbridged it
     * read 0, which is "no pack connected". Writes one ULONG through a guest
     * pointer it is given and nothing else, and XBOX_TO_NATIVE turns a guest
     * NULL into a host NULL so xbox_AvSendTVEncoderOption's own !Result guard
     * still fires. */
    case   2: return bridge_AvSendTVEncoderOption;
    /* Reads a LARGE_INTEGER and fills a TIME_FIELDS, both at caller-supplied
     * guest addresses -- the case the NOT ROUTED note below names as fine. */
    case 305: return bridge_RtlTimeToTimeFields;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    /* Timing */
    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimer;  /* KeSetTimerEx */

    /* DPC / Timer init */
    case 107: return bridge_KeInitializeDpc;
    case 113: return bridge_KeInitializeTimerEx;

    /* NV2A interrupt plumbing */
    case  44: return bridge_HalGetInterruptVector;
    case  98: return bridge_KeConnectInterrupt;
    case 100: return bridge_KeDisconnectInterrupt;
    case 119: return bridge_KeInsertQueueDpc;
    case 137: return bridge_KeRemoveQueueDpc;
    case 153: return bridge_KeSynchronizeExecution;
    case 109: return bridge_KeInitializeInterrupt;
    case  47: return bridge_HalRegisterShutdownNotification;
    case 168: return bridge_MmClaimGpuInstanceMemory;

    /* Synchronization */
    case 189: return bridge_NtCreateEvent;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case  99: return bridge_KeDelayExecutionThread;
    case 179: return bridge_MmQueryAddressProtect;
    case 232: return bridge_NtUserIoApcDispatcher;
    case  95: return bridge_KeBugCheck;
    case  96: return bridge_KeBugCheckEx;
    case 186: return bridge_NtClearEvent;
    case 205: return bridge_NtPulseEvent;
    case 225: return bridge_NtSetEvent;
    case 233: return bridge_NtWaitForSingleObject;
    case 234: return bridge_NtWaitForSingleObjectEx;
    case 235: return bridge_NtWaitForMultipleObjectsEx;
    case 238: return bridge_NtYieldExecution;

    /* Hardware */
    case   9: return bridge_HalReadSMCTrayState;
    case  49: return bridge_HalReturnToFirmware;

    /* Display */
    case   1: return bridge_AvGetSavedDataAddress;
    case   3: return bridge_AvSetDisplayMode;

    /* I/O */
    case  66: return bridge_IoCreateFile;
    case  65: return bridge_IoCreateDevice;
    case  67: return bridge_IoCreateSymbolicLink;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    /* Memory - I/O mapping */
    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    /* RTL */
    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;
    case 316: return bridge_RtlUpperChar;


    /* BISECT-OFF case 338: bridge_XcRC4Key */
    /* BISECT-OFF case 339: bridge_XcRC4Crypt */


    /* NOT ROUTED, deliberately. The wrappers above exist and compile, and each
     * has a working xbox_* behind it, but routing them made Halo 2276 crash
     * EARLIER than leaving them stubbed -- twice, with two different faults.
     * Bisected to a memory-model mismatch, not to the wrappers' arithmetic:
     *
     *   xbox_IoCreateDevice HeapAllocs from GetProcessHeap() and writes that
     *   NATIVE pointer through its out-parameter. The bridge hands it the
     *   native address of a 4-BYTE GUEST slot, so a 64-bit pointer is written
     *   into 4 bytes: it clobbers the adjacent guest dword and leaves the title
     *   a truncated pointer it then dereferences. Crash was a write to
     *   0x90909090.
     *
     *   xbox_ExFreePool calls HeapFree(GetProcessHeap(), P). Guest pool memory
     *   is not on the host heap, so P is a pointer HeapFree has never seen.
     *
     * These xbox_* functions were written for a NATIVE caller, where pointers
     * are host pointers and allocations are host allocations. The bridge is a
     * different world: pointers are guest VAs and memory lives in the mapped
     * guest space. XBOX_TO_NATIVE converts an address; it cannot convert an
     * allocator.
     *
     * So 'an xbox_* exists, therefore the wrapper is mechanical' is false, and
     * tools.kernel_audit.coverage no longer says it. Each of these needs its
     * memory model checked one at a time: which side owns the allocation, and
     * whether an out-pointer must carry a guest VA. Ones that only read or
     * write bytes at a caller-supplied address (the Xc* crypto group,
     * RtlTimeToTimeFields) should be fine; ones that allocate, free, or hand
     * back a pointer are not.
     *
     * Left in place rather than deleted: the wrappers are correct as argument
     * marshalling, and re-deriving them is the easy half of the work.
     *
     * AvGetSavedDataAddress (ordinal 1) does not belong on this list and was
     * moved out, above, into "Display": it takes void and its xbox_* just
     * returns a plain ULONG global (g_av_saved_data_address, set by
     * AvSetSavedDataAddress), never a host pointer or allocation -- none of
     * the memory-model hazards this list exists for apply. Left unrouted
     * before this only because it had never been looked at; SCDA calls it
     * once right after AvSetDisplayMode (docs/SCDA_WORKLOG.md, continued 47),
     * where the previous "no bridge" fallback already answered 0 -- the same
     * "no saved data" a fresh boot should report -- so routing it is a no-op
     * for behaviour today and just replaces a warning with the real getter.
     */
    /* case  17: bridge_ExFreePool */
    /* case  97: bridge_KeCancelTimer */
    /* case 100: routed above with the interrupt delivery (Update 347) */
    /* Routed: thread base priority, queried and set. Neither allocates,
     * frees, nor returns a pointer -- each takes a thread handle and a
     * LONG. Half-Life 2 calls both while starting its worker threads,
     * and unbridged they returned 0, so every thread read its own base
     * priority as 0 and any priority the title set was discarded. */
    case 124: return bridge_KeQueryBasePriorityThread;
    case 143: return bridge_KeSetBasePriorityThread;
    /* Routed. Both clear the memory-model bar above: neither allocates,
     * frees, nor hands back a host pointer. KeStallExecutionProcessor takes a
     * microsecond count and busy-waits -- no pointers at all.
     * MmLockUnlockBufferPages takes (BaseAddress, Length, UnlockPages) and
     * pins physical pages, which is a no-op on the host; XBOX_TO_NATIVE is the
     * correct marshalling for its one address argument, and it writes nothing
     * through it.
     *
     * Half-Life 2 calls both during C++ static initialisation. Unbridged they
     * returned 0 without stalling or locking anything -- harmless in isolation,
     * but they are exactly the kind of silent no-op that makes a later failure
     * unattributable. */
    case 151: return bridge_KeStallExecutionProcessor;
    case 175: return bridge_MmLockUnlockBufferPages;
    /* Routed, both checked against the memory-model bar above.
     *
     * MmQueryAllocationSize now answers from the guest heap's block table
     * instead of the host's VirtualQuery, so nothing crosses the two worlds.
     *
     * NtCreateMutant creates a host mutex and hands it back through
     * bridge_write_handle, which is a guest token -- the same shape as
     * NtCreateEvent, which has been routed all along. It allocates no guest
     * memory and returns no host pointer.
     *
     * The Xbox Dashboard needs the mutant: its audio thread creates one during
     * the first tick, and unbridged the call returned STATUS_SUCCESS without
     * writing a handle, so the main thread waited on five events that nothing
     * would ever signal. */
    case 180: return bridge_MmQueryAllocationSize;
    case 192: return bridge_NtCreateMutant;
    case 221: return bridge_NtReleaseMutant;
    /* Routed. Checked against the memory-model warning above rather than
     * assumed mechanical: NtResumeThread takes a handle token and writes a
     * 4-byte suspend count through an optional out-parameter. Guest ULONG and
     * host ULONG are both 4 bytes, XBOX_TO_NATIVE already maps NULL to NULL,
     * and xbox_NtResumeThread checks the pointer before writing. Nothing
     * allocates, frees, or hands back a host pointer -- which is what
     * disqualified IoCreateDevice and ExFreePool.
     *
     * Halo 2276 calls this immediately before its first camera frustum build;
     * unbridged it returned 0 (STATUS_SUCCESS) without resuming anything, so a
     * thread the title had created suspended never started. */
    case 224: return bridge_NtResumeThread;
    case 231: return bridge_NtSuspendThread;
    /* Same class of bug as NtSuspendThread/NtResumeThread above: the
     * implementation already existed (xbox_ObfDereferenceObject, a documented
     * no-op -- Win32 handles manage their own lifetime, so this doesn't
     * change behavior) but the dispatch entry was missing, so every call fell
     * through to the generic "no bridge" stub instead. Found live while
     * bisecting Update 233's post-Update-232 steady state: ObfDereferenceObject
     * is ordinal 250's __fastcall-in-ecx callee, so g_slot_arg_bytes[250]=0 is
     * correct and this wiring fix carries no stack-cleanup risk either way. */
    case 250: return bridge_ObfDereferenceObject;
    /* case 252: bridge_PhyGetLinkState */
    /* case 253: bridge_PhyInitialize */
    /* Routed. The memory-model note above already names this group as the
     * safe kind: each one reads or writes bytes at an address the caller
     * supplied, and none allocates, frees, or hands back a host pointer.
     *
     * Unbridged they returned 0 without hashing anything, which is invisible
     * until something checks a digest. The Xbox Dashboard verifies each XIP
     * archive it loads against a 20-byte digest in its own table
     * (sub_00034924) and calls HalReturnToFirmware(4) when the compare fails
     * -- so a no-op SHA does not corrupt anything, it reboots the console. */
    case 335: return bridge_XcSHAInit;
    case 336: return bridge_XcSHAUpdate;
    case 337: return bridge_XcSHAFinal;
    case 340: return bridge_XcHMAC;
    /* case 346: bridge_XcDESKeyParity */

    default:  return NULL;
    }
}

/* ── Per-slot bridge functions (resolved at init) ────────── */

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Xbox VA to sample around each bridge call; 0 = off. See dispatch. */
uint32_t g_kernel_watch_va = 0;

/* Arm the watch from the environment.
 *
 * The facility existed but nothing set it, so it was unreachable.
 * RECOMP_KERNEL_WATCH=<guest addr> samples that dword either side of every
 * bridge call and names the ordinal that changed it -- which is the one fact
 * a hardware watchpoint cannot give, because a bridge corrupting Xbox memory
 * faults inside ntdll with no recompiled frame to blame.
 *
 * A change seen between the previous call's "after" and this call's "before"
 * is guest code, not a bridge, which is just as useful to know. */
static void kernel_watch_arm_once(void)
{
    static int done;
    const char *env;
    if (done)
        return;
    done = 1;
    env = getenv("RECOMP_KERNEL_WATCH");
    if (env)
        g_kernel_watch_va = (uint32_t)strtoul(env, NULL, 0);
}

/* Current dispatching slot.
 *
 * recomp_lookup_kernel() records the synthetic thunk's slot and returns the
 * one shared C entry point below.  The lookup and that entry-point call are
 * separate operations, so this value must follow the guest register set and
 * simulated stack: one host thread can be preempted after lookup while another
 * resolves a different kernel thunk.  A process-global slot then makes the
 * first thread execute the second thread's ordinal and, critically, pop the
 * second ordinal's stdcall argument count from the first thread's guest stack.
 * SCDA exposed the race as slot 0 / RtlEnterCriticalSection (4 bytes) being
 * dispatched with slot 100 / NtWaitForMultipleObjectsEx (24 bytes), producing
 * the observed exact 20-byte stack over-pop. */
static RECOMP_TLS int g_kernel_dispatch_slot = -1;

static void kernel_thunk_dispatch(void)
{
    int slot = g_kernel_dispatch_slot;
    bridge_func_t bridge;
    ULONG ordinal;

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;  /* pop dummy return address */
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];

    g_kernel_call_count++;

    /* 2026-09-11: general-purpose O(1) per-ordinal call histogram (direct
     * array index, no scan) + a per-ordinal source-site ring. Built to
     * identify which kernel call drove the >76M-calls/90s throughput seen
     * after the NtWaitForMultipleObjectsEx stack-leak fix -- that
     * investigation is CLOSED (2026-09-11): the dominant calls were a
     * legitimate 0ms-timeout, non-alertable poll on a fixed event handle
     * (WaitForSingleObjectEx correctly alternating WAIT_TIMEOUT/
     * WAIT_OBJECT_0), which spins fast simply because nothing yet paces
     * this recomp to real display/vsync timing -- not a wait-semantics
     * bug. Kept as a general diagnostic (env-gated, RECOMP_KERNEL_HIST=1,
     * costs one cached getenv check when off) for the next throughput
     * question, rather than a one-off artifact to strip out. */
    {
    static int hist_on = -1;
    if (hist_on < 0) hist_on = getenv("RECOMP_KERNEL_HIST") ? 1 : 0;
    if (hist_on) {
        enum { HIST_MAX_ORDINAL = 512 };
        static uint64_t hist_count[HIST_MAX_ORDINAL];
        static uint32_t hist_source[HIST_MAX_ORDINAL][4];
        if (ordinal < HIST_MAX_ORDINAL) {
            uint64_t n = ++hist_count[ordinal];
            hist_source[ordinal][n & 3] = (uint32_t)g_icall_source;
        }
        {
            static DWORD last_hist_tick = 0;
            DWORD now = GetTickCount();
            if (last_hist_tick == 0) last_hist_tick = now;
            if (now - last_hist_tick >= 5000) {
                int k;
                fprintf(stderr, "  [KHIST] top ordinals at %lld total calls:\n",
                        (long long)g_kernel_call_count);
                for (k = 0; k < HIST_MAX_ORDINAL; k++)
                    if (hist_count[k] > 1000)
                        fprintf(stderr, "  [KHIST]   ordinal %d: %llu calls, recent sources: "
                                "0x%08X 0x%08X 0x%08X 0x%08X\n",
                                k, (unsigned long long)hist_count[k],
                                hist_source[k][0], hist_source[k][1], hist_source[k][2], hist_source[k][3]);
                fflush(stderr);
                last_hist_tick = now;
            }
        }
    }
    }

    if (KERNEL_LOG_ON()) {
        /* The guest return address sits at the top of the guest stack: the
         * caller pushed it before dispatching here. Logging it turns "some
         * function is calling this" into "this call site is", which is the
         * difference between guessing and knowing when a title recurses.
         * source=g_icall_source (SCDA addition) is the guest PC of the
         * indirect-call instruction itself, when the dispatch came through
         * one -- complementary to ret, which is the call's own return site. */
        fprintf(stderr,
                "  [KERNEL] #%d: source=0x%08X ordinal %u (slot %d) "
                "esp=0x%08X ret=0x%08X\n",
                g_kernel_call_count, (uint32_t)g_icall_source,
                ordinal, slot, g_esp,
                g_esp ? BRIDGE_MEM32(g_esp) : 0);
        fflush(stderr);
    }

    {
        static DWORD last_summary_tick = 0;
        DWORD now = GetTickCount();
        if (last_summary_tick == 0) last_summary_tick = now;
        if (now - last_summary_tick >= 2000 && g_kernel_call_count > 200) {
            fprintf(stderr, "  [KERNEL] summary: %d total calls, latest ordinal %u (slot %d) esp=0x%08X tick=%lu\n",
                    g_kernel_call_count, ordinal, slot, g_esp, (unsigned long)now);
            fflush(stderr);
            last_summary_tick = now;
        }
    }

    /* Pop the dummy return address that PUSH32(esp, 0) pushed before RECOMP_ICALL.
     * On real x86, "call [thunk]" pushes a real return address and "ret" pops it.
     * In our model, the bridge is called directly (not via the simulated stack),
     * so we must manually consume the dummy return address. */
    g_esp += 4;

    /* Name the bridge that corrupts a watched dword.
     *
     * A bridge hands Xbox pointers to real Win32 calls, so a bad one has
     * Windows write into Xbox memory -- the resulting wild write has a stack
     * inside ntdll with no recompiled frame to blame, and a watchpoint just
     * says "something changed". Sampling either side of the call names the
     * ordinal directly, which is the one fact those tools cannot give.
     *
     * Set g_kernel_watch_va to arm; zero (the default) costs one compare. */
    uint32_t _watch_before = 0;
    kernel_watch_arm_once();
    if (g_kernel_watch_va) {
        _watch_before = BRIDGE_MEM32(g_kernel_watch_va);
        /* Reporting only on change misses the case that matters most: a value
         * that was already wrong before the first call sampled it. Printing
         * every sample under RECOMP_KERNEL_WATCH_ALL shows when it changed
         * even if no single bridge did it. */
        if (getenv("RECOMP_KERNEL_WATCH_ALL")) {
            static uint32_t seen = 0xDEADBEEFu;
            if (_watch_before != seen) {
                seen = _watch_before;
                fprintf(stderr, "  [KWATCH] 0x%08X = %08X before ordinal %u"
                                " (call #%d)\n",
                        g_kernel_watch_va, _watch_before, ordinal,
                        g_kernel_call_count);
                fflush(stderr);
            }
        }
    }

    if (g_kernel_follow_left > 0 && bridge != bridge_NtOpenFile) {
        uint32_t ret_site = BRIDGE_MEM32(g_esp - 4);
        g_kernel_follow_left--;
        if (bridge)
            bridge();
        else
            g_eax = 0;
        fprintf(stderr, "  [KFOLLOW] ordinal %u (slot %d) ret=0x%08X "
                "args=%08X %08X %08X %08X %08X -> eax=0x%08X\n",
                ordinal, slot, ret_site,
                BRIDGE_MEM32(g_esp), BRIDGE_MEM32(g_esp + 4),
                BRIDGE_MEM32(g_esp + 8), BRIDGE_MEM32(g_esp + 12),
                BRIDGE_MEM32(g_esp + 16), g_eax);
        fflush(stderr);
    } else if (bridge) {
        bridge();
    } else {
        /* No specific bridge - return 0. Warn once per ordinal rather than
         * gating on g_kernel_call_count: a missing bridge is rare and is
         * usually the reason a game misbehaves, so it must not be swallowed
         * by the general call-trace throttle. Bounded to one line per slot. */
        static uint8_t warned[XBOX_KERNEL_THUNK_TABLE_SIZE];
        if (!warned[slot]) {
            warned[slot] = 1;
            fprintf(stderr, "  [KERNEL] WARNING: no bridge for ordinal %u (slot %d), returning 0\n",
                    ordinal, slot);
            /* "Returning 0" is the harmless half. The damaging half is the
             * stack: the Xbox kernel is stdcall, so the callee owes the caller
             * its arguments back, and an ordinal missing from
             * stdcall_args_for_ordinal() returns 0 bytes and leaves them
             * there. The caller's own `pop`s then run low by that much and it
             * returns with its callee-saved registers rotated -- silently,
             * frames away from here. Say so, because a title that dies of this
             * looks nothing like a title that is missing a kernel function. */
            if (g_slot_arg_bytes[slot] == 0)
                fprintf(stderr, "  [KERNEL]   ordinal %u has no entry in "
                        "stdcall_args_for_ordinal(). If it takes arguments, "
                        "this call is corrupting the caller's stack -- add its "
                        "argument size there before anything else.\n", ordinal);
            fflush(stderr);
        }
        g_eax = 0;
    }

    /* Clean stdcall args from the simulated stack.
     * On real x86, stdcall callee does "ret N" to pop the return address
     * and N bytes of arguments. We already popped the dummy return address
     * above; now pop the args. */
    g_esp += g_slot_arg_bytes[slot];

    /* A pending device interrupt or DPC runs here, on this guest thread,
     * once the call is complete (see xbox_irq_deliver). */
    g_irq_via = 6;
    xbox_irq_deliver();

    if (g_kernel_watch_va) {
        uint32_t _after = BRIDGE_MEM32(g_kernel_watch_va);
        if (_after != _watch_before) {
            fprintf(stderr,
                    "  [KWATCH] ordinal %u changed Xbox VA 0x%08X: "
                    "%08X -> %08X\n",
                    ordinal, g_kernel_watch_va, _watch_before, _after);
            fflush(stderr);
        }
    }

    if (KERNEL_LOG_ON()) {
        fprintf(stderr, "  [KERNEL] → returned 0x%08X\n", g_eax);
        fflush(stderr);
    }
}

/* ── Dispatch lookup ────────────────────────────────────── */

/**
 * Look up a kernel thunk by synthetic VA.
 * Called as a fallback when recomp_lookup() returns NULL.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            return kernel_thunk_dispatch;
        }
    }
    return NULL;
}

/* ── Initialization ─────────────────────────────────────── */

/*
 * Where this title's kernel thunk table lives. Defaults to the compile-time
 * constant, but every XBE puts it somewhere different (it comes from the
 * header's KernelImageThunkAddress), so xbox_MemoryLayoutInit() parses the
 * real address out of the binary and overrides it here.
 *
 * Halo build 2276 puts it at 0x00253090 against the default's 0x0036B7C0 --
 * without the override the bridge patches ordinals into whatever happens to
 * live at the wrong address and every kernel call goes somewhere arbitrary.
 */
static uint32_t g_thunk_table_base  = XBOX_KERNEL_THUNK_TABLE_BASE;
static uint32_t g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;

/**
 * Return the kernel thunk table address and entry count currently in effect.
 * The address is parsed from the XBE header during memory layout init, so the
 * values are per-title, not hardcoded. *base/*count are set to 0 if unavailable.
 */
void xbox_kernel_get_thunk_address(uint32_t *xbox_va, uint32_t *count)
{
    if (xbox_va) *xbox_va = g_thunk_table_base;
    if (count)  *count  = g_thunk_table_count;
}

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count)
{
    if (!xbox_va) {
        return;
    }

    g_thunk_table_base = xbox_va;

    /* count indexes g_slot_* arrays, which are sized by the macro. A title
     * importing more slots than the real kernel exports would run off them. */
    if (count && count <= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        g_thunk_table_count = count;
    } else if (count > XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr,
                "  Kernel thunk bridge: XBE declares %u thunk slots, clamping to %d\n",
                count, XBOX_KERNEL_THUNK_TABLE_SIZE);
        g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;
    }
}

/**
 * Resolve the kernel thunk table in Xbox memory.
 *
 * Must be called AFTER xbox_MemoryLayoutInit() so Xbox memory is mapped.
 *
 * Reads the actual ordinals from the XBE memory thunk table (0x80000000|ordinal),
 * resolves each to a per-ordinal bridge function, and replaces the entry
 * with a synthetic VA for dispatch.
 */
/* Per-title ordinal remap; NULL = identity (the kernel's own XDK). Set by
 * xbox_kernel_set_ordinal_remap before init. See kernel.h. */
static const unsigned short *g_ordinal_remap = NULL;
static int g_ordinal_remap_count = 0;

void xbox_kernel_set_ordinal_remap(const unsigned short *map, int count)
{
    g_ordinal_remap = map;
    g_ordinal_remap_count = count;
}

void xbox_kernel_bridge_init(void)
{
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            g_thunk_table_count, g_thunk_table_base);

    /* The thunk table lives in .rdata which is marked PAGE_READONLY.
     * Temporarily make it writable so we can patch the ordinals. */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        PAGE_READWRITE,
        &old_protect
    );

    /* Initialize kernel data export values first */
    kernel_data_init();

    for (i = 0; i < g_thunk_table_count; i++) {
        uint32_t va = g_thunk_table_base + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {
            /* Read the actual ordinal from Xbox memory, then translate it into
             * the kernel's canonical ordinal space. Identity unless the title
             * set a remap (a different XDK). Every routing decision below --
             * data export, bridge, arg size -- keys off the canonical ordinal,
             * so one translation here covers all three. */
            ULONG ordinal = current & 0x7FFFFFFF;
            if (g_ordinal_remap && ordinal < (ULONG)g_ordinal_remap_count
                && g_ordinal_remap[ordinal]) {
                ordinal = g_ordinal_remap[ordinal];
            }
            g_slot_ordinals[i] = ordinal;

            /* Check if this is a data export */
            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                /* DATA export: point thunk to actual data in mapped memory.
                 * This allows the game to dereference the thunk entry. */
                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            /* FUNCTION export: use synthetic VA for dispatch */
            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            g_slot_arg_bytes[i] = stdcall_args_for_ordinal(ordinal);
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
                fprintf(stderr, "  [KERNEL] unbridged function thunk: ordinal %lu"
                        " (slot %u, VA 0x%08X)\n",
                        (unsigned long)ordinal, i, KERNEL_VA_BASE + i * 4);
            }

            /* Replace Xbox memory entry with synthetic VA */
            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    /*
     * Thunk entries below the header-declared base.
     *
     * KernelImageThunkAddress points at the main import run, but the linker can
     * emit further runs just before it, separated by a NULL. Halo has three at
     * base-0x10 (ordinals 52, 51 and 5). Those stay unpatched, so game code
     * doing "mov ebx,[thunk]; call ebx" jumps to the raw 0x8000xxxx marker
     * instead of a kernel function. The indirect call cannot resolve it, yields
     * 0, and a caller looping until it sees an error code never sees one - in
     * Halo that hung main() in a file-enumeration loop before it reached any
     * initialisation.
     *
     * Only entries still carrying the ordinal marker are touched, so scanning
     * back over unrelated .rdata is harmless.
     */
    {
        const int LOOKBEHIND = 16;   /* entries, i.e. 64 bytes */
        DWORD scan_protect;
        uint32_t low = g_thunk_table_base - LOOKBEHIND * 4;

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, PAGE_READWRITE, &scan_protect);

        for (i = 1; i <= LOOKBEHIND; i++) {
            uint32_t va = g_thunk_table_base - i * 4;
            uint32_t current = BRIDGE_MEM32(va);
            int slot;

            if (!(current & 0x80000000)) {
                continue;            /* NULL separator or ordinary data */
            }
            slot = g_thunk_table_count + i;   /* park these above the main run */
            if (slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
                break;
            }

            g_slot_ordinals[slot] = current & 0x7FFFFFFF;
            g_slot_bridges[slot] = bridge_for_ordinal(g_slot_ordinals[slot]);
            g_slot_arg_bytes[slot] = stdcall_args_for_ordinal(g_slot_ordinals[slot]);
            BRIDGE_MEM32(va) = KERNEL_VA_BASE + slot * 4;
            resolved++;
            if (g_slot_bridges[slot]) bridged++; else unbridged++;

            fprintf(stderr, "  [KERNEL] extra thunk at 0x%08X: ordinal %u (slot %d)\n",
                    va, g_slot_ordinals[slot], slot);
        }

        VirtualProtect((LPVOID)((uintptr_t)low + g_xbox_mem_offset),
                       LOOKBEHIND * 4, scan_protect, &scan_protect);
    }

    /* Restore original protection */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, g_thunk_table_count, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);

}
