/* Local stand-in for a QEMU/xemu header (see src/gpu/PROVENANCE.md). */
#pragma once

/* Renderers built into xbox_gpu; pgraph.c's get_default_renderer (the
 * fallback when the configured renderer fails to start) checks these. */
#define CONFIG_OPENGL 1
#ifdef XBOX_GPU_VULKAN
#define CONFIG_VULKAN 1
#endif
