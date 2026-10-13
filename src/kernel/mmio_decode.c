/* Faulting-instruction emulation for trapped device apertures; see
 * mmio_decode.h. Moved out of src/gpu/nv2a_recomp.c (docs Update 355) so the
 * MCPX ACI (mcpx_aci.c) uses the same decoder. */
#include "mmio_decode.h"

typedef struct {
    mmio_read_fn rd;
    mmio_write_fn wr;
    void *opaque;
} MmioTarget;

static uint64_t *ctx_reg64(PCONTEXT ctx, int reg)
{
    switch (reg & 0xF) {
    case 0: return (uint64_t *)&ctx->Rax;
    case 1: return (uint64_t *)&ctx->Rcx;
    case 2: return (uint64_t *)&ctx->Rdx;
    case 3: return (uint64_t *)&ctx->Rbx;
    case 4: return (uint64_t *)&ctx->Rsp;
    case 5: return (uint64_t *)&ctx->Rbp;
    case 6: return (uint64_t *)&ctx->Rsi;
    case 7: return (uint64_t *)&ctx->Rdi;
    case 8: return (uint64_t *)&ctx->R8;
    case 9: return (uint64_t *)&ctx->R9;
    case 10: return (uint64_t *)&ctx->R10;
    case 11: return (uint64_t *)&ctx->R11;
    case 12: return (uint64_t *)&ctx->R12;
    case 13: return (uint64_t *)&ctx->R13;
    case 14: return (uint64_t *)&ctx->R14;
    default: return (uint64_t *)&ctx->R15;
    }
}

/* Register operand value / write-back at an access width, honouring the
 * legacy AH/CH/DH/BH encoding (8-bit, no REX, reg 4..7). */
static uint64_t reg_get(PCONTEXT ctx, int reg, int size, int has_rex)
{
    if (size == 1 && !has_rex && reg >= 4 && reg < 8)
        return (*ctx_reg64(ctx, reg - 4) >> 8) & 0xFF;
    uint64_t v = *ctx_reg64(ctx, reg);
    return size == 8 ? v : v & ((1ULL << (size * 8)) - 1);
}

static void reg_set(PCONTEXT ctx, int reg, int size, int has_rex, uint64_t v)
{
    uint64_t *r;
    if (size == 1 && !has_rex && reg >= 4 && reg < 8) {
        r = ctx_reg64(ctx, reg - 4);
        *r = (*r & ~0xFF00ULL) | ((v & 0xFF) << 8);
        return;
    }
    r = ctx_reg64(ctx, reg);
    if (size == 1) *r = (*r & ~0xFFULL) | (v & 0xFF);
    else if (size == 2) *r = (*r & ~0xFFFFULL) | (v & 0xFFFF);
    else if (size == 4) *r = v & 0xFFFFFFFFULL;     /* zero-extends, as x64 */
    else *r = v;
}

static int modrm_len(const uint8_t *p)
{
    uint8_t modrm = p[0];
    int mod = modrm >> 6, rm = modrm & 7, len = 1;
    if (mod == 3) return 1;
    if (rm == 4) {
        len++;
        if (mod == 0 && (p[1] & 7) == 5) len += 4;
    }
    if (mod == 0 && rm == 5) len += 4;
    else if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

static uint64_t mmio_rd(const MmioTarget *t, uint32_t off, int size)
{
    if (size == 8)
        return t->rd(t->opaque, off, 4)
               | (t->rd(t->opaque, off + 4, 4) << 32);
    return t->rd(t->opaque, off, size);
}

static void mmio_wr(const MmioTarget *t, uint32_t off, uint64_t v, int size)
{
    if (size < 8)
        v &= (1ULL << (size * 8)) - 1;   /* sign-extended immediates */
    if (size == 8) {
        t->wr(t->opaque, off, v & 0xFFFFFFFFu, 4);
        t->wr(t->opaque, off + 4, v >> 32, 4);
        return;
    }
    t->wr(t->opaque, off, v, size);
}

#define FL_CF 0x0001
#define FL_PF 0x0004
#define FL_ZF 0x0040
#define FL_SF 0x0080
#define FL_OF 0x0800

static void set_flags(PCONTEXT ctx, uint64_t res, int size, int cf, int of)
{
    uint64_t mask = size == 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
    uint8_t lo = (uint8_t)res;
    int parity = 1;
    res &= mask;
    for (int i = 0; i < 8; i++) parity ^= (lo >> i) & 1;
    ctx->EFlags &= ~(FL_CF | FL_PF | FL_ZF | FL_SF | FL_OF);
    if (!res) ctx->EFlags |= FL_ZF;
    if (res & (1ULL << (size * 8 - 1))) ctx->EFlags |= FL_SF;
    if (parity) ctx->EFlags |= FL_PF;
    if (cf) ctx->EFlags |= FL_CF;
    if (of) ctx->EFlags |= FL_OF;
}

/* ALU op on (mem, src): 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp. Returns
 * whether the result is written back. */
static int alu(PCONTEXT ctx, int op, uint64_t a, uint64_t b, int size, uint64_t *out)
{
    uint64_t mask = size == 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
    uint64_t sign = 1ULL << (size * 8 - 1);
    uint64_t r;
    a &= mask; b &= mask;
    switch (op) {
    case 0: r = (a + b) & mask;
        set_flags(ctx, r, size, r < a, (~(a ^ b) & (a ^ r) & sign) != 0);
        break;
    case 1: r = a | b; set_flags(ctx, r, size, 0, 0); break;
    case 4: r = a & b; set_flags(ctx, r, size, 0, 0); break;
    case 5: case 7: r = (a - b) & mask;
        set_flags(ctx, r, size, a < b, ((a ^ b) & (a ^ r) & sign) != 0);
        break;
    case 6: r = a ^ b; set_flags(ctx, r, size, 0, 0); break;
    default: return -1;
    }
    *out = r;
    return op != 7;
}

bool mmio_decode_and_run(PCONTEXT ctx, uint32_t off, mmio_read_fn rd,
                         mmio_write_fn wr, void *opaque)
{
    const MmioTarget tgt = { rd, wr, opaque }, *t = &tgt;
    const uint8_t *ip = (const uint8_t *)ctx->Rip;
    int n = 0, opsz16 = 0, rex = 0;
    for (;;) {
        uint8_t b = ip[n];
        if (b == 0x66) { opsz16 = 1; n++; }
        else if (b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x3E) n++;
        else if (b >= 0x40 && b <= 0x4F) { rex = b; n++; }
        else break;
    }
    const int rex_w = rex & 8, rex_r = (rex & 4) ? 8 : 0, has_rex = rex != 0;
    const uint8_t *op = ip + n;
    int size = rex_w ? 8 : opsz16 ? 2 : 4;
    int reg, ml;
    uint64_t v, r;

    switch (op[0]) {
    case 0x88: case 0x89:                       /* mov r/m, r */
        if (op[0] == 0x88) size = 1;
        reg = ((op[1] >> 3) & 7) | rex_r;
        mmio_wr(t, off, reg_get(ctx, reg, size, has_rex), size);
        ctx->Rip += n + 1 + modrm_len(op + 1);
        return true;
    case 0x8A: case 0x8B:                       /* mov r, r/m */
        if (op[0] == 0x8A) size = 1;
        reg = ((op[1] >> 3) & 7) | rex_r;
        reg_set(ctx, reg, size, has_rex, mmio_rd(t, off, size));
        ctx->Rip += n + 1 + modrm_len(op + 1);
        return true;
    case 0xC6:                                  /* mov r/m8, imm8 */
        ml = modrm_len(op + 1);
        mmio_wr(t, off, op[1 + ml], 1);
        ctx->Rip += n + 1 + ml + 1;
        return true;
    case 0xC7:                                  /* mov r/m, imm16/32 */
        ml = modrm_len(op + 1);
        if (size == 2) { v = *(const uint16_t *)(op + 1 + ml); ctx->Rip += n + 1 + ml + 2; }
        else { v = (uint64_t)(int64_t)*(const int32_t *)(op + 1 + ml); ctx->Rip += n + 1 + ml + 4; }
        mmio_wr(t, off, v, size);
        return true;
    case 0x0F:
        if (op[1] == 0xB6 || op[1] == 0xB7 || op[1] == 0xBE || op[1] == 0xBF) {
            int src = (op[1] & 1) ? 2 : 1;
            reg = ((op[2] >> 3) & 7) | rex_r;
            v = mmio_rd(t, off, src);
            if (op[1] >= 0xBE)
                v = src == 1 ? (uint64_t)(int64_t)(int8_t)v : (uint64_t)(int64_t)(int16_t)v;
            reg_set(ctx, reg, size, has_rex, v);
            ctx->Rip += n + 2 + modrm_len(op + 2);
            return true;
        }
        break;
    case 0x84: case 0x85:                       /* test r/m, r */
        if (op[0] == 0x84) size = 1;
        reg = ((op[1] >> 3) & 7) | rex_r;
        set_flags(ctx, mmio_rd(t, off, size) & reg_get(ctx, reg, size, has_rex), size, 0, 0);
        ctx->Rip += n + 1 + modrm_len(op + 1);
        return true;
    case 0xF6: case 0xF7:                       /* test r/m, imm */
        if (((op[1] >> 3) & 7) != 0) break;
        ml = modrm_len(op + 1);
        if (op[0] == 0xF6) { size = 1; v = op[1 + ml]; ctx->Rip += n + 1 + ml + 1; }
        else if (size == 2) { v = *(const uint16_t *)(op + 1 + ml); ctx->Rip += n + 1 + ml + 2; }
        else { v = (uint64_t)(int64_t)*(const int32_t *)(op + 1 + ml); ctx->Rip += n + 1 + ml + 4; }
        set_flags(ctx, mmio_rd(t, off, size) & v, size, 0, 0);
        return true;
    case 0x80: case 0x81: case 0x83: {          /* alu r/m, imm */
        int alu_op = (op[1] >> 3) & 7;
        ml = modrm_len(op + 1);
        if (op[0] == 0x80) { size = 1; v = op[1 + ml]; ml += 1; }
        else if (op[0] == 0x83) { v = (uint64_t)(int64_t)(int8_t)op[1 + ml]; ml += 1; }
        else if (size == 2) { v = *(const uint16_t *)(op + 1 + ml); ml += 2; }
        else { v = (uint64_t)(int64_t)*(const int32_t *)(op + 1 + ml); ml += 4; }
        int wb = alu(ctx, alu_op, mmio_rd(t, off, size), v, size, &r);
        if (wb < 0) break;
        if (wb) mmio_wr(t, off, r, size);
        ctx->Rip += n + 1 + ml;
        return true;
    }
    default: {
        /* alu r/m, r (00/01 add, 08/09 or, 20/21 and, 28/29 sub, 30/31 xor,
         * 38/39 cmp) and alu r, r/m (02/03, 0A/0B, 22/23, 2A/2B, 32/33, 3A/3B) */
        uint8_t o = op[0];
        if (o < 0x40 && (o & 7) <= 3 && ((o >> 3) == 0 || (o >> 3) == 1
                || (o >> 3) == 4 || (o >> 3) == 5 || (o >> 3) == 6 || (o >> 3) == 7)) {
            int alu_op = o >> 3, to_reg = o & 2;
            if (!(o & 1)) size = 1;
            reg = ((op[1] >> 3) & 7) | rex_r;
            uint64_t m = mmio_rd(t, off, size), rv = reg_get(ctx, reg, size, has_rex);
            int wb = to_reg ? alu(ctx, alu_op, rv, m, size, &r)
                            : alu(ctx, alu_op, m, rv, size, &r);
            if (wb < 0) break;
            if (wb) {
                if (to_reg) reg_set(ctx, reg, size, has_rex, r);
                else mmio_wr(t, off, r, size);
            }
            ctx->Rip += n + 1 + modrm_len(op + 1);
            return true;
        }
        break;
    }
    }
    return false;
}
