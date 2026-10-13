/*
 * Minimal stand-in for QEMU's "qemu/bswap.h", local to xboxrecomp.
 *
 * The DSP interpreter only uses the unaligned little-endian 32-bit load and
 * store (P-memory opcode cache). The host is little-endian x86-64, so these
 * are plain memcpy accesses.
 */
#pragma once

#include <stdint.h>
#include <string.h>

static inline uint32_t ldl_le_p(const void *ptr)
{
    uint32_t v;
    memcpy(&v, ptr, sizeof(v));
    return v;
}

static inline void stl_le_p(void *ptr, uint32_t v)
{
    memcpy(ptr, &v, sizeof(v));
}
