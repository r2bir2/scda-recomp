/**
 * Xbox Memory Layout Implementation
 *
 * Maps the XBE data sections to their expected virtual addresses on Windows.
 * This is critical for the recompiled code which references globals by
 * absolute address (e.g., mov eax, [0x004D532C]).
 *
 * Implementation:
 * 1. VirtualAlloc a contiguous region at XBOX_BASE_ADDRESS
 * 2. Copy .rdata and initialized .data from the XBE
 * 3. Zero-fill the BSS region
 * 4. Set memory protection (read-only for .rdata)
 */

#include "xbox_memory_layout.h"
#include "guest_cpu_pick.h"
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <intrin.h>
#include <process.h>

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120
#define XBE_TLS_ADDR_OFFSET     0x012C

/* XBE section header layout (56 bytes each) */
#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static ptrdiff_t g_memory_offset = 0;  /* actual_base - XBOX_BASE_ADDRESS */

/* Actual mapped RAM for this run; see the header. Default retail 64 MB. */
size_t g_xbox_total_ram = XBOX_TOTAL_RAM;
size_t g_xbox_map_size = 0;   /* 0 = same as RAM */

void xbox_SetTotalRam(size_t bytes)
{
    g_xbox_total_ram = bytes;
}


void xbox_SetMapSize(size_t bytes)
{
    g_xbox_map_size = bytes;
}

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};
static void *g_tiled_view = NULL;
/* Forward declarations for the narrowly scoped GPU-page diagnostic below.
 * Their initialized definitions remain with the contiguous-arena state. */
static void *g_contig_alias_view;
static uint32_t g_contig_alias_base;
static size_t xbox_TiledApertureSize(void);

/* Update 362: a narrowly armed write watch for one physical pushbuffer page.
 * The title can access a dynamically allocated GPU buffer through either its
 * contiguous mapping or the tiled aperture, so both host aliases must be
 * protected.  This never changes guest bytes: the first write is logged and
 * immediately retried after the normal read/write protection is restored. */
static volatile LONG s_gpu_pb_watch_armed;
static volatile LONG s_gpu_pb_watch_hits;
static volatile LONG s_gpu_pb_watch_veh_installed;
static volatile LONG s_gpu_pb_watch_rearm;
static volatile LONG s_gpu_pb_watch_events;
static uintptr_t s_gpu_pb_watch_tiled;
static uintptr_t s_gpu_pb_watch_contig;
static uint32_t s_gpu_pb_watch_phys;
static uint32_t s_gpu_pb_watch_slot_off;

/* The write watches below single-step via EFlags.TF and unwind with the
 * Windows x64 tables: Windows/x86-64 only (ARM port, docs Update 677). */
#if defined(_WIN32)
/* Unwind the *faulting* context, not the VEH's own dispatcher stack.  The
 * latter has no title frames because Windows invokes a VEH from ntdll. */
static unsigned xbox_gpu_pb_watch_unwind(const CONTEXT *fault_ctx,
                                         uintptr_t frames[8])
{
    CONTEXT ctx = *fault_ctx;
    unsigned n = 0;

    while (n < 8 && ctx.Rip) {
        DWORD64 image_base = 0, establisher = 0;
        PRUNTIME_FUNCTION fn;
        PVOID handler_data = NULL;
        KNONVOLATILE_CONTEXT_POINTERS nonvolatile = {0};

        frames[n++] = (uintptr_t)ctx.Rip;
        fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, NULL);
        if (!fn)
            break;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx,
                         &handler_data, &establisher, &nonvolatile);
    }
    return n;
}

static LONG CALLBACK xbox_gpu_pb_watch_veh(PEXCEPTION_POINTERS ep)
{
    PEXCEPTION_RECORD er = ep->ExceptionRecord;
    uintptr_t fault_address, fault;
    uintptr_t returns[6] = {0};
    uintptr_t frames[8] = {0};
    unsigned frame_count;
    DWORD old;

    /* Re-protect after exactly the instruction that caused the preceding
     * write fault.  The original one-shot watch always stopped at offset 0;
     * a c184 parameter can sit anywhere later in the same 4-KiB page. */
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP &&
        InterlockedCompareExchange(&s_gpu_pb_watch_rearm, 0, 0) != 0) {
        if (s_gpu_pb_watch_tiled)
            VirtualProtect((void *)s_gpu_pb_watch_tiled, 0x1000,
                           PAGE_READONLY, &old);
        if (s_gpu_pb_watch_contig &&
            s_gpu_pb_watch_contig != s_gpu_pb_watch_tiled)
            VirtualProtect((void *)s_gpu_pb_watch_contig, 0x1000,
                           PAGE_READONLY, &old);
        ep->ContextRecord->EFlags &= ~0x100u;
        InterlockedExchange(&s_gpu_pb_watch_rearm, 0);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        er->NumberParameters < 2 || er->ExceptionInformation[0] != 1 ||
        InterlockedCompareExchange(&s_gpu_pb_watch_armed, 0, 0) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    /* Keep the intra-page address as well as its page.  The prior trace
     * proved that a later generic copy can be the first writer to the same
     * recycled page; its offset tells us whether that copy even reaches the
     * exact command slot PGRAPH had just consumed. */
    fault_address = (uintptr_t)er->ExceptionInformation[1];
    fault = fault_address & ~(uintptr_t)0xFFF;
    if (fault != s_gpu_pb_watch_tiled && fault != s_gpu_pb_watch_contig) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (s_gpu_pb_watch_tiled)
        VirtualProtect((void *)s_gpu_pb_watch_tiled, 0x1000, PAGE_READWRITE, &old);
    if (s_gpu_pb_watch_contig && s_gpu_pb_watch_contig != s_gpu_pb_watch_tiled)
        VirtualProtect((void *)s_gpu_pb_watch_contig, 0x1000, PAGE_READWRITE, &old);

    {
        LONG event = InterlockedIncrement(&s_gpu_pb_watch_events);
        unsigned write_off = (unsigned)(fault_address & 0xFFFu);
        int slot_hit = write_off == s_gpu_pb_watch_slot_off;

        /* The first few writes make the forward progression auditable; the
         * exact target write supplies the one stack that matters. */
        if (event <= 4 || slot_hit) {
            if (event == 1)
                InterlockedIncrement(&s_gpu_pb_watch_hits);
        /* The faulting RIP can be a CRT copy primitive.  Its first stack
         * return addresses, captured before resuming the title, identify the
         * generated caller without relying on a debugger surviving MMIO VEH
         * traffic.  A malformed stack is diagnostic-only, never fatal. */
        __try {
            memcpy(returns, (const void *)ep->ContextRecord->Rsp,
                   sizeof(returns));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(returns, 0, sizeof(returns));
        }
        frame_count = xbox_gpu_pb_watch_unwind(ep->ContextRecord, frames);
        fprintf(stderr,
                "[GPU-PB-WRITER] phys=%08X fault=%p write_off=%03X slot_off=%03X "
                "event=%ld slot_hit=%d alias=%s rip=%p rsp=%p "
                "exe=%p ret=%p,%p,%p,%p,%p,%p bt%u=%p,%p,%p,%p,%p,%p,%p,%p tid=%lu\n",
                s_gpu_pb_watch_phys, (void *)fault,
                write_off, s_gpu_pb_watch_slot_off, event, slot_hit,
                fault == s_gpu_pb_watch_tiled ? "tiled" : "contig",
                (void *)XBOX_CONTEXT_PC(ep->ContextRecord), (void *)ep->ContextRecord->Rsp,
                GetModuleHandle(NULL),
                (void *)returns[0], (void *)returns[1], (void *)returns[2],
                (void *)returns[3], (void *)returns[4], (void *)returns[5],
                frame_count,
                (void *)frames[0], (void *)frames[1], (void *)frames[2],
                (void *)frames[3], (void *)frames[4], (void *)frames[5],
                (void *)frames[6], (void *)frames[7],
                GetCurrentThreadId());
        fflush(stderr);
        }

        if (slot_hit || event >= 1024) {
            if (!slot_hit)
                fprintf(stderr,
                        "[GPU-PB-WATCH] giving up after 1024 writes before "
                        "slot %03X on phys=%08X\n",
                        s_gpu_pb_watch_slot_off, s_gpu_pb_watch_phys);
            /* One exact hit (or its bounded negative result) is enough for
             * this experiment. Do not turn a normal F11 burst into eight
             * independent single-step traces. */
            InterlockedExchange(&s_gpu_pb_watch_hits, 8);
            InterlockedExchange(&s_gpu_pb_watch_armed, 0);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        InterlockedExchange(&s_gpu_pb_watch_rearm, 1);
        ep->ContextRecord->EFlags |= 0x100u;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Update 392: one-shot write watch on a guest heap range (base-RAM view),
 * for the arm-twitch hunt: names the code that writes one skeleton bone's
 * coords. Writes elsewhere in the same page are single-stepped through;
 * the first three writes inside [lo, hi) are logged with a host backtrace
 * of the faulting context, then the watch disarms. Guest bytes unchanged. */
static volatile LONG s_guest_watch_armed, s_guest_watch_rearm;
static volatile LONG s_guest_watch_hits, s_guest_watch_events;
static volatile LONG s_guest_watch_veh_installed;
static uintptr_t s_guest_watch_page;
static uint32_t s_guest_watch_lo, s_guest_watch_hi;
/* Update 457: limits and after-write value log for the env-armed watch
 * (RECOMP_GUEST_WATCH, see xbox_DebugWatchGuestRangeEx). */
static LONG s_guest_watch_max_hits = 3, s_guest_watch_max_events = 20000;
static volatile LONG s_guest_watch_log_after;
/* Update 504: writes by this thread are not hits (xbox_DebugWatchGuestRangeForeign)
 * -- to find another thread writing into one thread's stack. */
static volatile DWORD s_guest_watch_skip_tid;

static LONG CALLBACK xbox_guest_watch_veh(PEXCEPTION_POINTERS ep)
{
    PEXCEPTION_RECORD er = ep->ExceptionRecord;
    uintptr_t fault;
    DWORD old;

    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP &&
        InterlockedCompareExchange(&s_guest_watch_rearm, 0, 0) != 0) {
        if (InterlockedExchange(&s_guest_watch_log_after, 0)) {
            /* The in-range write has now executed: show what it stored. */
            const volatile uint32_t *w = (const volatile uint32_t *)
                ((uintptr_t)g_memory_offset + (s_guest_watch_lo & ~3u));
            fprintf(stderr, "[GUEST-WATCH]   after: %08X %08X %08X %08X\n",
                    w[0], w[1], w[2], w[3]);
            fflush(stderr);
        }
        if (InterlockedCompareExchange(&s_guest_watch_armed, 0, 0) != 0)
            VirtualProtect((void *)s_guest_watch_page, 0x1000, PAGE_READONLY, &old);
        ep->ContextRecord->EFlags &= ~0x100u;
        InterlockedExchange(&s_guest_watch_rearm, 0);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        er->NumberParameters < 2 || er->ExceptionInformation[0] != 1 ||
        InterlockedCompareExchange(&s_guest_watch_armed, 0, 0) == 0)
        return EXCEPTION_CONTINUE_SEARCH;
    fault = (uintptr_t)er->ExceptionInformation[1];
    if ((fault & ~(uintptr_t)0xFFF) != s_guest_watch_page)
        return EXCEPTION_CONTINUE_SEARCH;
    VirtualProtect((void *)s_guest_watch_page, 0x1000, PAGE_READWRITE, &old);
    {
        uint32_t guest = (uint32_t)(fault - (uintptr_t)g_memory_offset);
        LONG events = InterlockedIncrement(&s_guest_watch_events);
        if (guest >= s_guest_watch_lo && guest < s_guest_watch_hi &&
            GetCurrentThreadId() != s_guest_watch_skip_tid) {
            uintptr_t frames[8] = {0};
            unsigned n = xbox_gpu_pb_watch_unwind(ep->ContextRecord, frames);
            LONG hit = InterlockedIncrement(&s_guest_watch_hits);
            fprintf(stderr, "[GUEST-WATCH] hit=%ld guest=%08X event=%ld rip=%p "
                    "exe=%p bt%u=%p,%p,%p,%p,%p,%p,%p,%p tid=%lu\n", hit, guest,
                    events, (void *)XBOX_CONTEXT_PC(ep->ContextRecord), GetModuleHandle(NULL),
                    n, (void *)frames[0], (void *)frames[1], (void *)frames[2],
                    (void *)frames[3], (void *)frames[4], (void *)frames[5],
                    (void *)frames[6], (void *)frames[7], GetCurrentThreadId());
            fflush(stderr);
            if (hit >= s_guest_watch_max_hits) {
                InterlockedExchange(&s_guest_watch_armed, 0);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            InterlockedExchange(&s_guest_watch_log_after, 1);
        }
        if (events >= s_guest_watch_max_events) {
            fprintf(stderr, "[GUEST-WATCH] giving up after %ld writes\n", events);
            InterlockedExchange(&s_guest_watch_armed, 0);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    InterlockedExchange(&s_guest_watch_rearm, 1);
    ep->ContextRecord->EFlags |= 0x100u;
    return EXCEPTION_CONTINUE_EXECUTION;
}

#else
static LONG s_guest_watch_max_hits = 3, s_guest_watch_max_events = 20000;
static volatile DWORD s_guest_watch_skip_tid;
#endif /* _WIN32: write-watch VEHs */

void xbox_DebugWatchGuestRangeEx(uint32_t lo, uint32_t len, long max_hits,
                                 long max_events)
{
    s_guest_watch_max_hits = max_hits > 0 ? max_hits : 3;
    s_guest_watch_max_events = max_events > 0 ? max_events : 20000;
    xbox_DebugWatchGuestRange(lo, len);
}

void xbox_DebugWatchGuestRangeForeign(uint32_t lo, uint32_t len, long max_hits)
{
    s_guest_watch_skip_tid = GetCurrentThreadId();
    xbox_DebugWatchGuestRangeEx(lo, len, max_hits, 2000000000L);
}

void xbox_DebugWatchGuestRange(uint32_t lo, uint32_t len)
{
#if !defined(_WIN32)
    (void)lo; (void)len;
    fprintf(stderr, "[GUEST-WATCH] not available on this host\n");
}
#else
    DWORD old;

    if (!len || ((lo ^ (lo + len - 1)) & ~0xFFFu) ||
        InterlockedCompareExchange(&s_guest_watch_armed, 1, 0) != 0)
        return;
    s_guest_watch_lo = lo;
    s_guest_watch_hi = lo + len;
    s_guest_watch_page = ((uintptr_t)g_memory_offset + lo) & ~(uintptr_t)0xFFF;
    if (InterlockedCompareExchange(&s_guest_watch_veh_installed, 1, 0) == 0)
        AddVectoredExceptionHandler(1, xbox_guest_watch_veh);
    if (!VirtualProtect((void *)s_guest_watch_page, 0x1000, PAGE_READONLY, &old)) {
        fprintf(stderr, "[GUEST-WATCH] cannot protect %08X (%lu)\n", lo,
                GetLastError());
        InterlockedExchange(&s_guest_watch_armed, 0);
        return;
    }
    fprintf(stderr, "[GUEST-WATCH] armed %08X..%08X\n", lo, lo + len);
    fflush(stderr);
}
#endif

/*
 * Surface access tracking support (docs Update 492). xemu's TCG path asks to
 * be told when the CPU touches a GPU surface (mem_access_callback_insert,
 * implemented in src/gpu/nv2a_recomp.c). Here: which host addresses the CPU
 * reaches a physical page through, and the reverse for a faulting address.
 *
 * Only pages of the contiguous-allocation alias region are covered -- where
 * MmAllocateContiguousMemory puts every D3D surface. The CPU sees such a page
 * at exactly two places: the contiguous window (0x80000000 + P, the alias
 * view) and the tiled aperture (0xF0000000 + P). Plain RAM below
 * g_memory_size is also visible through the base view and 63 address-wrap
 * mirrors; surfaces there are left untracked (the old behaviour). The GPU
 * itself uses g_gpu_private_view, which is never protected.
 */
static void *g_gpu_private_view;
static size_t g_gpu_private_size;

int xbox_PhysCpuViews(uint32_t phys, uintptr_t host[2])
{
    uint32_t alias_lo, alias_hi;

    if (!g_tiled_view || !g_contig_alias_view)
        return 0;
    alias_lo = g_contig_alias_base - XBOX_CONTIG_BASE;
    alias_hi = alias_lo + XBOX_CONTIG_SIZE;
    if (phys < alias_lo || phys >= alias_hi || phys >= xbox_TiledApertureSize())
        return 0;
    host[0] = (uintptr_t)XBOX_CONTIG_BASE + phys + g_memory_offset;
    host[1] = (uintptr_t)XBOX_TILED_BASE + phys + g_memory_offset;
    return 2;
}

int xbox_HostToTrackedPhys(uintptr_t host, uint32_t *phys)
{
    uint32_t alias_lo, alias_hi;
    uintptr_t va;

    if (!g_tiled_view || !g_contig_alias_view || host < (uintptr_t)g_memory_offset)
        return 0;
    va = host - (uintptr_t)g_memory_offset;
    if (va > 0xFFFFFFFFu)
        return 0;
    alias_lo = g_contig_alias_base - XBOX_CONTIG_BASE;
    alias_hi = alias_lo + XBOX_CONTIG_SIZE;
    if (va >= XBOX_TILED_BASE && va - XBOX_TILED_BASE < xbox_TiledApertureSize()) {
        *phys = (uint32_t)(va - XBOX_TILED_BASE);
    } else if (va >= XBOX_CONTIG_BASE + alias_lo && va < XBOX_CONTIG_BASE + alias_hi) {
        *phys = (uint32_t)(va - XBOX_CONTIG_BASE);
    } else {
        return 0;
    }
    return *phys >= alias_lo && *phys < alias_hi;
}

/* A host API that writes into guest memory from kernel mode (ReadFile into a
 * title's buffer) does not raise an exception on a protected page -- it just
 * fails (STATUS_UNSUCCESSFUL; SCDA's loading-screen image read 307200 bytes
 * into a tracked surface and got none, docs Update 493). Touch each tracked
 * page first from user mode, rewriting the byte it holds: the access tracker
 * then sees an ordinary CPU write (download if the GPU copy is newer, mark for
 * re-upload) and leaves the surface writable for the call. */
void xbox_PrepareHostWrite(void *host, size_t len)
{
    uintptr_t p, end;
    uint32_t phys;

    if (!host || !len)
        return;
    p = (uintptr_t)host;
    end = p + len;
    while (p < end) {
        if (xbox_HostToTrackedPhys(p, &phys)) {
            volatile uint8_t *b = (volatile uint8_t *)p;
            *b = *b;
        }
        p = (p & ~(uintptr_t)0xFFF) + 0x1000;
    }
}

/* The same for a host API that reads guest memory from kernel mode
 * (WriteFile from a title's buffer): a read touch makes the tracker download
 * a surface whose GPU copy is newer and leaves its pages readable. */
void xbox_PrepareHostRead(const void *host, size_t len)
{
    uintptr_t p, end;
    uint32_t phys;

    if (!host || !len)
        return;
    p = (uintptr_t)host;
    end = p + len;
    while (p < end) {
        if (xbox_HostToTrackedPhys(p, &phys))
            (void)*(volatile const uint8_t *)p;
        p = (p & ~(uintptr_t)0xFFF) + 0x1000;
    }
}

void xbox_DebugWatchGpuPhysicalPage(uint32_t physical_address)
{
#if !defined(_WIN32)
    (void)physical_address;
}
#else
    uint32_t page = physical_address & ~0xFFFu;
    uintptr_t tiled, contig = 0;
    DWORD old;

    if (!g_tiled_view || page >= xbox_TiledApertureSize() ||
        InterlockedCompareExchange(&s_gpu_pb_watch_hits, 0, 0) >= 8 ||
        InterlockedCompareExchange(&s_gpu_pb_watch_armed, 1, 0) != 0) {
        return;
    }
    tiled = (uintptr_t)XBOX_TILED_BASE + page + g_memory_offset;
    if (g_contig_alias_view && page >= (g_contig_alias_base - XBOX_CONTIG_BASE))
        contig = (uintptr_t)XBOX_CONTIG_BASE + page + g_memory_offset;

    s_gpu_pb_watch_phys = page;
    s_gpu_pb_watch_slot_off = physical_address & 0xFFFu;
    s_gpu_pb_watch_tiled = tiled;
    s_gpu_pb_watch_contig = contig;
    InterlockedExchange(&s_gpu_pb_watch_events, 0);
    InterlockedExchange(&s_gpu_pb_watch_rearm, 0);
    if (InterlockedCompareExchange(&s_gpu_pb_watch_veh_installed, 1, 0) == 0)
        AddVectoredExceptionHandler(1, xbox_gpu_pb_watch_veh);

    if (!VirtualProtect((void *)tiled, 0x1000, PAGE_READONLY, &old)) {
        InterlockedExchange(&s_gpu_pb_watch_armed, 0);
        fprintf(stderr, "[GPU-PB-WATCH] cannot protect tiled phys=%08X (%lu)\n",
                page, GetLastError());
        return;
    }
    if (contig && !VirtualProtect((void *)contig, 0x1000, PAGE_READONLY, &old)) {
        VirtualProtect((void *)tiled, 0x1000, PAGE_READWRITE, &old);
        InterlockedExchange(&s_gpu_pb_watch_armed, 0);
        fprintf(stderr, "[GPU-PB-WATCH] cannot protect contig phys=%08X (%lu)\n",
                page, GetLastError());
        return;
    }
    fprintf(stderr, "[GPU-PB-WATCH] armed phys=%08X slot_off=%03X tiled=%p contig=%p\n",
            page, s_gpu_pb_watch_slot_off, (void *)tiled, (void *)contig);
    fflush(stderr);
}
#endif

/* Contiguous / physical memory window (see MemoryLayoutInit).
 * XBOX_CONTIG_BASE / XBOX_CONTIG_SIZE come from kernel.h - the bridges need
 * the same numbers for MmClaimGpuInstanceMemory. */
static void *g_contig_memory = NULL;
/* First 64 KiB of the contiguous window as a view of physical 0 (only with
 * RECOMP_NV2A_XEMU, see the window's allocation). */
static void *g_contig_low_view = NULL;

/* RECOMP_NV2A_XEMU=1 selects xemu's emulated NV2A (src/gpu, docs Update 355).
 * Read once; the memory layout differs slightly for it (see above). */
static int nv2a_xemu_requested(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_NV2A_XEMU");
        v = e && *e && *e != '0';
    }
    return v;
}

/* Alias region for *dynamically* bump-allocated contiguous memory -- see the
 * long comment above xbox_ContiguousAlloc and Update 228/229 in
 * docs/SCDA_WORKLOG.md for why this exists. g_contig_memory above (the
 * original 0x80000000 window) stays exactly as it always was, independent
 * storage, for whatever a title pokes there directly; this second view is a
 * real MapViewOfFileEx alias of the SAME file-mapping section as base RAM,
 * at *fresh* physical offsets (>= g_memory_size) that base RAM's own layout
 * never claims -- see g_contig_alias_need below.
 *
 * That deliberately does NOT make it reachable as a plain base-RAM address:
 * base RAM's own view and every RAM mirror only ever map file offset
 * [0, g_memory_size), which is exactly where the loaded image, the
 * simulated stack, and the heap already live. Extending base RAM's own
 * reach into this region instead of adding a fresh one would either (a)
 * shift where the 64 MB address-bus wraparound mirrors land, a real,
 * load-bearing behaviour other titles' code depends on (the RenderWare
 * memory walker; see the mirror setup below), or (b) require shrinking the
 * heap's own ceiling to free up genuinely spare room in the same 64 MB,
 * which is a real reduction in what a title can allocate and not what was
 * asked for here. So the alias this region provides is two-way -- contig
 * write visible via tiled, tiled write visible via contig, proven below --
 * not three-way; a title's own static data physically cannot collide with
 * a dynamic contig allocation any more, because the two no longer share any
 * physical offset at all. */
static void *g_contig_alias_view = NULL;
static uint32_t g_contig_alias_base = 0;
static uint32_t g_contig_top = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE
                              - XBOX_GPU_INSTANCE_DEFAULT;
/* How much xbox_TiledApertureSize() needs to cover beyond plain RAM to reach
 * the alias region above; set once, early in xbox_MemoryLayoutInit, before
 * anything (including the RAM-mirror setup) calls that function. */
static size_t g_contig_alias_need = 0;
/* Bump pointer for xbox_ContiguousAlloc, declared here (rather than next to
 * that function, further down) because xbox_MemoryLayoutInit needs to
 * redirect it into the alias region above once that is mapped and verified,
 * and this file builds top-to-bottom. Starts at the legacy window and is
 * moved forward on success; see the long comment further down for the
 * physical-aliasing rationale. */
static uint32_t g_contig_next = XBOX_CONTIG_BASE;

/* NV2A GPU register aperture (see MemoryLayoutInit). Backed as plain RAM so
 * that D3D8 code linked into the title can poke it without faulting. */
#define XBOX_NV2A_BASE 0xFD000000u
#define XBOX_NV2A_SIZE (16u * 1024u * 1024u)
static void *g_nv2a_memory = NULL;

/* RECOMP_NV2A_XEMU=1: the register aperture belongs to xemu's emulated NV2A
 * (src/gpu, docs Update 355) instead of being plain memory with the ack
 * thread's shortcuts. Read before the ack thread starts. */
static int g_nv2a_xemu = 0;

/* Whether the emulated NV2A (src/gpu) is attached; kernel_bridge.c gates
 * guest-KEVENT semantics on it (see guest_kevent there). */
int xbox_nv2a_xemu_active(void) { return g_nv2a_xemu; }
extern bool nv2a_recomp_attach(uint8_t *aperture_host, uint8_t *vram_host,
                               uint64_t vram_size);

/* MCPX southbridge register span: APU 0xFE800000 through NIC 0xFEF00000. */
#define XBOX_MCPX_BASE 0xFE800000u
#define XBOX_MCPX_SIZE (8u * 1024u * 1024u)
static void *g_mcpx_memory = NULL;

/* Flash ROM. The console's 256 KB flash is mirrored through the top of the
 * address space, and the MCPX span above stops one page short of it -- so a
 * title that touches it faulted on an address that is perfectly ordinary on
 * hardware.
 *
 * The Xbox Dashboard does, from two directions at once: its XIP workers hash
 * 64 KB from 0xFF000000 (it verifies archives against digests), and its render
 * path writes to 0xFF000040. Both are hard faults today, and they kill the
 * process a few dozen lines after its first frame clears.
 *
 * Plain memory, like the other two apertures, and mapped for the same stated
 * reason: a read of zero is survivable, a fault is not. Zeros are not the
 * console's BIOS, so a digest taken over this will not match one taken over
 * real flash -- that is a separate question from whether the access should
 * fault, and this is the half that has an obviously right answer. */
#define XBOX_FLASH_BASE 0xFF000000u
#define XBOX_FLASH_SIZE (1u * 1024u * 1024u)
static void *g_flash_memory = NULL;
/* How much of the tiled aperture can exist.
 *
 * Two ceilings, both below the mapped RAM size once that is large:
 *
 *   - it starts at 0xF0000000 in a 32-bit guest address space, so it can
 *     never reach past 0x100000000; and
 *   - the NV2A register aperture sits at 0xFD000000, which is where the
 *     window really ends on hardware.
 *
 * Asking for the full RAM size overlapped both and MapViewOfFileEx failed
 * with ERROR_INVALID_ADDRESS -- a warning at startup and then a fault on the
 * title's first surface write, with nothing connecting the two.
 *
 * Also has to reach far enough to alias g_contig_alias_view (see its own
 * comment): that view's physical offsets sit right after plain RAM, and the
 * whole point of it is that the same bytes are visible here too. */
static size_t xbox_TiledApertureSize(void)
{
    uint64_t end = XBOX_NV2A_BASE < 0x100000000ULL
                 ? XBOX_NV2A_BASE : 0x100000000ULL;
    size_t max = (size_t)(end - XBOX_TILED_BASE);
    size_t wanted = g_memory_size > g_contig_alias_need
                  ? g_memory_size : g_contig_alias_need;
    return wanted < max ? wanted : max;
}

static HANDLE g_nv2a_ack_thread = NULL;
static volatile LONG g_nv2a_ack_stop = 0;

/*
 * Thread placement (docs Update 345).
 *
 * 2026-09-11 pinned the whole process to one core, because guest threads the
 * console never runs in parallel raced each other on separate host cores
 * (Bink stream objects, sub_004B89E0/sub_004B8FC0). That also pinned the
 * device threads, and they are not guest threads: on hardware the GPU clears
 * a handshake bit while the CPU spins on it. With both on one core the
 * spinning guest thread holds the core for its whole scheduler quantum before
 * the NV2A acknowledgement thread can clear the bit -- D3D's kick at
 * sub_004A12B0 spins on PFB 0x100410 every frame, and a foreground window
 * stretches the quantum further. Frames took ~100 ms instead of ~33 ms, and
 * Bink's audio producer (one video frame's worth per frame) fell behind the
 * sound hardware until it reported "sound ran dry".
 *
 * So: guest threads on core 0, device threads anywhere else.
 */
extern void xbox_irq_register_guest_thread(void);

/* Keep the guest's whole physical core free (docs Update 637). Guest threads
 * are pinned to logical processor 0; everything else (GPU emulation, the
 * NV2A/MCPX helper, audio, CRT/driver threads) may otherwise land on its SMT
 * sibling and share the core's execution units with the game. A process
 * default CPU set without that core moves all threads that have no explicit
 * placement away from it; the guest threads keep their affinity mask.
 * RECOMP_RESERVE_GUEST_CORE=0 disables this. */
static char s_cpu_reserve_msg[160];

/* Sibling guard (docs Update 646). The guest core's SMT partner still takes
 * other processes' threads -- cooler/RGB tools, music players, overlays --
 * and two threads on one physical core share its execution units: a busy
 * thread pinned to logical CPU 1 took the hq2 turn test from 53.8 to 38.5
 * FPS, the slow spot the player measured. This thread holds the partner at
 * raised priority so the scheduler places other work elsewhere, and parks
 * it with AMD's user-mode MONITORX/MWAITX (a light sleep that leaves the
 * core to the game thread). Without MWAITX (hidden under VBS) it is off;
 * RECOMP_SIBLING_GUARD=2 forces a PAUSE spin instead, =0 disables it. */
static volatile uint8_t s_guard_line[64];
static int s_guard_mode;   /* 0 off, 1 mwaitx, 2 pause */

#if !defined(_WIN32)
/* POSIX hosts (ARM port, docs Update 677): no sibling guard, no CPU-set
 * reservation; the guest CPU is the last of the fastest cores. */
static void start_sibling_guard(int sibling_lp) { (void)sibling_lp; }

static int guest_cpu_auto(DWORD_PTR proc)
{
    int best = 0;
    long best_khz = -1;
    for (int cpu = 0; cpu < 64; cpu++) {
        char path[96];
        long khz = 0;
        FILE *f;
        if (!(proc & ((DWORD_PTR)1 << cpu)))
            continue;
        snprintf(path, sizeof path,
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%ld", &khz) != 1)
                khz = 0;
            fclose(f);
        }
        if (khz >= best_khz) {
            best_khz = khz;
            best = cpu;
        }
    }
    return best;
}

/* The fast cluster of a big.LITTLE host: every CPU within 10 % of the
 * highest maximum clock (RK3588: the A76 cores 4-7, not the A55s 0-3). */
static DWORD_PTR big_cpu_mask(DWORD_PTR proc)
{
    long khz[64] = {0}, top = 0;
    DWORD_PTR mask = 0;
    for (int cpu = 0; cpu < 64; cpu++) {
        char path[96];
        FILE *f;
        if (!(proc & ((DWORD_PTR)1 << cpu)))
            continue;
        snprintf(path, sizeof path,
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%ld", &khz[cpu]) != 1)
                khz[cpu] = 0;
            fclose(f);
        }
        if (khz[cpu] > top)
            top = khz[cpu];
    }
    for (int cpu = 0; cpu < 64; cpu++)
        if ((proc & ((DWORD_PTR)1 << cpu)) && khz[cpu] * 10 >= top * 9)
            mask |= (DWORD_PTR)1 << cpu;
    return mask ? mask : proc;
}
#else
static DWORD WINAPI sibling_guard_thread(LPVOID param)
{
    DWORD_PTR mask = (DWORD_PTR)param;
    SetThreadAffinityMask(GetCurrentThread(), mask);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (s_guard_mode == 1) {
        __try {
            for (;;) {
                _mm_monitorx((void *)s_guard_line, 0, 0);
                _mm_mwaitx(2, 0, 2000000);   /* timer-bounded, C1 hint */
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s_guard_mode = 2;   /* MWAITX not usable here: fall back */
        }
    }
    for (;;)
        _mm_pause();
    return 0;
}

static void start_sibling_guard(int sibling_lp)
{
    const char *e = getenv("RECOMP_SIBLING_GUARD");
    int regs[4];
    if ((e && e[0] == '0') || sibling_lp < 0 || sibling_lp >= 64)
        return;
    __cpuid(regs, 0x80000000);
    s_guard_mode = 2;
    if ((unsigned)regs[0] >= 0x80000001u) {
        __cpuid(regs, 0x80000001);
        if (regs[2] & (1 << 29))   /* MwaitExtended */
            s_guard_mode = 1;
    }
    /* PAUSE spinning costs the game thread more than it saves (turn test
     * 53.8 -> 46.4 FPS with no other load): only on request. MWAITX is
     * hidden while Windows runs under its hypervisor (VBS). */
    if (s_guard_mode == 2 && !(e && e[0] == '2')) {
        s_guard_mode = 0;
        return;
    }
    HANDLE h = CreateThread(NULL, 64 * 1024, sibling_guard_thread,
                            (LPVOID)((DWORD_PTR)1 << sibling_lp), 0, NULL);
    if (h) {
        SetThreadDescription(h, L"recomp sibling guard");
        CloseHandle(h);
    } else {
        s_guard_mode = 0;
    }
}

/* The logical processor the guest threads share (docs Update 646c, 647b):
 * RECOMP_GUEST_CPU=<n> overrides, otherwise chosen once at start:
 *
 *   1. only the fastest cores (highest EfficiencyClass: Intel P-cores,
 *      never E-cores);
 *   2. among their L3 caches the largest (an X3D part's V-Cache CCD),
 *      on a tie one without CPU 0 (second CCD / CCX), else the later one;
 *   3. in that cache the last physical core, not core 0, first thread.
 *
 * Windows runs much of its own work on core 0 and tends to place other
 * programs on the first CCD. hq2 turn test (5950X, heavy views): CPU 0
 * 53.9 FPS, CPU 14 (same CCD, last core) 56.1, CPU 16 (second CCD) 57.2;
 * under memory-heavy background load CPU 0 16.9 vs CPU 16 24.4. */
static int guest_cpu_auto(DWORD_PTR proc)
{
    GuestCpuLp lps[64];
    GuestCpuL3 l3[16];
    int nlp = 0, nl3 = 0;
    ULONG len = 0;

    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    uint8_t *cs = len ? (uint8_t *)malloc(len) : NULL;
    if (cs && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)cs, len, &len,
                                         GetCurrentProcess(), 0)) {
        for (ULONG off = 0; off < len && nlp < 64;) {
            SYSTEM_CPU_SET_INFORMATION *x = (SYSTEM_CPU_SET_INFORMATION *)(cs + off);
            if (x->Type == CpuSetInformation && x->CpuSet.Group == 0 &&
                x->CpuSet.LogicalProcessorIndex < 64 &&
                (proc & ((DWORD_PTR)1 << x->CpuSet.LogicalProcessorIndex))) {
                lps[nlp].lp = x->CpuSet.LogicalProcessorIndex;
                lps[nlp].core = x->CpuSet.CoreIndex;
                lps[nlp].eff = x->CpuSet.EfficiencyClass;
                nlp++;
            }
            off += x->Size;
        }
    }
    free(cs);

    DWORD clen = 0;
    GetLogicalProcessorInformationEx(RelationCache, NULL, &clen);
    uint8_t *cb = clen ? (uint8_t *)malloc(clen) : NULL;
    if (cb && GetLogicalProcessorInformationEx(RelationCache,
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)cb, &clen)) {
        for (DWORD off = 0; off < clen && nl3 < 16;) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX x =
                (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(cb + off);
            if (x->Relationship == RelationCache && x->Cache.Level == 3 &&
                x->Cache.GroupMask.Group == 0) {
                l3[nl3].mask = (uint64_t)x->Cache.GroupMask.Mask & (uint64_t)proc;
                l3[nl3].size = x->Cache.CacheSize;
                nl3++;
            }
            off += x->Size;
        }
    }
    free(cb);
    return guest_cpu_pick(lps, nlp, l3, nl3);
}
#endif /* _WIN32 */

int xbox_GuestCpu(void)
{
    static int lp = -1;
    if (lp < 0) {
        const char *e = getenv("RECOMP_GUEST_CPU");
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        if (e && *e) {
            int want = atoi(e);
            lp = (want > 0 && want < 64 && (proc & ((DWORD_PTR)1 << want))) ? want : 0;
        } else {
            char v[8];
            lp = guest_cpu_auto(proc);
            /* The APU library reads the variable itself (apu_guest_mask). */
            snprintf(v, sizeof v, "%d", lp);
            _putenv_s("RECOMP_GUEST_CPU", v);
            SetEnvironmentVariableA("RECOMP_GUEST_CPU", v);
        }
    }
    return lp;
}

static void reserve_guest_physical_core(void)
{
    static volatile LONG done;
    if (InterlockedExchange(&done, 1))
        return;
    const char *e = getenv("RECOMP_RESERVE_GUEST_CORE");
    if (e && e[0] == '0')
        return;
#if !defined(_WIN32)
    /* POSIX: threads started from now on avoid the guest core(s), like the
     * Windows CPU-set reservation, and on big.LITTLE hosts stay on the fast
     * cluster (RECOMP_DEVICE_ANY_CPU=1: any other core) */
    {
        DWORD_PTR proc = 0, sys = 0, guest, avail;
        const char *mask_env = getenv("RECOMP_GUEST_CPU_MASK");
        const char *any = getenv("RECOMP_DEVICE_ANY_CPU");
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        guest = (DWORD_PTR)1 << xbox_GuestCpu();
        if (mask_env && *mask_env && strtoull(mask_env, NULL, 0))
            guest = (DWORD_PTR)strtoull(mask_env, NULL, 0) & proc;
        avail = ((any && any[0] == '1') ? proc : big_cpu_mask(proc)) & ~guest;
        if (!avail)
            avail = proc & ~guest;
        if (avail)
            w32_set_default_thread_affinity(avail);
        snprintf(s_cpu_reserve_msg, sizeof(s_cpu_reserve_msg),
                 "[CPU] guest cpus 0x%llX reserved; other threads on 0x%llX\n",
                 (unsigned long long)guest, (unsigned long long)avail);
    }
    return;
#else
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len)
        return;
    uint8_t *buf = (uint8_t *)malloc(len);
    ULONG *ids = (ULONG *)malloc(len);
    if (!buf || !ids ||
        !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len,
                                    GetCurrentProcess(), 0)) {
        free(buf);
        free(ids);
        return;
    }
    int guest_core = -1, sibling_lp = -1;
    for (ULONG off = 0; off < len;) {
        SYSTEM_CPU_SET_INFORMATION *s = (SYSTEM_CPU_SET_INFORMATION *)(buf + off);
        if (s->Type == CpuSetInformation && s->CpuSet.Group == 0 &&
            s->CpuSet.LogicalProcessorIndex == xbox_GuestCpu())
            guest_core = s->CpuSet.CoreIndex;
        off += s->Size;
    }
    for (ULONG off = 0; off < len;) {
        SYSTEM_CPU_SET_INFORMATION *s = (SYSTEM_CPU_SET_INFORMATION *)(buf + off);
        if (s->Type == CpuSetInformation && s->CpuSet.Group == 0 &&
            s->CpuSet.CoreIndex == guest_core &&
            s->CpuSet.LogicalProcessorIndex != xbox_GuestCpu())
            sibling_lp = s->CpuSet.LogicalProcessorIndex;
        off += s->Size;
    }
    ULONG n = 0, total = 0;
    for (ULONG off = 0; off < len;) {
        SYSTEM_CPU_SET_INFORMATION *s = (SYSTEM_CPU_SET_INFORMATION *)(buf + off);
        if (s->Type == CpuSetInformation) {
            total++;
            if (!(s->CpuSet.Group == 0 && s->CpuSet.CoreIndex == guest_core))
                ids[n++] = s->CpuSet.Id;
        }
        off += s->Size;
    }
    BOOL ok = guest_core >= 0 && n && n < total &&
              SetProcessDefaultCpuSets(GetCurrentProcess(), ids, n);
    if (ok)
        start_sibling_guard(sibling_lp);
    /* The first call comes before the log is redirected: keep the line for
     * the next guest thread to print. */
    snprintf(s_cpu_reserve_msg, sizeof(s_cpu_reserve_msg),
             "[CPU] guest cpu %d core %d %s: other threads on %lu of %lu "
             "logical processors (err %lu), sibling %d guard %s\n", xbox_GuestCpu(), guest_core,
             ok ? "reserved" : "NOT reserved", n, total, ok ? 0 : GetLastError(),
             sibling_lp, s_guard_mode == 1 ? "mwaitx" :
                         s_guard_mode == 2 ? "pause" : "off");
    free(buf);
    free(ids);
#endif
}

void xbox_PinGuestThread(void)
{
    /* Room for the unhandled-exception filter after a host stack overflow:
     * without it the filter itself overflows and the log ends mid-line with
     * no call chain (docs Update 353, 0xC00000FD after the fade fix). */
    ULONG guarantee = 64 * 1024;

    static volatile LONG printed;
    /* RECOMP_GUEST_UNPINNED=1 (docs Update 677): guest threads run on any
     * core, as the TGP482 port does. The heap race the pin once hid is fixed
     * on its own; test switch for weak cores (ARM board under Box64). */
    static int unpinned = -1;
    if (unpinned < 0) {
        const char *e = getenv("RECOMP_GUEST_UNPINNED");
        unpinned = e && e[0] == '1';
    }
    if (unpinned) {
        if (InterlockedIncrement(&printed) == 2)
            fputs("[CPU] guest threads unpinned (RECOMP_GUEST_UNPINNED=1)\n",
                  stderr);
    } else {
        DWORD_PTR guest_mask = (DWORD_PTR)1 << xbox_GuestCpu();
#if !defined(_WIN32)
        /* Opt-in for heterogeneous ARM hosts: allow the guest threads to
         * share only selected fast cores, never the slower cluster. A single
         * pinned CPU is still the default until gameplay A/B validates this. */
        const char *mask_env = getenv("RECOMP_GUEST_CPU_MASK");
        if (mask_env && *mask_env) {
            char *end;
            unsigned long long requested = strtoull(mask_env, &end, 0);
            DWORD_PTR proc = 0, sys = 0;
            if (end != mask_env && !*end && requested &&
                GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) &&
                ((DWORD_PTR)requested & proc))
                guest_mask = (DWORD_PTR)requested & proc;
        }
#endif
        reserve_guest_physical_core();
        if (InterlockedIncrement(&printed) == 2) {
            if (s_cpu_reserve_msg[0])
                fputs(s_cpu_reserve_msg, stderr);
#if !defined(_WIN32)
            fprintf(stderr, "[CPU] guest affinity mask 0x%llX\n",
                    (unsigned long long)guest_mask);
#endif
        }
        SetThreadAffinityMask(GetCurrentThread(), guest_mask);
    }
    SetThreadStackGuarantee(&guarantee);
    /* Known to the async interrupt injector (kernel_bridge.c). */
    xbox_irq_register_guest_thread();
}

void xbox_PinDeviceThread(void *h)
{
    DWORD_PTR proc = 0, sys = 0;

    if (!h || !GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys))
        return;
#if !defined(_WIN32)
    /* big.LITTLE (ARM port): the device threads (PFIFO/renderer, APU,
     * window) on the fast cores next to the guest's, not on the slow
     * cluster; RECOMP_DEVICE_ANY_CPU=1 allows every other core again */
    {
        static int any = -1;
        if (any < 0) {
            const char *e = getenv("RECOMP_DEVICE_ANY_CPU");
            any = e && e[0] == '1';
        }
        if (!any) {
            DWORD_PTR fast = big_cpu_mask(proc) & ~((DWORD_PTR)1 << xbox_GuestCpu());
            if (fast) {
                SetThreadAffinityMask((HANDLE)h, fast);
                return;
            }
        }
    }
#endif
    if (proc & ~((DWORD_PTR)1 << xbox_GuestCpu()))
        SetThreadAffinityMask((HANDLE)h, proc & ~((DWORD_PTR)1 << xbox_GuestCpu()));
}

/*
 * NV2A busy-bit acknowledgement.
 *
 * D3D8 talks to the GPU through set-a-bit / wait-for-hardware-to-clear-it
 * handshakes. Against plain RAM the bit is set and nothing ever clears it, so
 * the title spins forever. Halo hangs in the push-buffer kick at 0x001EF930:
 *
 *     mov  [eax+0x100410], edx     ; set 0x10000
 *   L: test [eax+0x100410], 0x10000
 *     jne  L                       ; wait for the GPU
 *
 * Clearing those bits from a thread is not a hack around the handshake, it is
 * the handshake: on hardware the GPU clears them asynchronously, which is
 * exactly what this does. Work that would have been submitted is being done by
 * the D3D11 layer instead, so acknowledging immediately is honest.
 *
 * Only registers listed here are touched. Blanket-zeroing the aperture would
 * also wipe registers holding real state.
 *
 * ponytail: table-driven, extend as more handshakes turn up. A spin on a bit
 * that is not listed still hangs -- run the title and the watchdog sample will
 * name the register.
 */
static const struct { uint32_t offset; uint32_t busy_mask; } NV2A_ACK[] = {
    { 0x100410, 0x00010000u },  /* PFB flush kick, Halo 0x001EF930 */

    /* Interrupt status registers. These are write-1-to-clear on hardware, so
     * an ISR "clearing" one writes the pending bit back -- against plain RAM
     * that sets it instead, the interrupt stays pending forever, and the
     * service routine re-enters until the stack is gone. Halo dies exactly
     * that way: CMiniport::ServiceGrInterrupt writes 0x1000 to PGRAPH_INTR to
     * acknowledge, reads it back still pending, and recurses into a native
     * stack overflow.
     *
     * Holding them at zero is correct rather than convenient: nothing here
     * ever raises a GPU interrupt, so "none pending" is the truth. */
    { 0x000100, 0xFFFFFFFFu },  /* PMC_INTR_0    */
    { 0x001100, 0xFFFFFFFFu },  /* PBUS_INTR_0   */
    { 0x002100, 0xFFFFFFFFu },  /* PFIFO_INTR_0  */
    { 0x400100, 0xFFFFFFFFu },  /* PGRAPH_INTR   */
    { 0x600100, 0xFFFFFFFFu },  /* PCRTC_INTR_0  */
};

/*
 * Bits that must always read as SET. The mirror image of the table above:
 * where an interrupt-pending bit is false because nothing raises interrupts,
 * a queue-empty bit is true because nothing is queued.
 *
 * Halo's CMiniport::TilingUpdateIdle spins until the PFIFO caches report
 * empty (0x001F5CD1). Zeroed RAM says "not empty" forever, so tile setup
 * during CDevice::InitializeFrameBuffers never completes.
 *
 * Note 0x003220 is deliberately absent -- that one exits on the bit being
 * CLEAR, which zeroed memory already gives.
 */
static const struct { uint32_t offset; uint32_t idle_mask; } NV2A_IDLE[] = {
    { 0x002400, 0x00000010u },  /* PFIFO_RUNOUT_STATUS  LOW_MARK (empty) */
    { 0x003214, 0x00000010u },  /* PFIFO_CACHE1_STATUS  LOW_MARK (empty) */
};

/*
 * PFIFO channel DMA pointers. Software writes DMA_PUT and spins until the GPU
 * advances DMA_GET to match -- "you have consumed everything I submitted".
 * Halo's wait is at 0x001F3948:
 *
 *   L: call BusyLoop
 *      ecx = [[dev+0x2304] + 0x44]   ; DMA_GET
 *      edx = [dev]                   ; DMA_PUT
 *      test (edx ^ ecx), 0xfffffff
 *      jne L
 *
 * [dev+0x2304] is 0xFD800000, so the channel's USER area sits at aperture
 * offset 0x800000 and the two pointers are at +0x40 / +0x44. Copying PUT to
 * GET is the acknowledgement; the commands are not executed from the push
 * buffer here -- the D3D11 layer draws -- so reporting them consumed is the
 * truthful answer.
 *
 * This was written once, removed, and restored. It was removed because
 * [dev+0x2304] read as 0x0080F7FF, i.e. no register to acknowledge -- but that
 * garbage was a downstream symptom of ordinal 47 having no stdcall arg size,
 * which walked esp 8 bytes off and made D3D initialise the DMA channel with
 * `this` = 1. With that fixed the pointer is correct and so is this.
 */
#define NV2A_USER_DMA_PUT 0x800040u
#define NV2A_USER_DMA_GET 0x800044u

/*
 * Free-running counters in the MCPX aperture.
 *
 * Some hardware registers are clocks, not flags: software reads them and waits
 * until the value passes a target. Against zeroed RAM the value never moves and
 * the wait is forever. DirectSound's CMcpxCore::SetupVoiceProcessor spins on
 * the APU sample counter at 0xFE820010 exactly this way, which is where Halo
 * stopped once input initialisation started working.
 *
 * Ticking it is the honest model: on hardware this counter advances on its own
 * whether or not anything is listening.
 *
 * ponytail: the rate is "as fast as this thread loops", not 48 kHz. Nothing
 * paces audio off it yet. Derive it from a real clock if timing starts to
 * matter.
 */
static const uint32_t MCPX_COUNTERS[] = {
    0x020010,   /* APU GP sample counter, DirectSound SetupVoiceProcessor */
};

static void *g_mcpx_regs = NULL;
/* Set when the APU's registers are unmapped so they can be routed to the
 * emulated APU. Once that happens they are no longer plain memory, and the
 * counter ticking below must leave them alone -- writing through the pointer
 * faults, and the emulated APU owns those registers anyway. */
static int g_apu_mmio_trapped = 0;

/* The emulated APU (src/apu, extracted from xemu). Its MMIO handler decodes
 * the faulting host instruction, performs the register access against the
 * emulated device and advances RIP -- but nothing ever routed the faults to
 * it, so with the aperture trapped every APU access crashed the process in
 * DirectSound (SCDA sub_004D8511, docs Update 340). */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern void mcpx_apu_set_irq_handler(void (*handler)(int asserted));
extern void xbox_irq_apu_line(int asserted);
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

/* The emulated AC'97 controller at 0xFEC00000 (mcpx_aci.c). */
extern BOOL mcpx_aci_attach(uint8_t *mmio_page, uint8_t *phys);

static uintptr_t g_apu_host_lo, g_apu_host_hi;

/* Direct device-register access from recompiled driver code (IOR/IOW in
 * recomp_types.h, docs Update 643): the same call the fault handlers make
 * (apu_decode_and_handle, nv2a_mmio_veh), on the same thread, minus the
 * exception round trip. A device's bit is set once its trap and emulation
 * are both up; RECOMP_IO_DIRECT=0 keeps every access on the fault path.
 * Bit 0: APU (here), bit 1: NV2A (src/gpu/nv2a_recomp.c). */
int g_recomp_io_direct = 0;
extern ptrdiff_t g_xbox_mem_offset;   /* defined below */
extern uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr,
                                   unsigned int size);
extern void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val,
                                unsigned int size);
extern uint32_t nv2a_recomp_direct_read(uint32_t off, unsigned size);
extern void nv2a_recomp_direct_write(uint32_t off, uint32_t val, unsigned size);
static volatile LONG g_io_direct_count;

void xbox_io_direct_enable(int bit)
{
    const char *e = getenv("RECOMP_IO_DIRECT");
    if (!(e && e[0] == '0'))
        g_recomp_io_direct |= bit;
}

static void io_direct_note(void)
{
    if ((InterlockedIncrement(&g_io_direct_count) & 0x3FFFF) == 0) {
        static DWORD t0;
        DWORD now = GetTickCount();
        if (t0)
            fprintf(stderr, "[IO-DIRECT] %ld direct accesses, %.0f per second\n",
                    g_io_direct_count,
                    262144.0 * 1000.0 / (double)(now - t0 ? now - t0 : 1));
        t0 = now;
    }
}

uint32_t recomp_io_read(uint32_t va, uint32_t size)
{
    uintptr_t p = (uintptr_t)va + g_xbox_mem_offset;
    if (va - XBOX_MCPX_BASE < 0x00080000u) {
        if (g_recomp_io_direct & 1) {
            io_direct_note();
            return (uint32_t)mcpx_apu_mmio_read(g_apu_state, va - XBOX_MCPX_BASE, size);
        }
    } else if (va - XBOX_NV2A_BASE < 0x01000000u) {
        if (g_recomp_io_direct & 2) {
            io_direct_note();
            return nv2a_recomp_direct_read(va - XBOX_NV2A_BASE, size);
        }
    }
    /* Not routed: the ordinary access, faulting as before where trapped. */
    return size == 1 ? *(volatile uint8_t *)p
         : size == 2 ? *(volatile uint16_t *)p : *(volatile uint32_t *)p;
}

void recomp_io_write(uint32_t va, uint32_t val, uint32_t size)
{
    uintptr_t p = (uintptr_t)va + g_xbox_mem_offset;
    if (va - XBOX_MCPX_BASE < 0x00080000u) {
        if (g_recomp_io_direct & 1) {
            io_direct_note();
            mcpx_apu_mmio_write(g_apu_state, va - XBOX_MCPX_BASE, val, size);
            return;
        }
    } else if (va - XBOX_NV2A_BASE < 0x01000000u) {
        if (g_recomp_io_direct & 2) {
            io_direct_note();
            nv2a_recomp_direct_write(va - XBOX_NV2A_BASE, val, size);
            return;
        }
    }
    if (size == 1)
        *(volatile uint8_t *)p = (uint8_t)val;
    else if (size == 2)
        *(volatile uint16_t *)p = (uint16_t)val;
    else
        *(volatile uint32_t *)p = val;
}

static LONG CALLBACK apu_mmio_veh(PEXCEPTION_POINTERS ep)
{
    PEXCEPTION_RECORD er = ep->ExceptionRecord;
    uintptr_t fault;

    static int veh_trace = -1;
    if (veh_trace < 0) {
        const char *e = getenv("RECOMP_VEH_TRACE");
        veh_trace = (e && *e && *e != '0') ? 1 : 0;
    }

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION
            || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    fault = (uintptr_t)er->ExceptionInformation[1];
    if (fault < g_apu_host_lo || fault >= g_apu_host_hi) {
        /* Diagnostics (docs Update 344): a crash whose minidump never gets
         * written still leaves its first-chance fault here. */
        static volatile LONG traced;
        if (veh_trace && InterlockedIncrement(&traced) <= 40) {
            HMODULE mod = GetModuleHandleA(NULL);
            fprintf(stderr, "[VEH-AV] tid=%lu rip=%p (exe+0x%llX) %s 0x%llX\n",
                    GetCurrentThreadId(), (void *)XBOX_CONTEXT_PC(ep->ContextRecord),
                    (unsigned long long)(XBOX_CONTEXT_PC(ep->ContextRecord) - (uintptr_t)mod),
                    er->ExceptionInformation[0] == 1 ? "write" : "read",
                    (unsigned long long)fault);
            fflush(stderr);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (apu_hook_handle_mmio(ep->ContextRecord, fault,
                             (uint32_t)(XBOX_MCPX_BASE + (fault - g_apu_host_lo)),
                             er->ExceptionInformation[0] == 1))
        return EXCEPTION_CONTINUE_EXECUTION;
    {
        /* Not decodable: this becomes an unhandled crash, so say where. */
        static volatile LONG undecoded;
        if (InterlockedIncrement(&undecoded) <= 20) {
            const uint8_t *ip = (const uint8_t *)XBOX_CONTEXT_PC(ep->ContextRecord);
            fprintf(stderr, "[APU-VEH] undecoded %s at APU+0x%05llX, rip=%p "
                    "bytes %02X %02X %02X %02X %02X %02X\n",
                    er->ExceptionInformation[0] == 1 ? "write" : "read",
                    (unsigned long long)(fault - g_apu_host_lo), (void *)ip,
                    ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
            fflush(stderr);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/*
 * GPU completion fences the title waits on in guest memory rather than in the
 * aperture. See xbox_Nv2aMirrorFence in the header for why this is the same
 * acknowledgement the NV2A_ACK table makes, and why the address has to be
 * followed through the device struct instead of being a constant.
 */
#define XBOX_MAX_FENCE_MIRRORS 4

static struct {
    uint32_t device_ptr_va;
    uint32_t put_off;
    uint32_t get_ptr_off;
} g_fence_mirrors[XBOX_MAX_FENCE_MIRRORS];
static int g_fence_mirror_count = 0;

int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off)
{
    if (g_fence_mirror_count >= XBOX_MAX_FENCE_MIRRORS)
        return -1;
    g_fence_mirrors[g_fence_mirror_count].device_ptr_va = device_ptr_va;
    g_fence_mirrors[g_fence_mirror_count].put_off = put_off;
    g_fence_mirrors[g_fence_mirror_count].get_ptr_off = get_ptr_off;
    g_fence_mirror_count++;
    fprintf(stderr, "  NV2A fence mirror: device at 0x%08X,"
            " PUT +0x%X -> *(GET +0x%X)\n",
            device_ptr_va, put_off, get_ptr_off);
    return 0;
}

/* A guest address is usable only once the window is mapped and it lands
 * inside it; the chain is followed fresh every poll because the title may not
 * have built it yet. */
static int fence_readable(uint32_t va, uint32_t bytes)
{
    /* Page zero is unmapped, so the bound is the first mapped page rather than
     * just "not null": the device pointer is zero until the title creates the
     * device, and this thread polls from before that. Rejecting only 0 let
     * dev + get_ptr_off through as 0x34 and faulted on the very first tick. */
    if (g_memory_base == NULL || va < XBOX_FS_BASE)
        return 0;
    /* The contiguous window is mapped separately and sits far above the main
     * range, so a size check against g_memory_size rejects it. The fence a
     * title waits on is exactly the kind of block that lives there --
     * MmAllocateContiguousMemory is where a GPU-written semaphore comes
     * from -- so a chain ending in that window has to be followed, not
     * discarded. */
    if (va >= XBOX_CONTIG_BASE
            && (uint64_t)va + bytes <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE)
        return g_contig_memory != NULL;
    /* Dynamic contig allocations (xbox_ContiguousAlloc) live in the alias
     * region above, not the legacy window checked above -- a fence inside a
     * GPU-written buffer from there has to be followed too. */
    if (g_contig_alias_view && va >= g_contig_alias_base
            && (uint64_t)va + bytes <= (uint64_t)g_contig_alias_base + XBOX_CONTIG_SIZE)
        return 1;
    return (size_t)va + bytes <= g_memory_size;
}

/*
 * Frame counters the title polls to pace itself.
 *
 * D3D keeps a swap count inside the device and bumps it once per presented
 * frame; a title that wants to wait a frame reads it and spins until it moves.
 * Wreckless does exactly that at guest 0x000DC5E0 -- "loop while the counter
 * has advanced by less than 2" -- so a counter that never moves is not a
 * dropped frame, it is a hang with a full asset load behind it.
 *
 * Nothing here presents, so nothing would ever move it. Advancing it on a
 * clock is what makes the wait terminate, and 60 Hz is the rate the title
 * expects the display to run at. Followed through the device pointer for the
 * same reason the fence is: the device is allocated at runtime.
 */
#define XBOX_MAX_FRAME_COUNTERS 4
#define XBOX_FRAME_PERIOD_MS    16      /* ~60 Hz */

static struct {
    uint32_t device_ptr_va;
    uint32_t counter_off;
} g_frame_counters[XBOX_MAX_FRAME_COUNTERS];
static int   g_frame_counter_count = 0;
static DWORD g_frame_counter_last_ms = 0;

int xbox_Nv2aFrameCounter(uint32_t device_ptr_va, uint32_t counter_off)
{
    if (g_frame_counter_count >= XBOX_MAX_FRAME_COUNTERS)
        return -1;
    g_frame_counters[g_frame_counter_count].device_ptr_va = device_ptr_va;
    g_frame_counters[g_frame_counter_count].counter_off   = counter_off;
    g_frame_counter_count++;
    fprintf(stderr, "  Frame counter: device at 0x%08X, count +0x%X @ %d Hz\n",
            device_ptr_va, counter_off, 1000 / XBOX_FRAME_PERIOD_MS);
    return 0;
}

static void frame_counters_tick(void)
{
    DWORD now = GetTickCount();
    int i;

    if (!g_frame_counter_count)
        return;
    if (g_frame_counter_last_ms
            && (now - g_frame_counter_last_ms) < XBOX_FRAME_PERIOD_MS)
        return;
    g_frame_counter_last_ms = now;

    for (i = 0; i < g_frame_counter_count; i++) {
        uint32_t dev;

        if (!fence_readable(g_frame_counters[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_frame_counters[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_frame_counters[i].counter_off, 4))
            continue;
        *(volatile uint32_t *)((uintptr_t)(dev + g_frame_counters[i].counter_off)
                               + g_memory_offset) += 1;
    }
}

static void fence_mirrors_tick(void)
{
    for (int i = 0; i < g_fence_mirror_count; i++) {
        uint32_t dev, get_ptr;

        if (!fence_readable(g_fence_mirrors[i].device_ptr_va, 4))
            continue;
        dev = *(volatile uint32_t *)((uintptr_t)g_fence_mirrors[i].device_ptr_va
                                     + g_memory_offset);
        if (!fence_readable(dev + g_fence_mirrors[i].get_ptr_off, 4)
                || !fence_readable(dev + g_fence_mirrors[i].put_off, 4))
            continue;
        get_ptr = *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].get_ptr_off)
                                         + g_memory_offset);
        if (!fence_readable(get_ptr, 4))
            continue;
        {
            volatile uint32_t *fence =
                (volatile uint32_t *)((uintptr_t)get_ptr + g_memory_offset);
            uint32_t put =
                *(volatile uint32_t *)((uintptr_t)(dev + g_fence_mirrors[i].put_off)
                                       + g_memory_offset);
            if (*fence != put)
                *fence = put;
        }
    }
}

static int s_nv2a_trace = 0;

/* The display framebuffer, as reported by AvSetDisplayMode. Checksummed once a
 * second so a run can answer the only question that matters before building a
 * presenter: is the guest putting pixels anywhere at all, and do they change
 * from frame to frame. */
static uint32_t s_fb_va, s_fb_pitch, s_fb_height = 480;

void xbox_SetDisplayFramebuffer(uint32_t fb_va, uint32_t pitch)
{
    uint32_t resolved = 0;

    /* AvSetDisplayMode states the scanout address in physical form. Keep the
     * diagnostic probe on the same bytes the CPU/GPU surface aliases expose,
     * rather than accidentally reading the same numeric address in base RAM. */
    if (xbox_ResolveGpuPhysicalRange(fb_va, pitch * s_fb_height, &resolved))
        s_fb_va = resolved;
    else
        s_fb_va = 0;
    s_fb_pitch = pitch;
}

static void framebuffer_probe_tick(void)
{
    static DWORD last_ms;
    static uint32_t last_sum;
    DWORD now = GetTickCount();
    uint32_t sum = 0, nonzero = 0, i, n;
    const uint32_t *p;

    if (!s_nv2a_trace || !s_fb_va || !s_fb_pitch)
        return;
    if (last_ms && (now - last_ms) < 1000)
        return;
    last_ms = now;
    if ((size_t)s_fb_va + s_fb_pitch * s_fb_height > g_memory_size)
        return;
    p = (const uint32_t *)((uintptr_t)s_fb_va + g_memory_offset);
    n = (s_fb_pitch * s_fb_height) / 4;
    for (i = 0; i < n; i++) {
        sum = sum * 33u + p[i];
        if (p[i]) nonzero++;
    }
    fprintf(stderr, "  [FB] 0x%08X sum=%08X nonzero=%u/%u %s\n",
            s_fb_va, sum, nonzero, n,
            sum != last_sum ? "CHANGED" : "same");
    last_sum = sum;
    fflush(stderr);
}

static DWORD WINAPI nv2a_ack_thread(LPVOID param)
{
    volatile uint32_t *regs = (volatile uint32_t *)param;
    while (!InterlockedCompareExchange(&g_nv2a_ack_stop, 0, 0)) {
        /* With the emulated NV2A attached (RECOMP_NV2A_XEMU) the aperture is
         * trapped and every register below is the real device's business:
         * no acknowledgements, no GET=PUT, no fence mirror, no survey. Only
         * the non-GPU duties (MCPX counters, KeTickCount) remain. */
        if (g_nv2a_xemu)
            goto non_gpu;
        for (size_t i = 0; i < sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_ACK[i].offset);
            if (*r & NV2A_ACK[i].busy_mask) {
                *r &= ~NV2A_ACK[i].busy_mask;
            }
        }
        for (size_t i = 0; i < sizeof(NV2A_IDLE) / sizeof(NV2A_IDLE[0]); i++) {
            volatile uint32_t *r =
                (volatile uint32_t *)((char *)regs + NV2A_IDLE[i].offset);
            if ((*r & NV2A_IDLE[i].idle_mask) != NV2A_IDLE[i].idle_mask) {
                *r |= NV2A_IDLE[i].idle_mask;
            }
        }
        {
            volatile uint32_t *put =
                (volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            volatile uint32_t *get =
                (volatile uint32_t *)((char *)regs + NV2A_USER_DMA_GET);
            if (*get != *put) {
                *get = *put;
            }
        }
        fence_mirrors_tick();
        frame_counters_tick();
        framebuffer_probe_tick();

        /* Which framebuffer the display would be scanning out.
         *
         * PCRTC_START holds the address the CRTC reads pixels from, so
         * whatever the title last set there is the frame it believes is on
         * screen. Nothing here scans out, so this is the one place that says
         * whether the guest is producing an image at all -- and where it is.
         * Gated, because it is a bring-up question, not a runtime one. */
        if (s_nv2a_trace) {
            /* Is the title submitting GPU work at all? PUT is where the
             * title's pushbuffer writer has got to; if it never moves, nothing
             * is being drawn and the missing piece is upstream of the GPU. */
            static DWORD  last_put_ms;
            static uint32_t last_put;
            DWORD now_ms = GetTickCount();
            uint32_t put = *(volatile uint32_t *)((char *)regs + NV2A_USER_DMA_PUT);
            if (put != last_put || (now_ms - last_put_ms) > 2000) {
                /* Survey the segment the title just submitted, once. */
                {
                    extern void nv2a_pb_scan(uint32_t, uint32_t);
                    extern void nv2a_pb_scan_report(void);
                    static DWORD last_report;

                    /* DMA_PUT holds a PHYSICAL address -- Xbox D3D writes
                     * `VA & 0x0FFFFFFF` and reads the GPU's position back as
                     * `GET | 0x80000000`. nv2a_pb_scan reads guest VAs, so
                     * handing it the raw register value pointed it at low
                     * memory: for the Xbox Dashboard, whose pushbuffer is at
                     * 0x80001000, PUT reads 0x1000 and the survey walked the
                     * fake TIB. It reported a plausible-looking inventory of
                     * nothing, which is worse than reporting none -- the
                     * conclusion drawn was "the title submits no methods"
                     * while it was submitting them the whole time.
                     *
                     * The contiguous window IS the physical-address view, so
                     * OR-ing its base is the documented round trip, not a
                     * guess. */
                    if (last_put && put > last_put)
                        nv2a_pb_scan(XBOX_CONTIG_BASE | (last_put & 0x0FFFFFFFu),
                                     XBOX_CONTIG_BASE | (put      & 0x0FFFFFFFu));
                    /* Periodic, because what the title submits at init is not
                     * what it submits once it is drawing a menu, and the
                     * question the survey answers is about the latter. */
                    if (now_ms - last_report > 10000) {
                        last_report = now_ms;
                        nv2a_pb_scan_report();
                    }
                }
                last_put = put; last_put_ms = now_ms;
                /* GET as well as PUT. A title that stops submitting has either
                 * finished or is spinning on the GPU catching up, and only GET
                 * tells those apart -- D3D waits for GET to reach PUT before it
                 * reuses the buffer, so GET stuck behind PUT is the shape of a
                 * pushbuffer-full hang. Also show the same pair as the Xbox
                 * Dashboard reads them: its D3D holds a register-block pointer
                 * in its device struct rather than assuming 0xFD800000, and
                 * mirroring the wrong block leaves it spinning on a GET that
                 * never moves. */
                {
                    uint32_t g = *(volatile uint32_t *)
                                 ((char *)regs + NV2A_USER_DMA_GET);
                    fprintf(stderr, "  [NV2A] DMA_PUT = 0x%08X  DMA_GET = "
                            "0x%08X%s\n", put, g,
                            g == put ? "" : "  (GPU behind)");
                }
                fflush(stderr);
            }
        }
        if (s_nv2a_trace) {
            static uint32_t last_start = 0xFFFFFFFFu;
            uint32_t start = *(volatile uint32_t *)((char *)regs + 0x600800);
            if (start != last_start) {
                last_start = start;
                fprintf(stderr, "  [NV2A] PCRTC_START = 0x%08X\n", start);
                fflush(stderr);
            }
        }

non_gpu:
        if (g_mcpx_regs && !g_apu_mmio_trapped) {
            for (size_t i = 0; i < sizeof(MCPX_COUNTERS) / sizeof(MCPX_COUNTERS[0]); i++) {
                volatile uint32_t *c =
                    (volatile uint32_t *)((char *)g_mcpx_regs + MCPX_COUNTERS[i]);
                *c += 1;
            }
        }

        /* Advance KeTickCount. It was written once at init and left frozen,
         * which silently breaks every timeout that polls it: Halo's DHCP setup
         * waits on a tick deadline that never arrives and spins forever bringing
         * up XNet. A live clock is also just the truth -- KeTickCount ticks on
         * hardware whether or not anyone is asleep. GetTickCount() shares the
         * millisecond unit, so the rate matches. */
        *(volatile uint32_t *)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
                               + g_memory_offset) = xbox_PreciseTickCount();

        /* With the emulated NV2A and APU only KeTickCount (1 ms unit) is
         * left, so wait half a millisecond instead of spinning: the spin
         * burnt a whole host core, possibly the SMT sibling of the guest's
         * (docs Update 637). RECOMP_ACK_SPIN=1 restores the spin. */
        {
            static int spin = -1;
            static HANDLE timer;
            if (spin < 0) {
                const char *e = getenv("RECOMP_ACK_SPIN");
                spin = e && e[0] == '1';
                if (!spin)
                    timer = CreateWaitableTimerExW(NULL, NULL,
                                                   CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                   TIMER_ALL_ACCESS);
            }
            if (!spin && timer && g_nv2a_xemu &&
                (!g_mcpx_regs || g_apu_mmio_trapped)) {
                LARGE_INTEGER due;
                due.QuadPart = -5000;   /* 0.5 ms, relative */
                if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                    WaitForSingleObject(timer, INFINITE);
                else
                    Sleep(0);
            } else {
                Sleep(0);  /* yield; the waiter is spinning on another core */
            }
        }
    }
    return 0;
}

static void xbox_Nv2aAckStart(void)
{
    g_nv2a_ack_stop = 0;
    g_nv2a_ack_thread = CreateThread(NULL, 0, nv2a_ack_thread,
                                     g_nv2a_memory, 0, NULL);
    if (g_nv2a_ack_thread) {
        xbox_PinDeviceThread(g_nv2a_ack_thread);  /* it is the GPU side */
        fprintf(stderr, "  NV2A busy-bit ack: %zu register(s) acknowledged\n",
                sizeof(NV2A_ACK) / sizeof(NV2A_ACK[0]));
    }
}

/* Separate allocation for Xbox kernel address space (0x80010000+).
 * Some RenderWare code reads the kernel PE header to detect features. */
static void *g_kernel_memory = NULL;

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Bounds of the title's executable sections, from its own XBE section table.
 *
 * RECOMP_ICALL uses these to decide whether an indirect-call target is code
 * before dispatching it. This used to be a hardcoded "0x00400000..0xFE000000 is
 * not code" test, which is true for Burnout 3 -- its .text ends at 0x002CC200,
 * so everything above 0x400000 really is data -- and false for any title with
 * more code than that. Half-Life 2's .text runs to 0x005F4A6C, so the constant
 * silently discarded every indirect call into the top two thirds of the game,
 * including the one that enters its main. No log, no crash: eax = 0 and carry
 * on, which looks exactly like a function that returned early.
 *
 * Zero until the layout is initialised, which the macro treats as "allow" so
 * nothing breaks before the title is loaded. */
uint32_t g_xbox_image_lo = 0;
uint32_t g_xbox_image_hi = 0;
uint32_t g_xbox_code_lo = 0;
uint32_t g_xbox_code_hi = 0;

/* Global registers for recompiled code (via recomp_types.h) */
/* Each guest thread's TIB. The first thread uses the one the loader built;
 * a spawned thread gets its own from xbox_AllocThreadTib(). */
RECOMP_TLS uint32_t g_fs_base = XBOX_TIB_MAIN;

/* The shape of the TLS block the loader built, so a new thread can be
 * given one just like it: where the initialised image data starts, how
 * big the block is, and how big the per-thread structure slot 0 points
 * at is. Zero total means the image had no TLS directory. */
static uint32_t g_tls_template_va, g_tls_total, g_tls_thread_size = 64;

RECOMP_TLS uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
RECOMP_TLS uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;

#ifdef RECOMP_ABI_CHECK
/* Report a lifted function that returned without restoring ebx/esi/edi.
 *
 * Those are callee-saved on x86, and the recompiler keeps them in globals, so
 * a function whose epilogue was never lifted corrupts its caller rather than
 * itself -- an error with no crash and no message, just less work silently
 * done. Ranked by hit count so the routine breaking a hot loop stands out from
 * the one-offs; -DRECOMP_ABI_CHECK only, since it costs three compares on
 * every indirect call.
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

void recomp_abi_violation_log(uint32_t va, uint32_t ebx0, uint32_t esi0,
                              uint32_t edi0, uint32_t esp0)
{
    enum { SLOTS = 32 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
        fprintf(stderr, "[ABI] sub_%08X:%s%s%s%s\n"
                        "      ebx %08X->%08X esi %08X->%08X"
                        " edi %08X->%08X esp %08X->%08X\n",
                va,
                g_ebx != ebx0 ? " ebx" : "",
                g_esi != esi0 ? " esi" : "",
                g_edi != edi0 ? " edi" : "",
                g_esp < esp0 + 4 ? " esp(epilogue never ran)" : "",
                ebx0, g_ebx, esi0, g_esi, edi0, g_edi, esp0, g_esp);
        /* esp coming back too HIGH means some callee popped arguments that
         * were never pushed -- a convention mismatch the one-sided invariant
         * above cannot see. The most recent indirect targets are the usual
         * suspects, so name them. */
        {
            int t;
            fprintf(stderr, "      esp delta %+d, recent icall targets:",
                    (int)(g_esp - esp0));
            for (t = 4; t >= 1; t--)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx - t) & 15]);
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    hits[i]++;
}

/* Report an indirect call whose callee's `ret N` popped more bytes than
 * this call site actually pushed. RECOMP_ABI_CALL's own check misses this:
 * it only flags esp coming back too LOW, and an over-pop with ebx/esi/edi
 * otherwise untouched passes it silently. See RECOMP_ICALL_SAFE_STACK_CHECK
 * in recomp_types.h for why saved_esp is a hard ceiling regardless of
 * calling convention, and why this is worth a dedicated check: the corrupted
 * guest stack slot(s) are read back later, in some outer caller's own
 * PUSH32/POP32 pair, with no register mismatch at the call site that
 * actually over-popped. -DRECOMP_ABI_CHECK only. */
void recomp_icall_stack_overpop_log(uint32_t va, uint32_t saved_esp, uint32_t esp_after)
{
    extern RECOMP_TLS volatile uint32_t g_icall_source;
    enum { SLOTS = 32 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
        fprintf(stderr, "[ICALL-OVERPOP] sub_%08X popped %d byte(s) more than "
                        "its caller pushed (saved_esp=0x%08X esp_after=0x%08X, "
                        "icall_source=0x%08X)\n",
                va, (int)(esp_after - saved_esp), saved_esp, esp_after,
                (unsigned)g_icall_source);
        fflush(stderr);
    }
    hits[i]++;
}
#endif

/* SEH frame pointer bridge (see recomp_types.h for explanation) */
RECOMP_TLS uint32_t g_seh_ebp = 0;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top = 0;
/* x87 control and status. The reset default masks every exception and
 * rounds to nearest, which is what the CRT expects before _control87. */
RECOMP_TLS uint16_t g_fp_control_word = 0x037Fu;
RECOMP_TLS int g_fp_cmp = 0;
RECOMP_TLS uint16_t g_fp_cc = 0;   /* see RECOMP_FP_CMP_RAW, recomp_types.h */

/* Defined below, with the other guest registers. */
extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi;

/* ---- non-local jumps ---------------------------------------------------
 *
 * The native half of the guest's setjmp/longjmp. See recomp_types.h for why a
 * guest-only longjmp is not enough; in short, the recompiled frames are C
 * frames and something has to unwind them.
 *
 * Keyed by guest buffer address, per thread. Buffers nest, so jumping to an
 * outer one discards every inner entry -- those frames are gone.
 */
#define RECOMP_JMPBUF_SLOTS 32

typedef struct {
    uint32_t buf_va;
    jmp_buf  native;
} recomp_jmp_slot;

static RECOMP_TLS recomp_jmp_slot s_jmp[RECOMP_JMPBUF_SLOTS];
static RECOMP_TLS int             s_jmp_used;

jmp_buf *recomp_setjmp_slot(uint32_t buf_va)
{
    int i;

    for (i = 0; i < s_jmp_used; i++)
        if (s_jmp[i].buf_va == buf_va)
            return &s_jmp[i].native;      /* the same buffer, re-armed */
    if (s_jmp_used >= RECOMP_JMPBUF_SLOTS)
        s_jmp_used = RECOMP_JMPBUF_SLOTS - 1;   /* keep the deepest */
    s_jmp[s_jmp_used].buf_va = buf_va;
    return &s_jmp[s_jmp_used++].native;
}

int recomp_guest_longjmp(uint32_t buf_va, uint32_t value)
{
    const uint8_t *mem = (const uint8_t *)g_memory_offset;
    int i;

    for (i = s_jmp_used - 1; i >= 0; i--) {
        if (s_jmp[i].buf_va != buf_va)
            continue;

        /* The callee-saved registers and the stack, exactly as the CRT's
         * longjmp restores them: esp is the setjmp-time esp plus the return
         * address that setjmp's own ret would have popped. */
        g_ebx = *(const uint32_t *)(mem + buf_va + 0x04);
        g_edi = *(const uint32_t *)(mem + buf_va + 0x08);
        g_esi = *(const uint32_t *)(mem + buf_va + 0x0C);
        g_esp = *(const uint32_t *)(mem + buf_va + 0x10) + 4;

        /* ebp is a C local in every translated function, and a local modified
         * after setjmp is indeterminate once longjmp lands. Hand the resumed
         * frame its saved value back through the globals it already reads. */
        g_seh_ebp = *(const uint32_t *)(mem + buf_va + 0x00);
        g_ebp     = g_seh_ebp;

        s_jmp_used = i + 1;   /* the inner buffers died with their frames */
        longjmp(s_jmp[i].native, value ? (int)value : 1);
    }
    return 0;
}

/* Watchdog: dump the guest call stack if the title stops making progress.
 *
 * A hang gives nothing to work from -- no crash, no last log line, no native
 * stack that means anything, because the guest frames live in guest memory and
 * the native one only shows whichever translated function is spinning. Sampling
 * the guest stack from a second thread is the one view that says where the
 * title actually is. Same GS format the crash handler uses, so tools/
 * stackwalk.py reads either.
 *
 * Off unless RECOMP_WATCHDOG_SECS is set, so it costs a getenv in normal runs.
 */
/* Defined below, after the watchdog. */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

static uint32_t *s_watchdog_esp;
/* The other guest registers are thread-local too, so the watchdog has to be
 * handed the guest thread's copies rather than reading its own -- which are
 * always zero, and read as "every register is null" at exactly the moment the
 * registers are the thing being asked about. */
static uint32_t *s_watchdog_regs[6];
static unsigned  s_watchdog_secs;

static DWORD WINAPI xbox_watchdog_thread(LPVOID unused)
{
    const uint8_t *mem;
    uint32_t esp, i;

    (void)unused;
    Sleep(s_watchdog_secs * 1000u);

    mem = (const uint8_t *)g_memory_offset;
    esp = s_watchdog_esp ? *s_watchdog_esp : 0;
    fprintf(stderr, "[WATCHDOG] no exit after %us; guest esp=0x%08X\n"
            "  regs: eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            s_watchdog_secs, esp,
            s_watchdog_regs[0] ? *s_watchdog_regs[0] : 0,
            s_watchdog_regs[1] ? *s_watchdog_regs[1] : 0,
            s_watchdog_regs[2] ? *s_watchdog_regs[2] : 0,
            s_watchdog_regs[3] ? *s_watchdog_regs[3] : 0,
            s_watchdog_regs[4] ? *s_watchdog_regs[4] : 0,
            s_watchdog_regs[5] ? *s_watchdog_regs[5] : 0);
    /* The recent indirect-call targets name whatever is spinning: a stuck loop
     * inside a function reached through a pointer leaves no clue on the stack
     * beyond the return address of the call that entered it. */
    {
        uint32_t k;
        /* The running indirect-call total separates a hang from mere
         * slowness. Kernel calls cannot: a pure CPU loop makes none, so
         * "same count at 20s and 60s" proves nothing about it. */
        fprintf(stderr, "  icalls so far: %llu\n",
                (unsigned long long)g_icall_count);
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X",
                    g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    /* Guest globals worth seeing at the moment of the hang.
     *
     * RECOMP_PEEK is otherwise only sampled by the pushbuffer reporter, which
     * a title that hangs before rendering never reaches -- and a spin that
     * makes no kernel calls is invisible to RECOMP_KERNEL_WATCH too. A pure
     * CPU loop polling a global is exactly the case neither of those covers.
     */
    {
        const char *spec = getenv("RECOMP_PEEK");
        char buf[256], *q, *end;
        if (spec && *spec) {
            strncpy(buf, spec, sizeof buf - 1);
            buf[sizeof buf - 1] = 0;
            fprintf(stderr, "  peek:");
            for (q = buf; *q; ) {
                unsigned long va = strtoul(q, &end, 0);
                if (end == q)
                    break;
                if (va >= XBOX_BASE_ADDRESS && va < XBOX_TOTAL_RAM)
                    fprintf(stderr, " [%08lX]=%08X", va,
                            *(const uint32_t *)(mem + va));
                q = (*end == ',') ? end + 1 : end;
            }
            fprintf(stderr, "\n");
        }
    }

    for (i = 0; i < 400 && esp; i++) {
        uint32_t a = esp + i * 4;
        if (a < XBOX_STACK_BASE || a >= XBOX_STACK_TOP) break;
        fprintf(stderr, "    GS %08X %08X\n", a,
                *(const uint32_t *)(mem + a));
    }
    fflush(stderr);
    _exit(3);
    return 0;
}

void xbox_WatchdogStart(void)
{
    const char *secs = getenv("RECOMP_WATCHDOG_SECS");
    HANDLE h;

    if (!secs || !*secs)
        return;
    s_watchdog_secs = (unsigned)atoi(secs);
    if (!s_watchdog_secs)
        return;

    /* Taken on the guest thread: g_esp is thread-local, so the watchdog has to
     * be handed the address of the one that matters rather than reading its
     * own, which is always zero. */
    s_watchdog_esp = &g_esp;
    s_watchdog_regs[0] = &g_eax; s_watchdog_regs[1] = &g_ecx;
    s_watchdog_regs[2] = &g_edx; s_watchdog_regs[3] = &g_ebx;
    s_watchdog_regs[4] = &g_esi; s_watchdog_regs[5] = &g_edi;
    h = CreateThread(NULL, 0, xbox_watchdog_thread, NULL, 0, NULL);
    if (h) {
        xbox_PinDeviceThread(h);  /* host tooling, not guest code */
        CloseHandle(h);
    }
}

/* SSE. 128 bits of architectural state, per-thread like the rest. */
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
/* Last frame established by `mov ebp, esp`. Read by frameless functions
 * that address their caller's frame through ebp. */
RECOMP_TLS uint32_t g_ebp = 0;

/* The offsets SCDA uses through FS are TIB/KPCR offsets.  This storage is
 * intentionally outside the flat Xbox mapping so a normal write to VA 0x28
 * cannot alter fs:[0x28]. SCDA-specific, not part of upstream xboxrecomp;
 * kept RECOMP_TLS to match the per-thread register state around it. */
static RECOMP_TLS uint8_t g_xbox_fs[0x10000] = {0};

uint8_t xbox_fs_read8(uint32_t offset)
{
    return offset < sizeof(g_xbox_fs) ? g_xbox_fs[offset] : 0;
}

uint16_t xbox_fs_read16(uint32_t offset)
{
    uint16_t value = 0;
    if (offset <= sizeof(g_xbox_fs) - sizeof(value)) memcpy(&value, g_xbox_fs + offset, sizeof(value));
    return value;
}

uint32_t xbox_fs_read32(uint32_t offset)
{
    uint32_t value = 0;
    if (offset <= sizeof(g_xbox_fs) - sizeof(value)) memcpy(&value, g_xbox_fs + offset, sizeof(value));
    return value;
}

void xbox_fs_write8(uint32_t offset, uint8_t value)
{
    if (offset < sizeof(g_xbox_fs)) g_xbox_fs[offset] = value;
}

void xbox_fs_write16(uint32_t offset, uint16_t value)
{
    if (offset <= sizeof(g_xbox_fs) - sizeof(value)) memcpy(g_xbox_fs + offset, &value, sizeof(value));
}

void xbox_fs_write32(uint32_t offset, uint32_t value)
{
    if (offset <= sizeof(g_xbox_fs) - sizeof(value)) memcpy(g_xbox_fs + offset, &value, sizeof(value));
}

/* EFLAGS.DF. Zero means the string instructions walk forwards, which is the
 * ABI's resting state and what almost every one of them does -- so this is
 * almost always 0 and costs a predictable branch. The exceptions are the ones
 * that matter: MSVC's strrchr/wcsrchr scan backwards from the terminator with
 * `std; repne scasb`, and memmove goes backwards when its regions overlap the
 * wrong way. Thread-local, because `std` and the `cld` that undoes it can land
 * in different lifted bodies of the same guest routine. */
RECOMP_TLS int g_df = 0;

/* Synthetic EFLAGS for pushfd/popfd, and a real cpuid. See the comment on
 * the declarations in recomp_types.h for why these exist and what breaks
 * without them. ID (bit 21) starts set so a toggle-and-read-back CPUID
 * probe sees it available on the very first call, same as any real CPU
 * built after 1998. */
RECOMP_TLS uint32_t g_eflags = 0x00200202u;

#if !(defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
/* The guest MXCSR on hosts without SSE (ARM port, docs Update 677): its
 * rounding field drives recomp_cvt_rc in recomp_types.h. */
RECOMP_TLS uint32_t g_recomp_mxcsr = 0x1F80u;
#endif

void recomp_cpuid(uint32_t leaf, uint32_t subleaf,
                   uint32_t *out_eax, uint32_t *out_ebx,
                   uint32_t *out_ecx, uint32_t *out_edx)
{
    int regs[4] = {0, 0, 0, 0};
#if defined(_MSC_VER)
    __cpuidex(regs, (int)leaf, (int)subleaf);
#elif defined(__x86_64__) || defined(__i386__)
    __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#else
    /* No host CPUID (ARM port, docs Update 677): answer as the Xbox's own
     * Pentium III-class CPU (GenuineIntel, family 6 model 8 stepping 0xA,
     * FPU/TSC/CX8/CMOV/MMX/FXSR/SSE). */
    (void)subleaf;
    if (leaf == 0) {
        regs[0] = 2;
        regs[1] = 0x756E6547;   /* "Genu" */
        regs[3] = 0x49656E69;   /* "ineI" */
        regs[2] = 0x6C65746E;   /* "ntel" */
    } else if (leaf == 1) {
        regs[0] = 0x0000068A;
        regs[3] = 0x0383F9FF;
    } else if (leaf == 2) {
        regs[0] = 0x03020101;
        regs[3] = 0x0C040882;
    }
#endif
    *out_eax = (uint32_t)regs[0];
    *out_ebx = (uint32_t)regs[1];
    *out_ecx = (uint32_t)regs[2];
    *out_edx = (uint32_t)regs[3];
}

/* Guest rdtsc: see the declaration in recomp_types.h. The host TSC runs at
 * whatever rate the host CPU has (several GHz), so passing it through made
 * a title's 733 MHz-based clock run several times too fast; before that,
 * rdtsc was not lifted at all and returned stale registers. */
#define XBOX_TSC_HZ 733333333ull

static INIT_ONCE g_rdtsc_once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER g_rdtsc_qpc_freq;
static LARGE_INTEGER g_rdtsc_qpc_base;

static BOOL CALLBACK rdtsc_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    QueryPerformanceFrequency(&g_rdtsc_qpc_freq);
    QueryPerformanceCounter(&g_rdtsc_qpc_base);
    return TRUE;
}

uint64_t recomp_rdtsc(void)
{
    LARGE_INTEGER now;
    uint64_t delta, freq;

    InitOnceExecuteOnce(&g_rdtsc_once, rdtsc_init, NULL, NULL);
    QueryPerformanceCounter(&now);
    delta = xbox_GuestQpcDelta(now.QuadPart, g_rdtsc_qpc_base.QuadPart);
    freq = (uint64_t)g_rdtsc_qpc_freq.QuadPart;
    /* delta * XBOX_TSC_HZ / freq without overflowing 64 bits */
    return (delta / freq) * XBOX_TSC_HZ + ((delta % freq) * XBOX_TSC_HZ) / freq;
}

/* Unresolved-flags conditional reached at runtime (recomp_types.h). One
 * line per site, first execution only; a lock-free open-addressed set. */
int recomp_flag_miss(uint32_t site, int flags)
{
    enum { SLOTS = 4096 };
    static volatile LONG seen[SLOTS];
    uint32_t h = (site * 2654435761u) >> 20;   /* 12 bits */
    for (unsigned probe = 0; probe < SLOTS; probe++) {
        volatile LONG *slot = &seen[(h + probe) & (SLOTS - 1)];
        LONG cur = *slot;
        if (cur == (LONG)site)
            return flags;
        if (cur == 0) {
            if (InterlockedCompareExchange(slot, (LONG)site, 0) == 0) {
                fprintf(stderr, "[RECOMP-FLAGMISS] site 0x%08X first reached "
                        "(unresolved flags -> always false)\n", site);
                fflush(stderr);
                return flags;
            }
            if (*slot == (LONG)site)
                return flags;
        }
    }
    return flags;
}

/* ICALL trace ring buffer */
volatile uint32_t g_icall_trace[16] = {0};
volatile uint32_t g_icall_trace_idx = 0;
volatile uint64_t g_icall_count = 0;
RECOMP_TLS volatile uint32_t g_icall_source = 0;

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    if (g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: already initialized\n");
        return FALSE;
    }
    /* Pick the guest CPU before any device thread starts (the APU threads
     * read RECOMP_GUEST_CPU, which this sets when it chooses one). */
    fprintf(stderr, "  [CPU] guest threads on logical processor %d\n", xbox_GuestCpu());

    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Map the full Xbox address space (covers all sections + stack + heap).
     * Size is runtime-configurable: retail 64 MB, devkit debug builds 128 MB. */
    /* The mapped range, which is not necessarily RAM. Mirrors are placed
     * at multiples of this, so growing it is what stops a title's
     * above-RAM allocations from aliasing low memory. */
    g_memory_size = g_xbox_map_size ? g_xbox_map_size : g_xbox_total_ram;

    /*
     * Physical offset where the contig-allocation alias region starts, and
     * how many bytes the file mapping needs beyond plain RAM to hold it.
     *
     * Real Xbox physical page P is the same byte through base RAM (0+P),
     * the contiguous window (0x80000000+P), and the tiled aperture
     * (0xF0000000+P) -- see xbox_ContiguousAlloc. Base RAM's own P range,
     * [0, g_memory_size), is already fully claimed by the loaded image, the
     * simulated stack, and the heap (XBOX_HEAP_TOP reaches g_memory_size
     * exactly), so a dynamic contig allocation can never safely reuse a P in
     * that range -- reusing one is exactly Update 228's 0x004AB668 bug, a
     * contig surface's tiled alias landing on a base-RAM global that happens
     * to sit at the same physical offset. The first P past that claimed
     * range is g_memory_size itself; if XBOX_CONTIG_SIZE (the legacy
     * window's own size) is larger, round up to that instead so the alias
     * region also never overlaps the legacy window's own VirtualAlloc.
     */
    {
        size_t contig_alias_phys_off = g_memory_size > (size_t)XBOX_CONTIG_SIZE
                                      ? g_memory_size : (size_t)XBOX_CONTIG_SIZE;
        g_contig_alias_base = (uint32_t)(XBOX_CONTIG_BASE + contig_alias_phys_off);
        g_contig_alias_need = contig_alias_phys_off + (size_t)XBOX_CONTIG_SIZE;
    }

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     *
     * Sized past g_memory_size by g_contig_alias_need's own extra span: the
     * base view and every mirror below still only ever map the first
     * g_memory_size bytes of this section (file offset 0), so none of them
     * can see or shift into the extra tail -- it exists solely to back
     * g_contig_alias_view, mapped separately, below.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_contig_alias_need,   /* low DWORD of size (RAM + contig-alias tail) */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        fprintf(stderr, "xbox_MemoryLayoutInit: CreateFileMapping failed (error %lu)\n",
                GetLastError());
        return FALSE;
    }

    /*
     * Map the base view at the desired virtual address.
     * Try the original Xbox base address first. If that fails (common on
     * Windows 11 where low addresses are often reserved), try page-aligned
     * addresses upward until we find a free region.
     */
    {
        /* A high 64-bit base is tried FIRST, ahead of the low, Xbox-shaped
         * addresses below. The RAM mirror views (see XBOX_NUM_MIRRORS) need
         * to alias a full 4 GB of address space above this base to cover
         * every possible 32-bit guest address, and Windows keeps several
         * small, fixed structures of its own in every process's low 4 GB
         * (KUSER_SHARED_DATA at a hardcoded 0x7FFE0000 being the one that
         * actually bit us: sub_0007E03B/Update 164 computed a wild guest
         * table index that happened to land right next to it, and no
         * mirror can ever fully cover a hole that small and immovable no
         * matter how the surrounding views are laid out). A base up here
         * has 4+ GB of clear space above it with nothing else competing
         * for it, so the whole mirrored range never comes near any of
         * those low, fixed reservations in the first place. The low
         * addresses stay as a fallback for an environment where this
         * fails for some other reason; nothing in this codebase depends
         * on g_memory_base/g_xbox_mem_offset being small (XBOX_PTR always
         * adds the offset explicitly). */
        static const uintptr_t try_bases[] = {
            0x0000700000000000ull, /* high, clear of every low Xbox/Windows
                                     * reservation for the full 4 GB mirror range */
            0x0000001000000000ull, /* 64 GB: the same, for a 39-bit address
                                     * space (Wine/Box64 on ARM, docs Update 675),
                                     * where Box64 also holds 0x7F000000-0x82000000 */
            XBOX_BASE_ADDRESS,      /* 0x00010000 - original Xbox address */
            0x00800000,             /* 8 MB - above typical PEB/TEB region */
            0x01000000,             /* 16 MB */
            0x02000000,             /* 32 MB */
            0x10000000,             /* 256 MB */
            0,                      /* sentinel - let OS choose */
        };

        for (int i = 0; try_bases[i] != 0 || i == 0; i++) {
            LPVOID hint = try_bases[i] ? (LPVOID)try_bases[i] : NULL;
            g_memory_base = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,           /* offset into mapping */
                g_memory_size,  /* size */
                hint            /* desired base address */
            );
            if (g_memory_base) {
                if (try_bases[i] != 0 && (uintptr_t)g_memory_base != try_bases[i]) {
                    /* OS gave us a different address, retry */
                    UnmapViewOfFile(g_memory_base);
                    g_memory_base = NULL;
                    continue;
                }
                break;
            }
        }
    }

    if (!g_memory_base) {
        fprintf(stderr, "xbox_MemoryLayoutInit: failed to map base view (%zu KB)\n",
                g_memory_size / 1024);
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        return FALSE;
    }

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;

    /* Guest page zero: no access.
     *
     * Nothing legitimate lives there -- every XBE's image base is 0x00010000
     * and the TIB now sits at XBOX_FS_BASE -- so any access is a null pointer
     * the title dereferenced. Left readable it did quiet damage: a null check
     * of the form `cmp byte [ecx], 0` read whatever happened to be at 0 and
     * decided the pointer was fine, and a store through a null pointer landed
     * on real memory and surfaced as corruption somewhere unrelated. Faulting
     * here turns both into one access violation at the instruction that made
     * the mistake, which the crash handler can name.
     *
     * Opt-in through RECOMP_TRAP_NULL, because it converts a class of bug the
     * title currently survives into a hard stop: a guest that dereferences null
     * and ignores the result keeps running while page zero reads as zero, and
     * stops dead once it faults. That is the right default for hunting one of
     * these and the wrong one for making progress past the rest, so it is a
     * switch rather than a policy.
     *
     * Note this is separate from moving the TIB off page zero, which is not
     * optional: with the TIB gone, address 0 reads as plain zero, so a null
     * check written as a load through the pointer now gets the answer it
     * expects whether or not the page is trapped.
     *
     * Best-effort: failing to protect it costs only the diagnostic. */
    if (XBOX_MAP_START == 0 && getenv("RECOMP_TRAP_NULL")) {
        DWORD old_protect;
        if (VirtualProtect(g_memory_base, 0x1000, PAGE_NOACCESS, &old_protect))
            fprintf(stderr, "  guest page 0 is PAGE_NOACCESS"
                            " (null dereferences fault)\n");
    }

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /*
     * Copy XBE header to base address.
     * The Xbox kernel maps the XBE image header at 0x00010000.
     * Game code reads kernel thunk table, certificate data, and
     * section info from this region.
     */
    {
        /* XBE header size is at file offset 0x0108 (SizeOfImageHeader) */
        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;  /* fallback: 4KB */
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    /*
     * Dynamically load ALL XBE sections by parsing the section headers.
     *
     * This replaces the old approach of hardcoding section addresses for
     * a specific game (Burnout 3). By reading the section table from the
     * XBE header, any game's sections are loaded automatically.
     *
     * Every section is copied to its original Xbox VA:
     * - .text: needed because memory walkers may scan code pages
     * - .rdata: constants, vtables, kernel thunk table
     * - .data: global variables (initialized portion from XBE, BSS zeroed)
     * - XDK library sections (D3D, DSOUND, WMADEC, XPP, etc.)
     * - DOLBY, BINK, XTIMAGE, etc.
     */
    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;
        size_t total_bytes = 0;

        if (num_sections > 64) num_sections = 64;  /* sanity cap */

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            /* Read section name from XBE header */
            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            /* Validate: section must fit within our 64MB mapped region */
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            /* Determine copy size (raw_size may exceed vsize due to alignment) */
            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            /* Zero the full virtual size first (handles BSS) */
            memset(XBOX_VA(sec_va), 0, sec_vsize);

            /* Copy initialized data from XBE */
            if (copy_size > 0 && sec_raw_off + copy_size <= xbe_size) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            }

            /* Every loaded section, executable or not. Anything that writes
             * guest memory from outside the title -- the pushbuffer executor
             * clearing a surface, say -- needs to know where the title itself
             * lives, because scribbling on it is not a rendering artefact, it
             * is the title's code and globals gone. */
            if (!g_xbox_image_lo || sec_va < g_xbox_image_lo)
                g_xbox_image_lo = sec_va;
            if (sec_va + sec_vsize > g_xbox_image_hi)
                g_xbox_image_hi = sec_va + sec_vsize;

            /* Executable sections define the range indirect calls may target.
             * XBE section flag 0x04 is EXECUTABLE. */
            if (*(const DWORD *)(sh + SECTHDR_FLAGS) & 0x00000004u) {
                if (!g_xbox_code_lo || sec_va < g_xbox_code_lo)
                    g_xbox_code_lo = sec_va;
                if (sec_va + sec_vsize > g_xbox_code_hi)
                    g_xbox_code_hi = sec_va + sec_vsize;
            }

            sections_loaded++;
            total_bytes += copy_size;

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);

        /* 2026-09-03 (continued 23): one-shot verification for the
         * sub_00055666 reboot-gate bug hunt -- real hardware (xemu) has
         * 0x5B1D90=0 and 0x5B1D94=0x0047D997 (a valid static function
         * pointer baked into the retail .data section), confirmed against
         * the raw XBE bytes too. If our loader produces 0 here instead,
         * that's the root cause of the unwanted HalReturnToFirmware(2)
         * self-relaunch. See docs/SCDA_WORKLOG.md. */
        fprintf(stderr, "  [SCDA-REBOOTGATE-CHECK] 0x005B1D90=0x%08X 0x005B1D94=0x%08X "
                "(expected 0x00000000 / 0x0047D997)\n",
                *(const DWORD *)XBOX_VA(0x005B1D90u),
                *(const DWORD *)XBOX_VA(0x005B1D94u));
        fflush(stderr);
    }

    /*
     * Parse the kernel thunk table address from the XBE header.
     * The XBE stores KernelImageThunkAddress at offset 0x0158, XOR-encrypted.
     * The key differs between retail and debug XBEs, and there is no flag
     * saying which was used -- decode with both and keep whichever lands in
     * the mapped address range (this is what tools/xbe_parser does).
     *
     * Debug XBEs are not an edge case here: they are the builds most worth
     * recompiling, since they still carry assert strings and symbols. Halo's
     * cachebeta.xbe is one, and assuming the retail key decoded its thunk
     * table to 0xB4F98174 instead of 0x00253090, which silently fell back to
     * the compile-time default and resolved 0 of 378 kernel imports.
     */
    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;  /* retail XOR key */
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;  /* debug XOR key  */
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        /* Validate: thunk VA should be within our mapped region */
        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {
            /* Count thunk entries by scanning until we hit 0 */
            uint32_t thunk_count = 0;
            /* XBOX_KERNEL_THUNK_TABLE_SIZE, not 366: the kernel exports 378
             * slots, and kernel.h notes 366 is short by 12. A title importing
             * a high ordinal would have had its table truncated here. */
            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA

    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    /*
     * Initialize the Xbox stack for recompiled code.
     * The stack area lives at XBOX_STACK_BASE in Xbox address space.
     * g_esp is the global stack pointer shared by all translated functions.
     */
    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    /*
     * Populate the fake Thread Information Block (TIB) at Xbox VA 0x0.
     *
     * The original Xbox code uses fs:[offset] to read per-thread data,
     * but the recompiler drops the fs: segment prefix and generates
     * MEM32(offset) instead. Since we mapped low memory (0x0-0xFFFF),
     * we populate the TIB fields that game code accesses:
     *
     *   fs:[0x00] = SEH exception list (-1 = end of chain)
     *   fs:[0x04] = Xbox TLS slot vector pointer
     *   fs:[0x18] = self pointer (TIB address)
     *   fs:[0x20] = KPCR Prcb pointer (→ fake structure)
     *   fs:[0x28] = TLS / RW engine context pointer
     *
     * We use free space in the BSS area for the fake structures.
     */
    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        /* Fake TIB, stored through xbox_fs_write32 (a physically separate,
         * thread-local store -- see the note on g_xbox_fs above) rather than
         * upstream's flat-guest-memory-at-XBOX_FS_BASE approach: this keeps
         * per-thread TIB/stack-bounds/TLS state correct, which a single
         * shared guest-memory TIB cannot provide for a multi-threaded
         * title. */
        xbox_fs_write32(0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        xbox_fs_write32(0x04, XBOX_STACK_TOP);   /* Stack base (high address); overwritten below if the XBE has a TLS directory */
        xbox_fs_write32(0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        xbox_fs_write32(0x18, XBOX_FS_BASE);     /* Self pointer -- a stable identifier, not a real guest VA under this storage model */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure.
         *
         * A zeroed block in real (already-mapped, flat) guest memory rather
         * than a null pointer: pointing at real zeroed memory says the same
         * thing to the title ([fs:[0x20]+0x250] reads 0, skipping the cache
         * init) while surviving a title that dereferences fs:[0x20] itself,
         * not just the +0x250 field. */
        #define FAKE_PRCB_VA 0x00761000  /* zeroed KPCR Prcb stand-in */
        memset(XBOX_VA(FAKE_PRCB_VA), 0, 0x400);
        xbox_fs_write32(0x20, FAKE_PRCB_VA);
        #undef FAKE_PRCB_VA

        /*
         * fs:[0x28] - Thread local storage / RW engine context.
         * The RW engine reads [fs:[0x28] + 0x28] to get a pointer
         * to its data area. We allocate a fake structure at 0x00760000
         * (in the BSS area) and a data buffer at 0x00700000.
         */
        #define FAKE_TLS_VA          0x00760000  /* fs:[0x28] structure */
        #define FAKE_RWDATA_VA       0x00700000  /* RW engine data area */

        xbox_fs_write32(0x28, FAKE_TLS_VA);
        /* TLS[0x28] = pointer to RW data area */
        MEM32_INIT(FAKE_TLS_VA + 0x28, FAKE_RWDATA_VA);

        /*
         * XBE TLS directory.
         *
         * An image with __declspec(thread) data carries one, and on hardware
         * the loader acts on it. Nothing here did, so thread-local access read
         * whatever memory happened to be under fs:[4].
         *
         * Xbox reaches thread-local data through NtTib.StackBase -- fs:[4] --
         * not Win32's fs:[0x2C], and the block sits BELOW that pointer: the
         * image's entry point computes its own index, negative, as
         * -(blocksize/4). Wreckless does this at guest 0x000EB57E and arrives
         * at -5 for its 20-byte block, so [fs:[4] + index*4] is the block's
         * first dword. The rounding below mirrors that arithmetic exactly,
         * because fs:[4] has to land where the title's own index says it is.
         *
         * The index itself is deliberately NOT written here: the title
         * computes and stores it. What the loader owes it is a block in the
         * right place. (SCDA's own TLS directory -- init_size 0, zero_fill
         * 144 -- rounds to a 148-byte total here, i.e. index -37: exactly
         * the value that used to be hand-derived and hardcoded as
         * SCDA_TLS_INDEX before this generic computation replaced it.)
         *
         * Slot 0 holds a pointer to per-thread data -- XAPI's SetLastError is
         * [[fs:[4] + index*4] + 4] = err -- so it gets a zeroed block rather
         * than being left NULL, which had SetLastError writing the error code
         * over fs:[4] itself and the next call faulting at guest 0xFFFFFFEF.
         *
         * ponytail: one block for the whole process, not one per thread.
         * Every guest thread therefore shares LastError. Give this a per-thread
         * allocation when a title is observed to care.
         */
        #define FAKE_TLS_BLOCK_VA  0x00770000  /* image TLS data          */
        #define FAKE_TLS_THREAD_VA 0x00770200  /* what slot 0 points at   */
        {
            DWORD tls_dir_va = *(const DWORD *)(xbe + XBE_TLS_ADDR_OFFSET);

            if (tls_dir_va) {
                const uint32_t *tls = (const uint32_t *)XBOX_VA(tls_dir_va);
                uint32_t data_start = tls[0];
                uint32_t data_end   = tls[1];
                uint32_t zero_fill  = tls[4];
                uint32_t init_size  = (data_end > data_start)
                                    ? data_end - data_start : 0;
                uint32_t total      = ((init_size + zero_fill + 0xF) & ~0xFu) + 4;

                memset(XBOX_VA(FAKE_TLS_BLOCK_VA), 0, total);
                memset(XBOX_VA(FAKE_TLS_THREAD_VA), 0, 64);
                if (init_size)
                    memcpy(XBOX_VA(FAKE_TLS_BLOCK_VA),
                           XBOX_VA(data_start), init_size);

                MEM32_INIT(FAKE_TLS_BLOCK_VA, FAKE_TLS_THREAD_VA);
                xbox_fs_write32(0x04, FAKE_TLS_BLOCK_VA + total);

                g_tls_template_va = FAKE_TLS_BLOCK_VA;
                g_tls_total       = total;

                fprintf(stderr, "  TLS: %u-byte block at 0x%08X,"
                        " fs:[4] = 0x%08X (index will be %d)\n",
                        total, FAKE_TLS_BLOCK_VA, FAKE_TLS_BLOCK_VA + total,
                        -(int)(total / 4));
            }
        }
        #undef FAKE_TLS_BLOCK_VA
        #undef FAKE_TLS_THREAD_VA

        fprintf(stderr, "  TIB: fake TIB at fs-store offset 0x0, TLS at 0x%08X, RW data at 0x%08X\n",
                FAKE_TLS_VA, FAKE_RWDATA_VA);

        #undef FAKE_TLS_VA
        #undef FAKE_RWDATA_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * Contiguous / physical memory window at 0x80000000.
     *
     * MmAllocateContiguousMemory hands back addresses in this window: physical
     * page P is visible at 0x80000000 + P. Titles that pin buffers at fixed
     * physical addresses then use the whole range, so it has to be backed for
     * its full length - Halo pins 3.4 MB at 0x61000 and 22 MB at 0x3A6000, and
     * with only the fake kernel page mapped here a write walked off the end of
     * it a few pages in.
     *
     * Deliberately NOT a view of the 64 MB RAM mapping. On hardware this window
     * aliases physical RAM, but we load the XBE image into the low addresses of
     * that same region, so aliasing would put a title's pinned pools on top of
     * its own code. Separate storage costs an extra mapping and behaves
     * correctly; nothing here depends on the aliasing.
     *
     * That is still true for whatever lands here directly (Halo's pins above,
     * the fake kernel page below) -- their own physical offsets are low
     * enough to coincide with base RAM's own layout on real hardware too, and
     * nothing here reads them back through the tiled aperture, so the missing
     * alias is invisible to them. It stopped being true for dynamically
     * bump-allocated contiguous memory (xbox_ContiguousAlloc): a title's own,
     * already-faithfully-translated D3D/driver code reaches such a surface
     * through its documented tiled-aperture alias, and that alias has to
     * land on the surface's own bytes, not on whatever base-RAM global
     * happens to share its physical offset (Update 228, docs/SCDA_WORKLOG.md
     * -- a device-pointer global at guest VA 0x004AB668 sat exactly there).
     * g_contig_alias_view below is the real alias for that case; this window
     * keeps doing exactly what it always did for everything else.
     *
     * Reserved before the kernel page below, which lives inside it.
     */
    {
        uintptr_t contig_native = XBOX_CONTIG_BASE + g_memory_offset;
        /* With the emulated NV2A (docs Update 355) the window's first 64 KiB
         * is instead a view of physical RAM 0..64K, as it is on hardware
         * (VA 0x80000000 + P is physical P). Xbox D3D writes the pushbuffer's
         * entry jump through the hard-coded VA 0x80000000 and then starts
         * the GPU at DMA_GET = 0; with separate storage there the GPU read
         * zeros up to the XBE header at physical 0x10000 and faulted
         * (run355d: VA 0x80000000 = 044AE001, physical 0 = 0). Only the
         * unused guest page zero sits underneath; 64 KiB is the mapping
         * granularity. The rest of the window is untouched. */
        void *low_view = NULL;
        if (nv2a_xemu_requested()) {
            low_view = MapViewOfFileEx(g_mapping_handle, FILE_MAP_ALL_ACCESS,
                                       0, 0, 0x10000, (LPVOID)contig_native);
            if (!low_view)
                fprintf(stderr, "  WARNING: physical-0 view at 0x%08X failed "
                        "(error %lu); the GPU will not see D3D's entry jump\n",
                        XBOX_CONTIG_BASE, GetLastError());
        }
        g_contig_memory = VirtualAlloc(
            (LPVOID)(contig_native + (low_view ? 0x10000 : 0)),
            XBOX_CONTIG_SIZE - (low_view ? 0x10000 : 0),
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_contig_memory && low_view) {
            g_contig_low_view = low_view;
            g_contig_memory = (void *)contig_native;
            fprintf(stderr, "  Contiguous window: first 64 KB aliases "
                    "physical 0 (NV2A pushbuffer entry)\n");
        }
        if (g_contig_memory) {
            fprintf(stderr, "  Contiguous window: %u MB at Xbox VA 0x%08X\n",
                    XBOX_CONTIG_SIZE / (1024 * 1024), XBOX_CONTIG_BASE);
        } else {
            fprintf(stderr, "  WARNING: contiguous window at 0x%08X failed "
                    "(error %lu); pinned physical allocations will fault\n",
                    XBOX_CONTIG_BASE, GetLastError());
        }
    }

    /*
     * Contiguous-allocation alias region -- a second view of the SAME file
     * mapping section as base RAM, at physical offsets base RAM itself never
     * claims (see g_contig_alias_need, computed above before the file
     * mapping was even created). xbox_ContiguousAlloc's bump pointer is
     * redirected here on success, so every *dynamic* contig allocation is
     * genuinely the same bytes through the tiled aperture and this window,
     * exactly like real Xbox physical memory. The legacy window above is
     * untouched and keeps serving anything that addresses it directly
     * (Halo's pins, the fake kernel page) -- see the comment on that block
     * for why leaving those alone is correct rather than a shortcut.
     *
     * Mapped here, before the RAM mirrors just below: this region's guest
     * VA (XBOX_CONTIG_BASE + g_memory_size, e.g. 0x84000000) is also exactly
     * where a generic 64 MB address-bus-wrap mirror would otherwise go --
     * the mirror grid tiles the whole 4 GB range above g_memory_base with no
     * idea that this particular slice means something else. Committing it
     * first makes the mirror loop's own VirtualQuery-based occupied-range
     * check see real, already-mapped memory here and fail over/skip that
     * one mirror, the same way it already does for the legacy contig
     * window, the kernel page, and the NV2A/MCPX/flash apertures -- instead
     * of the mirror mapping successfully first and this view then failing
     * with ERROR_INVALID_ADDRESS against an address that looks free but
     * is not.
     *
     * The self-test that proves this actually aliases the tiled aperture
     * runs later, once that aperture exists -- see below, after it is
     * mapped.
     */
    {
        uintptr_t contig_alias_native = (uintptr_t)g_contig_alias_base + g_memory_offset;
        size_t contig_alias_phys_off = g_contig_alias_base - XBOX_CONTIG_BASE;

        g_contig_alias_view = MapViewOfFileEx(
            g_mapping_handle,
            FILE_MAP_ALL_ACCESS,
            0, (DWORD)contig_alias_phys_off,
            XBOX_CONTIG_SIZE,
            (LPVOID)contig_alias_native
        );
        if (g_contig_alias_view) {
            g_contig_next = g_contig_alias_base;
            g_contig_top  = g_contig_alias_base + XBOX_CONTIG_SIZE;
        } else {
            fprintf(stderr, "  WARNING: contig alias view at 0x%08X failed"
                    " (error %lu); dynamic contiguous allocations will use"
                    " the legacy window and remain subject to the tiled-"
                    "aperture aliasing gap (Update 228)\n",
                    g_contig_alias_base, GetLastError());
        }
    }

    /*
     * NV2A hardware register aperture at 0xFD000000 (16 MB).
     *
     * The GPU's registers are memory-mapped here on real hardware. A title
     * that only calls D3D never notices, but the D3D8 library is linked into
     * the XBE rather than provided by the kernel, so once execution is inside
     * it the register pokes are just loads and stores in recompiled code.
     * Halo faults reading 0xFD001804 during rasterizer_preinitialize, a few
     * instructions after Direct3DCreate8 returns.
     *
     * Backed as ordinary zeroed RAM. That is enough to get through
     * initialisation, and reads returning zero are the benign answer for the
     * status and capability registers touched here.
     *
     * ponytail: plain memory, no register semantics. A spin loop waiting for
     * a bit to *set* would hang rather than fault -- if that shows up, the fix
     * is to bridge the D3D8 entry point that owns the loop, not to start
     * emulating NV2A. Nothing has needed that yet.
     */
    {
        uintptr_t nv2a_native = XBOX_NV2A_BASE + g_memory_offset;
        g_nv2a_xemu = nv2a_xemu_requested();
        g_nv2a_memory = VirtualAlloc(
            (LPVOID)nv2a_native,
            XBOX_NV2A_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        /* The pushbuffer survey rides on the same poll, so either
         * variable arms it. */
        s_nv2a_trace = getenv("RECOMP_NV2A_TRACE") != NULL
                    || getenv("RECOMP_PB_SCAN") != NULL
                    || getenv("RECOMP_PB_EXEC") != NULL;
        if (g_nv2a_memory) {
            fprintf(stderr, "  NV2A register aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, no register semantics)\n",
                    XBOX_NV2A_SIZE / (1024 * 1024), XBOX_NV2A_BASE);
        } else {
            fprintf(stderr, "  WARNING: NV2A aperture at 0x%08X failed "
                    "(error %lu); D3D register access will fault\n",
                    XBOX_NV2A_BASE, GetLastError());
        }
    }

    /*
     * MCPX device apertures.
     *
     * The NV2A block above is not the only hardware the title touches
     * directly. The southbridge devices live higher up:
     *
     *   0xFE800000  APU (audio processing unit)
     *   0xFEC00000  AC97
     *   0xFED00000  USB0 / USB1
     *   0xFEF00000  NIC
     *
     * Halo faults reading 0xFED00000 during input initialisation -- the XDK's
     * USB code talks to the host controller's registers rather than going
     * through a driver. Back the whole span as plain RAM for the same reason
     * the NV2A aperture is backed: a read of zero is survivable, a fault is
     * not.
     *
     * ponytail: no register semantics anywhere in here. If something spins
     * waiting for a bit to set, extend the NV2A ack thread's table rather than
     * emulating the device.
     */
    {
        uintptr_t mcpx_native = XBOX_MCPX_BASE + g_memory_offset;
        g_mcpx_memory = VirtualAlloc(
            (LPVOID)mcpx_native,
            XBOX_MCPX_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        g_mcpx_regs = g_mcpx_memory;
        if (g_mcpx_memory) {
            /* AC'97 codec ready.
             *
             * DirectSound resets the codec by setting a bit in 0xFEC0012C and
             * then polls 0xFEC00130 for bit 8 a thousand times waiting for the
             * codec to come up. On zeroed registers that bit never appears, so
             * the wait times out and DirectSoundCreate returns DSERR_NODRIVER
             * (0x88780078).
             *
             * That failure is not confined to audio. Wreckless initialises its
             * whole engine object behind `if (DirectSoundCreate() >= 0)`, so a
             * failed create skips the initialisation, leaves the object's table
             * pointer null, and the null propagates: a null-derived divisor
             * produces a NaN transform matrix, which produces a garbage index,
             * which crashes. Reporting the codec as present is what lets the
             * engine initialise at all.
             *
             * The aperture is plain memory, so setting the bit once is enough:
             * nothing clears it, and the poll reads it on the first pass. */
            #define MCPX_AC97_CODEC_STATUS 0x00400130u   /* 0xFEC00130 */
            #define MCPX_AC97_CODEC_READY  0x00000100u
            /* Opt-in, and not because it is wrong.
             *
             * Reporting the codec is the correct answer -- DSERR_NODRIVER is
             * not what hardware returns -- but it is only correct as far as it
             * goes. DirectSound then hands the audio DSP a command block in
             * RAM and spins until the DSP clears it, and there is no DSP here,
             * so the title trades a late crash for an early hang: 44 assets
             * loaded and then a fault, versus one asset and a stall in audio
             * init. Until the DSP handshake is answered, the honest default is
             * the failure that gets further, with the correct behaviour one
             * variable away. */
            if (getenv("RECOMP_AC97_READY")) {
                /* The APU's registers have to fault so they can be routed to
                 * the emulated APU, which is the half that answers the DSP
                 * handshake. Backed as plain memory the guest's writes go
                 * nowhere the APU can see, so it initialises and then waits
                 * forever. The APU's own 512K is unmapped here; the AC'97
                 * page is trapped separately below (mcpx_aci.c).
                 *
                 * Enabled by the same variable, because neither half is any
                 * use without the other. */
                DWORD old_protect;
                if (VirtualProtect((char *)g_mcpx_memory, 0x00080000u,
                                   PAGE_NOACCESS, &old_protect))
                    g_apu_mmio_trapped = 1;
                if (g_apu_mmio_trapped) {
                    /* Physical RAM starts at guest VA 0 in this layout (the
                     * contiguous window aliases the same pages), which is what
                     * the APU's DMA/scatter-gather reads expect as ram_ptr. */
                    if (!g_apu_state)
                        g_apu_state = mcpx_apu_init_standalone(
                            (uint8_t *)(uintptr_t)g_memory_offset);
                    /* IRQ 5 -> the title's connected ISR (kernel_bridge.c). */
                    mcpx_apu_set_irq_handler(xbox_irq_apu_line);
                    g_apu_host_lo = (uintptr_t)g_mcpx_memory;
                    g_apu_host_hi = g_apu_host_lo + 0x00080000u;
                    AddVectoredExceptionHandler(1, apu_mmio_veh);
                    if (g_apu_state)
                        xbox_io_direct_enable(1);
                    fprintf(stderr, "  APU: 0x%08X..0x%08X trapped for MMIO "
                            "(emulated APU %s, direct calls %s)\n",
                            XBOX_MCPX_BASE, XBOX_MCPX_BASE + 0x00080000u,
                            g_apu_state ? "attached" : "FAILED to init",
                            (g_recomp_io_direct & 1) ? "on" : "off");
                }
                /* The AC'97 controller has register semantics DirectSound
                 * depends on beyond the codec-ready bit: it resets a bus
                 * master with CR_RR and spins until the bit reads back
                 * clear, which plain memory never does (docs Update 355).
                 * The emulated ACI (mcpx_aci.c, from xemu ac97.c) reports
                 * the codec ready itself (GLOB_STA | GS_S0CR). */
                if (mcpx_aci_attach((uint8_t *)g_mcpx_memory
                                        + (MCPX_AC97_CODEC_STATUS & ~0xFFFu),
                                    (uint8_t *)(uintptr_t)g_memory_offset)) {
                    fprintf(stderr, "  AC97: codec reported ready at 0x%08X"
                                    " (DirectSound will initialise)\n",
                            XBOX_MCPX_BASE + MCPX_AC97_CODEC_STATUS);
                } else {
                    *(volatile uint32_t *)((char *)g_mcpx_memory
                                           + MCPX_AC97_CODEC_STATUS)
                        |= MCPX_AC97_CODEC_READY;
                    fprintf(stderr, "  AC97: emulation unavailable, codec "
                                    "bit set in plain memory at 0x%08X\n",
                            XBOX_MCPX_BASE + MCPX_AC97_CODEC_STATUS);
                }
            }
            fprintf(stderr, "  MCPX device aperture: %u MB at Xbox VA "
                    "0x%08X (APU/AC97/USB/NIC, zeroed)\n",
                    XBOX_MCPX_SIZE / (1024 * 1024), XBOX_MCPX_BASE);
        } else {
            fprintf(stderr, "  WARNING: MCPX aperture at 0x%08X failed "
                    "(error %lu); USB/audio register access will fault\n",
                    XBOX_MCPX_BASE, GetLastError());
        }
    }

    /* Flash ROM aperture -- see XBOX_FLASH_BASE for why. */
    {
        uintptr_t flash_native = XBOX_FLASH_BASE + g_memory_offset;

        g_flash_memory = VirtualAlloc(
            (LPVOID)flash_native,
            XBOX_FLASH_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_flash_memory) {
            fprintf(stderr, "  Flash ROM aperture: %u MB at Xbox VA "
                    "0x%08X (zeroed, not a real BIOS image)\n",
                    XBOX_FLASH_SIZE / (1024 * 1024), XBOX_FLASH_BASE);
        } else {
            fprintf(stderr, "  WARNING: flash aperture at 0x%08X failed "
                    "(error %lu); a title reading flash will fault\n",
                    XBOX_FLASH_BASE, GetLastError());
        }
    }

    if (g_nv2a_memory) {
        xbox_Nv2aAckStart();
    }

    /*
     * Allocate a page at Xbox kernel address space (0x80010000).
     *
     * RenderWare's Xbox driver code (xbcache.c) reads MEM32(0x8001003C)
     * to parse the Xbox kernel's PE header and find the INIT section for
     * CPU cache line sizing. On PC, we provide a minimal fake PE header
     * with 0 sections so the function gracefully skips the cache init.
     *
     * The actual native address is 0x80010000 + g_memory_offset.
     */
    {
        #define XBOX_KERNEL_BASE 0x80010000u
        #define KERNEL_PAGE_SIZE 4096
        uintptr_t kernel_native = XBOX_KERNEL_BASE + g_memory_offset;
        /* Already committed if the contiguous window above succeeded -
         * 0x80010000 sits inside it - so just use that storage. */
        g_kernel_memory = g_contig_memory
            ? (void *)kernel_native
            : VirtualAlloc((LPVOID)kernel_native, KERNEL_PAGE_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (g_kernel_memory) {
            /* Zero-fill then set e_lfanew = 0x80 (offset to PE header).
             * With the rest zeroed, NumberOfSections = 0 and the INIT
             * section search finds nothing, which is the safe path. */
            memset(g_kernel_memory, 0, KERNEL_PAGE_SIZE);
            *(uint32_t *)((uint8_t *)g_kernel_memory + 0x3C) = 0x80;  /* e_lfanew */
            fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x%08X (native %p)\n",
                    XBOX_KERNEL_BASE, g_kernel_memory);
        } else {
            fprintf(stderr, "  WARNING: could not map Xbox kernel VA 0x%08X\n",
                    XBOX_KERNEL_BASE);
        }
        #undef XBOX_KERNEL_BASE
        #undef KERNEL_PAGE_SIZE
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            (unsigned)((XBOX_HEAP_TOP - XBOX_HEAP_BASE) / (1024 * 1024)),
            XBOX_HEAP_BASE, XBOX_HEAP_TOP);

    /*
     * Map mirror views of the 64 MB region.
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        int mirrors_ok = 0;
        /* The tiled aperture is a specific architectural alias -- physical RAM
         * a second time at 0xF0000000, which is where titles render -- while
         * these mirrors are a generic emulation of the address wrap. When the
         * mapped size is large enough that a mirror would cover 0xF0000000,
         * the mirror wins the address and the tiled mapping fails with
         * ERROR_INVALID_ADDRESS; Half-Life 2 then faults on its first surface
         * write. The specific alias is worth more than one wrap mirror, so
         * skip any that would overlap it.
         *
         * Guest addresses, not host: mirror m covers guest
         * (m + 1) * g_memory_size. */
        uint64_t tiled_lo = XBOX_TILED_BASE;
        uint64_t tiled_hi = tiled_lo + xbox_TiledApertureSize();

        for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
            uint64_t guest_lo = (uint64_t)(m + 1) * g_memory_size;
            uint64_t guest_hi = guest_lo + g_memory_size;

            if (guest_lo < tiled_hi && tiled_lo < guest_hi) {
                fprintf(stderr, "  Mirror %d: skipped, overlaps the tiled"
                                " aperture at 0x%08X\n",
                        m + 1, (unsigned)XBOX_TILED_BASE);
                continue;
            }
            g_mirror_views[m] = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                (LPVOID)mirror_base
            );
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                /* The full 64 MB doesn't always fit: Windows keeps a handful
                 * of small, fixed regions of its own inside every process's
                 * address space (KUSER_SHARED_DATA near 0x7FFE0000 being the
                 * best-known example, though the exact free/occupied layout
                 * near the 2 GB boundary shifts slightly run to run). Those
                 * fragments can sit anywhere inside one mirror's 64 MB span,
                 * not just at its start, so a single retry at the mirror's
                 * base address isn't enough. A wild guest read that lands in
                 * the reachable part of that mirror still deserves the same
                 * wraparound behavior real hardware gives it (see
                 * sub_0007E03B/Update 164), so walk the mirror's range and
                 * map every free sub-run we find at its correct file offset
                 * -- a mirror missing one small fixed OS page is still far
                 * better than a mirror dropped entirely. This does not
                 * apply to the guest windows that are deliberately not
                 * RAM-mirror views (contig/NV2A/MCPX/flash): those are
                 * already committed private memory, not free, so no sub-run
                 * is found there and the mirror is skipped exactly as
                 * before. */
                uintptr_t scan = mirror_base;
                uintptr_t mirror_end = mirror_base + g_memory_size;
                int sub_ok = 0;

                while (scan < mirror_end) {
                    MEMORY_BASIC_INFORMATION mbi;
                    uintptr_t region_end;

                    if (!VirtualQuery((LPVOID)scan, &mbi, sizeof(mbi)))
                        break;
                    region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                    if (region_end > mirror_end)
                        region_end = mirror_end;

                    if (mbi.State == MEM_FREE && region_end > scan) {
                        /* MapViewOfFileEx requires both the target address
                         * and the file offset to land on a 64 KB allocation
                         * granularity boundary, but VirtualQuery's regions
                         * are only 4 KB page-granular -- round the start up
                         * to the next 64 KB boundary (mirror_base is always
                         * 64 KB-aligned already, so this keeps file_off and
                         * the address aligned to the same boundary). */
                        uintptr_t aligned = (scan + 0xFFFF) & ~(uintptr_t)0xFFFF;
                        if (aligned < region_end) {
                            SIZE_T sub_size = region_end - aligned;
                            DWORD file_off = (DWORD)(aligned - mirror_base);
                            void *view = MapViewOfFileEx(
                                g_mapping_handle,
                                FILE_MAP_ALL_ACCESS,
                                0, file_off,
                                sub_size,
                                (LPVOID)aligned
                            );
                            if (view) {
                                sub_ok++;
                                if (!g_mirror_views[m])
                                    g_mirror_views[m] = view;
                            }
                        }
                    }
                    scan = region_end > scan ? region_end : scan + 0x1000;
                }

                if (sub_ok > 0) {
                    mirrors_ok++;
                    fprintf(stderr, "  Mirror %d: %d partial view(s) at %p"
                                    " (one or more small OS regions excluded)\n",
                            m + 1, sub_ok, (void *)mirror_base);
                } else {
                    fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                            m + 1, (void *)mirror_base, GetLastError());
                }
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, XBOX_NUM_MIRRORS,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    /*
     * Tiled / write-combined aperture at 0xF0000000.
     *
     * The NV2A exposes physical RAM a second time here and titles render
     * through it. Wreckless's first surface write goes to guest 0xF1954000 --
     * the tiled alias of physical 0x01954000, already inside our RAM -- and
     * faulted because nothing was mapped there.
     *
     * A view of the same section rather than fresh storage: the title writes a
     * surface through the tiled address and reads it back through the normal
     * one, so the two have to be the same bytes. That is the whole reason the
     * RAM lives in a file mapping.
     */
    {
        uintptr_t tiled_native = XBOX_TILED_BASE + g_memory_offset;
        size_t tiled_size = xbox_TiledApertureSize();
        g_tiled_view = MapViewOfFileEx(
            g_mapping_handle,
            FILE_MAP_ALL_ACCESS,
            0, 0,
            tiled_size,
            (LPVOID)tiled_native
        );
        if (g_tiled_view) {
            /* Prove the alias rather than assert it. Everything the title
             * renders goes through this window and is read back through the
             * physical address, so if the two are not the same bytes the GPU
             * sees empty buffers and the screen stays black -- with nothing
             * anywhere to say why. One write and one read turns that into a
             * startup line. */
            {
                volatile uint32_t *via_tiled =
                    (volatile uint32_t *)((uintptr_t)(XBOX_TILED_BASE + 0x1000)
                                          + g_memory_offset);
                volatile uint32_t *via_ram =
                    (volatile uint32_t *)((uintptr_t)0x1000 + g_memory_offset);
                uint32_t saved = *via_ram;

                *via_tiled = 0xA5C30F17u;
                if (*via_ram != 0xA5C30F17u)
                    fprintf(stderr, "  WARNING: tiled aperture does NOT alias"
                            " RAM (wrote A5C30F17, read %08X) -- rendering"
                            " will read empty buffers\n", *via_ram);
                else
                    fprintf(stderr, "  Tiled aperture alias verified\n");
                *via_ram = saved;
            }
            fprintf(stderr, "  Tiled aperture: %u MB at Xbox VA 0x%08X"
                    " (aliases RAM)\n",
                    (unsigned)(g_memory_size / (1024 * 1024)),
                    XBOX_TILED_BASE);
        } else {
            fprintf(stderr, "  WARNING: tiled aperture at 0x%08X failed"
                    " (error %lu); rendering writes will fault\n",
                    XBOX_TILED_BASE, GetLastError());
        }

        /* xemu's NV2A (src/gpu, docs Update 355). Its VRAM is Xbox physical
         * RAM, which is exactly this flat view; from here on the register
         * aperture traps into the emulated GPU instead of being plain
         * memory, and the ack thread above leaves it alone. */
        if (g_nv2a_xemu && g_tiled_view && g_nv2a_memory) {
            /* The GPU gets its own view of the same section (docs Update
             * 492): surface access tracking write-protects the CPU's views
             * of a surface's pages, and the GPU's reads/writes of guest RAM
             * must not trip over that. Same bytes, different addresses. */
            void *gpu_view = MapViewOfFile(g_mapping_handle, FILE_MAP_ALL_ACCESS,
                                           0, 0, tiled_size);
            if (gpu_view) {
                g_gpu_private_view = gpu_view;
                g_gpu_private_size = tiled_size;
                fprintf(stderr, "  GPU private view of physical RAM at %p\n",
                        gpu_view);
            }
            if (!nv2a_recomp_attach((uint8_t *)g_nv2a_memory,
                                    (uint8_t *)(gpu_view ? gpu_view : g_tiled_view),
                                    tiled_size))
                fprintf(stderr, "  WARNING: xemu NV2A failed to attach\n");
        }
    }

    /*
     * Contiguous-allocation alias self-test.
     *
     * g_contig_alias_view itself was already mapped earlier, right after the
     * legacy contig window and before the RAM mirrors -- deliberately before
     * the mirrors, so their own VirtualQuery-based occupied-range detection
     * (see the mirror loop above) treats this region as already-committed
     * and skips/fails-over it exactly like it already does for the legacy
     * contig window, the kernel page, and the NV2A/MCPX/flash apertures,
     * instead of a mirror silently grabbing this address first. Mapping it
     * that early meant the tiled aperture did not exist yet to test against,
     * so the actual round-trip proof happens here, now that it does.
     */
    if (g_contig_alias_view) {
        size_t contig_alias_phys_off = g_contig_alias_base - XBOX_CONTIG_BASE;

        if (!g_tiled_view || xbox_TiledApertureSize() < g_contig_alias_need) {
            /* The tiled aperture itself failed, or the NV2A-register ceiling
             * clamped it short of reaching this region (only possible with
             * an unusually large g_xbox_map_size) -- either way the round
             * trip this whole fix depends on cannot be proven, so do not
             * trust it silently. */
            fprintf(stderr, "  WARNING: tiled aperture does not reach the"
                    " contig alias region's physical offset 0x%08X"
                    " (mapped %u MB) -- falling back to the legacy,"
                    " non-aliased window\n",
                    (unsigned)contig_alias_phys_off,
                    g_tiled_view ? (unsigned)(xbox_TiledApertureSize() / (1024 * 1024)) : 0);
            g_contig_next = XBOX_CONTIG_BASE;
            g_contig_top  = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE
                           - XBOX_GPU_INSTANCE_DEFAULT;
        } else {
            /* Prove the alias rather than assume it, the same way the
             * tiled-aperture check above proves base<->tiled. A title's
             * whole point in calling MmAllocateContiguousMemory is to hand
             * the GPU an address it can also reach through the tiled
             * window; if that silently is not the same memory, the failure
             * shows up as a corrupted global far from here (Update 228's
             * 0x004AB668), with nothing pointing back at this mapping.
             * Both directions: contig write must read back through tiled,
             * and tiled write must read back through contig. */
            volatile uint32_t *via_contig =
                (volatile uint32_t *)((uintptr_t)(g_contig_alias_base + 0x1000)
                                      + g_memory_offset);
            volatile uint32_t *via_tiled =
                (volatile uint32_t *)((uintptr_t)(XBOX_TILED_BASE
                                                  + (uint32_t)contig_alias_phys_off
                                                  + 0x1000)
                                      + g_memory_offset);
            uint32_t saved = *via_contig;
            int ok = 1;

            *via_contig = 0x0C0714A5u;
            if (*via_tiled != 0x0C0714A5u) {
                fprintf(stderr, "  WARNING: contig alias does NOT alias"
                        " the tiled aperture (wrote 0C0714A5 via contig,"
                        " read %08X via tiled)\n", *via_tiled);
                ok = 0;
            } else {
                *via_tiled = ~0x0C0714A5u;
                if (*via_contig != ~0x0C0714A5u) {
                    fprintf(stderr, "  WARNING: tiled aperture write does"
                            " NOT alias back to the contig window (wrote"
                            " F3F8EB5A via tiled, read %08X via contig)\n",
                            *via_contig);
                    ok = 0;
                }
            }
            *via_contig = saved;

            if (ok) {
                fprintf(stderr, "  Contig alias verified"
                        " (both directions, contig<->tiled)\n");
            } else {
                fprintf(stderr, "  WARNING: falling back to the legacy,"
                        " non-aliased contig window\n");
                g_contig_next = XBOX_CONTIG_BASE;
                g_contig_top  = XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE
                               - XBOX_GPU_INSTANCE_DEFAULT;
            }
        }

        fprintf(stderr, "  Contig alias: %u MB at Xbox VA 0x%08X"
                " (physical offset 0x%08X, %s)\n",
                XBOX_CONTIG_SIZE / (1024 * 1024), g_contig_alias_base,
                (unsigned)contig_alias_phys_off,
                g_contig_next == g_contig_alias_base
                    ? "aliases RAM/tiled, in use"
                    : "mapped but NOT in use, see warning above");
    }

    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

/*
 * Make every RAM mirror read-only, for finding writes that reach low memory
 * through an alias.
 *
 * Xbox RAM is visible at 28 virtual addresses that alias the same pages, so a
 * store to 0x04000004 changes Xbox VA 4 without ever touching VA 4. Both a
 * page-protection watchpoint and a DR0 hardware watchpoint on VA 4 therefore
 * report nothing while the memory demonstrably changes -- which is exactly
 * what happened chasing Halo's fs:[4] corruption.
 *
 * Debug aid, not part of normal startup: a title that legitimately writes
 * through a mirror will fault here too, and the fault address names the alias
 * and the code.
 */
void xbox_ProtectMirrorsForDebug(void)
{
    int n = 0;
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        DWORD old;
        if (g_mirror_views[m] &&
            VirtualProtect(g_mirror_views[m], g_memory_size,
                           PAGE_READONLY, &old)) {
            n++;
        }
    }
    fprintf(stderr, "  Mirrors: %d/%d made read-only (debug)\n",
            n, XBOX_NUM_MIRRORS);
}

void xbox_MemoryLayoutShutdown(void)
{
    if (g_kernel_memory) {
        VirtualFree(g_kernel_memory, 0, MEM_RELEASE);
        g_kernel_memory = NULL;
    }
    if (g_nv2a_ack_thread) {
        InterlockedExchange(&g_nv2a_ack_stop, 1);
        WaitForSingleObject(g_nv2a_ack_thread, 1000);
        CloseHandle(g_nv2a_ack_thread);
        g_nv2a_ack_thread = NULL;
    }
    if (g_nv2a_memory) {
        VirtualFree(g_nv2a_memory, 0, MEM_RELEASE);
        g_nv2a_memory = NULL;
    }
    /* Unmap mirror views first */
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            UnmapViewOfFile(g_mirror_views[m]);
            g_mirror_views[m] = NULL;
        }
    }
    /* Unmap base view */
    if (g_memory_base) {
        UnmapViewOfFile(g_memory_base);
        g_memory_base = NULL;
        g_memory_size = 0;
    }
    /* Close file mapping handle */
    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

/* Bump allocator for pure address-space reservations, above RAM.
 *
 * A MEM_RESERVE costs no memory on real hardware -- it takes address space out
 * of a 4 GB range, not pages out of the 64 MB the console has -- so titles
 * reserve far more than exists and commit a fraction. Satisfying that out of
 * the RAM heap does not work: Half-Life 2 asks for 128 MB and then 200 MB, and
 * clamping those to what the heap can back left it sub-allocating across a
 * range it believed it owned, walking past the top of RAM and aliasing low
 * memory through the mirrors.
 *
 * So reservations come from the mapped space *above* RAM instead. Those pages
 * are already backed and distinct, nothing else hands them out, and a commit
 * inside one is a no-op because it is real memory already.
 *
 * Returns 0 when the mapping is no larger than RAM -- the default for titles
 * that never call xbox_SetMapSize -- which leaves the old behaviour untouched.
 *
 * ponytail: a bump allocator with no free. A reservation is address space, the
 * range is large, and a title that reserves and releases repeatedly would need
 * a real allocator; none has yet.
 */
static uint32_t g_reserve_next;

uint32_t xbox_ReserveAlloc(uint32_t size, uint32_t align)
{
    uint32_t base;

    if (g_memory_size <= g_xbox_total_ram || size == 0)
        return 0;
    if (!align)
        align = 4096;
    if (!g_reserve_next)
        g_reserve_next = (uint32_t)g_xbox_total_ram;

    base = (g_reserve_next + align - 1) & ~(align - 1);
    if ((size_t)base + size > g_memory_size)
        return 0;
    g_reserve_next = base + size;
    return base;
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

/* ── Dynamic heap allocator ────────────────────────────────
 *
 * Simple bump allocator for MmAllocateContiguousMemory and similar.
 * Returns Xbox VAs within the mapped region so MEM32() works correctly.
 * No free support (bump-only for now).
 */
static uint32_t g_heap_next = XBOX_HEAP_BASE;
static uint32_t g_heap_top_next;   /* 0 = no MEM_TOP_DOWN block yet */

static int g_heap_alloc_count = 0;

/* Block table backing xbox_HeapFree. A bump pointer alone never reclaims,
 * which is fine for a title that allocates once and fatal for a debug build
 * that churns. Flat array rather than an intrusive list: allocations come back
 * in bump order, so index order is address order and coalescing is a
 * neighbour check. */
#define XBOX_HEAP_MAX_BLOCKS 65536
static struct { uint32_t addr; uint32_t size; uint8_t free; }
    g_heap_blocks[XBOX_HEAP_MAX_BLOCKS];
static int g_heap_block_count = 0;

/*
 * Simulated stacks for spawned threads.
 *
 * The main thread owns the top of the XBOX_STACK region and grows down; worker
 * stacks are carved from the bottom upward so the two cannot meet until the
 * whole 8 MB is gone. Xbox VAs, not host memory: recompiled code addresses its
 * stack through MEM32() like any other Xbox pointer.
 */
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

#define XBOX_THREAD_STACK_SIZE  (512 * 1024)  /* when the title asks for 0 */
#define XBOX_THREAD_STACK_MIN   (64 * 1024)   /* floor for explicit requests */
#define XBOX_MAX_THREAD_STACKS  64

/* Live worker stacks: top -> size, so each is freed with its own size.
 *
 * The pool used to be a fixed 8 x 512 KB. The console has no such limit --
 * PsCreateSystemThreadEx gives every thread a kernel stack of the requested
 * size, bounded by memory -- and SCDA exceeds 8: loading Echelon.SP2 starts
 * seven streaming workers on top of the Bink/DirectSound ones, the ninth ran
 * "inline" on the caller and the loading screen never finished (docs Update
 * 345). 64 slots is a table size, not a model of the hardware. */
static struct { uint32_t top, size; } g_thread_stacks[XBOX_MAX_THREAD_STACKS];
static int g_thread_stacks_used = 0;

/* A TIB and TLS block for a newly spawned guest thread.
 *
 * A TIB is per-thread on the console and was per-process here: one address,
 * 0x1000, for everyone. Two things live in it that must not be shared. fs:[0]
 * is the SEH chain head, so two threads unwinding at once walk each other's
 * frames. fs:[4] points at the image's TLS block, whose slot 0 is the CRT's
 * per-thread data -- errno, the locale, and the bookkeeping _lock() uses to
 * decide who owns which lock.
 *
 * Half-Life 2 deadlocked on the last of those: two threads inside _lock(),
 * each holding the CRT lock the other was waiting for, because "which thread
 * am I" was a single shared answer.
 *
 * The new block is a copy of the template the loader built, so a thread starts
 * with the image's initialised thread-local data rather than zeros, and its
 * own per-thread structure behind slot 0.
 */
uint32_t xbox_AllocThreadTib(void)
{
    /* XBOX_VA is scoped to the loader; the same arithmetic, spelled here. */
    #define TIB_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
    const uint32_t tib_size = 0x40;
    uint32_t tib, block, thread_data, total;

    if (!g_tls_total)
        return 0;                    /* image has no TLS; nothing to copy */

    total = g_tls_total;
    tib = xbox_HeapAlloc(tib_size + total + g_tls_thread_size, 16);
    if (!tib)
        return 0;
    block       = tib + tib_size;
    thread_data = block + total;

    /* The TIB itself, copied so stack bounds and the fields the title filled
     * in are inherited, then the two that must not be. */
    memcpy(TIB_VA(tib), TIB_VA(XBOX_TIB_MAIN), tib_size);
    memcpy(TIB_VA(block), TIB_VA(g_tls_template_va), total);
    memset(TIB_VA(thread_data), 0, g_tls_thread_size);

    *(uint32_t *)TIB_VA(tib + 0x00) = 0xFFFFFFFFu;   /* own SEH chain    */
    *(uint32_t *)TIB_VA(block)      = thread_data;   /* slot 0           */
    *(uint32_t *)TIB_VA(tib + 0x04) = block + total; /* fs:[4], see above*/

    /* The two writes above land in guest heap memory at `tib`, which is
     * where g_fs_base (set by the caller right after this returns) points --
     * but fs:[N] reads/writes in generated code go through xbox_fs_read/write*
     * instead, which store into g_xbox_fs, a *different*, per-host-thread
     * array (see the comment on g_xbox_fs). That array starts zeroed for
     * every new native thread and nothing else ever populates it for a
     * spawned worker, so a worker's own fs:[4] read came back 0 instead of
     * this thread's own TLS block pointer -- silently wrong hardware-Xbox
     * behavior on real hardware would never have hit (its fs:[N] and the TLS
     * lookup are the same underlying storage), and a worker thread reaching
     * real guest code was untested until multi-threaded execution actually
     * started working. Mirror the one field known to matter (0x04) into the
     * thread's own g_xbox_fs, matching what xbox_MemoryLayoutInit does for
     * the main thread's fs:[4]. */
    xbox_fs_write32(0x00, 0xFFFFFFFFu);
    xbox_fs_write32(0x04, block + total);

    /* fs:[0x28] has the exact same gap, just discovered later: a worker's
     * read came back 0 (never populated) instead of xbox_MemoryLayoutInit's
     * FAKE_TLS_VA (0x00760000, hardcoded there since it is never exposed
     * past that function's local scope). A worker thread's C++ runtime
     * support code (e.g. sub_0036AD0E's per-thread current-exception-object
     * lookup) reads fs:[0x28], finds 0, and takes the "no TLS" fallback
     * branch onto a shared default object instead of resolving its own
     * fs:[4]-based per-thread slot (which worker threads DO already have
     * correctly, via the write above) -- ending up operating on that
     * default object's sentinel value and calling the object destructor /
     * free() on a bogus pointer. Every thread shares the same fake
     * RW-engine-context stand-in already (xbox_MemoryLayoutInit sets up
     * exactly one, not one per thread), so mirroring the same fixed address
     * here is consistent with that existing simplification, not a new one.
     * fs:[0x20] (KPCR Prcb) is the same story -- a zeroed, shared stand-in,
     * safe to give every thread the same value. */
    xbox_fs_write32(0x20, 0x00761000u); /* FAKE_PRCB_VA, see xbox_MemoryLayoutInit */
    xbox_fs_write32(0x28, 0x00760000u); /* FAKE_TLS_VA, see xbox_MemoryLayoutInit */

    return tib;
    #undef TIB_VA
}

uint32_t xbox_AllocThreadStack(void)
{
    return xbox_AllocThreadStackSized(0);
}

uint32_t xbox_AllocThreadStackSized(uint32_t requested)
{
    uint32_t base, size;
    int slot;

    for (slot = 0; slot < XBOX_MAX_THREAD_STACKS; slot++)
        if (!g_thread_stacks[slot].top)
            break;
    if (slot == XBOX_MAX_THREAD_STACKS)
        return 0;

    /* The size the title asked for, page-rounded, with a floor for margin;
     * 0 keeps the previous default. */
    size = requested ? ((requested + 4095u) & ~4095u) : XBOX_THREAD_STACK_SIZE;
    if (requested && size < XBOX_THREAD_STACK_MIN)
        size = XBOX_THREAD_STACK_MIN;

    /* From the heap, not from XBOX_STACK_BASE.
     *
     * The stack region begins at 0x00780000, which is fine only while the
     * title's image ends below that. Half-Life 2's image runs to 0x009B68C0,
     * so the first thread stack (0x00780000..0x00800000) landed inside its
     * .rdata and .data: the worker spawned during engine init wrote its
     * frames over the game's own static data. Nothing faults -- the pages are
     * mapped and writable -- so it shows up later as globals that were
     * correct when written and wrong when read.
     *
     * The heap already starts above the image and knows how big it is, so
     * taking slices from it is correct for any image size instead of only
     * for small ones.
     */
    base = xbox_HeapAlloc(size, 4096);
    if (!base)
        return 0;
    g_thread_stacks_used++;

    /* Top of the slice, 16-byte aligned, growing down. */
    g_thread_stacks[slot].top = base + size - 16;
    g_thread_stacks[slot].size = size;
    return base + size - 16;
}

/* Give a worker's stack back when the worker ends.
 *
 * The counter used to only ever go up, so a title that creates and destroys
 * threads ran the pool dry no matter how few were alive at once. The Xbox
 * Dashboard spawns one worker per ambient WAV and terminates it before loading
 * the next; after XBOX_MAX_THREAD_STACKS files the pool was empty and
 * PsCreateSystemThreadEx fell back to running the worker inline. That fallback
 * is a deadlock here rather than a slowdown: the worker ran to completion
 * before the caller reached its wait, so the main thread then waited forever on
 * events whose only signaller had already finished. It looked like an audio
 * hang, three layers away from the cause.
 *
 * Takes the value AllocThreadStack returned, so callers never do the arithmetic.
 */
void xbox_FreeThreadStack(uint32_t stack_top)
{
    int slot;

    if (!stack_top)
        return;
    for (slot = 0; slot < XBOX_MAX_THREAD_STACKS; slot++) {
        if (g_thread_stacks[slot].top == stack_top) {
            xbox_HeapFree(stack_top + 16 - g_thread_stacks[slot].size);
            g_thread_stacks[slot].top = 0;
            g_thread_stacks[slot].size = 0;
            if (g_thread_stacks_used > 0)
                g_thread_stacks_used--;
            return;
        }
    }
}

/* Bump allocator over the contiguous window -- g_contig_next and g_contig_top
 * are declared up near g_contig_alias_view (xbox_MemoryLayoutInit needs them
 * before this point in the file to redirect the bump pointer into the alias
 * region once it is mapped).
 *
 * MmAllocateContiguousMemory hands back physical memory, and on Xbox physical
 * page P is visible at 0x80000000 + P. Drivers rely on that being an exact
 * round trip: Xbox D3D writes its pushbuffer position to the NV2A as
 * `VA & 0x0FFFFFFF` and reads the GPU's position back as `GET | 0x80000000`,
 * then compares the two. That holds for any address in this window and for
 * nothing in the general heap, whose position depends on what the title
 * reserved first -- Half-Life 2 reserves 128 MB and then 200 MB before D3D
 * allocates its pushbuffer, which put the buffer at 0x15782000 and left the
 * engine comparing 0x857844C0 against it forever.
 *
 * Grows up from the base (the alias region's own base once redirected there);
 * g_contig_top marks where XBOX_GPU_INSTANCE_DEFAULT is carved off for the
 * GPU-instance bridge in the legacy window, or simply the alias region's own
 * end (no carve-out needed there -- see bridge_MmClaimGpuInstanceMemory,
 * which always computes its address from the legacy XBOX_CONTIG_BASE window
 * directly and never consults this bump pointer).
 * Never freed: contiguous blocks are framebuffers and pushbuffers, which a
 * title allocates once. */

/* 2026-09-11: g_contig_next (above) and g_heap_next/g_heap_blocks/
 * g_heap_block_count (xbox_HeapAlloc et al., below) were bump/free-list
 * allocators with no locking at all, despite being reachable from more than
 * one guest thread -- SCDA runs at least two Bink decode worker threads for
 * the whole title's lifetime, each doing its own streaming-buffer heap
 * traffic, plus xbox_AllocThreadStack (which carves a spawned worker's own
 * simulated stack out of this same heap) racing against whichever thread is
 * already running.
 *
 * Two threads racing xbox_HeapAlloc read the same stale g_heap_next, both
 * compute the same base address, and both believe they own it -- one
 * thread's "private" buffer or worker stack then physically aliases
 * another thread's live memory. Caught live: sub_004B89E0 (a Bink node-list
 * walker) intermittently got back a corrupted saved-esi stack slot with no
 * register mismatch at any single call site that could explain it -- the
 * value that landed there (0x01199914, one Bink stream object's own pointer)
 * pointed at what turned out to be another concurrently-running worker
 * thread's data, consistent with its simulated stack overlapping this
 * thread's. The already-known rare CONTIG-arena-exhaustion crash
 * (docs/SCDA_WORKLOG.md) is the same shape of bug on g_contig_next.
 *
 * A single process-wide spinlock is enough: both allocators do a short,
 * bounded amount of work per call (bump-pointer arithmetic plus a linear
 * scan/insert into a flat block table), so there is nothing to gain from two
 * separate locks and one is simpler to get right. _InterlockedCompareExchange
 * matches the spinlock pattern already used elsewhere in this codebase
 * (per-object state tables) rather than introducing CRITICAL_SECTION's
 * static-initialization-order questions for what is, in practice, always
 * uncontended. */
static volatile LONG g_alloc_lock = 0;
static void xbox_alloc_lock(void)
{
    while (InterlockedCompareExchange(&g_alloc_lock, 1, 0) != 0)
        ; /* spin: critical section is a few dozen instructions, never blocking */
}
static void xbox_alloc_unlock(void)
{
    InterlockedExchange(&g_alloc_lock, 0);
}

uint32_t xbox_ContiguousAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    xbox_alloc_lock();

    if (alignment < 4096) alignment = 4096;
    result = (g_contig_next + alignment - 1) & ~(alignment - 1);

    /* g_contig_top is the alias region's own top when g_contig_alias_view
     * mapped successfully (no GPU-instance carve-out needed there -- that
     * bridge (bridge_MmClaimGpuInstanceMemory) always computes its address
     * from the legacy XBOX_CONTIG_BASE window directly, never from here), or
     * the legacy window's top-minus-GPU-instance-reservation on fallback. */
    if ((uint64_t)result + size > (uint64_t)g_contig_top) {
        fprintf(stderr, "  [CONTIG] arena exhausted (%u requested, base=0x%08X"
                " next=0x%08X top=0x%08X, source=0x%08X ecx=0x%08X esi=0x%08X)\n",
                size, g_contig_alias_view ? g_contig_alias_base : XBOX_CONTIG_BASE,
                g_contig_next, g_contig_top,
                (uint32_t)g_icall_source, g_ecx, g_esi);
        fflush(stderr);
        xbox_alloc_unlock();
        return 0;
    }

    g_contig_next = result + size;
    xbox_alloc_unlock();
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
    return result;
}

BOOL xbox_ResolveGpuPhysicalRange(uint32_t physical_address, uint32_t bytes,
                                  uint32_t *guest_va)
{
    uint64_t end;

    if (!guest_va || !bytes)
        return FALSE;
    end = (uint64_t)physical_address + bytes;
    if (end > 0x100000000ull)
        return FALSE;

    if (g_contig_alias_view && g_contig_next >= g_contig_alias_base) {
        uint32_t alias_phys = g_contig_alias_base - XBOX_CONTIG_BASE;
        uint64_t allocated_end = (uint64_t)alias_phys
                               + (g_contig_next - g_contig_alias_base);

        /* Dynamic contiguous allocations on this branch live at e.g.
         * VA 0x8420C000 and are presented to NV2A as physical 0x0420C000.
         * XBOX_CONTIG_BASE + P is therefore the exact reverse mapping. */
        if (physical_address >= alias_phys && end <= allocated_end) {
            *guest_va = XBOX_CONTIG_BASE + physical_address;
            return TRUE;
        }
    } else if (g_contig_next >= XBOX_CONTIG_BASE) {
        /* Startup fallback when the alias view could not be established: the
         * legacy contiguous window begins at physical zero. Prefer it over
         * identity RAM for offsets the allocator actually handed out. */
        uint64_t allocated_end = g_contig_next - XBOX_CONTIG_BASE;
        if (end <= allocated_end) {
            *guest_va = XBOX_CONTIG_BASE + physical_address;
            return TRUE;
        }
    }

    /* Ordinary physical RAM remains identity-addressed. This path is kept
     * separate from the alias test above so a dynamic surface can never land
     * on the loaded image merely because its physical number resembles a VA. */
    if (end <= g_memory_size) {
        *guest_va = physical_address;
        return TRUE;
    }
    return FALSE;
}

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint32_t result;

    xbox_alloc_lock();

    if (alignment < 4) alignment = 4;

    /* Enforce minimum allocation size.
     * The Xbox D3D8 code sometimes computes resource sizes from GPU
     * capabilities that return 0 (since we don't have real NV2A hardware),
     * resulting in zero-size allocations. With a bump allocator, these all
     * return the same address, causing overlapping structures. Enforce a
     * minimum of 4096 bytes so each allocation gets its own memory. */
    if (size < 16) size = 16;

    /* Reuse a freed block first. Without this the heap only ever grows: Halo's
     * debug build allocates and releases heavily through init, exhausted all
     * 48 MB in 4,726 allocations, and its second D3D CreateDevice then failed
     * with E_OUTOFMEMORY -- which the title reports by clearing
     * global_d3d_device, so the rasterizer asserts and startup stops. */
    for (int i = 0; i < g_heap_block_count; i++) {
        if (!g_heap_blocks[i].free || g_heap_blocks[i].size < size) {
            continue;
        }
        if (g_heap_blocks[i].addr & (alignment - 1)) {
            continue;   /* wrong alignment for this request */
        }
        g_heap_blocks[i].free = 0;
        result = g_heap_blocks[i].addr;
        xbox_alloc_unlock();
        {
            static int trace = -1;
            static volatile LONG logged;
            if (trace < 0)
                trace = getenv("RECOMP_NTVM_TRACE") != NULL;
            if (trace && InterlockedIncrement(&logged) <= 400)
                fprintf(stderr, "[HEAP-REUSE] 0x%08X (block %u) for size=%u align=%u "
                        "tid=%lu\n", result, g_heap_blocks[i].size, size, alignment,
                        GetCurrentThreadId());
        }
        memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
        return result;
    }

    /* Align the next pointer */
    result = (g_heap_next + alignment - 1) & ~(alignment - 1);

    if (result + size > XBOX_HEAP_TOP ||
        (g_heap_top_next && result + size > g_heap_top_next)) {
        fprintf(stderr, "xbox_HeapAlloc: out of memory (requested %u, used %u/%u)\n",
                size, g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        /* Who ate the heap? Group live blocks by size -- an exhausted heap is
         * nearly always one request size repeated, and the count names it. */
        {
            static int dumped = 0;
            static struct { uint32_t size; int n; } hist[256];
            if (!dumped) {
                int used = 0;
                dumped = 1;
                for (int i = 0; i < g_heap_block_count; i++) {
                    int j = 0;
                    if (g_heap_blocks[i].free || !g_heap_blocks[i].size) continue;
                    while (j < used && hist[j].size != g_heap_blocks[i].size) j++;
                    if (j == used) {
                        if (used == 256) continue;   /* ponytail: 256 distinct sizes is plenty */
                        hist[used].size = g_heap_blocks[i].size;
                        hist[used++].n = 0;
                    }
                    hist[j].n++;
                }
                for (int j = 0; j < used; j++) {
                    if ((uint64_t)hist[j].n * hist[j].size < 1024 * 1024) continue;
                    fprintf(stderr, "  [HEAP] %d live blocks of %u bytes (%u KB)\n",
                            hist[j].n, hist[j].size,
                            (unsigned)((uint64_t)hist[j].n * hist[j].size / 1024));
                }
                fflush(stderr);
            }
        }
        xbox_alloc_unlock();
        return 0;
    }

    g_heap_next = result + size;

    if (g_heap_block_count < XBOX_HEAP_MAX_BLOCKS) {
        g_heap_blocks[g_heap_block_count].addr = result;
        g_heap_blocks[g_heap_block_count].size = size;
        g_heap_blocks[g_heap_block_count].free = 0;
        g_heap_block_count++;
    }

    g_heap_alloc_count++;
    /* Rate-limited: a debug title makes thousands of these and the log is a
     * diagnostic, not a transaction record. */
    if (g_heap_alloc_count <= 32 || (g_heap_alloc_count % 512) == 0) {
        fprintf(stderr, "  [HEAP] #%d: size=%u align=%u → 0x%08X..0x%08X (used %u/%u)\n",
                g_heap_alloc_count, size, alignment, result, result + size,
                g_heap_next - XBOX_HEAP_BASE,
                (unsigned)(XBOX_HEAP_TOP - XBOX_HEAP_BASE));
        fflush(stderr);
    }

    xbox_alloc_unlock();

    /* Zero-fill the allocated block (Xbox memory is always zeroed). Outside
     * the lock: this block is reserved (g_heap_next already moved past it)
     * before any other thread can be handed it, so the write is exclusive
     * without holding the allocator's own lock across a potentially large
     * memset. */
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    return result;
}

/* How big is the block at this guest address?
 *
 * MmQueryAllocationSize and ExQueryPoolBlockSize both ask this, and both used
 * to answer 0 -- ExQueryPoolBlockSize by returning a literal, and
 * MmQueryAllocationSize by having no bridge at all. The host cannot answer it:
 * VirtualQuery on the translated address reports the size of the whole 64 MB
 * guest mapping, which is a worse answer than none. The block table already
 * has the real one, and it is the same table xbox_HeapFree matches against.
 *
 * Interior addresses count: a title that asks about a pointer it has walked
 * forward is asking about the block that contains it. Returns 0 for an address
 * this heap never handed out, which is what "not one of mine" has to look like.
 */
uint32_t xbox_HeapBlockSize(uint32_t xbox_va)
{
    int i;
    uint32_t result = 0;

    if (!xbox_va)
        return 0;
    xbox_alloc_lock();
    for (i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].free)
            continue;
        if (xbox_va >= g_heap_blocks[i].addr &&
            xbox_va <  g_heap_blocks[i].addr + g_heap_blocks[i].size) {
            result = g_heap_blocks[i].size - (xbox_va - g_heap_blocks[i].addr);
            break;
        }
    }
    xbox_alloc_unlock();
    return result;
}

void xbox_HeapFree(uint32_t xbox_va)
{
    static int frees = 0, matched = 0;

    if (!xbox_va) {
        return;
    }
    xbox_alloc_lock();
    frees++;
    if (frees <= 8) {
        fprintf(stderr, "  [HEAP] free #%d va=0x%08X blocks=%d\n",
                frees, xbox_va, g_heap_block_count);
        fflush(stderr);
    }
    for (int i = 0; i < g_heap_block_count; i++) {
        if (g_heap_blocks[i].addr != xbox_va || g_heap_blocks[i].free) {
            continue;
        }
        g_heap_blocks[i].free = 1;
        if (++matched % 512 == 0) {
            fprintf(stderr, "  [HEAP] frees=%d matched=%d blocks=%d\n",
                    frees, matched, g_heap_block_count);
            fflush(stderr);
        }

        /* Coalesce with neighbours. Blocks are recorded in bump order, so
         * index order is address order and adjacency is a simple end==start
         * test. Keeps large contiguous requests satisfiable after a lot of
         * small churn. */
        if (i + 1 < g_heap_block_count && g_heap_blocks[i + 1].free &&
            g_heap_blocks[i].addr + g_heap_blocks[i].size == g_heap_blocks[i + 1].addr) {
            g_heap_blocks[i].size += g_heap_blocks[i + 1].size;
            g_heap_blocks[i + 1].size = 0;
            g_heap_blocks[i + 1].addr = 0;
        }
        if (i > 0 && g_heap_blocks[i - 1].free &&
            g_heap_blocks[i - 1].addr + g_heap_blocks[i - 1].size == g_heap_blocks[i].addr) {
            g_heap_blocks[i - 1].size += g_heap_blocks[i].size;
            g_heap_blocks[i].size = 0;
            g_heap_blocks[i].addr = 0;
        }
        xbox_alloc_unlock();
        return;
    }
    xbox_alloc_unlock();
}

/* MEM_TOP_DOWN reservations (NtAllocateVirtualMemory, docs Update 505):
 * carved from the top of the heap range downwards, as the console's memory
 * manager places them. Bottom-up allocation stops below the lowest one.
 * SCDA's sbrk-style CRT heap grows its region in place at the address
 * after it; with its 1 MB TOP_DOWN blocks bump-allocated right behind it,
 * that address was never free. Returns 0 when it does not fit. */
uint32_t xbox_HeapAllocTopDown(uint32_t size, uint32_t alignment)
{
    uint32_t top, result;

    if (alignment < 4096) alignment = 4096;
    size = (size + 0xFFFu) & ~0xFFFu;
    xbox_alloc_lock();
    top = g_heap_top_next ? g_heap_top_next : XBOX_HEAP_TOP;
    if (size > top || ((top - size) & ~(alignment - 1)) < g_heap_next ||
        g_heap_block_count >= XBOX_HEAP_MAX_BLOCKS) {
        xbox_alloc_unlock();
        return 0;
    }
    result = (top - size) & ~(alignment - 1);
    g_heap_top_next = result;
    g_heap_blocks[g_heap_block_count].addr = result;
    g_heap_blocks[g_heap_block_count].size = size;
    g_heap_blocks[g_heap_block_count].free = 0;
    g_heap_block_count++;
    xbox_alloc_unlock();
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);
    return result;
}

/* Lowest top-down allocation (XBOX_HEAP_TOP when there is none). */
uint32_t xbox_GetHeapTopDownLow(void)
{
    return g_heap_top_next ? g_heap_top_next : XBOX_HEAP_TOP;
}

uint32_t xbox_GetHeapEnd(void)
{
    return g_heap_next;
}

HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}
