/*
 * Minimal stand-in for QEMU's "qemu/osdep.h", local to xboxrecomp.
 *
 * Provides what the imported xemu NV2A/PGRAPH files (src/gpu/xemu, see
 * src/gpu/PROVENANCE.md) use from QEMU's base layer: C headers, hwaddr,
 * MIN/MAX/ARRAY_SIZE, bit helpers, byte-order loads, atomics and the few GLib
 * allocation/string helpers. Nothing of QEMU itself is pulled in.
 */
#pragma once

#include "config-host.h"   /* as QEMU's osdep.h does */

/* QEMU refuses to build with NDEBUG; xemu's asserts are runtime checks. */
#undef NDEBUG
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(_SSIZE_T_DEFINED)
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif

#include "qemu/compiler.h"

typedef uint64_t hwaddr;
#define HWADDR_PRIx PRIx64
#define HWADDR_PRId PRId64

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif
#ifndef ROUND_UP
#define ROUND_UP(n, d) (((n) + (d) - 1) & -(0 ? (n) : (d)))
#endif
#ifndef QEMU_ALIGN_UP
#define QEMU_ALIGN_UP(n, m) (DIV_ROUND_UP(n, m) * (m))
#endif
#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

/* ---- bit helpers (qemu/host-utils.h, qemu/bitops.h) ---- */
static inline int ctz32(uint32_t v)
{
    unsigned long i;
    if (!v) return 32;
#ifdef _MSC_VER
    _BitScanForward(&i, v);
    return (int)i;
#else
    return __builtin_ctz(v);
#endif
}
static inline int clz32(uint32_t v)
{
    unsigned long i;
    if (!v) return 32;
#ifdef _MSC_VER
    _BitScanReverse(&i, v);
    return 31 - (int)i;
#else
    return __builtin_clz(v);
#endif
}
static inline int ctpop32(uint32_t v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (int)((((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24);
}
static inline bool is_power_of_2(uint64_t v) { return v && !(v & (v - 1)); }

/* ---- byte-order loads/stores (qemu/bswap.h); x86 is little endian ---- */
static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t lduw_le_p(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void stl_le_p(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void stw_le_p(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void stq_le_p(void *p, uint64_t v) { memcpy(p, &v, 8); }
static inline uint64_t ldq_le_p(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t bswap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

/* ---- atomics (qemu/atomic.h) ---- */
#include "qemu/atomic.h"

/* ---- GLib subset ---- */
#include "glib_shim.h"

/* (a * b) / c without intermediate overflow (qemu/host-utils.h).
 * c == 0 happens only while PTIMER's numerator is still unprogrammed -- on
 * hardware the kernel sets it at boot; here the HLE kernel may not. QEMU
 * would trap; answer 0 and say so once instead (nv2a_recomp.c). */
void nv2a_recomp_note_muldiv_zero(void);
static inline uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c)
{
    uint64_t hi, lo, rem;
    if (c == 0) {
        nv2a_recomp_note_muldiv_zero();
        return 0;
    }
#ifdef _MSC_VER
    lo = _umul128(a, b, &hi);
    if (hi >= c) return UINT64_MAX;
    return _udiv128(hi, lo, c, &rem);
#else
    (void)hi; (void)lo; (void)rem;
    return (uint64_t)(((unsigned __int128)a * b) / c);
#endif
}

/* ---- file helpers used by the (disabled) on-disk shader cache ---- */
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define qemu_mkdir(path) _mkdir(path)
#define qemu_unlink(path) _unlink(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define qemu_mkdir(path) mkdir(path, 0755)
#define qemu_unlink(path) unlink(path)
#endif
#define qemu_fopen(path, mode) fopen(path, mode)

/* ---- misc QEMU base ---- */
typedef struct Error Error;
extern Error *error_fatal;
static inline const char *error_get_pretty(Error *err) { (void)err; return "renderer init failed"; }
static inline void error_free(Error *err) { (void)err; }
/* error_setg marks *errp so callers that test it (the Vulkan renderer's
 * init, then pgraph.c's fallback to OpenGL) see the failure; the text goes
 * to stderr. error_fatal is never written. */
extern char nv2a_recomp_error_mark;
#define error_setg(errp, ...) do {                                        \
        fprintf(stderr, "nv2a error: " __VA_ARGS__); fputc('\n', stderr); \
        if ((errp) && (errp) != &error_fatal)                             \
            *(errp) = (Error *)&nv2a_recomp_error_mark;                   \
    } while (0)
#define error_prepend(errp, ...) do {                                     \
        fprintf(stderr, "nv2a error context: " __VA_ARGS__);              \
        fputc('\n', stderr);                                              \
    } while (0)
#define error_report(...) do { fprintf(stderr, "nv2a: " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define warn_report(...) do { fprintf(stderr, "nv2a warning: " __VA_ARGS__); fputc('\n', stderr); } while (0)

static inline void rcu_register_thread(void) {}
static inline void rcu_unregister_thread(void) {}
