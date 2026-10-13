/*
 * NV2A vertex-program interpreter -- implementation.
 *
 * Field bit layout matches xemu's field_mapping[] (vsh-prog.c) exactly:
 * DWORD indices below are 0-based and match program[row][i] directly; xemu's
 * table uses "DWORD 1..3" for a 4-word row and never touches word 0, which is
 * why a captured row always starts with 0x00000000.
 *
 * Every MAC/ILU opcode formula is ported directly from the GLSL function
 * bodies xemu compiles and runs on real hardware/xemu's GPU backend
 * (vsh-prog.c's _MOV/_MUL/_ADD/.../_LIT), not re-derived from a datasheet.
 *
 * The output-register write-back routing (which of MAC/ILU's R-register
 * write wins when they are paired, and which one gets to also write the
 * shared "muxed" O-register) is ported from decode_opcode()'s actual branch
 * structure, not from a paraphrase of it:
 *   - MAC's R-register write is silently dropped when paired with a non-NOP
 *     ILU and MAC's own out_r happens to be 1 (R1 is reserved for the paired
 *     ILU's result in that case).
 *   - ILU's R-register write always targets R1 when paired with a non-NOP
 *     MAC, unconditionally -- regardless of which of the two owns the shared
 *     muxed O-register output this row.
 *   - The shared O-register write happens from whichever of MAC/ILU the
 *     per-instruction FLD_OUT_MUX bit names, gated on that slot actually
 *     being non-NOP.
 * Register 12 is not a 13th general-purpose temp: on real NV2A hardware it is
 * a direct alias for oPos (xemu: "#define R12 oPos" in the generated GLSL;
 * this project's own src/d3d/d3d8_vsh.c already relies on the same alias).
 * Every read or write of R12 below goes through oreg[VSH_OREG_POS] instead
 * of a 13th r[] slot.
 */
#include "vsh_interp.h"
#include <math.h>
#include <string.h>

#ifndef INFINITY
#define INFINITY (1.0f / 0.0f)
#endif

static uint32_t vshf(const uint32_t *w, int dword, int start, int len)
{
    return (w[dword] >> start) & ((1u << len) - 1u);
}

/* xemu's convert_c_register(): maps the 8-bit encoded constant index
 * (top 3 bits a signed "bank" -3..+3, low 5 bits an offset 0..31) onto a
 * flat 0..191 index via VSH_D3DSCM_CORRECTION (96). */
static int vsh_convert_c_register(int c)
{
    int r = ((((c >> 5) & 7) - 3) * 32) + (c & 31);
    return r + 96;
}

static Vec4 vsh_swz(Vec4 v, const uint32_t s[4])
{
    Vec4 r;
    float c[4];
    int i;
    c[0] = v.x[0]; c[1] = v.x[1]; c[2] = v.x[2]; c[3] = v.x[3];
    for (i = 0; i < 4; i++)
        r.x[i] = c[s[i] & 3];
    return r;
}

/* Reads MAC/ILU input slot `which` (0=A, 1=B, 2=C) of one instruction.
 * `r` is the 16-entry temp register file (r[12] is never read here -- see
 * the file comment), `vattr` the fetched v0..v15, `consts` the constant
 * bank, `opos` the live oPos value (for the R12 alias) and `a0` the current
 * address register (for FLD_A0X-relative constant addressing). */
static Vec4 vsh_read_slot(const uint32_t *w, int which, const Vec4 *r,
                          const Vec4 *vattr,
                          const uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4],
                          Vec4 opos, int a0)
{
    static const int neg_field[3]   = { 8, 25, 10 };   /* dword 1 or 2, bit */
    static const int neg_dword[3]   = { 1, 2, 2 };
    static const int swz_dword[3]   = { 1, 2, 2 };
    static const int swz_start[3][4] = { {6,4,2,0}, {23,21,19,17}, {8,6,4,2} };
    static const int mux_dword[3]   = { 2, 2, 3 };
    static const int mux_start[3]   = { 26, 11, 28 };
    uint32_t mux, neg, s[4];
    Vec4 v;
    int i;

    mux = vshf(w, mux_dword[which], mux_start[which], 2);
    neg = vshf(w, neg_dword[which], neg_field[which], 1);
    for (i = 0; i < 4; i++)
        s[i] = vshf(w, swz_dword[which], swz_start[which][i], 2);

    if (mux == 1) {                              /* PARAM_R */
        uint32_t reg = (which == 0) ? vshf(w, 2, 28, 4)
                     : (which == 1) ? vshf(w, 2, 13, 4)
                     : ((vshf(w, 2, 0, 2) << 2) | vshf(w, 3, 30, 2));
        v = ((reg & 0xF) == 12) ? opos : r[reg & 0xF];
    } else if (mux == 2) {                        /* PARAM_V */
        uint32_t vi = vshf(w, 1, 9, 4);
        v = vattr[vi & 0xF];
    } else if (mux == 3) {                        /* PARAM_C */
        uint32_t cbits = vshf(w, 1, 13, 8);
        int creg = vsh_convert_c_register((int)cbits);
        if (vshf(w, 3, 1, 1))                     /* FLD_A0X: c[A0+n] */
            creg += a0;
        if (creg < 0) creg = 0;
        if (creg >= NV2A_VERTEXSHADER_CONSTANTS) creg = NV2A_VERTEXSHADER_CONSTANTS - 1;
        memcpy(v.x, consts[creg], sizeof v.x);
    } else {
        v.x[0] = v.x[1] = v.x[2] = v.x[3] = 0.0f;
    }

    v = vsh_swz(v, s);
    if (neg)
        for (i = 0; i < 4; i++) v.x[i] = -v.x[i];
    return v;
}

/* ---- MAC opcodes (vsh-prog.c's _MOV/_MUL/_ADD/_MAD/_DP3/_DPH/_DP4/_DST/
 * _MIN/_MAX/_SLT/_SGE, ported from their GLSL bodies verbatim). ---- */

static int vfnan(float f) { return f != f; }
static float vnan_to_one(float f) { return vfnan(f) ? 1.0f : f; }
static float vfsign(float f) { return f > 0.0f ? 1.0f : (f < 0.0f ? -1.0f : 0.0f); }

/* _MUL: "anything * 0 must == 0" even across NaN/Inf, per xemu's comment --
 * a plain src0*src1 lets NaN/Inf poison an otherwise-zero result. */
static Vec4 vmul(Vec4 a, Vec4 b)
{
    Vec4 r;
    int i;
    for (i = 0; i < 4; i++) {
        float zero = vfsign(vnan_to_one(a.x[i])) * vfsign(vnan_to_one(b.x[i]));
        r.x[i] = (zero == 0.0f) ? 0.0f : a.x[i] * b.x[i];
    }
    return r;
}

static Vec4 vadd(Vec4 a, Vec4 b)
{
    Vec4 r;
    int i;
    for (i = 0; i < 4; i++) r.x[i] = a.x[i] + b.x[i];
    return r;
}

static Vec4 vmad(Vec4 a, Vec4 b, Vec4 c)
{
    Vec4 m = vmul(a, b);
    return vadd(m, c);
}

static Vec4 vsplat(float f) { Vec4 r = { { f, f, f, f } }; return r; }

static Vec4 vdp3(Vec4 a, Vec4 b)
{
    return vsplat(a.x[0]*b.x[0] + a.x[1]*b.x[1] + a.x[2]*b.x[2]);
}
static Vec4 vdph(Vec4 a, Vec4 b)
{
    return vsplat(a.x[0]*b.x[0] + a.x[1]*b.x[1] + a.x[2]*b.x[2] + 1.0f*b.x[3]);
}
static Vec4 vdp4(Vec4 a, Vec4 b)
{
    return vsplat(a.x[0]*b.x[0] + a.x[1]*b.x[1] + a.x[2]*b.x[2] + a.x[3]*b.x[3]);
}
static Vec4 vdst(Vec4 a, Vec4 b)
{
    Vec4 r;
    r.x[0] = 1.0f;
    r.x[1] = a.x[1] * b.x[1];
    r.x[2] = a.x[2];
    r.x[3] = b.x[3];
    return r;
}
static Vec4 vmin4(Vec4 a, Vec4 b)
{
    Vec4 r; int i;
    for (i = 0; i < 4; i++) r.x[i] = a.x[i] < b.x[i] ? a.x[i] : b.x[i];
    return r;
}
static Vec4 vmax4(Vec4 a, Vec4 b)
{
    Vec4 r; int i;
    for (i = 0; i < 4; i++) r.x[i] = a.x[i] > b.x[i] ? a.x[i] : b.x[i];
    return r;
}
static Vec4 vslt(Vec4 a, Vec4 b)
{
    Vec4 r; int i;
    for (i = 0; i < 4; i++) r.x[i] = (a.x[i] < b.x[i]) ? 1.0f : 0.0f;
    return r;
}
static Vec4 vsge(Vec4 a, Vec4 b)
{
    Vec4 r; int i;
    for (i = 0; i < 4; i++) r.x[i] = (a.x[i] >= b.x[i]) ? 1.0f : 0.0f;
    return r;
}

/* ---- ILU opcodes (_MOV/_RCP/_RCC/_RSQ/_EXP/_LOG/_LIT). All but MOV/LIT
 * read only the input's .x component, per xemu's ilu_force_scalar[]. ---- */

/* Clamp to [2^-64, 2^64] or [-2^64, -2^-64], keeping sign and avoiding exact
 * zero/infinity -- xemu's clampAwayZeroInf(), used only by RCC. */
static float clamp_away_zero_inf(float t)
{
    const float min_pos = 5.42101086e-20f;  /* 2^-64 */
    const float max_pos = 1.8446744e19f;    /*  2^64 */
    if (t > 0.0f || (t == 0.0f && !signbit(t))) {
        if (t < min_pos) t = min_pos;
        else if (t > max_pos) t = max_pos;
    } else {
        if (t > -min_pos) t = -min_pos;
        else if (t < -max_pos) t = -max_pos;
    }
    return t;
}

static Vec4 vlit(Vec4 s)
{
    const float epsilon = 1.0f / 256.0f;
    Vec4 t = { { 1.0f, 0.0f, 0.0f, 1.0f } };
    if (s.x[3] < -(128.0f - epsilon)) s.x[3] = -(128.0f - epsilon);
    if (s.x[3] >  (128.0f - epsilon)) s.x[3] =  (128.0f - epsilon);
    if (s.x[0] < 0.0f) s.x[0] = 0.0f;
    if (s.x[1] < 0.0f) s.x[1] = 0.0f;
    t.x[1] = s.x[0];
    t.x[2] = (s.x[0] > 0.0f) ? exp2f(s.x[3] * log2f(s.x[1])) : 0.0f;
    return t;
}

void vsh_run_program(const uint32_t program[][4], uint32_t program_len,
                      uint32_t start_pc,
                      const uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4],
                      const Vec4 vattr_in[NV2A_VSH_NUM_ATTRIBUTES],
                      VshResult *out, FILE *trace_fp)
{
    Vec4 r[16];
    Vec4 oreg[NV2A_VSH_NUM_OUTPUTS];
    int a0 = 0;
    uint32_t pc = start_pc;
    int steps, i;

    memset(r, 0, sizeof r);                 /* real temps R0..R11; r[12] is
                                              * never touched (see above) */
    for (i = 0; i < NV2A_VSH_NUM_OUTPUTS; i++)
        oreg[i] = vsplat(0.0f), oreg[i].x[3] = 1.0f;  /* hardware default */

    out->status = 1;
    out->stop_row = start_pc;
    out->stop_mac = out->stop_ilu = 0;

    for (steps = 0; steps < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH; steps++) {
        const uint32_t *w;
        uint32_t mac, ilu, final;
        Vec4 a = vsplat(0.0f), b = vsplat(0.0f), c = vsplat(0.0f);
        Vec4 mac_res = vsplat(0.0f), ilu_res = vsplat(0.0f);
        Vec4 opos;
        int mac_has_a = 0, mac_has_b = 0, mac_has_c = 0;
        int unsupported = 0;

        if (pc >= program_len) {
            out->status = 1;
            out->stop_row = pc;
            break;
        }
        w = program[pc];
        mac   = vshf(w, 1, 21, 4);
        ilu   = vshf(w, 1, 25, 3);
        final = vshf(w, 3, 0, 1);
        opos  = oreg[VSH_OREG_POS];

        switch (mac) {
        case VSH_MAC_MOV: mac_has_a = 1; break;
        case VSH_MAC_MUL: mac_has_a = mac_has_b = 1; break;
        case VSH_MAC_ADD: mac_has_a = mac_has_c = 1; break;
        case VSH_MAC_MAD: mac_has_a = mac_has_b = mac_has_c = 1; break;
        case VSH_MAC_DP3: case VSH_MAC_DPH: case VSH_MAC_DP4:
        case VSH_MAC_DST: case VSH_MAC_MIN: case VSH_MAC_MAX:
        case VSH_MAC_SLT: case VSH_MAC_SGE:
            mac_has_a = mac_has_b = 1; break;
        case VSH_MAC_ARL: mac_has_a = 1; break;
        default: break;   /* NOP */
        }

        if (mac_has_a || ilu != VSH_ILU_NOP) a = vsh_read_slot(w, 0, r, vattr_in, consts, opos, a0);
        if (mac_has_b)                       b = vsh_read_slot(w, 1, r, vattr_in, consts, opos, a0);
        if (mac_has_c || ilu != VSH_ILU_NOP) c = vsh_read_slot(w, 2, r, vattr_in, consts, opos, a0);

        switch (mac) {
        case VSH_MAC_NOP: break;
        case VSH_MAC_MOV: mac_res = a; break;
        case VSH_MAC_MUL: mac_res = vmul(a, b); break;
        case VSH_MAC_ADD: mac_res = vadd(a, c); break;
        case VSH_MAC_MAD: mac_res = vmad(a, b, c); break;
        case VSH_MAC_DP3: mac_res = vdp3(a, b); break;
        case VSH_MAC_DPH: mac_res = vdph(a, b); break;
        case VSH_MAC_DP4: mac_res = vdp4(a, b); break;
        case VSH_MAC_DST: mac_res = vdst(a, b); break;
        case VSH_MAC_MIN: mac_res = vmin4(a, b); break;
        case VSH_MAC_MAX: mac_res = vmax4(a, b); break;
        case VSH_MAC_SLT: mac_res = vslt(a, b); break;
        case VSH_MAC_SGE: mac_res = vsge(a, b); break;
        case VSH_MAC_ARL: a0 = (int)floorf(a.x[0] + 0.001f); break;
        default: unsupported = 1; break;
        }

        switch (ilu) {
        case VSH_ILU_NOP: break;
        case VSH_ILU_MOV: ilu_res = c; break;
        case VSH_ILU_RCP: ilu_res = vsplat(1.0f / c.x[0]); break;
        case VSH_ILU_RCC: ilu_res = vsplat(clamp_away_zero_inf(1.0f / c.x[0])); break;
        case VSH_ILU_RSQ:
            if (c.x[0] == 0.0f) ilu_res = vsplat(INFINITY);
            else if (isinf(c.x[0])) ilu_res = vsplat(0.0f);
            else ilu_res = vsplat(1.0f / sqrtf(fabsf(c.x[0])));
            break;
        case VSH_ILU_EXP:
            ilu_res.x[0] = exp2f(floorf(c.x[0]));
            ilu_res.x[1] = c.x[0] - floorf(c.x[0]);
            ilu_res.x[2] = exp2f(c.x[0]);
            ilu_res.x[3] = 1.0f;
            break;
        case VSH_ILU_LOG: {
            float tmp = fabsf(c.x[0]);
            if (tmp == 0.0f) {
                ilu_res.x[0] = -INFINITY; ilu_res.x[1] = 1.0f;
                ilu_res.x[2] = -INFINITY; ilu_res.x[3] = 1.0f;
            } else {
                ilu_res.x[0] = floorf(log2f(tmp));
                ilu_res.x[1] = tmp / exp2f(floorf(log2f(tmp)));
                ilu_res.x[2] = log2f(tmp);
                ilu_res.x[3] = 1.0f;
            }
            break;
        }
        case VSH_ILU_LIT: ilu_res = vlit(c); break;
        default: unsupported = 1; break;
        }

        if (unsupported) {
            out->status = -2;
            out->stop_row = pc;
            out->stop_mac = mac;
            out->stop_ilu = ilu;
            break;
        }

        /* Write-back -- see the file comment for the exact pairing rules. */
        {
            uint32_t out_mux  = vshf(w, 3, 2, 1);
            uint32_t out_r    = vshf(w, 3, 20, 4);
            uint32_t mac_mask = vshf(w, 3, 24, 4);
            uint32_t ilu_mask = vshf(w, 3, 16, 4);
            uint32_t o_mask   = vshf(w, 3, 12, 4);
            uint32_t out_orb  = vshf(w, 3, 11, 1);
            uint32_t out_addr = vshf(w, 3, 3, 8) & 0xF;
            int paired = (mac != VSH_MAC_NOP) && (ilu != VSH_ILU_NOP);

            if (mac != VSH_MAC_NOP && mac != VSH_MAC_ARL && mac_mask
                    && !(paired && out_r == 1)) {
                for (i = 0; i < 4; i++)
                    if (mac_mask & (8u >> i)) {
                        if ((out_r & 0xF) == 12) oreg[VSH_OREG_POS].x[i] = mac_res.x[i];
                        else r[out_r & 0xF].x[i] = mac_res.x[i];
                    }
            }
            if (ilu != VSH_ILU_NOP && ilu_mask) {
                uint32_t target = paired ? 1u : (out_r & 0xF);
                for (i = 0; i < 4; i++)
                    if (ilu_mask & (8u >> i)) {
                        if (target == 12) oreg[VSH_OREG_POS].x[i] = ilu_res.x[i];
                        else r[target].x[i] = ilu_res.x[i];
                    }
            }
            if (o_mask && out_orb
                    && ((out_mux == 0 && mac != VSH_MAC_NOP && mac != VSH_MAC_ARL)
                     || (out_mux == 1 && ilu != VSH_ILU_NOP))) {
                Vec4 muxed = (out_mux == 1) ? ilu_res : mac_res;
                for (i = 0; i < 4; i++)
                    if (o_mask & (8u >> i)) oreg[out_addr].x[i] = muxed.x[i];
            }

            if (trace_fp)
                fprintf(trace_fp,
                        "  [VSH]   row=%u mac=%u ilu=%u a0=%d"
                        " a=(%.4f,%.4f,%.4f,%.4f) b=(%.4f,%.4f,%.4f,%.4f)"
                        " c=(%.4f,%.4f,%.4f,%.4f)"
                        " out_r=%u mac_mask=%u ilu_mask=%u o_mask=%u out_addr=%u"
                        " oPos=(%.4f,%.4f,%.4f,%.4f)\n",
                        pc, mac, ilu, a0,
                        a.x[0],a.x[1],a.x[2],a.x[3], b.x[0],b.x[1],b.x[2],b.x[3],
                        c.x[0],c.x[1],c.x[2],c.x[3],
                        out_r, mac_mask, ilu_mask, o_mask, out_addr,
                        oreg[VSH_OREG_POS].x[0], oreg[VSH_OREG_POS].x[1],
                        oreg[VSH_OREG_POS].x[2], oreg[VSH_OREG_POS].x[3]);
        }

        pc++;
        if (final) {
            out->status = 0;
            out->stop_row = pc - 1;
            break;
        }
    }

    out->steps = steps;
    memcpy(out->oreg, oreg, sizeof oreg);
}
