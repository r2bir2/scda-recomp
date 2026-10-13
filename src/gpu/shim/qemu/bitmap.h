/* The two QEMU bitmap helpers pgraph.h uses (see src/gpu/PROVENANCE.md). */
#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define BITS_PER_LONG (sizeof(unsigned long) * CHAR_BIT)
#define BITS_TO_LONGS(nr) (((nr) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]

static inline void bitmap_set(unsigned long *map, long start, long nr)
{
    for (long i = start; i < start + nr; i++)
        map[i / BITS_PER_LONG] |= 1UL << (i % BITS_PER_LONG);
}

static inline void bitmap_zero(unsigned long *map, long nbits)
{
    memset(map, 0, BITS_TO_LONGS(nbits) * sizeof(unsigned long));
}

static inline bool test_bit(long nr, const unsigned long *addr)
{
    return (addr[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1;
}

/* Used by the Vulkan renderer's vertex-RAM upload tracking (vk/buffer.c,
 * vk/vertex.c, vk/draw.c). */
static inline unsigned long *bitmap_new(long nbits)
{
    return (unsigned long *)calloc(BITS_TO_LONGS(nbits), sizeof(unsigned long));
}

static inline void bitmap_clear(unsigned long *map, long start, long nr)
{
    for (long i = start; i < start + nr; i++)
        map[i / BITS_PER_LONG] &= ~(1UL << (i % BITS_PER_LONG));
}

/* Index of the first set bit in [offset, size), or size if there is none. */
static inline unsigned long find_next_bit(const unsigned long *addr,
                                          unsigned long size,
                                          unsigned long offset)
{
    while (offset < size) {
        unsigned long word = addr[offset / BITS_PER_LONG] >>
                             (offset % BITS_PER_LONG);
        if (word) {
            unsigned long bit = offset;
            while (!(word & 1UL)) {
                word >>= 1;
                bit++;
            }
            return bit < size ? bit : size;
        }
        offset = (offset / BITS_PER_LONG + 1) * BITS_PER_LONG;
    }
    return size;
}
