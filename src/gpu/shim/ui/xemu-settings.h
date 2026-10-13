/* Local stand-in for a QEMU/xemu header (see src/gpu/PROVENANCE.md). */
#pragma once
#include "xemu-config.h"
#include <stdbool.h>
/* The fields the imported NV2A code reads; set in nv2a_recomp.c. */
struct config {
    struct {
        CONFIG_DISPLAY_RENDERER renderer;
        struct { int surface_scale; } quality;
        struct {
            bool validation_layers;
            bool debug_shaders;
            bool assert_on_validation_msg;
        } vulkan;
    } display;
    struct { bool cache_shaders; } perf;
};
extern struct config g_config;
const char *xemu_settings_get_base_path(void);
