/*
 * Attaches xemu's NV2A emulation (src/gpu/xemu, see PROVENANCE.md) to the
 * recompiled title. Takes the place of xemu's hw/xbox/nv2a/nv2a.c, whose job
 * there is QEMU device plumbing (PCI, VGA console, VM state).
 *
 *  - Memory: the NV2A is UMA; its "VRAM" is Xbox physical RAM. The runtime
 *    maps all of physical RAM (base RAM + contiguous-allocation alias) as one
 *    flat view at guest VA 0xF0000000 (the tiled aperture,
 *    src/kernel/xbox_memory_layout.c), so that view is vram_ptr. RAMIN is a
 *    separate 1 MiB block, as in xemu.
 *  - Registers: the 16 MiB aperture at 0xFD000000 is made PAGE_NOACCESS; a
 *    vectored exception handler decodes the faulting host instruction and
 *    routes it to the xemu block handlers (same technique as
 *    src/apu/apu_mmio_hook.c).
 *  - Interrupts: nv2a_update_irq drives IRQ 3 through the kernel's
 *    xbox_irq_set_line; the title's own ISR (D3D) services it.
 *  - VBlank: a 60 Hz thread raises PCRTC_INTR_0_VBLANK like xemu's VGA
 *    refresh callback and hands the scanned-out surface to the window.
 *
 * Enabled with RECOMP_NV2A_XEMU=1 (xbox_memory_layout.c calls
 * nv2a_recomp_attach). Local code for xboxrecomp.
 */
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/gl/renderer.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-notifications.h"
#include "xemu-version.h"
#include "qemu/fast-hash.h"

#include <windows.h>
#include <intrin.h>   /* _ReturnAddress */
#include "nv2a_recomp.h"
#include "../kernel/mmio_decode.h"

/* The GPU target intentionally has no dependency on the kernel target's
 * private include paths.  This diagnostic bridge is implemented there. */
extern void xbox_DebugWatchGpuPhysicalPage(uint32_t physical_address);

/* ---- xemu globals nv2a.c / the UI layer would define ---- */
struct config g_config;
bool g_nv2a_recomp_tcg;   /* RECOMP_NV2A_TCG, see shim/hw/hw.h */
Error *error_fatal;
const char xemu_version[] = "0.8.66-xboxrecomp";
const int xemu_version_len = sizeof(xemu_version) - 1;
const int xemu_version_major = 0, xemu_version_minor = 8, xemu_version_patch = 66;
char nv2a_recomp_error_mark;   /* error_setg's *errp marker, shim/qemu/osdep.h */

void xemu_queue_notification(const char *msg)
{
    fprintf(stderr, "[GPU] %s\n", msg);
}

void xemu_queue_error_message(const char *msg)
{
    fprintf(stderr, "[GPU] ERROR: %s\n", msg);
}

const char *xemu_settings_get_base_path(void)
{
    return ".\\";
}

extern void pgraph_gl_register_renderer(void);
extern void pgraph_gl_display_wait(void);   /* gl/display.c, Update 480 */
#ifdef XBOX_GPU_VULKAN
extern void pgraph_vk_register_renderer(void);
/* nv2a_recomp_vk.c: the Vulkan renderer's own SurfaceBinding type cannot
 * share a translation unit with the GL one. */
extern bool nv2a_recomp_vk_surface_pitch(NV2AState *d, hwaddr addr,
                                         uint32_t *pitch);
#endif

/* The renderer that is actually running: RECOMP_RENDERER=vulkan asks for
 * Vulkan, and pgraph.c falls back to OpenGL if that fails to initialise. */
static bool renderer_is_vulkan(NV2AState *d)
{
    const PGRAPHRenderer *r = d->pgraph.renderer;
    return r && r->type == CONFIG_DISPLAY_RENDERER_VULKAN;
}

static void access_report(void);   /* surface access tracking, below */

/* The OpenGL renderer's display pass is fenced for the window's context
 * (gl/display.c); the Vulkan one has finished its submission by the time
 * nv2a_get_framebuffer_surface returns. */
static void display_wait(NV2AState *d)
{
    if (!renderer_is_vulkan(d))
        pgraph_gl_display_wait();
}

extern void xbox_irq_set_line(unsigned line, int asserted);
extern void xbox_irq_deliver_from_mmio(void);
extern void xbox_io_direct_enable(int bit);   /* xbox_memory_layout.c */
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowSetSource(
    int (*fn)(uint32_t *dst, uint32_t width, uint32_t height));
extern void xbox_FramebufferWindowSetPresenter(
    int (*fn)(void *hwnd, uint32_t width, uint32_t height));
extern void xbox_FramebufferWindowAspect(uint32_t *w, uint32_t *h);

#define NV2A_IRQ_LINE     3
#define XBOX_TILED_VA     0xF0000000u
#define NV2A_APERTURE     0x01000000u
#define NV2A_RAMIN_OFFSET 0x00700000u
#define NV2A_RAMIN_SIZE   0x00100000u

static NV2AState *s_d;
static MemoryRegion s_vram;
static uintptr_t s_ap_lo, s_ap_hi;       /* host range of the trapped aperture */
static volatile LONG s_attached;

void nv2a_recomp_watch_pushbuffer_page(uint32_t physical_address)
{
    xbox_DebugWatchGpuPhysicalPage(physical_address);
}

/* ---- Big QEMU Lock ----
 * In xemu every MMIO handler and timer runs under the BQL, and PGRAPH takes
 * it around interrupt updates. The same discipline keeps the register state
 * here consistent between guest threads, the VBlank thread and PFIFO. */
static CRITICAL_SECTION s_bql;

void bql_lock(void) { EnterCriticalSection(&s_bql); }
void bql_unlock(void) { LeaveCriticalSection(&s_bql); }

/* ---- clock ---- */
int64_t nv2a_recomp_clock_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (int64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
}

void nv2a_recomp_note_muldiv_zero(void)
{
    static volatile LONG said;
    if (InterlockedExchange(&said, 1) == 0)
        fprintf(stderr, "[GPU] PTIMER read before its numerator was "
                "programmed; answering 0 (the Xbox kernel sets it at boot)\n");
}

/* ---- interrupts (xemu nv2a.c) ---- */
void nv2a_update_irq(NV2AState *d)
{
    if (d->pfifo.pending_interrupts & d->pfifo.enabled_interrupts)
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PFIFO;
    else
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PFIFO;

    if (d->pcrtc.pending_interrupts & d->pcrtc.enabled_interrupts)
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PCRTC;
    else
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PCRTC;

    if (d->pgraph.pending_interrupts & d->pgraph.enabled_interrupts)
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PGRAPH;
    else
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PGRAPH;

    xbox_irq_set_line(NV2A_IRQ_LINE,
                      d->pmc.pending_interrupts && d->pmc.enabled_interrupts);
}

/* ---- DMA objects (xemu nv2a.c) ---- */
DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address)
{
    assert(dma_obj_address < memory_region_size(&d->ramin));

    uint32_t *dma_obj = (uint32_t *)(d->ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj);
    uint32_t limit = ldl_le_p(dma_obj + 1);
    uint32_t frame = ldl_le_p(dma_obj + 2);

    DMAObject o;
    o.dma_class = GET_MASK(flags, NV_DMA_CLASS);
    o.dma_target = GET_MASK(flags, NV_DMA_TARGET);
    o.address = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST);
    o.limit = limit;
    return o;
}

void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len)
{
    DMAObject dma = nv_dma_load(d, dma_obj_address);

    dma.address &= 0x07FFFFFF;
    assert(dma.address < memory_region_size(d->vram));
    *len = dma.limit;
    return d->vram_ptr + dma.address;
}

/* ---- register blocks (xemu nv2a.c's blocktable; PRAMIN handled below) ---- */
#define ENTRY(NAME, LNAME, OFFSET, SIZE)                                      \
    [NV_##NAME] = { #NAME, OFFSET, SIZE, { LNAME##_read, LNAME##_write } }
const NV2ABlockInfo blocktable[NV_NUM_BLOCKS] = {
    ENTRY(PMC,      pmc,      0x000000, 0x001000),
    ENTRY(PBUS,     pbus,     0x001000, 0x001000),
    ENTRY(PFIFO,    pfifo,    0x002000, 0x002000),
    ENTRY(PRMA,     prma,     0x007000, 0x001000),
    ENTRY(PVIDEO,   pvideo,   0x008000, 0x001000),
    ENTRY(PTIMER,   ptimer,   0x009000, 0x001000),
    ENTRY(PCOUNTER, pcounter, 0x00a000, 0x001000),
    ENTRY(PVPE,     pvpe,     0x00b000, 0x001000),
    ENTRY(PTV,      ptv,      0x00d000, 0x001000),
    ENTRY(PRMFB,    prmfb,    0x0a0000, 0x020000),
    ENTRY(PRMVIO,   prmvio,   0x0c0000, 0x001000),
    ENTRY(PFB,      pfb,      0x100000, 0x001000),
    ENTRY(PSTRAPS,  pstraps,  0x101000, 0x001000),
    ENTRY(PGRAPH,   pgraph,   0x400000, 0x002000),
    ENTRY(PCRTC,    pcrtc,    0x600000, 0x001000),
    ENTRY(PRMCIO,   prmcio,   0x601000, 0x001000),
    ENTRY(PRAMDAC,  pramdac,  0x680000, 0x001000),
    ENTRY(PRMDIO,   prmdio,   0x681000, 0x001000),
    ENTRY(USER,     user,     0x800000, 0x800000),
};
#undef ENTRY

static volatile LONG s_unmapped_reads, s_unmapped_writes;

/* Update 361 diagnostic: submissions identify the recompiled instruction
 * which released a pushbuffer segment. The PGRAPH trace can then associate a
 * malformed constant block with its CPU-side emitter. The saved RIP is scoped
 * to the active VEH callback and is never used for emulation. */
extern unsigned xbox_FramebufferBurstGeneration(void);
static __declspec(thread) uintptr_t s_mmio_fault_rip;

/* Opt-in guest pushbuffer producer rate, on the same QPC axis as Bink's
 * decoder trace. No NV2A state or guest scheduling is changed. */
static void recomp_trace_dma_put_rate(uint32_t off)
{
    static int enabled = -1;
    static LARGE_INTEGER hz;
    static volatile LONG64 next_report, puts;
    LARGE_INTEGER now;
    if (off < 0x800000 || (off & 0xFFFF) != NV_USER_DMA_PUT)
        return;
    if (enabled < 0) {
        enabled = getenv("RECOMP_SCDA_GPU_SUBMIT_TIMING") != NULL;
        if (enabled)
            QueryPerformanceFrequency(&hz);
    }
    if (!enabled || !hz.QuadPart)
        return;
    QueryPerformanceCounter(&now);
    InterlockedIncrement64(&puts);
    {
        LONGLONG next = next_report;
        if (now.QuadPart >= next &&
            InterlockedCompareExchange64(&next_report,
                                         now.QuadPart + 5 * hz.QuadPart,
                                         next) == next)
            fprintf(stderr,
                    "[GPU-SUBMIT-RATE] qpc=%lld hz=%lld puts=%lld "
                    "tid=%lu sample_rip=%p\n",
                    (long long)now.QuadPart, (long long)hz.QuadPart,
                    (long long)InterlockedExchange64(&puts, 0),
                    (unsigned long)GetCurrentThreadId(),
                    (void *)s_mmio_fault_rip);
    }
}

static void recomp_trace_dma_put(uint32_t off, uint64_t val)
{
    static unsigned last_burst, records;
    unsigned burst = xbox_FramebufferBurstGeneration();

    if (!burst || off < 0x800000 || (off & 0xFFFF) != NV_USER_DMA_PUT) {
        return;
    }
    if (burst != last_burst) {
        last_burst = burst;
        records = 0;
        fprintf(stderr, "[GPU-SUBMIT] F11 burst %u begins\n", burst);
    }
    if (records++ >= 128) {
        return;
    }
    fprintf(stderr,
            "[GPU-SUBMIT] burst=%u n=%u put=%08x rip=%p tid=%lu\n",
            burst, records, (uint32_t)val, (void *)s_mmio_fault_rip,
            GetCurrentThreadId());
}

/* RECOMP_NV2A_MMIO_TRACE=N: log the first N register accesses outside
 * PRAMIN (block, offset, value). Bring-up aid. */
static long s_mmio_trace = -1;
static volatile LONG s_mmio_traced;

static void mmio_trace(const char *dir, uint32_t off, uint64_t val, unsigned size)
{
    if (s_mmio_trace < 0) {
        const char *e = getenv("RECOMP_NV2A_MMIO_TRACE");
        s_mmio_trace = e ? strtol(e, NULL, 0) : 0;
    }
    if (s_mmio_trace && InterlockedIncrement(&s_mmio_traced) <= s_mmio_trace) {
        const char *blk = "?";
        for (int i = 0; i < NV_NUM_BLOCKS; i++)
            if (blocktable[i].name && off >= blocktable[i].offset
                    && off < blocktable[i].offset + blocktable[i].size)
                blk = blocktable[i].name;
        fprintf(stderr, "[GPU-MMIO] %s %-7s +0x%06X = 0x%08llX (%u) tid=%lu\n",
                dir, blk, off, (unsigned long long)val, size,
                GetCurrentThreadId());
    }
}

uint64_t nv2a_recomp_mmio_read(uint32_t off, unsigned size)
{
    uint64_t r = 0;
    NV2AState *d = s_d;

    if (off >= NV2A_RAMIN_OFFSET && off + size <= NV2A_RAMIN_OFFSET + NV2A_RAMIN_SIZE) {
        memcpy(&r, d->ramin_ptr + (off - NV2A_RAMIN_OFFSET), size);
        return r;
    }
    bql_lock();
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        const NV2ABlockInfo *b = &blocktable[i];
        if (b->name && off >= b->offset && off < b->offset + b->size) {
            r = b->ops.read(d, off - b->offset, size);
            bql_unlock();
            mmio_trace("R", off, r, size);
            return r;
        }
    }
    bql_unlock();
    if (InterlockedIncrement(&s_unmapped_reads) <= 10)
        fprintf(stderr, "[GPU] read of unmapped NV2A register 0x%06X\n", off);
    return 0;
}

void nv2a_recomp_mmio_write(uint32_t off, uint64_t val, unsigned size)
{
    NV2AState *d = s_d;

    if (off >= NV2A_RAMIN_OFFSET && off + size <= NV2A_RAMIN_OFFSET + NV2A_RAMIN_SIZE) {
        memcpy(d->ramin_ptr + (off - NV2A_RAMIN_OFFSET), &val, size);
        return;
    }
    mmio_trace("W", off, val, size);
    recomp_trace_dma_put_rate(off);
    recomp_trace_dma_put(off, val);
    bql_lock();
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        const NV2ABlockInfo *b = &blocktable[i];
        if (b->name && off >= b->offset && off < b->offset + b->size) {
            b->ops.write(d, off - b->offset, val, size);
            bql_unlock();
            return;
        }
    }
    bql_unlock();
    if (InterlockedIncrement(&s_unmapped_writes) <= 10)
        fprintf(stderr, "[GPU] write of unmapped NV2A register 0x%06X = 0x%llX\n",
                off, (unsigned long long)val);
}

/* ---- VGA aliases (PRMCIO/PRMVIO): hold what is written ---- */
uint32_t vga_ioport_read(void *opaque, uint32_t addr)
{
    VGACommonState *s = opaque;
    switch (addr) {
    case VGA_CRT_IC: case VGA_CRT_IM: return s->cr_index;
    case VGA_CRT_DC: case VGA_CRT_DM: return s->cr[s->cr_index];
    case VGA_SEQ_I: return s->sr_index;
    case VGA_SEQ_D: return s->sr[s->sr_index];
    case VGA_GFX_I: return s->gr_index;
    case VGA_GFX_D: return s->gr[s->gr_index];
    case VGA_ATT_W: return s->ar_index;
    case VGA_ATT_R: return s->ar[(s->ar_index & 0x1F) % 21];
    case VGA_MIS_R: return s->msr;
    case VGA_IS1_RC: case VGA_IS1_RM:
        s->ar_flip_flop = 0;
        return s->st01 ^= 0x09; /* toggle retrace bits */
    default: return 0;
    }
}

void vga_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    VGACommonState *s = opaque;
    switch (addr) {
    case VGA_CRT_IC: case VGA_CRT_IM: s->cr_index = (uint8_t)val; break;
    case VGA_CRT_DC: case VGA_CRT_DM: s->cr[s->cr_index] = (uint8_t)val; break;
    case VGA_SEQ_I: s->sr_index = (uint8_t)val; break;
    case VGA_SEQ_D: s->sr[s->sr_index] = (uint8_t)val; break;
    case VGA_GFX_I: s->gr_index = (uint8_t)val; break;
    case VGA_GFX_D: s->gr[s->gr_index] = (uint8_t)val; break;
    case VGA_ATT_W:
        if (s->ar_flip_flop == 0)
            s->ar_index = (uint8_t)val;
        else
            s->ar[s->ar_index % 21] = (uint8_t)val;
        s->ar_flip_flop ^= 1;
        break;
    case VGA_MIS_W: s->msr = (uint8_t)val; break;
    default: break;
    }
}

void vga_invalidate_scanlines(VGACommonState *s, int y1, int y2)
{
    (void)s; (void)y1; (void)y2;
}

/* What the console kernel's AvSetDisplayMode leaves in the CRTC: it
 * programs the NV2A from its per-mode tables; the recompiled title calls
 * our HLE kernel instead (kernel_bridge.c), which hands over the
 * framebuffer geometry. Only the fields xemu reads back are set: display
 * width/height (vga_get_resolution), line offset (nv2a_get_params) and the
 * pixel depth (nv2a_get_bpp). Without them the display texture was 8x2. */
void nv2a_recomp_set_display_mode(uint32_t width, uint32_t height,
                                  uint32_t pitch, uint32_t bpp)
{
    VGACommonState *s;
    uint32_t hd = width / 8 - 1, vd = height - 1, lo = pitch / 8;

    if (!s_d || !width || !height)
        return;
    s = &s_d->vga;
    bql_lock();
    s->cr[VGA_CRTC_H_DISP] = (uint8_t)hd;
    s->cr[VGA_CRTC_V_DISP_END] = (uint8_t)vd;
    s->cr[VGA_CRTC_OVERFLOW] = (uint8_t)((s->cr[VGA_CRTC_OVERFLOW] & ~0x42) |
                                         ((vd >> 7) & 0x02) | ((vd >> 3) & 0x40));
    s->cr[0x13] = (uint8_t)lo;
    s->cr[0x19] = (uint8_t)((s->cr[0x19] & 0x1F) | ((lo >> 3) & 0xE0));
    s->cr[0x25] = (uint8_t)((s->cr[0x25] & ~0x20) | ((lo >> 6) & 0x20));
    s->cr[0x28] = (uint8_t)((s->cr[0x28] & ~3) | (bpp == 32 ? 3 : bpp ? 2 : 0));
    /* Progressive unless 1080i: render_display doubles the height when this
     * register holds anything but DISABLED. The field-start value the
     * kernel writes for 1080i only matters there; mark it non-disabled. */
    s->cr[NV_PRMCIO_INTERLACE_MODE] = height >= 1080
        ? 0 : NV_PRMCIO_INTERLACE_MODE_DISABLED;
    if (bpp == 16)
        s_d->pramdac.general_control |= NV_PRAMDAC_GENERAL_CONTROL_ALT_MODE_SEL;
    else if (bpp == 15)
        s_d->pramdac.general_control &= ~NV_PRAMDAC_GENERAL_CONTROL_ALT_MODE_SEL;
    bql_unlock();
    fprintf(stderr, "[GPU-DISPLAY] display mode %ux%u pitch %u, %u bpp\n",
            width, height, pitch, bpp);
}

/* xemu nv2a.c nv2a_get_params and QEMU/xemu hw/display/vga.c
 * vga_get_resolution (non-VBE branch), from the CRTC registers the title
 * programs. Constants 640x480 / line_offset 0 made the display shader
 * sample the wrong rows (image squashed into the top half, Update 355). */
static void recomp_vga_get_params(VGACommonState *s, VGADisplayParams *p)
{
    p->line_offset = (s->cr[0x13] | ((s->cr[0x19] & 0xe0) << 3) |
                      ((s->cr[0x25] & 0x20) << 6))
                     << 3;
    p->start_addr = (uint32_t)(s_d->pcrtc.start / 4);
    p->line_compare = s->cr[VGA_CRTC_LINE_COMPARE] |
                      ((s->cr[VGA_CRTC_OVERFLOW] & 0x10) << 4) |
                      ((s->cr[VGA_CRTC_MAX_SCAN] & 0x40) << 3);
}

static void recomp_vga_get_resolution(VGACommonState *s, int *w, int *h)
{
    int height;
    *w = (s->cr[VGA_CRTC_H_DISP] + 1) * 8;
    height = s->cr[VGA_CRTC_V_DISP_END] |
             ((s->cr[VGA_CRTC_OVERFLOW] & 0x02) << 7) |
             ((s->cr[VGA_CRTC_OVERFLOW] & 0x40) << 3);
    *h = height + 1;
}

/* ---- CPU-write detection (replaces QEMU's dirty log) ----
 *
 * QEMU tells xemu cheaply which pages the CPU wrote. Here that costs a hash,
 * and PGRAPH asks per draw, so each range is checked at most once per frame
 * (pgraph.frame_time advances on FLIP_INCREMENT_WRITE). CPU writes to
 * textures and render targets happen between frames (Bink frames, locked
 * surfaces), which a per-frame check sees.
 *
 *  - DIRTY_MEMORY_NV2A (vertex data, gl/vertex.c): always dirty -- the range
 *    is re-uploaded, which is what a dirty page would cause anyway.
 *  - DIRTY_MEMORY_NV2A_TEX (textures): dirty on the first query of a frame;
 *    gl/texture.c then compares a content hash before re-uploading.
 *  - surfaces (nv2a_recomp_surface_cpu_dirty, surface.c): content hash
 *    against the last GPU<->RAM sync, at most once per frame.
 */
#define DIRTY_SLOTS 4096
typedef struct {
    hwaddr addr, size;
    int frame;
    uint64_t hash;
    bool valid;
} DirtySlot;
static DirtySlot s_tex_slots[DIRTY_SLOTS];
static DirtySlot s_surf_slots[256];

static DirtySlot *dirty_slot(DirtySlot *tab, unsigned n, hwaddr addr, hwaddr size)
{
    unsigned h = (unsigned)((addr >> 6) ^ (addr >> 17) ^ size) & (n - 1);
    for (unsigned probe = 0; probe < 8; probe++) {
        DirtySlot *s = &tab[(h + probe) & (n - 1)];
        if (!s->valid || (s->addr == addr && s->size == size))
            return s;
    }
    return &tab[h];    /* evict */
}

/* Vertex ranges for the Vulkan renderer (vk/draw.c sync_vertex_ram_buffer):
 * "always dirty" made it re-upload every range before every draw, and since
 * the range had already been uploaded for an earlier draw in the same
 * command buffer, finish (submit + wait) before nearly every draw -- ~950 of
 * ~1000 draws in 30 menu frames (docs Update 485). The GL renderer keeps
 * "always dirty" (gl/vertex.c re-uploads with glBufferSubData, no flush).
 *
 * The check compares guest RAM against a host-side copy of the GPU mirror
 * (BUFFER_VERTEX_RAM): every upload goes through nv2a_recomp_vertex_snapshot,
 * so the copy always holds exactly what the mirror holds, whichever range
 * the upload covered. Update 485 kept one content hash per (addr, size) range
 * instead; an upload of an overlapping range with other bounds changed the
 * mirror without touching that hash, so when the guest later rewrote the old
 * bytes (rotating dynamic vertex buffers) the range counted as clean while
 * the mirror held another frame's vertices: single frames with a stretched
 * object across the screen (docs Update 501). */
static uint8_t *s_vtx_copy;           /* == BUFFER_VERTEX_RAM, per valid page */
static uint8_t *s_vtx_copy_valid;     /* per 4 KiB page */

void *nv2a_recomp_vertex_snapshot(hwaddr addr, void *data, size_t size)
{
    if (!s_vtx_copy && s_vram.size) {
        /* Committed on first touch: only pages that hold vertices cost RAM. */
        s_vtx_copy = VirtualAlloc(NULL, s_vram.size, MEM_RESERVE | MEM_COMMIT,
                                  PAGE_READWRITE);
        s_vtx_copy_valid = calloc(s_vram.size >> 12, 1);
        if (!s_vtx_copy || !s_vtx_copy_valid) {
            fprintf(stderr, "[GPU] vertex mirror copy unavailable -- every "
                    "vertex range counts as changed\n");
            s_vtx_copy = NULL;
            return data;
        }
    }
    if (!s_vtx_copy || !size || addr + size > s_vram.size)
        return data;
    memcpy(s_vtx_copy + addr, data, size);
    /* Only whole pages are known to match the mirror. */
    for (hwaddr p = (addr + 0xFFF) >> 12; p < (addr + size) >> 12; p++)
        s_vtx_copy_valid[p] = 1;
    return s_vtx_copy + addr;
}

static bool vertex_range_dirty(hwaddr addr, hwaddr size)
{
    if (!size || addr + size > s_vram.size || !s_vtx_copy)
        return true;
    for (hwaddr p = addr >> 12; p <= (addr + size - 1) >> 12; p++)
        if (!s_vtx_copy_valid[p])
            return true;
    return memcmp(s_d->vram_ptr + addr, s_vtx_copy + addr, size) != 0;
}

bool nv2a_recomp_region_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size,
                              unsigned client)
{
    static int tex_always = -1, vtx_always = -1;
    (void)mr;
    if (client == DIRTY_MEMORY_NV2A && renderer_is_vulkan(s_d)) {
        /* RECOMP_VK_VERTEX_DIRTY_ALWAYS=1 restores the old behaviour (A/B). */
        if (vtx_always < 0)
            vtx_always = getenv("RECOMP_VK_VERTEX_DIRTY_ALWAYS") != NULL;
        return vtx_always || vertex_range_dirty(addr, size);
    }
    if (client != DIRTY_MEMORY_NV2A_TEX)
        return true;
    /* RECOMP_NV2A_TEX_DIRTY_ALWAYS=1: every texture use re-checks the
     * content hash (texture.c), catching CPU writes inside a frame that the
     * once-per-frame gate below cannot see (A/B diagnosis, Update 357). */
    if (tex_always < 0)
        tex_always = getenv("RECOMP_NV2A_TEX_DIRTY_ALWAYS") != NULL;
    if (tex_always)
        return true;
    DirtySlot *s = dirty_slot(s_tex_slots, DIRTY_SLOTS, addr, size);
    int frame = s_d->pgraph.frame_time;
    if (s->valid && s->addr == addr && s->size == size && s->frame == frame)
        return false;
    s->valid = true;
    s->addr = addr;
    s->size = size;
    s->frame = frame;
    return true;
}

void nv2a_recomp_region_set_client_dirty(MemoryRegion *mr, hwaddr addr,
                                         hwaddr size, unsigned client)
{
    (void)mr;
    if (client == DIRTY_MEMORY_NV2A_TEX) {
        /* The GPU just wrote this range to RAM: textures over it must be
         * re-examined, and a surface there is now in sync with RAM. */
        for (unsigned i = 0; i < DIRTY_SLOTS; i++) {
            DirtySlot *s = &s_tex_slots[i];
            if (s->valid && s->addr < addr + size && addr < s->addr + s->size)
                s->valid = false;
        }
        DirtySlot *ss = dirty_slot(s_surf_slots, 256, addr, size);
        ss->valid = true;
        ss->addr = addr;
        ss->size = size;
        ss->frame = s_d->pgraph.frame_time;
        ss->hash = fast_hash(s_d->vram_ptr + addr, size);
    }
}

bool nv2a_recomp_surface_cpu_dirty(hwaddr addr, hwaddr size)
{
    DirtySlot *s = dirty_slot(s_surf_slots, 256, addr, size);
    int frame = s_d->pgraph.frame_time;
    uint64_t h;

    if (addr + size > s_vram.size)
        return false;
    if (s->valid && s->addr == addr && s->size == size) {
        if (s->frame == frame)
            return false;
        h = fast_hash(s_d->vram_ptr + addr, size);
        s->frame = frame;
        if (h == s->hash)
            return false;
        s->hash = h;
        return true;
    }
    s->valid = true;
    s->addr = addr;
    s->size = size;
    s->frame = frame;
    s->hash = fast_hash(s_d->vram_ptr + addr, size);
    return true;       /* first sight: the surface starts from RAM */
}

/* ---- VBlank + scan-out ---- */
static uint32_t s_last_scanout, s_last_pitch;

/* Guest-RAM scan-out for frames that are not a GPU surface (CPU-written or
 * cleared memory, e.g. before the first render): the window reads the
 * memory at pcrtc.start, as xemu falls back to its VGA console there. GPU
 * surfaces are shown through nv2a_fb_host_source instead. */
static void update_scanout_fallback(NV2AState *d)
{
    PGRAPHGLState *r;
    SurfaceBinding *surface;
    hwaddr start = d->pcrtc.start;

    if (!start)
        return;
    qemu_mutex_lock(&d->pfifo.lock);
    qemu_mutex_lock(&d->pgraph.lock);
#ifdef XBOX_GPU_VULKAN
    if (renderer_is_vulkan(d)) {
        uint32_t pitch;
        if (nv2a_recomp_vk_surface_pitch(d, start, &pitch))
            s_last_pitch = pitch;
    } else
#endif
    {
        r = d->pgraph.gl_renderer_state;
        surface = r ? pgraph_gl_surface_get_within(d, start) : NULL;
        if (surface)
            s_last_pitch = surface->pitch;
    }
    if ((uint32_t)start != s_last_scanout) {
        s_last_scanout = (uint32_t)start;
        xbox_FramebufferWindowSet(XBOX_TILED_VA + (uint32_t)start,
                                  s_last_pitch ? s_last_pitch : 640 * 4);
    }
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_unlock(&d->pfifo.lock);
}

/* The framebuffer window's pixel source (src/video/fb_present.c), called on
 * the window thread. Same path as xemu's UI: nv2a_get_framebuffer_surface()
 * has the PFIFO thread render the scanned-out surface into the display
 * texture (pgraph_gl_sync -> render_display, fenced) and returns it; the
 * window's own GL context, in the same share group, reads it back. The
 * earlier route -- flagging the surface for download into guest RAM at
 * VBlank and letting the window read RAM -- raced the download and the
 * next flip and showed stale frames every few hundred ms (Update 355). */
/* Created in nv2a_recomp_attach before the PFIFO thread makes the render
 * context current: WGL refuses to share with a context current elsewhere. */
static GloContext *s_fb_ctx;

static int nv2a_fb_host_source(uint32_t *dst, uint32_t width, uint32_t height)
{
    GloContext *ctx = s_fb_ctx;
    static uint32_t *buf;
    static size_t buf_px;
    GLint tw = 0, th = 0;
    int tex;

    if (!s_d || !ctx)
        return 0;
    glo_set_current(ctx);
    tex = nv2a_get_framebuffer_surface();
    if (!tex) {
        nv2a_release_framebuffer_surface();
        return 0;
    }
    display_wait(s_d);          /* GPU-side wait for the display pass */
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    if (tw <= 0 || th <= 0) {
        nv2a_release_framebuffer_surface();
        return 0;
    }
    {
        static GLint last_w, last_h;
        if (tw != last_w || th != last_h) {
            last_w = tw; last_h = th;
            fprintf(stderr, "[GPU-DISPLAY] display texture %dx%d\n", tw, th);
        }
    }
    if ((size_t)tw * (size_t)th > buf_px) {
        free(buf);
        buf_px = (size_t)tw * (size_t)th;
        buf = malloc(buf_px * 4);
        if (!buf) {
            buf_px = 0;
            nv2a_release_framebuffer_surface();
            return 0;
        }
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_BYTE, buf);
    glBindTexture(GL_TEXTURE_2D, 0);
    nv2a_release_framebuffer_surface();

    /* GL rows run bottom-up; nearest-neighbour to the window size. */
    for (uint32_t y = 0; y < height; y++) {
        uint32_t sy = (uint32_t)th - 1 - (uint32_t)((uint64_t)y * (uint32_t)th / height);
        const uint32_t *srow = buf + (size_t)sy * (size_t)tw;
        uint32_t *drow = dst + (size_t)y * width;
        for (uint32_t x = 0; x < width; x++)
            drow[x] = srow[(uint64_t)x * (uint32_t)tw / width] & 0x00FFFFFFu;
    }
    return 1;
}

/* The window's presenter (docs Update 479): the same display texture as
 * nv2a_fb_host_source, but blitted by the GPU into the window's back buffer
 * and swapped -- no glGetTexImage readback, no CPU scaling, no GDI. That path
 * showed ~32 refreshes a second while the guest flipped 60. Runs on the
 * window thread with s_fb_ctx made current on the window's DC (same pixel
 * format as every GloContext). RECOMP_VSYNC=0 turns off the swap interval.
 * Returns 0 (the window falls back to nv2a_fb_host_source / guest RAM) when
 * there is no display surface yet or GL presenting is unavailable. */
extern HDC glo_window_dc(HWND hwnd);
extern int glo_make_current_on(GloContext *context, HDC hdc);

void *nv2a_recomp_xchg_ptr(void *volatile *p, void *v)
{
    return InterlockedExchangePointer((PVOID volatile *)p, v);
}

static int nv2a_fb_host_present(void *hwnd_v, uint32_t width, uint32_t height)
{
    static HWND win;
    static HDC win_dc;
    static GLuint fbo;
    static int failed;
    HWND hwnd = (HWND)hwnd_v;
    GLint tw = 0, th = 0;
    int tex;
    int frame;
    static int present_wall_probe = -1;
    bool sample_wall;
    int64_t present_start = 0, acquired_at = 0, before_swap = 0;

    if (failed || !s_d || !s_fb_ctx)
        return 0;
    if (!win_dc || win != hwnd) {
        win = hwnd;
        win_dc = glo_window_dc(hwnd);
        if (!win_dc) {
            fprintf(stderr, "[GPU-DISPLAY] GL present unavailable (pixel format), "
                    "using readback\n");
            failed = 1;
            return 0;
        }
    }
    /* Only a frame the guest has flipped since the last present (docs
     * Update 482): re-presenting the same image at the monitor rate made
     * frame counters such as MSI Afterburner read a steady 60 while the
     * game ran slower, and cost a sync with the PFIFO thread for nothing.
     * 2 = "nothing new, keep showing the last image". */
    {
        static int last_frame = -1;
        frame = qatomic_read(&s_d->pgraph.frame_time);
        if (frame == last_frame && fbo)
            return 2;
        last_frame = frame;
    }
    if (present_wall_probe < 0) {
        const char *e = getenv("RECOMP_VK_DISPLAY_WALL_PROBE");
        present_wall_probe = e && e[0] == '1';
    }
    sample_wall = present_wall_probe && frame >= 120 && frame % 30 == 0;
    if (sample_wall)
        present_start = nv2a_recomp_clock_ns();
    if (!glo_make_current_on(s_fb_ctx, win_dc)) {
        fprintf(stderr, "[GPU-DISPLAY] GL present unavailable (wglMakeCurrent %lu), "
                "using readback\n", GetLastError());
        failed = 1;
        return 0;
    }
    if (!fbo) {
        typedef BOOL (WINAPI *swap_interval_fn)(int);
        swap_interval_fn swap_interval =
            (swap_interval_fn)wglGetProcAddress("wglSwapIntervalEXT");
        const char *vs = getenv("RECOMP_VSYNC");
        if (swap_interval)
            swap_interval(vs && vs[0] == '0' ? 0 : 1);
        glGenFramebuffers(1, &fbo);
        fprintf(stderr, "[GPU-DISPLAY] presenting with GL blit + SwapBuffers "
                "(vsync %s)\n", vs && vs[0] == '0' ? "off" : "on");
    }
    tex = nv2a_get_framebuffer_surface();
    if (!tex) {
        nv2a_release_framebuffer_surface();
        return 0;
    }
    display_wait(s_d);          /* GPU-side wait for the display pass */
    if (sample_wall)
        acquired_at = nv2a_recomp_clock_ns();
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (tw <= 0 || th <= 0) {
        nv2a_release_framebuffer_surface();
        return 0;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, (GLuint)tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    {
        /* Fit at the picture's aspect ratio, black bars for the rest (docs
         * Update 483): a resized or maximized window keeps 4:3 / 16:9. */
        uint32_t aw = 4, ah = 3, dw = width, dh = height, dx = 0, dy = 0;
        xbox_FramebufferWindowAspect(&aw, &ah);
        if (aw && ah) {
            if ((uint64_t)width * ah > (uint64_t)height * aw)
                dw = (uint32_t)((uint64_t)height * aw / ah);    /* pillarbox */
            else
                dh = (uint32_t)((uint64_t)width * ah / aw);     /* letterbox */
            dx = (width - dw) / 2;
            dy = (height - dh) / 2;
        }
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        /* RECOMP_PRESENT_ZOOM=<factor> (docs Update 632, viewing aid): F6
         * toggles a magnified view (sharp pixels) around the point under
         * the mouse pointer at the moment F6 was pressed. */
        GLint sx0 = 0, sy0 = 0, sx1 = tw, sy1 = th;
        GLenum filter = GL_LINEAR;
        {
            static int zoom = -1, zoom_on, f6_was;
            static double zx, zy;
            if (zoom < 0) {
                const char *z = getenv("RECOMP_PRESENT_ZOOM");
                zoom = z ? atoi(z) : 0;
                if (zoom < 2)
                    zoom = 0;
                if (zoom > 16)
                    zoom = 16;
            }
            if (zoom) {
                int f6 = (GetAsyncKeyState(VK_F6) & 0x8000) &&
                         GetForegroundWindow() == hwnd;
                if (f6 && !f6_was) {
                    zoom_on = !zoom_on;
                    if (zoom_on) {
                        POINT p;
                        GetCursorPos(&p);
                        ScreenToClient(hwnd, &p);
                        zx = dw ? (double)((LONG)p.x - (LONG)dx) / dw : 0.5;
                        zy = dh ? (double)((LONG)p.y - (LONG)dy) / dh : 0.5;
                        zx = zx < 0.0 ? 0.0 : zx > 1.0 ? 1.0 : zx;
                        zy = zy < 0.0 ? 0.0 : zy > 1.0 ? 1.0 : zy;
                    }
                }
                f6_was = f6;
                if (zoom_on) {
                    GLint w = tw / zoom, h = th / zoom;
                    /* window y runs down, the GL source runs up */
                    GLint cx = (GLint)(zx * tw), cy = (GLint)((1.0 - zy) * th);
                    sx0 = cx - w / 2;
                    sy0 = cy - h / 2;
                    sx0 = sx0 < 0 ? 0 : sx0 > tw - w ? tw - w : sx0;
                    sy0 = sy0 < 0 ? 0 : sy0 > th - h ? th - h : sy0;
                    sx1 = sx0 + w;
                    sy1 = sy0 + h;
                    filter = GL_NEAREST;
                }
            }
        }
        /* Both GL framebuffers are bottom-up, so no flip (the readback path
         * flips because GDI wants top-down rows). */
        glBlitFramebuffer(sx0, sy0, sx1, sy1, (GLint)dx, (GLint)dy,
                          (GLint)(dx + dw), (GLint)(dy + dh),
                          GL_COLOR_BUFFER_BIT, filter);
    }
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    nv2a_release_framebuffer_surface();
    if (sample_wall)
        before_swap = nv2a_recomp_clock_ns();
    SwapBuffers(win_dc);
    if (sample_wall) {
        int64_t end = nv2a_recomp_clock_ns();
        fprintf(stderr,
                "[GL-PRESENT-WALL] frame=%d source=%dx%d window=%ux%u "
                "acquire_us=%lld blit_us=%lld swap_us=%lld total_us=%lld\n",
                frame, tw, th, width, height,
                (long long)((acquired_at - present_start) / 1000),
                (long long)((before_swap - acquired_at) / 1000),
                (long long)((end - before_swap) / 1000),
                (long long)((end - present_start) / 1000));
    }
    return 1;
}

/* Every few seconds: where PFIFO stands and why it might be stalled. A
 * title spinning in "is the GPU busy" (GET != PUT) with an idle PFIFO thread
 * means one of these gates is closed. */
static void gpu_status_report(NV2AState *d)
{
    static int64_t last;
    static unsigned frames_at_last;
    int64_t now = nv2a_recomp_clock_ns();
    if (now - last < 5000000000LL)
        return;
    last = now;
    bql_lock();
    uint32_t *pf = d->pfifo.regs;
    PGRAPHState *pg = &d->pgraph;
    uint32_t surface = pgraph_reg_r(pg, NV_PGRAPH_SURFACE);
    fprintf(stderr, "[GPU-STAT] GET=%08X PUT=%08X push0=%X dma_push=%08X "
            "dma_state=%08X pull0=%X status=%08X | pgraph_fifo=%X intr=%08X/%08X "
            "wait nop=%d flip=%d ctx=%d surface=%08X(r=%u w=%u m=%u) | "
            "pmc=%08X/%08X pcrtc=%08X/%08X "
            "start=%08llX | frames=%d (+%u)\n",
            pf[NV_PFIFO_CACHE1_DMA_GET], pf[NV_PFIFO_CACHE1_DMA_PUT],
            pf[NV_PFIFO_CACHE1_PUSH0], pf[NV_PFIFO_CACHE1_DMA_PUSH],
            pf[NV_PFIFO_CACHE1_DMA_STATE], pf[NV_PFIFO_CACHE1_PULL0],
            pf[NV_PFIFO_CACHE1_STATUS], pg->regs_[NV_PGRAPH_FIFO],
            pg->pending_interrupts, pg->enabled_interrupts,
            pg->waiting_for_nop, pg->waiting_for_flip,
            pg->waiting_for_context_switch,
            surface,
            GET_MASK(surface, NV_PGRAPH_SURFACE_READ_3D),
            GET_MASK(surface, NV_PGRAPH_SURFACE_WRITE_3D),
            GET_MASK(surface, NV_PGRAPH_SURFACE_MODULO_3D),
            d->pmc.pending_interrupts, d->pmc.enabled_interrupts,
            d->pcrtc.pending_interrupts, d->pcrtc.enabled_interrupts,
            (unsigned long long)d->pcrtc.start, pg->frame_time,
            (unsigned)(pg->frame_time - frames_at_last));
    frames_at_last = pg->frame_time;
    bql_unlock();

    /* RECOMP_GPU_PROFILE=1: xemu's per-frame counters (pgraph/profile.c)
     * summed over the last 30 frames -- why the renderer flushed, how often
     * it submitted, uploaded and downloaded. */
    {
        static int prof = -1;
        if (prof < 0) {
            const char *e = getenv("RECOMP_GPU_PROFILE");
            prof = e && e[0] == '1';
        }
        if (prof) {
            char line[1024];
            int n = 0;
            for (unsigned c = 0; c < NV2A_PROF__COUNT; c++) {
                long sum = 0;
                for (int f = 0; f < 30; f++) {
                    unsigned idx = (g_nv2a_stats.frame_ptr +
                                    NV2A_PROF_NUM_FRAMES - 1 - f) %
                                   NV2A_PROF_NUM_FRAMES;
                    sum += g_nv2a_stats.frame_history[idx].counters[c];
                }
                if (sum && n < (int)sizeof(line) - 64)
                    n += snprintf(line + n, sizeof(line) - n, " %s=%ld",
                                  nv2a_profile_get_counter_name(c), sum);
            }
            fprintf(stderr, "[GPU-PROF] last 30 frames:%s\n", n ? line : " -");
            access_report();
        }
    }
}

/* ---- F8 draw trace (docs Update 500) ----
 * F8 in the game window: for the next 60 guest frames every draw is logged
 * ([DRAW] lines: frame, draw number, primitive, vertex source address in
 * guest RAM, vertex count, render target, texture 0, combiner state), and
 * the framebuffer window dumps the same 60 frames as images (F11 burst,
 * each image tagged with its frame). Costs nothing until F8 is pressed. */
volatile int g_nv2a_draw_trace;
static int s_trace_until_frame, s_trace_last_frame = -1, s_trace_draw_no;
extern void xbox_FramebufferBurstStart(void);

extern void nv2a_recomp_vk_dump_surface(NV2AState *d, hwaddr addr,
                                        const char *name);

/* RECOMP_ZDUMP=1 with the auto draw trace (docs Update 627): in the first
 * fully traced frame, when the first draw without a colour target (the
 * shadow-map pass after the depth prepass) has run, the main depth buffer of
 * the previous draws is read back and written to zdump_main.bin. */
void nv2a_recomp_after_draw(NV2AState *d)
{
    /* RECOMP_ZDUMP=2 (docs Update 631): also dump the next frame to
     * zdump_main_b.bin, for A/B within one run (zbias action with parity). */
    static int done, first_frame = -1, dumped_frame = -1;
    static uint32_t main_zeta;
    PGRAPHState *pg = &d->pgraph;
    const char *zd = getenv("RECOMP_ZDUMP");
    if (done || !zd)
        return;
    if (first_frame < 0)
        first_frame = pg->frame_time + 1;
    int second = zd[0] == '2' && pg->frame_time == first_frame + 1;
    if (pg->frame_time != first_frame && !second)
        return;
    if (pg->surface_color.offset) {
        main_zeta = (uint32_t)pg->surface_zeta.offset;
        return;
    }
    if (main_zeta && dumped_frame != pg->frame_time) {
        dumped_frame = pg->frame_time;
        done = zd[0] != '2' || second;
        nv2a_recomp_vk_dump_surface(d, main_zeta,
                                    second ? "zdump_main_b.bin" : "zdump_main.bin");
    }
}

int nv2a_recomp_frame_time(void)
{
    return s_d ? qatomic_read(&s_d->pgraph.frame_time) : -1;
}

/* RECOMP_ZBIAS_MESHES=<hash>[+<hash>...]:<const>:<slope> (docs Update 634):
 * draws whose vertex data matches one of these signatures get an extra
 * polygon offset (depth steps / slope units, negative = towards the camera).
 * The signature is an FNV-1a hash over the first 16 vertices of attribute 0
 * (position bytes only), so it survives the title re-streaming the mesh to
 * another address. For decals that the title placed (almost) exactly on a
 * surface, where the original hardware z-fights too (SCDA JBA papers).
 * RECOMP_ZBIAS_MESH_LOG=1 prints each new signature/address pair once. */
int nv2a_recomp_mesh_zbias(NV2AState *d, float *bias_const, float *bias_slope)
{
    static int parsed, n_hash, log_on;
    static uint32_t hashes[16];
    static float b_const, b_slope;
    static uint32_t logged[512][2];
    static int n_logged;
    PGRAPHState *pg = &d->pgraph;

    if (!parsed) {
        parsed = 1;
        const char *e = getenv("RECOMP_ZBIAS_MESHES");
        while (e && *e && n_hash < 16) {
            char *end;
            unsigned long v = strtoul(e, &end, 16);
            if (end == e)
                break;
            hashes[n_hash++] = (uint32_t)v;
            e = end;
            if (*e == '+') {
                e++;
                continue;
            }
            if (*e != ':' || sscanf(e, ":%f:%f", &b_const, &b_slope) != 2)
                n_hash = 0;
            break;
        }
        const char *l = getenv("RECOMP_ZBIAS_MESH_LOG");
        log_on = l && l[0] == '1';
        if (n_hash)
            fprintf(stderr, "[ZBIAS-MESH] %d mesh signature(s), offset %g / slope %g\n",
                    n_hash, b_const, b_slope);
    }
    if (!n_hash && !log_on)
        return 0;

    VertexAttribute *a = &pg->vertex_attributes[0];
    unsigned bytes = a->count * a->size;
    if (!bytes || !a->stride || a->inline_buffer_populated)
        return 0;
    if (bytes > 16)
        bytes = 16;
    DMAObject dma = nv_dma_load(d, a->dma_select ? pg->dma_vertex_b
                                                 : pg->dma_vertex_a);
    hwaddr addr = (dma.address & 0x07FFFFFF) + a->offset;
    if (addr + 16 * (hwaddr)a->stride > memory_region_size(d->vram))
        return 0;
    const uint8_t *p = d->vram_ptr + addr;
    uint32_t hash = 2166136261u;
    for (int v = 0; v < 16; v++)
        for (unsigned k = 0; k < bytes; k++)
            hash = (hash ^ p[v * a->stride + k]) * 16777619u;

    int hit = 0;
    for (int i = 0; i < n_hash && !hit; i++)
        hit = hashes[i] == hash;
    if (log_on || hit) {
        int seen = 0;
        for (int i = 0; i < n_logged && !seen; i++)
            seen = logged[i][0] == hash && logged[i][1] == (uint32_t)addr;
        if (!seen && n_logged < 512) {
            logged[n_logged][0] = hash;
            logged[n_logged][1] = (uint32_t)addr;
            n_logged++;
            if (log_on || hit)
                fprintf(stderr, "[ZBIAS-MESH] hash=%08X addr=%08X stride=%u frame=%d%s\n",
                        hash, (uint32_t)addr, a->stride, pg->frame_time,
                        hit ? " -> offset" : "");
        }
    }
    if (hit) {
        *bias_const = b_const;
        *bias_slope = b_slope;
    }
    return hit;
}

/* RECOMP_DRAW_SKIP=<a>-<b>[,<c>-<d>...] (diagnostic, docs Update 622):
 * leave out the draws with these per-frame numbers (1-based, as in the draw
 * trace), from guest frame RECOMP_DRAW_SKIP_FROM on (default 0). With a
 * static camera the draw list repeats, so bisecting over it shows which draw
 * produces a given pixel. */
int nv2a_recomp_draw_skip(NV2AState *d)
{
    static int parsed, n_ranges, from, last_frame = -1, draw_no;
    static int lo[16], hi[16];
    int frame = d->pgraph.frame_time;

    if (!parsed) {
        const char *s = getenv("RECOMP_DRAW_SKIP");
        const char *f = getenv("RECOMP_DRAW_SKIP_FROM");
        parsed = 1;
        from = f ? atoi(f) : 0;
        while (s && *s && n_ranges < 16) {
            char *end;
            long a = strtol(s, &end, 10), b = a;
            if (end == s)
                break;
            s = end;
            if (*s == '-') {
                b = strtol(s + 1, &end, 10);
                s = end;
            }
            lo[n_ranges] = (int)a;
            hi[n_ranges] = (int)b;
            n_ranges++;
            if (*s == ',')
                s++;
        }
        if (n_ranges)
            fprintf(stderr, "[DRAW] skip: %d range(s) from frame %d, first %d-%d\n",
                    n_ranges, from, lo[0], hi[0]);
    }
    /* RECOMP_DRAW_SKIP_IF=blend=<hex>[,tex0=<hex>][,ctl0=<hex>]: only draws
     * whose BLEND / TEXOFFSET0 / CONTROL_0 registers match (all given ones);
     * without RECOMP_DRAW_SKIP every draw number is in range. */
    static int cond_parsed, has_blend, has_tex0, has_ctl0, has_rt;
    static uint32_t c_blend, c_tex0, c_ctl0, c_rt;
    /* vtx0s=<hex>+<hex>: attribute-0 RAM address is any of these (docs Update 627) */
    static uint32_t c_vtx0s[16];
    static int n_vtx0s;
    /* tex0s=<hex>+<hex>+...: TEXOFFSET0 is any of these (docs Update 624) */
    static uint32_t c_tex0s[256];
    static int n_tex0s;
    if (!cond_parsed) {
        const char *c = getenv("RECOMP_DRAW_SKIP_IF");
        const char *p;
        cond_parsed = 1;
        if (c && (p = strstr(c, "blend=")))
            has_blend = 1, c_blend = (uint32_t)strtoul(p + 6, NULL, 16);
        if (c && (p = strstr(c, "tex0=")))
            has_tex0 = 1, c_tex0 = (uint32_t)strtoul(p + 5, NULL, 16);
        if (c && (p = strstr(c, "ctl0=")))
            has_ctl0 = 1, c_ctl0 = (uint32_t)strtoul(p + 5, NULL, 16);
        if (c && (p = strstr(c, "tex0s="))) {
            const char *q = p + 6;
            while (*q && n_tex0s < 256) {
                char *end;
                unsigned long v = strtoul(q, &end, 16);
                if (end == q)
                    break;
                c_tex0s[n_tex0s++] = (uint32_t)v;
                q = *end == '+' ? end + 1 : end;
                if (*end != '+')
                    break;
            }
            has_tex0 = 0;
        }
        if (c && (p = strstr(c, "vtx0s="))) {
            const char *q = p + 6;
            while (*q && n_vtx0s < 16) {
                char *end;
                unsigned long v = strtoul(q, &end, 16);
                if (end == q)
                    break;
                c_vtx0s[n_vtx0s++] = (uint32_t)v;
                if (*end != '+')
                    break;
                q = end + 1;
            }
        }
        /* rt=<hex>: colour target offset (docs Update 624) */
        if (c && (p = strstr(c, "rt=")))
            has_rt = 1, c_rt = (uint32_t)strtoul(p + 3, NULL, 16);
        if (c)
            fprintf(stderr, "[DRAW] skip-if: blend %d/%08X tex0 %d/%08X ctl0 %d/%08X\n",
                    has_blend, c_blend, has_tex0, c_tex0, has_ctl0, c_ctl0);
    }
    if (!n_ranges && !has_blend && !has_tex0 && !has_ctl0 && !has_rt && !n_tex0s && !n_vtx0s)
        return 0;
    if (frame != last_frame) {
        last_frame = frame;
        draw_no = 0;
    }
    draw_no++;
    if (frame < from)
        return 0;
    {
        PGRAPHState *pg = &d->pgraph;
        if ((has_blend && pgraph_reg_r(pg, NV_PGRAPH_BLEND) != c_blend) ||
            (has_tex0 && pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0) != c_tex0) ||
            (has_ctl0 && pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) != c_ctl0) ||
            (has_rt && (uint32_t)pg->surface_color.offset != c_rt))
            return 0;
        if (n_vtx0s) {
            VertexAttribute *a = &pg->vertex_attributes[0];
            DMAObject dma = nv_dma_load(d, a->dma_select ? pg->dma_vertex_b
                                                         : pg->dma_vertex_a);
            uint32_t va = (uint32_t)((dma.address & 0x07FFFFFF) + a->offset);
            int hit = 0;
            for (int i = 0; i < n_vtx0s && !hit; i++)
                hit = c_vtx0s[i] == va;
            if (!hit)
                return 0;
        }
        if (n_tex0s) {
            uint32_t t = pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0);
            int hit = 0;
            for (int i = 0; i < n_tex0s && !hit; i++)
                hit = c_tex0s[i] == t;
            if (!hit)
                return 0;
        }
    }
    if (!n_ranges)
        return 1;
    for (int i = 0; i < n_ranges; i++)
        if (draw_no >= lo[i] && draw_no <= hi[i])
            return 1;
    return 0;
}

static void draw_trace_poll(NV2AState *d)
{
    static int was_down;
    int down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    /* RECOMP_DRAW_TRACE_AT=<frame> (docs Update 622): trace
     * RECOMP_DRAW_TRACE_FRAMES (default 2) frames from that guest frame on
     * without a key press and without the F11 image burst. */
    static int auto_at = -2;
    if (auto_at == -2) {
        const char *e = getenv("RECOMP_DRAW_TRACE_AT");
        auto_at = e && *e ? atoi(e) : -1;
    }
    /* RECOMP_TRACE_BURST_AT=<s> (docs Update 624): like pressing F8 that
     * many seconds after start -- 60 traced frames plus the F11 image
     * burst of the same frames (needs RECOMP_FB_BURST_DIR). */
    static int64_t burst_at_ms = -2;
    static ULONGLONG t0;
    if (burst_at_ms == -2) {
        const char *e = getenv("RECOMP_TRACE_BURST_AT");
        burst_at_ms = e && *e ? (int64_t)(atof(e) * 1000.0) : -1;
        t0 = GetTickCount64();
    }
    if (burst_at_ms >= 0 && (int64_t)(GetTickCount64() - t0) >= burst_at_ms &&
        !g_nv2a_draw_trace) {
        s_trace_until_frame = d->pgraph.frame_time + 60;
        s_trace_last_frame = -1;
        fprintf(stderr, "[DRAW] timed trace+burst for 60 frames from frame %d\n",
                d->pgraph.frame_time);
        burst_at_ms = -1;
        /* RECOMP_TRACE_BURST_NOTRACE=1: images only (the trace slows the
         * guest down a lot, which changes what is being filmed). */
        if (!getenv("RECOMP_TRACE_BURST_NOTRACE"))
            g_nv2a_draw_trace = 1;
        xbox_FramebufferBurstStart();
    }
    if (auto_at >= 0 && d->pgraph.frame_time >= auto_at && !g_nv2a_draw_trace) {
        /* RECOMP_PHYS_DUMP=<hex addr>:<hex len>[,...] (docs Update 626):
         * guest physical memory (vertex buffers etc.) to physdump_<addr>.bin
         * when the auto trace starts. */
        const char *pd = getenv("RECOMP_PHYS_DUMP");
        while (pd && *pd) {
            char *end;
            unsigned long a = strtoul(pd, &end, 16), len = 0;
            if (end == pd)
                break;
            if (*end == ':')
                len = strtoul(end + 1, &end, 16);
            if (len && a + len <= memory_region_size(d->vram)) {
                char name[64];
                FILE *f;
                snprintf(name, sizeof(name), "physdump_%08lX.bin", a);
                if ((f = fopen(name, "wb"))) {
                    fwrite((uint8_t *)d->vram_ptr + a, 1, len, f);
                    fclose(f);
                }
            }
            pd = *end == ',' ? end + 1 : end;
        }
        const char *e = getenv("RECOMP_DRAW_TRACE_FRAMES");
        int frames = e && *e ? atoi(e) : 2;
        s_trace_until_frame = d->pgraph.frame_time + 1 + (frames > 0 ? frames : 2);
        s_trace_last_frame = -1;
        fprintf(stderr, "[DRAW] auto trace for %d frames from frame %d\n",
                frames, d->pgraph.frame_time + 1);
        auto_at = -1;
        g_nv2a_draw_trace = 1;
    }
    if (down && !was_down && !g_nv2a_draw_trace) {
        DWORD pid = 0;
        HWND fg = GetForegroundWindow();
        if (fg)
            GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) {
            s_trace_until_frame = d->pgraph.frame_time + 60;
            s_trace_last_frame = -1;
            fprintf(stderr, "[DRAW] F8: tracing draws for 60 frames from frame %d\n",
                    d->pgraph.frame_time);
            g_nv2a_draw_trace = 1;
            xbox_FramebufferBurstStart();
        }
    }
    was_down = down;
}

void nv2a_recomp_trace_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    int frame = pg->frame_time;
    char attrs[256];
    int n = 0;
    unsigned verts;

    if (frame >= s_trace_until_frame) {
        g_nv2a_draw_trace = 0;
        fprintf(stderr, "[DRAW] trace finished at frame %d\n", frame);
        return;
    }
    if (frame != s_trace_last_frame) {
        s_trace_last_frame = frame;
        s_trace_draw_no = 0;
    }
    s_trace_draw_no++;
    /* RECOMP_DUMP_AT_DRAW=<n> + RECOMP_DUMP_SURFS=<hex>+<hex>... (bloom
     * ghost diagnosis, docs Update 652): right before traced draw n of the
     * first traced frame, write those surfaces to surf_<addr>.bin (guest
     * layout, see nv2a_recomp_vk_dump_surface). */
    {
        static int dump_at = -2, dumped;
        static uint32_t dump_tex0;
        if (dump_at == -2) {
            const char *e = getenv("RECOMP_DUMP_AT_DRAW");
            const char *t = getenv("RECOMP_DUMP_AT_TEX0");
            dump_at = e ? atoi(e) : -1;
            dump_tex0 = t ? (uint32_t)strtoul(t, NULL, 16) : 0;
        }
        /* or RECOMP_DUMP_AT_TEX0=<hex>: the first draw that samples that
         * address on stage 0 additively (BLEND 0x11A) into another target */
        int tex0_hit = dump_tex0 &&
            pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0) == dump_tex0 &&
            pgraph_reg_r(pg, NV_PGRAPH_BLEND) == 0x11Au &&
            (uint32_t)pg->surface_color.offset != dump_tex0;
        /* RECOMP_DUMP_AT_RT=<hex>: the first draw into that colour target
         * that samples another surface (a chain's first downsample) */
        static uint32_t dump_rt;
        static int rt_parsed;
        if (!rt_parsed) {
            const char *r = getenv("RECOMP_DUMP_AT_RT");
            rt_parsed = 1;
            dump_rt = r ? (uint32_t)strtoul(r, NULL, 16) : 0;
        }
        static int dumped_rt, rt_draw_no, rt_frame, rel_n = -1, rel_off[24];
        int rt_hit = !dumped_rt && dump_rt &&
            (uint32_t)pg->surface_color.offset == dump_rt &&
            pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0) != dump_rt;
        if (rt_hit) {
            rt_draw_no = s_trace_draw_no;
            rt_frame = frame;
        }
        /* RECOMP_DUMP_REL=<n>,<n>...: also dump before the draws that many
         * draws after the RT trigger, as r<n>_surf_*.bin (the state left by
         * the draw before), to follow one surface through a chain */
        if (rel_n < 0) {
            const char *e = getenv("RECOMP_DUMP_REL");
            rel_n = 0;
            while (e && *e && rel_n < 24) {
                char *end;
                long v = strtol(e, &end, 10);
                if (end == e)
                    break;
                rel_off[rel_n++] = (int)v;
                e = *end == ',' ? end + 1 : end;
            }
        }
        /* RECOMP_DUMP_EACH_TEX0=<hex>: around every draw of the first traced
         * frame that samples that address on stage 0, dump the surfaces
         * before it (e<n>_pre_*) and before the next draw (e<n>_post_*). */
        {
            static uint32_t each_tex0;
            static int each_parsed, each_frame = -1, pending;
            if (!each_parsed) {
                const char *e = getenv("RECOMP_DUMP_EACH_TEX0");
                each_parsed = 1;
                each_tex0 = e ? (uint32_t)strtoul(e, NULL, 16) : 0;
            }
            int hit = each_tex0 &&
                pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0) == each_tex0 &&
                (each_frame < 0 || each_frame == frame);
            for (int phase = 0; phase < 2; phase++) {
                int go = phase == 0 ? pending != 0 : hit;
                if (!go)
                    continue;
                const char *s = getenv("RECOMP_DUMP_SURFS");
                int no = phase == 0 ? pending : s_trace_draw_no;
                while (s && *s) {
                    char *end, name[64];
                    unsigned long a = strtoul(s, &end, 16);
                    if (end == s)
                        break;
                    snprintf(name, sizeof name, "e%d_%s_surf_%08lX.bin", no,
                             phase == 0 ? "post" : "pre", a);
                    nv2a_recomp_vk_dump_surface(d, (hwaddr)a, name);
                    s = *end == '+' ? end + 1 : end;
                }
                if (phase == 0)
                    pending = 0;
            }
            if (hit) {
                each_frame = frame;
                pending = s_trace_draw_no;
            }
        }
        if (dumped_rt && rt_draw_no && frame == rt_frame) {
            for (int k = 0; k < rel_n; k++) {
                if (s_trace_draw_no != rt_draw_no + rel_off[k])
                    continue;
                const char *s = getenv("RECOMP_DUMP_SURFS");
                fprintf(stderr, "[ZDUMP] before draw #%d (rel %d)\n",
                        s_trace_draw_no, rel_off[k]);
                while (s && *s) {
                    char *end, name[64];
                    unsigned long a = strtoul(s, &end, 16);
                    if (end == s)
                        break;
                    snprintf(name, sizeof name, "r%d_surf_%08lX.bin", rel_off[k], a);
                    nv2a_recomp_vk_dump_surface(d, (hwaddr)a, name);
                    s = *end == '+' ? end + 1 : end;
                }
            }
        }
        if (rt_hit ||
            (!dumped && ((dump_at > 0 && s_trace_draw_no == dump_at) || tex0_hit))) {
            /* the RT trigger writes rt_surf_*.bin, the others surf_*.bin,
             * so both can fire in one traced frame */
            fprintf(stderr, "[ZDUMP] before draw #%d\n", s_trace_draw_no);
            const char *s = getenv("RECOMP_DUMP_SURFS");
            if (rt_hit)
                dumped_rt = 1;
            else
                dumped = 1;
            while (s && *s) {
                char *end, name[64];
                unsigned long a = strtoul(s, &end, 16);
                if (end == s)
                    break;
                snprintf(name, sizeof name, "%ssurf_%08lX.bin",
                         rt_hit ? "rt_" : "", a);
                nv2a_recomp_vk_dump_surface(d, (hwaddr)a, name);
                s = *end == '+' ? end + 1 : end;
            }
        }
    }
    /* Vertex attributes with a RAM source: slot:address/stride/format.count */
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES && n < (int)sizeof(attrs) - 40; i++) {
        VertexAttribute *a = &pg->vertex_attributes[i];
        if (!a->count)
            continue;
        DMAObject dma = nv_dma_load(d, a->dma_select ? pg->dma_vertex_b
                                                     : pg->dma_vertex_a);
        uint32_t addr = (uint32_t)((dma.address & 0x07FFFFFF) + a->offset);
        n += snprintf(attrs + n, sizeof(attrs) - n, " a%d=%08X/%u/%x.%u", i,
                      addr, a->stride, a->format, a->count);
    }
    if (pg->draw_arrays_length) {
        verts = 0;
        for (unsigned i = 0; i < pg->draw_arrays_length; i++)
            verts += pg->draw_arrays_count[i];
    } else if (pg->inline_elements_length) {
        verts = pg->inline_elements_length;
    } else if (pg->inline_buffer_length) {
        verts = pg->inline_buffer_length;
    } else {
        verts = pg->inline_array_length;
    }
    fprintf(stderr, "[DRAW] f=%d #%d prim=%u kind=%s n=%u rt=%08llX zt=%08llX "
            "tex0=%08X cc=%08X sp=%08X%s\n",
            frame, s_trace_draw_no, pg->primitive_mode,
            pg->draw_arrays_length ? "arrays" :
            pg->inline_elements_length ? "elements" :
            pg->inline_buffer_length ? "ibuffer" : "iarray",
            verts, (unsigned long long)pg->surface_color.offset,
            (unsigned long long)pg->surface_zeta.offset,
            pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0),
            pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL),
            pgraph_reg_r(pg, NV_PGRAPH_SHADERPROG), attrs);
    /* First 16 bytes of attribute 0 (docs Update 652). */
    {
        VertexAttribute *a = &pg->vertex_attributes[0];
        DMAObject dma = nv_dma_load(d, a->dma_select ? pg->dma_vertex_b
                                                     : pg->dma_vertex_a);
        uint32_t va = (uint32_t)((dma.address & 0x07FFFFFF) + a->offset);
        if (a->count && va + 16 <= 0x08000000u) {
            const uint8_t *v = d->vram_ptr + va;
            fprintf(stderr, "[DRAW-V0] f=%d #%d %08X:", frame, s_trace_draw_no, va);
            for (int i = 0; i < 16; i++)
                fprintf(stderr, " %02X", v[i]);
            fputc('\n', stderr);
        }
    }
    /* Guest surface shape of this draw (docs Update 652). */
    fprintf(stderr, "[DRAW-SHAPE] f=%d #%d clip=%u,%u %ux%u log=%ux%u aa=%u "
            "zfmt=%u cfmt=%u rect0=%08X ctl1_0=%08X\n", frame, s_trace_draw_no,
            pg->surface_shape.clip_x, pg->surface_shape.clip_y,
            pg->surface_shape.clip_width, pg->surface_shape.clip_height,
            pg->surface_shape.log_width, pg->surface_shape.log_height,
            pg->surface_shape.anti_aliasing, pg->surface_shape.zeta_format,
            pg->surface_shape.color_format,
            pgraph_reg_r(pg, NV_PGRAPH_TEXIMAGERECT0),
            pgraph_reg_r(pg, NV_PGRAPH_TEXCTL1_0));
    /* Blend/raster and combiner state of the same draw (docs Update 622). */
    {
        char cmb[512];
        int m = 0;
        unsigned stages = pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL) & 0xFF;
        if (stages > 8)
            stages = 8;
        for (unsigned i = 0; i < stages && m < (int)sizeof(cmb) - 60; i++)
            m += snprintf(cmb + m, sizeof(cmb) - m, " s%u=%08X/%08X/%08X/%08X",
                          i, pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORI0 + i * 4),
                          pgraph_reg_r(pg, NV_PGRAPH_COMBINECOLORO0 + i * 4),
                          pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAI0 + i * 4),
                          pgraph_reg_r(pg, NV_PGRAPH_COMBINEALPHAO0 + i * 4));
        fprintf(stderr, "[DRAW+] f=%d #%d blend=%08X ctl0=%08X ctl3=%08X "
                "csv0d=%08X csv0c=%08X spec=%08X/%08X fmt=%08X/%08X/%08X/%08X "
                "tex=%08X/%08X/%08X fac=%08X/%08X%s\n",
                frame, s_trace_draw_no, pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0),
                pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                pgraph_reg_r(pg, NV_PGRAPH_CSV0_D),
                pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
                pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG0),
                pgraph_reg_r(pg, NV_PGRAPH_COMBINESPECFOG1),
                pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0),
                pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + 4),
                pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + 8),
                pgraph_reg_r(pg, NV_PGRAPH_TEXFMT0 + 12),
                pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + 4),
                pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + 8),
                pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + 12),
                pgraph_reg_r(pg, NV_PGRAPH_COMBINEFACTOR0),
                pgraph_reg_r(pg, NV_PGRAPH_COMBINEFACTOR0 + 0x20), cmb);
        /* Vertex program identity (docs Update 627): start slot and a hash
         * of its tokens; each program's tokens are logged once ([VSH]). */
        uint32_t vsh_hash = 0, vsh_len = 0;
        int vsh_start = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
                                 NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);
        if (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_D), NV_PGRAPH_CSV0_D_MODE) == 2) {
            vsh_hash = 2166136261u;
            for (int i = vsh_start; i < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH; i++) {
                const uint32_t *t = (const uint32_t *)&pg->program_data[i];
                for (int k = 0; k < VSH_TOKEN_SIZE; k++)
                    vsh_hash = (vsh_hash ^ t[k]) * 16777619u;
                vsh_len++;
                if (t[3] & 1) /* FLD_FINAL */
                    break;
            }
            static uint32_t seen[128];
            static int n_seen;
            int known = 0;
            for (int i = 0; i < n_seen; i++)
                known |= seen[i] == vsh_hash;
            if (!known && n_seen < 128) {
                seen[n_seen++] = vsh_hash;
                fprintf(stderr, "[VSH] %08X start=%d len=%u:", vsh_hash, vsh_start, vsh_len);
                for (unsigned i = 0; i < vsh_len; i++) {
                    const uint32_t *t = (const uint32_t *)&pg->program_data[vsh_start + i];
                    fprintf(stderr, " %08X%08X%08X%08X", t[0], t[1], t[2], t[3]);
                }
                fprintf(stderr, "\n");
            }
        }
        fprintf(stderr, "[DRAW#] f=%d #%d vsh=%08X/%u ", frame, s_trace_draw_no,
                vsh_hash, vsh_len);
        /* Transform constants c[104..107] (SCDA's position matrix) and the
         * viewport scale/offset c[58]/c[59] (docs Update 629). */
        {
            static const int cs[] = { 104, 105, 106, 107, 58, 59 };
            fprintf(stderr, "c=");
            for (int i = 0; i < 6; i++) {
                const uint32_t *v = pg->vsh_constants[cs[i]];
                fprintf(stderr, "%s%.9g,%.9g,%.9g,%.9g", i ? "|" : "",
                        *(const float *)&v[0], *(const float *)&v[1],
                        *(const float *)&v[2], *(const float *)&v[3]);
            }
            fprintf(stderr, " ");
        }
        /* Texture filter / LOD control per stage (docs Update 630). */
        fprintf(stderr, "filt=%08X/%08X/%08X/%08X texctl=%08X/%08X/%08X/%08X ",
                pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0),
                pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + 4),
                pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + 8),
                pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + 12),
                pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0),
                pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + 4),
                pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + 8),
                pgraph_reg_r(pg, NV_PGRAPH_TEXCTL0_0 + 12));
        fprintf(stderr, "raster=%08X zbias=%08X zfactor=%08X "
                "sffac=%08X/%08X ctl1=%08X ctl2=%08X\n",
                pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETBIAS),
                pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETFACTOR),
                pgraph_reg_r(pg, NV_PGRAPH_SPECFOGFACTOR0),
                pgraph_reg_r(pg, NV_PGRAPH_SPECFOGFACTOR1),
                pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2));
        /* RECOMP_DRAW_TRACE_CONSTS=<hex>+<hex>: for draws whose attribute-0
         * RAM address matches, log all 192 transform constants, texture
         * address modes, border colours and all combiner factors, to find
         * per-frame changes beyond the position matrix (docs Update 631). */
        static uint32_t tc_vtx[16];
        static int tc_n = -1;
        if (tc_n < 0) {
            tc_n = 0;
            const char *e = getenv("RECOMP_DRAW_TRACE_CONSTS");
            while (e && *e && tc_n < 16) {
                char *end;
                unsigned long v = strtoul(e, &end, 16);
                if (end == e)
                    break;
                tc_vtx[tc_n++] = (uint32_t)v;
                e = *end == '+' ? end + 1 : end;
            }
        }
        if (tc_n) {
            VertexAttribute *a = &pg->vertex_attributes[0];
            DMAObject dma = nv_dma_load(d, a->dma_select ? pg->dma_vertex_b
                                                         : pg->dma_vertex_a);
            uint32_t va = (uint32_t)((dma.address & 0x07FFFFFF) + a->offset);
            int hit = 0;
            for (int i = 0; i < tc_n && !hit; i++)
                hit = tc_vtx[i] == va;
            if (hit) {
                fprintf(stderr, "[DRAWC] f=%d #%d addr=%08X/%08X/%08X/%08X "
                        "border=%08X/%08X/%08X/%08X fac=", frame, s_trace_draw_no,
                        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0),
                        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + 4),
                        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + 8),
                        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + 12),
                        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0),
                        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR1),
                        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR2),
                        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR3));
                for (int i = 0; i < 8; i++)
                    fprintf(stderr, "%s%08X/%08X", i ? "," : "",
                            pgraph_reg_r(pg, NV_PGRAPH_COMBINEFACTOR0 + i * 4),
                            pgraph_reg_r(pg, NV_PGRAPH_COMBINEFACTOR1 + i * 4));
                fprintf(stderr, " c=");
                for (int i = 0; i < 192; i++) {
                    const uint32_t *v = pg->vsh_constants[i];
                    fprintf(stderr, "%s%.7g,%.7g,%.7g,%.7g", i ? "|" : "",
                            *(const float *)&v[0], *(const float *)&v[1],
                            *(const float *)&v[2], *(const float *)&v[3]);
                }
                fprintf(stderr, "\n");
            }
        }
    }
}

static DWORD WINAPI vblank_thread(LPVOID arg)
{
    NV2AState *d = arg;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    const int64_t period_ns = 16683333;   /* 59.94 Hz */
    int64_t next = nv2a_recomp_clock_ns() + period_ns;

    for (;;) {
        int64_t now = nv2a_recomp_clock_ns();
        if (next > now) {
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)((next - now) / 100);
                SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep((DWORD)((next - now) / 1000000));
            }
        }
        next += period_ns;
        if (nv2a_recomp_clock_ns() > next + 10 * period_ns)
            next = nv2a_recomp_clock_ns() + period_ns;  /* fell behind */

        bql_lock();
        d->pcrtc.pending_interrupts |= NV_PCRTC_INTR_0_VBLANK;
        d->pcrtc.raster = 0;
        nv2a_update_irq(d);
        bql_unlock();

        update_scanout_fallback(d);
        gpu_status_report(d);
        draw_trace_poll(d);
    }
    return 0;
}

/* ---- MMIO trap: the faulting instruction is emulated by the shared decoder
 * (src/kernel/mmio_decode.c) against the xemu block handlers ---- */
static uint64_t nv2a_mmio_rd_cb(void *opaque, uint32_t off, unsigned size)
{
    (void)opaque;
    return nv2a_recomp_mmio_read(off, size);
}

static void nv2a_mmio_wr_cb(void *opaque, uint32_t off, uint64_t v, unsigned size)
{
    (void)opaque;
    nv2a_recomp_mmio_write(off, v, size);
}

/* Direct register access from recompiled D3D code (IOR/IOW, recomp_io_read
 * in src/kernel/xbox_memory_layout.c, docs Update 643): what nv2a_mmio_veh
 * does for a decoded access, without the exception. */
uint32_t nv2a_recomp_direct_read(uint32_t off, unsigned size)
{
    uintptr_t saved_rip = s_mmio_fault_rip;
    s_mmio_fault_rip = (uintptr_t)_ReturnAddress();
    uint32_t r = (uint32_t)nv2a_recomp_mmio_read(off, size);
    s_mmio_fault_rip = saved_rip;
    xbox_irq_deliver_from_mmio();
    return r;
}

void nv2a_recomp_direct_write(uint32_t off, uint32_t val, unsigned size)
{
    uintptr_t saved_rip = s_mmio_fault_rip;
    s_mmio_fault_rip = (uintptr_t)_ReturnAddress();
    nv2a_recomp_mmio_write(off, val, size);
    s_mmio_fault_rip = saved_rip;
    xbox_irq_deliver_from_mmio();
}

static LONG CALLBACK nv2a_mmio_veh(PEXCEPTION_POINTERS ep)
{
    PEXCEPTION_RECORD er = ep->ExceptionRecord;
    uintptr_t fault;

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    fault = (uintptr_t)er->ExceptionInformation[1];
    if (fault < s_ap_lo || fault >= s_ap_hi)
        return EXCEPTION_CONTINUE_SEARCH;
    {
        /* RECOMP_NV2A_MMIO_SITES=1 (docs Update 643): trap rate plus the
         * hottest host instructions and registers, one block per 2^16
         * traps (exe offsets; resolve with cdb `ln`). */
        static int sites = -1;
        if (sites < 0) {
            const char *e = getenv("RECOMP_NV2A_MMIO_SITES");
            sites = e && e[0] == '1';
        }
        if (sites) {
            static uint64_t site_rip[48], site_n[48];
            static uint32_t site_off[48];
            static unsigned long long calls;
            static DWORD t0;
            uint64_t rip = (uint64_t)ep->ContextRecord->Rip;
            uint32_t off = (uint32_t)(fault - s_ap_lo);
            if (!t0)
                t0 = GetTickCount();
            for (int i = 0; i < 48; i++) {
                if ((site_rip[i] == rip && site_off[i] == off) || site_rip[i] == 0) {
                    site_rip[i] = rip;
                    site_off[i] = off;
                    site_n[i]++;
                    break;
                }
            }
            if ((++calls & 0xFFFF) == 0) {
                DWORD now = GetTickCount();
                uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
                fprintf(stderr, "[NV2A-MMIO-RATE] %llu traps, %.0f per second (tid %lu)\n",
                        calls, 65536.0 * 1000.0 / (double)(now - t0 ? now - t0 : 1),
                        GetCurrentThreadId());
                for (int i = 0; i < 48 && site_rip[i]; i++)
                    if (site_n[i] * 50 >= 65536)
                        fprintf(stderr, "[NV2A-MMIO-SITE] exe+0x%llX reg 0x%06X %.1f%%\n",
                                (unsigned long long)(site_rip[i] - base), site_off[i],
                                100.0 * (double)site_n[i] / 65536.0);
                memset(site_rip, 0, sizeof(site_rip));
                memset(site_n, 0, sizeof(site_n));
                t0 = now;
            }
        }
    }
    uintptr_t saved_rip = s_mmio_fault_rip;
    s_mmio_fault_rip = (uintptr_t)ep->ContextRecord->Rip;
    bool decoded = mmio_decode_and_run(ep->ContextRecord,
                                       (uint32_t)(fault - s_ap_lo),
                                       nv2a_mmio_rd_cb, nv2a_mmio_wr_cb, NULL);
    s_mmio_fault_rip = saved_rip;
    if (decoded) {
        /* A register access is where a spinning driver can be interrupted:
         * run a pending GPU (or other) ISR here, as hardware would. */
        xbox_irq_deliver_from_mmio();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    {
        static volatile LONG undecoded;
        if (InterlockedIncrement(&undecoded) <= 20) {
            const uint8_t *ip = (const uint8_t *)ep->ContextRecord->Rip;
            fprintf(stderr, "[GPU-VEH] undecoded %s at NV2A+0x%06llX, rip=%p "
                    "bytes %02X %02X %02X %02X %02X %02X %02X\n",
                    er->ExceptionInformation[0] == 1 ? "write" : "read",
                    (unsigned long long)(fault - s_ap_lo), (void *)ip,
                    ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6]);
            fflush(stderr);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- surface access tracking (docs Update 492) ----
 * xemu's TCG path registers every GPU surface with
 * mem_access_callback_insert and expects a call whenever the CPU touches it:
 * a read of a surface the GPU drew into must first download it, a write must
 * mark it for re-upload. Without this (the old stubs), RECOMP_NV2A_TCG=1 --
 * the fast mode -- showed stale videos and yellow patches, and the non-TCG
 * mode instead downloads every dirty surface after every semaphore release.
 *
 * Emulated with page protection on the CPU's views of the surface
 * (xbox_PhysCpuViews: contiguous window + tiled aperture); the GPU works on a
 * private view and never faults. Watch levels per surface:
 *   2  GPU copy newer than RAM  -> PAGE_NOACCESS (reads and writes trap)
 *   1  GPU copy equals RAM      -> PAGE_READONLY (writes trap)
 *   0  RAM newer (upload due)   -> PAGE_READWRITE
 * The renderer raises the level when the GPU takes ownership
 * (nv2a_recomp_access_watch after an upload, a download or a draw); a fault
 * runs the callback and lowers it (read -> 1, write -> 0). The callback runs
 * without any lock held: it may wait for the PFIFO thread, which itself
 * inserts and removes entries. RECOMP_NV2A_ACCESS_CB=0 restores the stubs. */
extern int xbox_PhysCpuViews(uint32_t phys, uintptr_t host[2]);
extern int xbox_HostToTrackedPhys(uintptr_t host, uint32_t *phys);

typedef void (*access_cb_fn)(void *opaque, MemoryRegion *mr, hwaddr addr,
                             hwaddr len, bool write);

struct MemAccessCallback {
    MemAccessCallback *next;
    hwaddr addr, len;          /* page-aligned outward */
    access_cb_fn fn;
    void *opaque;
    MemoryRegion *mr;
    int level;
    unsigned draw_gen;         /* bumped when the GPU draws (level 2) */
    int busy;                  /* callbacks in flight */
    bool dead;                 /* removed while busy: freed by the last one */
};

static SRWLOCK s_acc_lock = SRWLOCK_INIT;
static MemAccessCallback *s_acc_list;
static int s_acc_enabled = -1;
static volatile LONG s_acc_reads, s_acc_writes, s_acc_spurious;

static bool access_tracking_on(void)
{
    if (s_acc_enabled < 0) {
        const char *e = getenv("RECOMP_NV2A_ACCESS_CB");
        s_acc_enabled = !(e && e[0] == '0');
        if (s_acc_enabled)
            fprintf(stderr, "[GPU] surface access tracking by page protection "
                    "(RECOMP_NV2A_ACCESS_CB=0 disables)\n");
    }
    return s_acc_enabled;
}

static DWORD level_prot(int level)
{
    return level >= 2 ? PAGE_NOACCESS : level == 1 ? PAGE_READONLY
                                                   : PAGE_READWRITE;
}

/* Strongest level any live entry wants on the page (lock held). */
static int page_level(hwaddr page)
{
    int lv = 0;
    for (MemAccessCallback *e = s_acc_list; e; e = e->next)
        if (!e->dead && page < e->addr + e->len && e->addr < page + 0x1000 &&
            e->level > lv)
            lv = e->level;
    return lv;
}

static void protect_phys(hwaddr addr, hwaddr len, int level)
{
    uintptr_t host[2];
    DWORD old;
    int n = xbox_PhysCpuViews((uint32_t)addr, host);
    for (int v = 0; v < n; v++)
        VirtualProtect((void *)host[v], (SIZE_T)len, level_prot(level), &old);
}

/* Apply e's pages (lock held). Interior pages are e's alone; the first and
 * last page may be shared with a neighbouring surface. */
static void apply_entry(MemAccessCallback *e)
{
    hwaddr first = e->addr, last = e->addr + e->len - 0x1000;
    protect_phys(first, 0x1000, page_level(first));
    if (last > first) {
        if (last > first + 0x1000)
            protect_phys(first + 0x1000, last - first - 0x1000,
                         e->dead ? 0 : e->level);
        protect_phys(last, 0x1000, page_level(last));
    }
}

static LONG CALLBACK access_veh(PEXCEPTION_POINTERS ep)
{
    PEXCEPTION_RECORD er = ep->ExceptionRecord;
    enum { MAXHIT = 8 };
    MemAccessCallback *hit[MAXHIT];
    unsigned draw_gen[MAXHIT];
    int nhit = 0;
    uint32_t phys;
    bool write;

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        er->NumberParameters < 2 ||
        !xbox_HostToTrackedPhys((uintptr_t)er->ExceptionInformation[1], &phys))
        return EXCEPTION_CONTINUE_SEARCH;
    write = er->ExceptionInformation[0] == 1;
    hwaddr page = phys & ~(hwaddr)0xFFF;

    AcquireSRWLockExclusive(&s_acc_lock);
    for (MemAccessCallback *e = s_acc_list; e && nhit < MAXHIT; e = e->next) {
        if (e->dead || e->level == 0 || page >= e->addr + e->len ||
            e->addr >= page + 0x1000)
            continue;
        if (!write && e->level < 2)
            continue;           /* reads are allowed at level 1 */
        e->busy++;
        draw_gen[nhit] = e->draw_gen;
        hit[nhit++] = e;
    }
    if (!nhit) {
        /* Nothing wants this page protected any more (a level changed
         * between the access and here): re-apply and retry. */
        int lv = page_level(page);
        protect_phys(page, 0x1000, lv);
        ReleaseSRWLockExclusive(&s_acc_lock);
        InterlockedIncrement(&s_acc_spurious);
        if (lv >= 2 || (lv == 1 && write)) {
            static volatile LONG said;
            if (InterlockedIncrement(&said) <= 5)
                fprintf(stderr, "[GPU-ACCESS] unexpected %s fault at phys "
                        "%08X (level %d)\n", write ? "write" : "read", phys, lv);
            /* Should not happen; never loop on it. */
            protect_phys(page, 0x1000, 0);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    ReleaseSRWLockExclusive(&s_acc_lock);

    InterlockedIncrement(write ? &s_acc_writes : &s_acc_reads);
    {
        /* RECOMP_NV2A_ACCESS_TRACE=1 (docs Update 495): every callback with
         * the surface range, its level, the faulting host code (exe offset)
         * and how long the callback took (a download takes ~0.1 ms+). */
        static int trace = -1;
        static volatile LONG traced;
        if (trace < 0) {
            const char *t = getenv("RECOMP_NV2A_ACCESS_TRACE");
            trace = t && t[0] == '1';
        }
        for (int i = 0; i < nhit; i++) {
            LARGE_INTEGER t0, t1, f;
            if (trace)
                QueryPerformanceCounter(&t0);
            hit[i]->fn(hit[i]->opaque, hit[i]->mr, phys, 1, write);
            if (trace && InterlockedIncrement(&traced) <= 20000) {
                QueryPerformanceCounter(&t1);
                QueryPerformanceFrequency(&f);
                fprintf(stderr, "[GPU-ACCESS-EV] %s phys=%08X surf=%08llX+%llX "
                        "level=%d tid=%lu rip=exe+%llX frame=%d cb_us=%lld\n",
                        write ? "W" : "R", phys,
                        (unsigned long long)hit[i]->addr,
                        (unsigned long long)hit[i]->len, hit[i]->level,
                        GetCurrentThreadId(),
                        (unsigned long long)(ep->ContextRecord->Rip -
                            (uintptr_t)GetModuleHandleA(NULL)),
                        s_d->pgraph.frame_time,
                        (t1.QuadPart - t0.QuadPart) * 1000000LL / f.QuadPart);
            }
        }
    }

    AcquireSRWLockExclusive(&s_acc_lock);
    for (int i = 0; i < nhit; i++) {
        MemAccessCallback *e = hit[i];
        e->busy--;
        if (e->dead) {
            if (!e->busy)
                free(e);
            continue;
        }
        /* Unless the GPU drew into it meanwhile, RAM is now current (read)
         * or newer (write). A download or upload during the callback does
         * not change that. */
        if (e->draw_gen == draw_gen[i]) {
            int lv = write ? 0 : 1;
            if (e->level != lv) {
                e->level = lv;
                apply_entry(e);
            }
        }
    }
    /* The faulting page itself must now allow this access. */
    {
        int lv = page_level(page);
        if (lv >= 2 || (lv == 1 && write))
            protect_phys(page, 0x1000, write ? 0 : 1);
    }
    ReleaseSRWLockExclusive(&s_acc_lock);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void access_report(void)
{
    if (s_acc_enabled > 0) {
        int tracked = 0;
        AcquireSRWLockShared(&s_acc_lock);
        for (MemAccessCallback *e = s_acc_list; e; e = e->next)
            tracked++;
        ReleaseSRWLockShared(&s_acc_lock);
        fprintf(stderr, "[GPU-ACCESS] last 5 s: cpu reads=%ld writes=%ld "
                "spurious=%ld, tracked surfaces=%d\n",
                InterlockedExchange(&s_acc_reads, 0),
                InterlockedExchange(&s_acc_writes, 0),
                InterlockedExchange(&s_acc_spurious, 0), tracked);
    }
}

void mem_access_callback_insert(void *cpu, MemoryRegion *mr, hwaddr addr,
                                hwaddr len, MemAccessCallback **cb, void *fn,
                                void *opaque)
{
    static PVOID veh;
    uintptr_t host[2];
    MemAccessCallback *e;
    hwaddr lo = addr & ~(hwaddr)0xFFF;
    hwaddr hi = (addr + len + 0xFFF) & ~(hwaddr)0xFFF;

    (void)cpu;
    *cb = NULL;
    if (!len || !access_tracking_on() ||
        !xbox_PhysCpuViews((uint32_t)lo, host) ||
        !xbox_PhysCpuViews((uint32_t)(hi - 1), host))
        return;     /* outside the tracked region: untracked, as before */
    e = calloc(1, sizeof(*e));
    if (!e)
        return;
    e->addr = lo;
    e->len = hi - lo;
    e->fn = (access_cb_fn)fn;
    e->opaque = opaque;
    e->mr = mr;
    if (!veh)
        veh = AddVectoredExceptionHandler(1, access_veh);
    AcquireSRWLockExclusive(&s_acc_lock);
    e->next = s_acc_list;
    s_acc_list = e;
    ReleaseSRWLockExclusive(&s_acc_lock);
    *cb = e;
}

void mem_access_callback_remove_by_ref(void *cpu, MemAccessCallback *cb)
{
    (void)cpu;
    if (!cb)
        return;
    AcquireSRWLockExclusive(&s_acc_lock);
    for (MemAccessCallback **pp = &s_acc_list; *pp; pp = &(*pp)->next) {
        if (*pp == cb) {
            cb->dead = true;
            cb->level = 0;
            apply_entry(cb);
            *pp = cb->next;
            break;
        }
    }
    if (!cb->busy)
        free(cb);
    ReleaseSRWLockExclusive(&s_acc_lock);
}

void nv2a_recomp_access_watch(MemAccessCallback *cb, int level)
{
    if (!cb || cb->level == level)
        return;
    AcquireSRWLockExclusive(&s_acc_lock);
    if (!cb->dead && cb->level != level) {
        cb->level = level;
        if (level >= 2)
            cb->draw_gen++;
        apply_entry(cb);
    }
    ReleaseSRWLockExclusive(&s_acc_lock);
}

/* ---- bring-up ---- */
static void nv2a_recomp_reset(NV2AState *d)
{
    /* xemu nv2a_reset, minus the FIFO handshake (nothing runs yet). */
    d->pcrtc.start = 0;
    d->pramdac.core_clock_coeff = 0x00011C01;
    d->pramdac.core_clock_freq = 233333324;
    d->pramdac.memory_clock_coeff = 0;
    d->pramdac.video_clock_coeff = 0x0003C20D;
    d->pfifo.regs[NV_PFIFO_CACHE1_STATUS] |= NV_PFIFO_CACHE1_STATUS_LOW_MARK;
    d->vga.msr = VGA_MIS_COLOR;
    for (int i = 0; i < 256; i++) {
        d->puserdac.palette[i * 3] = (uint8_t)i;
        d->puserdac.palette[i * 3 + 1] = (uint8_t)i;
        d->puserdac.palette[i * 3 + 2] = (uint8_t)i;
    }
}

bool nv2a_recomp_attach(uint8_t *aperture_host, uint8_t *vram_host,
                        uint64_t vram_size)
{
    NV2AState *d;
    DWORD old;

    if (InterlockedExchange(&s_attached, 1))
        return true;
    InitializeCriticalSection(&s_bql);

    d = calloc(1, sizeof(*d));
    if (!d)
        return false;
    s_d = d;

    /* PCI identity (PBUS mirrors it): NVIDIA NV2A rev A1, VGA class. */
    pci_set_word(d->parent_obj.config + PCI_VENDOR_ID, 0x10DE);
    pci_set_word(d->parent_obj.config + PCI_DEVICE_ID, 0x02A0);
    pci_set_long(d->parent_obj.config + PCI_CLASS_REVISION, 0x030000A1);

    s_vram.ptr = vram_host;
    s_vram.size = vram_size;
    d->vram = &s_vram;
    d->vram_ptr = vram_host;
    d->ramin.ptr = calloc(1, NV2A_RAMIN_SIZE);
    d->ramin.size = NV2A_RAMIN_SIZE;
    d->ramin_ptr = d->ramin.ptr;

    d->vga.get_params = recomp_vga_get_params;
    d->vga.get_resolution = recomp_vga_get_resolution;
    d->vga.vram_ptr = vram_host;

    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);
    nv2a_recomp_reset(d);

    g_nv2a_recomp_tcg = getenv("RECOMP_NV2A_TCG") != NULL;
    if (g_nv2a_recomp_tcg)
        fprintf(stderr, "[GPU] RECOMP_NV2A_TCG: xemu TCG surface path\n");
    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_OPENGL;
    /* RECOMP_RENDERER=vulkan: xemu's Vulkan renderer (pgraph/vk). OpenGL
     * stays registered as pgraph.c's fallback if Vulkan cannot start.
     * RECOMP_VK_VALIDATION=1 enables the Khronos validation layer when it
     * is installed. */
    {
        const char *rn = getenv("RECOMP_RENDERER");
        if (rn && (!_stricmp(rn, "vulkan") || !_stricmp(rn, "vk"))) {
#ifdef XBOX_GPU_VULKAN
            const char *val = getenv("RECOMP_VK_VALIDATION");
            g_config.display.renderer = CONFIG_DISPLAY_RENDERER_VULKAN;
            g_config.display.vulkan.validation_layers = val && val[0] == '1';
            pgraph_vk_register_renderer();
            fprintf(stderr, "[GPU] RECOMP_RENDERER=vulkan: Vulkan renderer "
                    "requested (OpenGL fallback)\n");
#else
            fprintf(stderr, "[GPU] RECOMP_RENDERER=vulkan: not built with "
                    "XBOX_GPU_VULKAN, using OpenGL\n");
#endif
        }
    }
    /* RECOMP_SURFACE_SCALE=N (1-4): xemu's internal render scale -- every
     * GPU surface is drawn at N times its guest size and scaled back down
     * only when the guest reads it; the display texture, and with it the
     * framebuffer window, is N times as large (docs Update 473). */
    {
        const char *sc = getenv("RECOMP_SURFACE_SCALE");
        int scale = sc ? atoi(sc) : 1;
        g_config.display.quality.surface_scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
        if (g_config.display.quality.surface_scale > 1)
            fprintf(stderr, "[GPU] RECOMP_SURFACE_SCALE: rendering at %dx\n",
                    g_config.display.quality.surface_scale);
    }
    g_config.perf.cache_shaders = false;

    pgraph_gl_register_renderer();
    nv2a_context_init();            /* creates the GL contexts (WGL) */
    glo_set_current(NULL);          /* the PFIFO thread takes them over */
    pgraph_init(d);
    s_fb_ctx = glo_context_create();   /* the window's share-group context */
    glo_set_current(NULL);
    xbox_FramebufferWindowSetSource(nv2a_fb_host_source);
    xbox_FramebufferWindowSetPresenter(nv2a_fb_host_present);

    qemu_thread_create(&d->pfifo.thread, "nv2a.pfifo_thread", pfifo_thread,
                       d, QEMU_THREAD_JOINABLE);
    CloseHandle(CreateThread(NULL, 0, vblank_thread, d, 0, NULL));

    /* Route the aperture: from here on every register access traps. */
    s_ap_lo = (uintptr_t)aperture_host;
    s_ap_hi = s_ap_lo + NV2A_APERTURE;
    AddVectoredExceptionHandler(1, nv2a_mmio_veh);
    if (!VirtualProtect(aperture_host, NV2A_APERTURE, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "[GPU] could not trap the NV2A aperture (%lu)\n",
                GetLastError());
        return false;
    }
    xbox_io_direct_enable(2);
    fprintf(stderr, "[GPU] xemu NV2A attached: VRAM %llu MiB at %p, "
            "aperture trapped, IRQ %d\n",
            (unsigned long long)(vram_size >> 20), (void *)vram_host,
            NV2A_IRQ_LINE);
    return true;
}
