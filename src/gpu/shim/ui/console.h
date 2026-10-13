/* Local stand-in for a QEMU/xemu header (see src/gpu/PROVENANCE.md). */
#pragma once
typedef struct GraphicHwOps { void (*gfx_update)(void *opaque); } GraphicHwOps;
