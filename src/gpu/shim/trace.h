/* Local stand-in for xemu's generated trace.h (see src/gpu/PROVENANCE.md).
 * Every trace point the imported NV2A files use, compiled out. Regenerate
 * with the grep in PROVENANCE.md when files are added. */
#pragma once
#include <stdint.h>

#define trace_enable_events(...) ((void)0)
#define trace_nv2a_dma_map(...) ((void)0)
#define trace_nv2a_irq(...) ((void)0)
#define trace_nv2a_pgraph_flip_increment_write(...) ((void)0)
#define trace_nv2a_pgraph_flip_stall(...) ((void)0)
#define trace_nv2a_pgraph_method(...) ((void)0)
#define trace_nv2a_pgraph_method_abbrev(...) ((void)0)
#define trace_nv2a_pgraph_method_unhandled(...) ((void)0)
#define trace_nv2a_pgraph_surface_compare_mismatch(...) ((void)0)
#define trace_nv2a_pgraph_surface_cpu_access(...) ((void)0)
#define trace_nv2a_pgraph_surface_download(...) ((void)0)
#define trace_nv2a_pgraph_surface_evict_overlapping(...) ((void)0)
#define trace_nv2a_pgraph_surface_evict_reason(...) ((void)0)
#define trace_nv2a_pgraph_surface_invalidated(...) ((void)0)
#define trace_nv2a_pgraph_surface_migrate_type(...) ((void)0)
#define trace_nv2a_pgraph_surface_render_to_texture(...) ((void)0)
#define trace_nv2a_pgraph_surface_target(...) ((void)0)
#define trace_nv2a_pgraph_surface_texture_compat_failed(...) ((void)0)
#define trace_nv2a_pgraph_surface_upload(...) ((void)0)
#define trace_nv2a_reg_read(...) ((void)0)
#define trace_nv2a_reg_write(...) ((void)0)

/* The Vulkan renderer (vk/surface.c) takes these six by address, so they
 * are real (empty) functions rather than macros. */
#define NV2A_TRACE_SURFACE_FN(name)                                              static inline void name(uint32_t addr, uint32_t width, uint32_t height,                              const char *layout, uint32_t anti_aliasing,                                  uint32_t clip_x, uint32_t clip_width,                                        uint32_t clip_y, uint32_t clip_height,                                       uint32_t pitch)                                      {                                                                                (void)addr; (void)width; (void)height; (void)layout;                         (void)anti_aliasing; (void)clip_x; (void)clip_width; (void)clip_y;           (void)clip_height; (void)pitch;                                          }
NV2A_TRACE_SURFACE_FN(trace_nv2a_pgraph_surface_create_color)
NV2A_TRACE_SURFACE_FN(trace_nv2a_pgraph_surface_create_zeta)
NV2A_TRACE_SURFACE_FN(trace_nv2a_pgraph_surface_hit_color)
NV2A_TRACE_SURFACE_FN(trace_nv2a_pgraph_surface_hit_zeta)
NV2A_TRACE_SURFACE_FN(trace_nv2a_pgraph_surface_match_color)
NV2A_TRACE_SURFACE_FN(trace_nv2a_pgraph_surface_match_zeta)
