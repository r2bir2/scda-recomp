/* Local stand-in for QEMU's "qemu/compiler.h" (see src/gpu/PROVENANCE.md). */
#pragma once

#ifdef _MSC_VER
#include <intrin.h>
#define QEMU_PACKED
#define QEMU_NORETURN __declspec(noreturn)
#define G_NORETURN __declspec(noreturn)
#define QEMU_ALWAYS_INLINE __forceinline
#define likely(x) (x)
#define unlikely(x) (x)
#define QEMU_FALLTHROUGH
#define G_GNUC_PRINTF(a, b)
#define G_GNUC_UNUSED
#define __attribute__(x)
#else
#define QEMU_PACKED __attribute__((packed))
#define QEMU_NORETURN __attribute__((noreturn))
#define G_NORETURN __attribute__((noreturn))
#define QEMU_ALWAYS_INLINE inline __attribute__((always_inline))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define QEMU_FALLTHROUGH __attribute__((fallthrough))
#define G_GNUC_PRINTF(a, b) __attribute__((format(printf, a, b)))
#define G_GNUC_UNUSED __attribute__((unused))
#endif

#define tostring(s) #s
#define stringify(s) tostring(s)

#define QEMU_BUILD_BUG_ON(x) typedef char qemu_build_bug_on_##__LINE__[(x) ? -1 : 1]
