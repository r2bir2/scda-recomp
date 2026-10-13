/* Local stand-in for QEMU's "qemu/atomic.h" (see src/gpu/PROVENANCE.md).
 *
 * xemu uses qatomic_read/qatomic_set on bools and ints shared between the
 * PFIFO thread and the CPU-side register handlers. On x86-64 aligned loads
 * and stores of these sizes are atomic; volatile keeps the compiler from
 * caching them, and the full barrier matches QEMU's smp_mb(). */
#pragma once

#ifdef _MSC_VER
#include <intrin.h>
#define smp_mb() _ReadWriteBarrier(), MemoryBarrierShim()
static __forceinline void MemoryBarrierShim(void) { __faststorefence(); }
#define smp_rmb() _ReadWriteBarrier()
#define smp_wmb() _ReadWriteBarrier()
#else
#define smp_mb() __sync_synchronize()
#define smp_rmb() __asm__ __volatile__("" ::: "memory")
#define smp_wmb() __asm__ __volatile__("" ::: "memory")
#endif

#define qatomic_read(ptr) (*(volatile __typeof__(*(ptr)) *)(ptr))
#define qatomic_set(ptr, v) (*(volatile __typeof__(*(ptr)) *)(ptr) = (v))
#define qatomic_mb_read(ptr) qatomic_read(ptr)
#define qatomic_mb_set(ptr, v) do { qatomic_set(ptr, v); smp_mb(); } while (0)
