/* Faulting-instruction emulation for trapped device apertures on AArch64
 * hosts (SCDA ARM port, docs Update 677); the x86-64 decoder is
 * mmio_decode.c, the interface mmio_decode.h.
 *
 * AArch64 reaches memory only through loads and stores, so this covers the
 * integer LDR/STR family (unsigned offset, unscaled, pre/post-index, register
 * offset; byte/half/word/doubleword, sign-extending loads), LDP/STP pairs and
 * PRFM (skipped). 8-byte accesses are split into two 4-byte ones, as the
 * 32-bit bus does. SIMD loads/stores are not decoded. */
#include "mmio_decode.h"

#if defined(__aarch64__)

typedef struct {
    mmio_read_fn rd;
    mmio_write_fn wr;
    void *opaque;
} MmioTarget;

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
        v &= (1ULL << (size * 8)) - 1;
    if (size == 8) {
        t->wr(t->opaque, off, v & 0xFFFFFFFFu, 4);
        t->wr(t->opaque, off + 4, v >> 32, 4);
        return;
    }
    t->wr(t->opaque, off, v, size);
}

/* register 31 is the zero register as a data operand */
static uint64_t xreg(PCONTEXT ctx, int r) { return r == 31 ? 0 : ctx->X[r]; }
static void xset(PCONTEXT ctx, int r, uint64_t v) { if (r != 31) ctx->X[r] = v; }

/* base register: 31 is SP */
static void base_add(PCONTEXT ctx, int rn, int64_t delta)
{
    if (rn == 31) ctx->Sp += (uint64_t)delta;
    else ctx->X[rn] += (uint64_t)delta;
}

static int64_t sext(uint64_t v, int bits)
{
    uint64_t m = 1ULL << (bits - 1);
    v &= (1ULL << bits) - 1;
    return (int64_t)((v ^ m) - m);
}

/* One load: size in bytes, opc from the encoding (0 store handled outside):
 * opc 1 = zero-extending load, 2 = sign-extend to 64, 3 = sign-extend to 32. */
static void do_load(PCONTEXT ctx, const MmioTarget *t, uint32_t off, int size,
                    int opc, int rt)
{
    uint64_t v = mmio_rd(t, off, size);
    if (opc == 2)
        v = (uint64_t)sext(v, size * 8);
    else if (opc == 3)
        v = (uint64_t)(uint32_t)sext(v, size * 8);
    xset(ctx, rt, v);
}

bool mmio_decode_and_run(PCONTEXT ctx, uint32_t off, mmio_read_fn rd,
                         mmio_write_fn wr, void *opaque)
{
    const MmioTarget tgt = { rd, wr, opaque }, *t = &tgt;
    const uint32_t insn = *(const uint32_t *)(uintptr_t)ctx->Pc;
    const int rt = insn & 31, rn = (insn >> 5) & 31;

    /* ---- single register, integer (V = 0) ---- */
    if (((insn >> 27) & 7) == 7 && !((insn >> 26) & 1)) {
        const int size = 1 << ((insn >> 30) & 3);
        const int opc = (insn >> 22) & 3;
        const int sel = (insn >> 24) & 3;
        int64_t writeback = 0;

        if (sel == 1) {
            /* unsigned scaled offset: no writeback */
        } else if (sel == 0) {
            const int mode = (insn >> 10) & 3;
            if ((insn >> 21) & 1) {
                if (mode != 2)
                    return false;   /* atomics etc. */
                /* register offset: no writeback */
            } else if (mode == 1 || mode == 3) {
                writeback = sext(insn >> 12, 9);   /* post / pre-index */
            }
        } else {
            return false;
        }
        if (size == 8 && opc == 2)
            goto done;   /* PRFM: a hint */
        if (size == 4 && opc == 3)
            return false;   /* unallocated */
        if (opc == 0)
            mmio_wr(t, off, xreg(ctx, rt), size);
        else if (size == 8 || (size == 4 && opc == 1))
            do_load(ctx, t, off, size, 1, rt);
        else
            do_load(ctx, t, off, size, opc, rt);
        if (writeback)
            base_add(ctx, rn, writeback);
        goto done;
    }

    /* ---- register pair, integer (LDP/STP/LDPSW, V = 0) ---- */
    if (((insn >> 27) & 7) == 5 && !((insn >> 26) & 1)) {
        const int opc = (insn >> 30) & 3;
        const int load = (insn >> 22) & 1;
        const int idx = (insn >> 23) & 7;   /* 0 noalloc, 1 post, 2 offset, 3 pre */
        const int rt2 = (insn >> 10) & 31;
        int size;
        if (opc == 0) size = 4;
        else if (opc == 2) size = 8;
        else if (opc == 1 && load) size = 4;   /* LDPSW */
        else return false;
        if (idx > 3)
            return false;
        const int64_t imm = sext(insn >> 15, 7) * size;
        if (load) {
            const int sign = opc == 1 ? 2 : 1;
            do_load(ctx, t, off, size, sign, rt);
            do_load(ctx, t, off + size, size, sign, rt2);
        } else {
            mmio_wr(t, off, xreg(ctx, rt), size);
            mmio_wr(t, off + size, xreg(ctx, rt2), size);
        }
        if (idx == 1 || idx == 3)
            base_add(ctx, rn, imm);
        goto done;
    }
    return false;

done:
    ctx->Pc += 4;
    return true;
}

#endif /* __aarch64__ */
