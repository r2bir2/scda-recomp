/*
 * MMX_PADDUSW / MMX_PSUBUSW regression tests -- standalone, no game/runtime
 * dependency.
 *
 * These two runtime macros (recomp_types.h) were entirely missing until
 * Update 259: the lifter's `_MMX_BINARY` table only had the byte-sized
 * saturating add/sub (paddusb/psubusb), not the word-sized ones the SCDA
 * Bink YUV->RGB decoder actually uses. The gap was invisible to every
 * existing test because nothing checked MMX_* helper coverage the way
 * test_runtime_helpers_defined.py already does for XMM_* helpers, and
 * nothing exercised the macros' actual saturation arithmetic. This file
 * covers the arithmetic; test_runtime_helpers_defined.py (extended
 * alongside this) covers the lifter/runtime name-coverage gap.
 *
 * Build (from a Developer/vcvars64 command prompt or equivalent PATH/INCLUDE/
 * LIB):
 *   cl /nologo /W4 /I ../../templates/runtime test_main.c /Fe:test_mmx_ops.exe
 *   test_mmx_ops.exe
 */
/* The MMX register file (RecompMmx, MMX_* macros) lives inside recomp_types.h's
 * `#ifdef RECOMP_GENERATED_CODE` block, exactly like every real generated .c
 * file defines it before including recomp_funcs.h -- see recomp_0027.c. */
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdio.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(name, cond) \
    do { g_checks++; if (!(cond)) { printf("FAIL: %s\n", name); g_failures++; } } while (0)

static RecompMmx mk4(uint16_t w0, uint16_t w1, uint16_t w2, uint16_t w3)
{
    RecompMmx r;
    r.q = 0;
    r.uw[0] = w0; r.uw[1] = w1; r.uw[2] = w2; r.uw[3] = w3;
    return r;
}

#define CHECK_LANES(name, got, e0, e1, e2, e3) \
    do { \
        RecompMmx _g = (got); \
        CHECK(name ": lane0", _g.uw[0] == (e0)); \
        CHECK(name ": lane1", _g.uw[1] == (e1)); \
        CHECK(name ": lane2", _g.uw[2] == (e2)); \
        CHECK(name ": lane3", _g.uw[3] == (e3)); \
    } while (0)

static void test_paddusw_normal(void)
{
    /* Plain in-range adds: no saturation should trigger. */
    RecompMmx a = mk4(100, 0, 1, 0x1234);
    RecompMmx b = mk4(200, 0, 0, 0x1111);
    CHECK_LANES("paddusw normal", MMX_PADDUSW(a, b), 300, 0, 1, 0x2345);
}

static void test_paddusw_saturates_at_0xffff(void)
{
    /* 0xFFFF + 1 must clamp to 0xFFFF, not wrap to 0. */
    RecompMmx a = mk4(0xFFFF, 0x8000, 0xFFFF, 0);
    RecompMmx b = mk4(1, 0x8000, 0xFFFF, 0);
    /* lane1: 0x8000+0x8000 = 0x10000 unsaturated -- must clamp, not wrap. */
    CHECK_LANES("paddusw saturate high", MMX_PADDUSW(a, b),
                0xFFFF, 0xFFFF, 0xFFFF, 0);
}

static void test_paddusw_boundary_exact(void)
{
    /* Exactly at the boundary: 0xFFFF + 0 stays 0xFFFF (no false clamp
     * path taken when the true sum already equals the saturation limit). */
    RecompMmx a = mk4(0xFFFF, 0, 0xFFFE, 0xFFFF);
    RecompMmx b = mk4(0, 0, 1, 1);
    CHECK_LANES("paddusw boundary", MMX_PADDUSW(a, b), 0xFFFF, 0, 0xFFFF, 0xFFFF);
}

static void test_psubusw_normal(void)
{
    RecompMmx a = mk4(300, 5, 0x2345, 10);
    RecompMmx b = mk4(100, 5, 0x1111, 3);
    CHECK_LANES("psubusw normal", MMX_PSUBUSW(a, b), 200, 0, 0x1234, 7);
}

static void test_psubusw_saturates_at_zero(void)
{
    /* A negative true result must clamp to 0, not wrap to a huge uint16. */
    RecompMmx a = mk4(0, 100, 0, 0xFFFF);
    RecompMmx b = mk4(1, 200, 0xFFFF, 0xFFFF);
    CHECK_LANES("psubusw saturate low", MMX_PSUBUSW(a, b), 0, 0, 0, 0);
}

static void test_psubusw_boundary_exact(void)
{
    /* Exactly zero and exactly non-negative-by-one: both must NOT clamp
     * away from their true (in-range) result. */
    RecompMmx a = mk4(0, 1, 0xFFFF, 5);
    RecompMmx b = mk4(0, 1, 0xFFFF, 5);
    CHECK_LANES("psubusw boundary", MMX_PSUBUSW(a, b), 0, 0, 0, 0);
}

static void test_paddsw_then_psubusw_clamp_idiom(void)
{
    /* This is the exact idiom sub_004C6140 uses: add a bias with signed
     * saturation, then subtract it back with unsigned saturation, which
     * also floors the result at 0. Update 259's bug was that the second
     * half never ran (translator TODO), so a value that should have
     * floored to 0 stayed biased upward instead (the observed
     * 0x7F7F7F12-instead-of-black symptom).
     *
     * lane0 (500): 500 -> +2000 = 2500 -> -2000 = 500. Round-trips exactly,
     * no saturation involved -- the idiom's ordinary happy path.
     *
     * lane1 (0xFFFF, i.e. -1 as the signed int16 PADDSW reads it): -1 ->
     * +2000 = 1999 -> -2000 = -1 true math, which PSUBUSW must clamp to 0.
     * Without the fix (subtraction skipped), this lane would have stayed
     * at 1999 -- a nonzero leftover exactly like the bug's symptom. */
    RecompMmx lowValue = mk4(500, 0xFFFF, 0, 0);
    RecompMmx bias = mk4(2000, 2000, 2000, 2000);
    RecompMmx biased = MMX_PADDSW(lowValue, bias);
    RecompMmx clamped = MMX_PSUBUSW(biased, bias);
    CHECK("clamp idiom lane0 round-trips", clamped.uw[0] == 500);
    CHECK("clamp idiom lane1 floors to 0", clamped.uw[1] == 0);
}

int main(void)
{
    test_paddusw_normal();
    test_paddusw_saturates_at_0xffff();
    test_paddusw_boundary_exact();
    test_psubusw_normal();
    test_psubusw_saturates_at_zero();
    test_psubusw_boundary_exact();
    test_paddsw_then_psubusw_clamp_idiom();

    printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
