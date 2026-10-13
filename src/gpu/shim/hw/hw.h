/* Local stand-in for the QEMU memory/device API the imported xemu NV2A files
 * use (see src/gpu/PROVENANCE.md). Included through "hw/hw.h" and from the
 * other hw/ shims.
 *
 * There is no QEMU memory map here. The NV2A sees Xbox physical RAM as one
 * flat block (the tiled aperture view, src/kernel/xbox_memory_layout.c) and
 * RAMIN as the NV2A aperture's PRAMIN window; both are plain host pointers.
 *
 * Dirty tracking (QEMU's DIRTY_MEMORY_* bitmaps) does not exist either, so
 * memory_region_test_and_clear_dirty is answered per client in
 * nv2a_recomp.c -- see nv2a_recomp_region_dirty for the policy. */
#pragma once

#include "qemu/osdep.h"

typedef struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, hwaddr addr, unsigned int size);
    void (*write)(void *opaque, hwaddr addr, uint64_t val, unsigned int size);
} MemoryRegionOps;

typedef struct MemoryRegion {
    uint8_t *ptr;
    uint64_t size;
} MemoryRegion;

enum {
    DIRTY_MEMORY_VGA = 0,
    DIRTY_MEMORY_NV2A = 3,
    DIRTY_MEMORY_NV2A_TEX = 4,
};

static inline uint64_t memory_region_size(MemoryRegion *mr) { return mr->size; }
static inline uint8_t *memory_region_get_ram_ptr(MemoryRegion *mr) { return mr->ptr; }

bool nv2a_recomp_region_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size,
                              unsigned client);
bool nv2a_recomp_surface_cpu_dirty(hwaddr addr, hwaddr size);
void nv2a_recomp_region_set_client_dirty(MemoryRegion *mr, hwaddr addr,
                                         hwaddr size, unsigned client);
/* Vulkan vertex RAM mirror: copies RAM into the host-side copy of the mirror
 * and returns that copy, which the caller then uploads (vk/vertex.c). */
void *nv2a_recomp_vertex_snapshot(hwaddr addr, void *data, size_t size);

static inline bool memory_region_test_and_clear_dirty(MemoryRegion *mr,
                                                      hwaddr addr, hwaddr size,
                                                      unsigned client)
{
    return nv2a_recomp_region_dirty(mr, addr, size, client);
}

static inline void memory_region_set_client_dirty(MemoryRegion *mr, hwaddr addr,
                                                  hwaddr size, unsigned client)
{
    nv2a_recomp_region_set_client_dirty(mr, addr, size, client);
}

/* The recompiled CPU is native code, i.e. QEMU's non-TCG (KVM/WHPX) case:
 * surfaces are not guarded by per-access CPU callbacks. RECOMP_NV2A_TCG=1
 * selects xemu's usual TCG surface path instead (A/B diagnosis, docs Update
 * 357): no flush-on-unbind, no forced display upload, no CPU-dirty check --
 * but also no CPU access callbacks (the stubs below do nothing). */
extern bool g_nv2a_recomp_tcg;
static inline bool tcg_enabled(void) { return g_nv2a_recomp_tcg; }

/* Per-access CPU callbacks (xemu's TCG path), emulated with page protection
 * in nv2a_recomp.c (docs Update 492): the CPU's views of a surface's pages
 * are protected while the GPU copy is authoritative; a fault calls the
 * renderer's callback, then the access continues. The renderer re-arms the
 * watch when the GPU takes ownership again (nv2a_recomp_access_watch). */
typedef struct MemAccessCallback MemAccessCallback;
static inline void *qemu_get_cpu(int index) { (void)index; return NULL; }
void mem_access_callback_insert(void *cpu, MemoryRegion *mr, hwaddr addr,
                                hwaddr len, MemAccessCallback **cb, void *fn,
                                void *opaque);
void mem_access_callback_remove_by_ref(void *cpu, MemAccessCallback *cb);
/* level: 0 = no watch (guest RAM is newer), 1 = writes (GPU copy == RAM),
 * 2 = reads and writes (GPU copy is newer than RAM). */
void nv2a_recomp_access_watch(MemAccessCallback *cb, int level);

/* F8 draw trace (nv2a_recomp.c, docs Update 500): nonzero while active;
 * pgraph.c logs every draw then. */
struct NV2AState;
extern volatile int g_nv2a_draw_trace;
void nv2a_recomp_trace_draw(struct NV2AState *d);
/* RECOMP_DRAW_SKIP (diagnostic, docs Update 622): nonzero = leave out the
 * draw that is beginning now (numbered per frame like the draw trace). */
int nv2a_recomp_draw_skip(struct NV2AState *d);
/* RECOMP_ZDUMP (diagnostic, docs Update 627), after each traced draw. */
void nv2a_recomp_after_draw(struct NV2AState *d);
int nv2a_recomp_mesh_zbias(struct NV2AState *d, float *bias_const, float *bias_slope);

typedef struct QEMUTimer QEMUTimer;
typedef void *qemu_irq;

#define QEMU_CLOCK_REALTIME 0
#define QEMU_CLOCK_VIRTUAL 1
int64_t nv2a_recomp_clock_ns(void);
static inline int64_t qemu_clock_get_ns(int clock) { (void)clock; return nv2a_recomp_clock_ns(); }
static inline int64_t qemu_clock_get_us(int clock) { (void)clock; return nv2a_recomp_clock_ns() / 1000; }
static inline int64_t qemu_clock_get_ms(int clock) { (void)clock; return nv2a_recomp_clock_ns() / 1000000; }
#define NANOSECONDS_PER_SECOND 1000000000LL
