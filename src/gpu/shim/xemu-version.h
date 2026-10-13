/* Local stand-in for a QEMU/xemu header (see src/gpu/PROVENANCE.md). */
#pragma once
extern const char xemu_version[];
extern const int xemu_version_len;
/* Reported to the Vulkan driver as the application version (vk/instance.c). */
extern const int xemu_version_major, xemu_version_minor, xemu_version_patch;
