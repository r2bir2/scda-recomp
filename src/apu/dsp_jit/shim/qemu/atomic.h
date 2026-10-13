/* Stand-in for QEMU's "qemu/atomic.h" (src/apu/dsp_jit): only qatomic_add
 * on the 64-bit statistics counters is used. */
#pragma once
#include <intrin.h>
#define qatomic_add(p, n) \
    ((void)_InterlockedExchangeAdd64((volatile __int64 *)(p), (__int64)(n)))
