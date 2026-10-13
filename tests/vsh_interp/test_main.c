/*
 * vsh_interp regression tests -- standalone, no game/runtime dependency.
 *
 * Builds instruction words with enc_row(), the exact field-by-field inverse
 * of vsh_interp.c's own vshf() reads (not a re-derivation from a spec), so a
 * field-layout regression in either the encoder here or the decoder under
 * test would have to break in a matching way to go unnoticed. Each test
 * documents the hand-computed expected value it checks against.
 *
 * Build (from a Developer/vcvars64 command prompt or equivalent PATH/INCLUDE/
 * LIB):
 *   cl /nologo /W4 /I ../../src/kernel test_main.c ../../src/kernel/vsh_interp.c /Fe:test_vsh_interp.exe
 *   test_vsh_interp.exe
 */
#include "vsh_interp.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(name, cond) \
    do { g_checks++; if (!(cond)) { printf("FAIL: %s\n", name); g_failures++; } } while (0)

#define CHECK_NEAR(name, got, want, eps) \
    do { g_checks++; float _g=(got), _w=(want); \
         if (fabsf(_g-_w) > (eps)) { \
             printf("FAIL: %s (got %.6f, want %.6f)\n", name, _g, _w); g_failures++; } \
    } while (0)

/* mux values, matching vsh_read_slot: 0=zero, 1=PARAM_R, 2=PARAM_V, 3=PARAM_C */
enum { MUX_ZERO = 0, MUX_R = 1, MUX_V = 2, MUX_C = 3 };

typedef struct {
    int mac, ilu;
    int a_mux, a_reg, a_neg; int a_swz[4];
    int b_mux, b_reg, b_neg; int b_swz[4];
    int c_mux, c_reg, c_neg; int c_swz[4];
    int c_bits;      /* raw FLD_CONST 8-bit value, used when c_mux == MUX_C */
    int a0x;
    int out_r, mac_mask, ilu_mask;
    int out_mux, o_mask, out_orb, out_addr;
    int final;
} RowSpec;

static void row_defaults(RowSpec *s)
{
    memset(s, 0, sizeof *s);
    s->a_swz[0] = 0; s->a_swz[1] = 1; s->a_swz[2] = 2; s->a_swz[3] = 3; /* xyzw */
    s->b_swz[0] = 0; s->b_swz[1] = 1; s->b_swz[2] = 2; s->b_swz[3] = 3;
    s->c_swz[0] = 0; s->c_swz[1] = 1; s->c_swz[2] = 2; s->c_swz[3] = 3;
}

/* Exact field-by-field inverse of vsh_interp.c's decode (vshf() call sites),
 * so this is a mechanical transcription, not a second guess at the layout. */
static void enc_row(const RowSpec *s, uint32_t w[4])
{
    memset(w, 0, sizeof(uint32_t) * 4);

    w[1] |= ((uint32_t)s->mac & 0xF) << 21;
    w[1] |= ((uint32_t)s->ilu & 0x7) << 25;
    w[3] |= (s->final ? 1u : 0u) << 0;

    /* A operand (which=0): neg dword1 bit8, swz dword1 bits{6,4,2,0},
     * mux dword2 bits26-27, reg (PARAM_R) dword2 bits28-31. */
    w[1] |= (s->a_neg ? 1u : 0u) << 8;
    w[1] |= ((uint32_t)s->a_swz[0] & 3) << 6 | ((uint32_t)s->a_swz[1] & 3) << 4
          | ((uint32_t)s->a_swz[2] & 3) << 2 | ((uint32_t)s->a_swz[3] & 3) << 0;
    w[2] |= ((uint32_t)s->a_mux & 3) << 26;
    if (s->a_mux == MUX_R) w[2] |= ((uint32_t)s->a_reg & 0xF) << 28;

    /* B operand (which=1): neg dword2 bit25, swz dword2 bits{23,21,19,17},
     * mux dword2 bits11-12, reg (PARAM_R) dword2 bits13-16. */
    w[2] |= (s->b_neg ? 1u : 0u) << 25;
    w[2] |= ((uint32_t)s->b_swz[0] & 3) << 23 | ((uint32_t)s->b_swz[1] & 3) << 21
          | ((uint32_t)s->b_swz[2] & 3) << 19 | ((uint32_t)s->b_swz[3] & 3) << 17;
    w[2] |= ((uint32_t)s->b_mux & 3) << 11;
    if (s->b_mux == MUX_R) w[2] |= ((uint32_t)s->b_reg & 0xF) << 13;

    /* C operand (which=2): neg dword2 bit10, swz dword2 bits{8,6,4,2},
     * mux dword3 bits28-29, reg (PARAM_R) split dword2 bits0-1 (high) /
     * dword3 bits30-31 (low). PARAM_C's FLD_CONST (dword1 bits13-20) and
     * FLD_A0X (dword3 bit1) are shared fields, not per-slot. */
    w[2] |= (s->c_neg ? 1u : 0u) << 10;
    w[2] |= ((uint32_t)s->c_swz[0] & 3) << 8 | ((uint32_t)s->c_swz[1] & 3) << 6
          | ((uint32_t)s->c_swz[2] & 3) << 4 | ((uint32_t)s->c_swz[3] & 3) << 2;
    w[3] |= ((uint32_t)s->c_mux & 3) << 28;
    if (s->c_mux == MUX_R) {
        w[2] |= (((uint32_t)s->c_reg >> 2) & 3) << 0;
        w[3] |= ((uint32_t)s->c_reg & 3) << 30;
    }
    w[3] |= (s->a0x ? 1u : 0u) << 1;

    /* PARAM_V's FLD_V (dword1 bits9-12) and PARAM_C's FLD_CONST (dword1
     * bits13-20) are each a SINGLE shared field on real hardware -- if two
     * operand slots both select PARAM_V (or both PARAM_C) in one
     * instruction they necessarily read the same v-attribute (or the same
     * constant); that is a hardware constraint this encoder mirrors, not a
     * bug, so tests needing two distinct sources use one V and one C. */
    if (s->a_mux == MUX_V || s->b_mux == MUX_V || s->c_mux == MUX_V) {
        int vi = (s->a_mux == MUX_V) ? s->a_reg : (s->b_mux == MUX_V) ? s->b_reg : s->c_reg;
        w[1] |= ((uint32_t)vi & 0xF) << 9;
    }
    if (s->a_mux == MUX_C || s->b_mux == MUX_C || s->c_mux == MUX_C)
        w[1] |= ((uint32_t)s->c_bits & 0xFF) << 13;

    /* Write-back fields, all dword3. */
    w[3] |= ((uint32_t)s->out_mux & 1) << 2;
    w[3] |= ((uint32_t)s->out_addr & 0xFF) << 3;
    w[3] |= ((uint32_t)s->out_orb & 1) << 11;
    w[3] |= ((uint32_t)s->o_mask & 0xF) << 12;
    w[3] |= ((uint32_t)s->ilu_mask & 0xF) << 16;
    w[3] |= ((uint32_t)s->out_r & 0xF) << 20;
    w[3] |= ((uint32_t)s->mac_mask & 0xF) << 24;
}

static Vec4 vec4(float x, float y, float z, float w) { Vec4 v = { { x, y, z, w } }; return v; }

static void make_attrs(Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES])
{
    int i;
    for (i = 0; i < NV2A_VSH_NUM_ATTRIBUTES; i++) vattr[i] = vec4(0, 0, 0, 1);
}

static void make_consts(uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4])
{
    memset(consts, 0, sizeof(uint32_t) * NV2A_VERTEXSHADER_CONSTANTS * 4);
}

static void set_const(uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4], int idx, Vec4 v)
{
    memcpy(consts[idx], v.x, sizeof(float) * 4);
}

/* ---- 1. R12/oPos alias: a write to R12 must land in oPos, and a
 * subsequent read of R12 must see it -- not a 13th, independent temp. ---- */
static void test_r12_is_opos_alias(void)
{
    uint32_t program[4][4];
    RowSpec s;
    VshResult res;
    Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES];
    uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4];

    make_attrs(vattr);
    make_consts(consts);

    /* Row 0: MOV R12, v0  (writes oPos directly via the R-register path,
     * out_r=12, no shared O-write). v0 = (2,3,4,1). */
    row_defaults(&s);
    s.mac = VSH_MAC_MOV; s.a_mux = MUX_V; s.a_reg = 0;
    s.out_r = 12; s.mac_mask = 0xF;
    enc_row(&s, program[0]);

    /* Row 1: MOV R1, R12  (reads back R12 -- must see oPos == v0, not a
     * separate untouched register). Final. */
    row_defaults(&s);
    s.mac = VSH_MAC_MOV; s.a_mux = MUX_R; s.a_reg = 12;
    s.out_r = 1; s.mac_mask = 0xF;
    s.final = 1;
    enc_row(&s, program[1]);

    vattr[0] = vec4(2.0f, 3.0f, 4.0f, 1.0f);
    vsh_run_program(program, 2, 0, consts, vattr, &res, NULL);

    CHECK("r12_alias: status ended normally", res.status == 0);
    CHECK_NEAR("r12_alias: oPos.x written via R12", res.oreg[VSH_OREG_POS].x[0], 2.0f, 1e-6f);
    CHECK_NEAR("r12_alias: oPos.y written via R12", res.oreg[VSH_OREG_POS].x[1], 3.0f, 1e-6f);
    CHECK_NEAR("r12_alias: oPos.z written via R12", res.oreg[VSH_OREG_POS].x[2], 4.0f, 1e-6f);

    /* R1 must have received the SAME value oPos now holds -- proving the
     * read of R12 in row 1 saw row 0's write, not a stale/independent 0. */
    {
        Vec4 vattr2[NV2A_VSH_NUM_ATTRIBUTES];
        VshResult res2;
        uint32_t p2[3][4];

        make_attrs(vattr2);
        row_defaults(&s);
        s.mac = VSH_MAC_MOV; s.a_mux = MUX_V; s.a_reg = 0;
        s.out_r = 12; s.mac_mask = 0xF;
        enc_row(&s, p2[0]);
        row_defaults(&s);
        s.mac = VSH_MAC_MOV; s.a_mux = MUX_R; s.a_reg = 12;
        s.out_r = 1; s.mac_mask = 0xF;
        enc_row(&s, p2[1]);
        /* Row 2: MOV oPos.xyz, R1 (shared O-write, so the R1 value becomes
         * externally observable via oPos again -- exercises the O-write
         * path independently of the direct R-write path row 0 used). */
        row_defaults(&s);
        s.mac = VSH_MAC_MOV; s.a_mux = MUX_R; s.a_reg = 1;
        s.out_mux = 0; s.o_mask = 0xE /* xyz */; s.out_orb = 1; s.out_addr = VSH_OREG_POS;
        s.final = 1;
        enc_row(&s, p2[2]);

        vattr2[0] = vec4(5.0f, 6.0f, 7.0f, 1.0f);
        vsh_run_program(p2, 3, 0, consts, vattr2, &res2, NULL);
        CHECK_NEAR("r12_alias: round-trip via R1 and O-write", res2.oreg[VSH_OREG_POS].x[0], 5.0f, 1e-6f);
        CHECK_NEAR("r12_alias: round-trip .y", res2.oreg[VSH_OREG_POS].x[1], 6.0f, 1e-6f);
    }
}

/* ---- 2. Temporary R-registers: written value persists across rows and is
 * scoped per component by the write mask. ---- */
static void test_temp_registers_and_mask(void)
{
    uint32_t program[3][4];
    RowSpec s;
    VshResult res;
    Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES];
    uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4];

    make_attrs(vattr);
    make_consts(consts);
    vattr[0] = vec4(1.0f, 2.0f, 3.0f, 4.0f);

    /* Row 0: MOV R5, v0, mask = xy only (mask bits are x=8,y=4,z=2,w=1 --
     * FLD_OUT_MAC_MASK bit i corresponds to (8>>i) meaning bit3=x). Only x,y
     * should be written; z,w must stay at the zero-initialised default. */
    row_defaults(&s);
    s.mac = VSH_MAC_MOV; s.a_mux = MUX_V; s.a_reg = 0;
    s.out_r = 5; s.mac_mask = 0xC; /* 1100 = x,y */
    enc_row(&s, program[0]);

    /* Row 1: ADD R6, R5, R5 (reads R5 back on a later row -- proves it
     * persisted, not just visible within the same row). */
    row_defaults(&s);
    s.mac = VSH_MAC_ADD; s.a_mux = MUX_R; s.a_reg = 5; s.c_mux = MUX_R; s.c_reg = 5;
    s.out_r = 6; s.mac_mask = 0xF;
    enc_row(&s, program[1]);

    /* Row 2: MOV oPos, R6 -- make the result observable. Final. */
    row_defaults(&s);
    s.mac = VSH_MAC_MOV; s.a_mux = MUX_R; s.a_reg = 6;
    s.out_mux = 0; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_POS;
    s.final = 1;
    enc_row(&s, program[2]);

    vsh_run_program(program, 3, 0, consts, vattr, &res, NULL);
    CHECK_NEAR("temp_reg: x doubled (masked component written)", res.oreg[VSH_OREG_POS].x[0], 2.0f, 1e-6f);
    CHECK_NEAR("temp_reg: y doubled (masked component written)", res.oreg[VSH_OREG_POS].x[1], 4.0f, 1e-6f);
    CHECK_NEAR("temp_reg: z stayed at zero-init (mask excluded it)", res.oreg[VSH_OREG_POS].x[2], 0.0f, 1e-6f);
    CHECK_NEAR("temp_reg: w stayed at zero-init (mask excluded it)", res.oreg[VSH_OREG_POS].x[3], 0.0f, 1e-6f);
}

/* ---- 3. Constants: the -3..+3 bank / 0..31 offset encoding maps to the
 * documented flat 0..191 index (VSH_D3DSCM_CORRECTION = 96), and the raw
 * dword bits are read back as the IEEE float they represent. ---- */
static void test_constants(void)
{
    uint32_t program[1][4];
    RowSpec s;
    VshResult res;
    Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES];
    uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4];

    make_attrs(vattr);
    make_consts(consts);

    /* bank=3 (top 3 bits of the 8-bit field, value 3), offset=5 =>
     * c = ((3-3)*32)+5 = 5, flat index = 5+96 = 101. Field layout: bits[7:5]
     * bank, bits[4:0] offset -- matches vsh_convert_c_register's own
     * ((c>>5)&7) / (c&31) extraction. */
    set_const(consts, 101, vec4(11.0f, 22.0f, 33.0f, 44.0f));

    /* MAC MOV only reads operand A, so the constant goes through the A
     * slot's mux (PARAM_C's bit field is shared across slots regardless of
     * which one selects it -- see enc_row's comment). */
    row_defaults(&s);
    s.mac = VSH_MAC_MOV; s.a_mux = MUX_C; s.c_bits = (3 << 5) | 5;
    s.out_mux = 0; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_D0;
    s.final = 1;
    enc_row(&s, program[0]);

    vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
    CHECK_NEAR("const: bank/offset -> flat index 101, x", res.oreg[VSH_OREG_D0].x[0], 11.0f, 1e-6f);
    CHECK_NEAR("const: y", res.oreg[VSH_OREG_D0].x[1], 22.0f, 1e-6f);
    CHECK_NEAR("const: z", res.oreg[VSH_OREG_D0].x[2], 33.0f, 1e-6f);
    CHECK_NEAR("const: w", res.oreg[VSH_OREG_D0].x[3], 44.0f, 1e-6f);
}

/* ---- 4. Vector/scalar operations: MAC DP3/DP4/MIN/MAX/SLT and ILU
 * RCP/RCC/RSQ against hand-computed values. ---- */
static void test_vector_scalar_ops(void)
{
    Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES];
    uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4];

    make_attrs(vattr);
    make_consts(consts);
    vattr[0] = vec4(1.0f, 2.0f, 3.0f, 4.0f);   /* v0 */
    /* Operand B/C uses a constant rather than a second v-attribute: FLD_V
     * (dword1 bits9-12) is a single field shared by every operand slot that
     * selects PARAM_V, so two DIFFERENT v-attributes cannot both be read by
     * one instruction on real hardware -- mixing one PARAM_V and one
     * PARAM_C source is the realistic way to get two distinct operands. */
    set_const(consts, 50, vec4(5.0f, 6.0f, 7.0f, 8.0f));

    /* DP3: dot((1,2,3),(5,6,7)) = 5+12+21 = 38, broadcast to all 4. */
    {
        uint32_t program[1][4];
        RowSpec s; VshResult res;
        row_defaults(&s);
        s.mac = VSH_MAC_DP3; s.a_mux = MUX_V; s.a_reg = 0; s.b_mux = MUX_C; s.c_bits = 50;
        s.out_mux = 0; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_D0; s.final = 1;
        enc_row(&s, program[0]);
        vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
        CHECK_NEAR("DP3", res.oreg[VSH_OREG_D0].x[0], 38.0f, 1e-4f);
        CHECK_NEAR("DP3 broadcasts to w too", res.oreg[VSH_OREG_D0].x[3], 38.0f, 1e-4f);
    }
    /* DP4: dot((1,2,3,4),(5,6,7,8)) = 5+12+21+32 = 70. */
    {
        uint32_t program[1][4];
        RowSpec s; VshResult res;
        row_defaults(&s);
        s.mac = VSH_MAC_DP4; s.a_mux = MUX_V; s.a_reg = 0; s.b_mux = MUX_C; s.c_bits = 50;
        s.out_mux = 0; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_D0; s.final = 1;
        enc_row(&s, program[0]);
        vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
        CHECK_NEAR("DP4", res.oreg[VSH_OREG_D0].x[0], 70.0f, 1e-4f);
    }
    /* MIN/MAX componentwise: min(1,5)=1, max(1,5)=5 on x. */
    {
        uint32_t program[1][4];
        RowSpec s; VshResult res;
        row_defaults(&s);
        s.mac = VSH_MAC_MIN; s.a_mux = MUX_V; s.a_reg = 0; s.b_mux = MUX_C; s.c_bits = 50;
        s.out_mux = 0; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_D0; s.final = 1;
        enc_row(&s, program[0]);
        vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
        CHECK_NEAR("MIN.x", res.oreg[VSH_OREG_D0].x[0], 1.0f, 1e-6f);
    }
    {
        uint32_t program[1][4];
        RowSpec s; VshResult res;
        row_defaults(&s);
        s.mac = VSH_MAC_MAX; s.a_mux = MUX_V; s.a_reg = 0; s.b_mux = MUX_C; s.c_bits = 50;
        s.out_mux = 0; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_D0; s.final = 1;
        enc_row(&s, program[0]);
        vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
        CHECK_NEAR("MAX.x", res.oreg[VSH_OREG_D0].x[0], 5.0f, 1e-6f);
    }
    /* SLT: (1<5, 2<6, 3<7, 4<8) all true -> (1,1,1,1). */
    {
        uint32_t program[1][4];
        RowSpec s; VshResult res;
        row_defaults(&s);
        s.mac = VSH_MAC_SLT; s.a_mux = MUX_V; s.a_reg = 0; s.b_mux = MUX_C; s.c_bits = 50;
        s.out_mux = 0; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_D0; s.final = 1;
        enc_row(&s, program[0]);
        vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
        CHECK_NEAR("SLT all-true", res.oreg[VSH_OREG_D0].x[0], 1.0f, 1e-6f);
        CHECK_NEAR("SLT all-true w", res.oreg[VSH_OREG_D0].x[3], 1.0f, 1e-6f);
    }
    /* MAD: v0*v0 + c[50] on x = 1*1+5 = 6 (one of SCDA's captured opcodes,
     * combining a MUL and an ADD in one instruction). */
    {
        uint32_t program[1][4];
        RowSpec s; VshResult res;
        row_defaults(&s);
        s.mac = VSH_MAC_MAD; s.a_mux = MUX_V; s.a_reg = 0; s.b_mux = MUX_V; s.b_reg = 0;
        s.c_mux = MUX_C; s.c_bits = 50;
        s.out_mux = 0; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_D0; s.final = 1;
        enc_row(&s, program[0]);
        vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
        CHECK_NEAR("MAD.x = v0.x*v0.x + c.x", res.oreg[VSH_OREG_D0].x[0], 6.0f, 1e-6f);
        CHECK_NEAR("MAD.y = v0.y*v0.y + c.y", res.oreg[VSH_OREG_D0].x[1], 10.0f, 1e-6f);
    }
    /* ILU RCP of v0.x=1 broadcast via swizzle xxxx before the op reads .x:
     * RCP(4.0) = 0.25. */
    {
        uint32_t program[1][4];
        RowSpec s; VshResult res;
        row_defaults(&s);
        s.ilu = VSH_ILU_RCP; s.c_mux = MUX_V; s.c_reg = 0;
        s.c_swz[0] = 3; s.c_swz[1] = 3; s.c_swz[2] = 3; s.c_swz[3] = 3;  /* .wwww -> .x reads v0.w=4 */
        s.out_mux = 1; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_D0; s.final = 1;
        enc_row(&s, program[0]);
        vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
        CHECK_NEAR("ILU RCP(4.0)=0.25", res.oreg[VSH_OREG_D0].x[0], 0.25f, 1e-6f);
    }
    /* ILU RSQ(0) must be +Infinity (xemu's explicit special case, not a
     * divide-by-zero crash or NaN). */
    {
        uint32_t program[1][4];
        RowSpec s; VshResult res;
        row_defaults(&s);
        s.ilu = VSH_ILU_RSQ; s.c_mux = MUX_V; s.c_reg = 0;
        s.c_swz[0] = 0; s.c_swz[1] = 0; s.c_swz[2] = 0; s.c_swz[3] = 0;
        vattr[0].x[0] = 0.0f;
        s.out_mux = 1; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_D0; s.final = 1;
        enc_row(&s, program[0]);
        vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
        CHECK("ILU RSQ(0) = +Inf", isinf(res.oreg[VSH_OREG_D0].x[0]) && res.oreg[VSH_OREG_D0].x[0] > 0);
        vattr[0].x[0] = 1.0f; /* restore for later tests */
    }
}

/* ---- 5. Output registers: the shared muxed O-write only fires from
 * whichever of MAC/ILU the per-row FLD_OUT_MUX names, and writes the
 * addressed O-register, not always oPos. ---- */
static void test_output_registers(void)
{
    uint32_t program[1][4];
    RowSpec s; VshResult res;
    Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES];
    uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4];

    make_attrs(vattr);
    make_consts(consts);
    vattr[0] = vec4(9.0f, 8.0f, 7.0f, 6.0f);

    /* MAC MOV v0 -> oT0 (out_addr=VSH_OREG_T0), out_mux=0 (MAC owns it). */
    row_defaults(&s);
    s.mac = VSH_MAC_MOV; s.a_mux = MUX_V; s.a_reg = 0;
    s.out_mux = 0; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_T0;
    s.final = 1;
    enc_row(&s, program[0]);
    vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
    CHECK_NEAR("oT0 written, not oPos", res.oreg[VSH_OREG_T0].x[0], 9.0f, 1e-6f);
    CHECK_NEAR("oPos untouched (still hardware default)", res.oreg[VSH_OREG_POS].x[3], 1.0f, 1e-6f);
    CHECK_NEAR("oPos.x untouched", res.oreg[VSH_OREG_POS].x[0], 0.0f, 1e-6f);

    /* Same row but out_mux=1 (ILU claims it) with MAC active too: since ILU
     * is NOP this row, the shared write must NOT fire at all (out_mux names
     * ILU, but ILU is NOP) -- oT1 must stay at the hardware default. */
    row_defaults(&s);
    s.mac = VSH_MAC_MOV; s.a_mux = MUX_V; s.a_reg = 0;
    s.out_mux = 1; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_T1;
    s.final = 1;
    enc_row(&s, program[0]);
    vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
    CHECK_NEAR("out_mux naming a NOP slot suppresses the O-write",
               res.oreg[VSH_OREG_T1].x[0], 0.0f, 1e-6f);
}

/* ---- 6. SCDA's concrete opcodes (MAC MOV/MUL/ADD/MAD; ILU MOV/RCC) in a
 * paired instruction, checking the corrected write-back routing: ILU's
 * R-write is forced to R1 whenever paired with a non-NOP MAC (regardless of
 * which of the two owns the shared O-output), and MAC's own R-write is
 * dropped only when paired AND its own target is also R1. ---- */
static void test_paired_mac_ilu_routing(void)
{
    Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES];
    uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4];

    make_attrs(vattr);
    make_consts(consts);
    vattr[0] = vec4(2.0f, 0.0f, 0.0f, 1.0f);   /* v0.x = 2 */

    /* Row: MAC MUL R9, v0, v0 (paired) / ILU RCC R1, v0.x (forced to R1
     * regardless of out_mux). out_r=9 (not 1), so MAC's write is NOT
     * suppressed. RCC(2.0) = 0.5. Then a second row reads both R9 and R1
     * back to prove they landed where expected. */
    {
        uint32_t program[2][4];
        RowSpec s; VshResult res;

        row_defaults(&s);
        s.mac = VSH_MAC_MUL; s.a_mux = MUX_V; s.a_reg = 0; s.b_mux = MUX_V; s.b_reg = 0;
        s.ilu = VSH_ILU_RCC; s.c_mux = MUX_V; s.c_reg = 0;
        s.out_r = 9; s.mac_mask = 0xF; s.ilu_mask = 0xF;
        s.out_mux = 0; /* MAC owns the (unused here) shared O-output */
        enc_row(&s, program[0]);

        /* MAD oPos, R9, {1,1,1,1}(via v-default? use a constant instead},
         * R1 -- simplest: ADD oPos, R9, R1, mask all, final. */
        row_defaults(&s);
        s.mac = VSH_MAC_ADD; s.a_mux = MUX_R; s.a_reg = 9; s.c_mux = MUX_R; s.c_reg = 1;
        s.out_mux = 0; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_POS;
        s.final = 1;
        enc_row(&s, program[1]);

        vsh_run_program(program, 2, 0, consts, vattr, &res, NULL);
        /* R9.x = 2*2 = 4; R1.x (forced ILU target) = RCC(2.0) = 0.5.
         * oPos.x = R9.x + R1.x = 4.5. */
        CHECK_NEAR("paired: MAC->R9 and forced ILU->R1 both land, unsuppressed",
                   res.oreg[VSH_OREG_POS].x[0], 4.5f, 1e-5f);
    }

    /* Same pairing, but MAC's own out_r is ALSO 1 this time -- per
     * decode_opcode(), MAC's R-write must be dropped entirely (R1 belongs to
     * the paired ILU), so only the ILU's RCC(2.0)=0.5 should end up in R1. */
    {
        uint32_t program[2][4];
        RowSpec s; VshResult res;

        row_defaults(&s);
        s.mac = VSH_MAC_MUL; s.a_mux = MUX_V; s.a_reg = 0; s.b_mux = MUX_V; s.b_reg = 0;
        s.ilu = VSH_ILU_RCC; s.c_mux = MUX_V; s.c_reg = 0;
        s.out_r = 1;                       /* MAC's own target is also R1 */
        s.mac_mask = 0xF; s.ilu_mask = 0xF;
        enc_row(&s, program[0]);

        row_defaults(&s);
        s.mac = VSH_MAC_MOV; s.a_mux = MUX_R; s.a_reg = 1;
        s.out_mux = 0; s.o_mask = 0xF; s.out_orb = 1; s.out_addr = VSH_OREG_POS;
        s.final = 1;
        enc_row(&s, program[1]);

        vsh_run_program(program, 2, 0, consts, vattr, &res, NULL);
        /* If MAC's write to R1 (value 4.0) had NOT been suppressed, R1 would
         * read back as 4.0 (whichever write "won"); the spec says ILU's
         * 0.5 must be what actually lands, with MAC's write to R1 dropped
         * entirely rather than racing it. */
        CHECK_NEAR("paired: MAC's own R1-write is suppressed, ILU's wins",
                   res.oreg[VSH_OREG_POS].x[0], 0.5f, 1e-5f);
    }
}

/* ---- 7. Unimplemented opcode: must stop cleanly (status -2), not read
 * garbage or crash -- the same defensive contract the old capture-only
 * evaluator had, now for the full opcode set (nothing above MAC_ARL / above
 * ILU_LIT is a valid field value, but this checks the reporting path since
 * every real opcode is now implemented). A synthetic out-of-range program
 * length instead exercises the "ran off the end" status. */
static void test_ran_off_end_status(void)
{
    uint32_t program[1][4];
    RowSpec s; VshResult res;
    Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES];
    uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4];

    make_attrs(vattr);
    make_consts(consts);
    row_defaults(&s);
    s.mac = VSH_MAC_NOP; s.ilu = VSH_ILU_NOP;
    /* no FLD_FINAL set */
    enc_row(&s, program[0]);

    vsh_run_program(program, 1, 0, consts, vattr, &res, NULL);
    CHECK("ran_off_end: status==1 when FLD_FINAL never seen", res.status == 1);
    CHECK("ran_off_end: stop_row is the first row past what was loaded", res.stop_row == 1);
}

int main(void)
{
    test_r12_is_opos_alias();
    test_temp_registers_and_mask();
    test_constants();
    test_vector_scalar_ops();
    test_output_registers();
    test_paired_mac_ilu_routing();
    test_ran_off_end_status();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
