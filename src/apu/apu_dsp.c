/*
 * MCPX APU DSP (GP/EP) - Global Processor and Encode Processor
 *
 * Adapted from xemu hw/xbox/mcpx/apu/dsp/gp_ep.c at commit
 * f9b14039e5bb56ae2d8f028e31e7cc19f13f7e12 (unmodified reference copy:
 * src/apu/dsp/upstream_ref/gp_ep.c; provenance: src/apu/dsp/PROVENANCE.md).
 * Replaces the earlier passthrough stub, whose DSPs never executed anything:
 * DirectSound hands the GP a command and waits for the GP program to answer
 * it, so a title waiting on the stub waited forever (SCDA sub_004D1D8F,
 * docs Update 342).
 *
 * Local adaptations (keep in sync with PROVENANCE.md):
 *  - GNU range cases -> explicit range checks (MSVC).
 *  - scatter_gather_rw: SGE entries are resolved as guest addresses
 *    (apu_phys_ptr/apu_phys_valid in apu_shim.h -- our MmGetPhysicalAddress
 *    returns the guest VA), not as offsets into 64 MB of RAM; an out-of-range
 *    transfer is reported and skipped instead of asserting on a QEMU
 *    MemoryRegion size. No dirty tracking (we have none).
 *  - g_config (xemu settings) is defined here: use_dsp from RECOMP_APU_USE_DSP,
 *    JIT always off.
 *  - GP/EP MMIO is exported as functions for apu_core.c's dispatcher instead
 *    of being registered as QEMU memory regions.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/* Upstream (QEMU) code relies on assert() as a runtime check and QEMU never
 * builds with NDEBUG; keep that here (see dsp/shim/qemu/osdep.h). */
#undef NDEBUG
#include <assert.h>

#include "apu_state.h"
#include "fpconv.h"
#include "ui/xemu-settings.h"
#include "apu_surround.h"

#include <stdlib.h>
#include <string.h>

XemuSettings g_config = {
    .audio = {
        .volume_limit = 1.0f,
        .hrtf = false,
        .vp = { .num_workers = 1 },
        .use_dsp = false,
        .use_dsp_jit = false,
    },
};

static const int16_t ep_silence[256][2] = { 0 };

#define IN_RANGE(a, base, words) ((a) >= (base) && (a) < (base) + (words) * 4)

/* Observation only (docs Update 343): which GP/EP registers the title uses,
 * and whether the DSPs leave reset and execute. Reset-register changes are
 * always logged (they are rare); everything else needs RECOMP_DSP_TRACE. */
static int dsp_trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DSP_TRACE");
        on = (e && *e && *e != '0') ? 1 : 0;
    }
    return on;
}

/* Opt-in observation at three distinct stages. Nonzero VP bins are not
 * evidence of six *output* channels; GP/EP must also execute and emit data. */
static int surround_probe_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_SURROUND_PROBE");
        on = e && *e && *e != '0';
    }
    return on;
}

static void surround_probe_fifo(const char *unit, unsigned index,
                                const uint8_t *ptr, size_t len)
{
    static uint64_t bytes[2][4], nonzero[2][4], sync[2][4];
    static uint8_t previous[2][4];
    static uint64_t total;
    unsigned u = unit[0] == 'E';
    if (!surround_probe_on() || index >= 4)
        return;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = ptr[i];
        nonzero[u][index] += b != 0;
        sync[u][index] += (previous[u][index] == 0x0b && b == 0x77) ||
                          (previous[u][index] == 0x77 && b == 0x0b);
        previous[u][index] = b;
    }
    bytes[u][index] += len;
    total += len;
    if (total >= 192000) {
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned j = 0; j < 4; ++j)
                if (bytes[side][j]) {
                    fprintf(stderr, "[SURROUND-FIFO] %s%u bytes=%llu nonzero=%llu ac3sync=%llu\n",
                            side ? "EP" : "GP", j,
                            (unsigned long long)bytes[side][j],
                            (unsigned long long)nonzero[side][j],
                            (unsigned long long)sync[side][j]);
                    bytes[side][j] = nonzero[side][j] = sync[side][j] = 0;
                }
        total = 0;
    }
}

static void surround_probe_bins(float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    static unsigned frames;
    static unsigned nonzero[NUM_MIXBINS];
    static float peak[NUM_MIXBINS];
    if (!surround_probe_on())
        return;
    for (unsigned bin = 0; bin < NUM_MIXBINS; ++bin)
        for (unsigned sample = 0; sample < NUM_SAMPLES_PER_FRAME; ++sample) {
            float v = mixbins[bin][sample];
            if (v < 0) v = -v;
            nonzero[bin] += v > 0.00001f;
            if (v > peak[bin]) peak[bin] = v;
        }
    if (++frames >= 1500) {
        fprintf(stderr, "[SURROUND-VP] frames=%u", frames);
        for (unsigned bin = 0; bin < NUM_MIXBINS; ++bin) {
            if (nonzero[bin])
                fprintf(stderr, " B%u=%u/%.4f", bin, nonzero[bin], peak[bin]);
            nonzero[bin] = 0;
            peak[bin] = 0;
        }
        fprintf(stderr, "\n");
        frames = 0;
    }
}

static void dsp_trace_reg(const char *who, int is_write, hwaddr addr,
                          uint64_t val)
{
    static unsigned n;
    if (!dsp_trace_on() || n >= 400)
        return;
    n++;
    fprintf(stderr, "[DSPREG] %s %s 0x%04X = 0x%08X\n", who,
            is_write ? "write" : "read ", (unsigned)addr, (unsigned)val);
}

/* Diagnostics (docs Update 344): with RECOMP_DSP_TRACE, an abort (e.g. the
 * interpreter's "Illegal dsp instruction" assert) prints the GP/EP PC history
 * and core registers and writes P/X memory next to the executable. P memory
 * is also saved right after every GP bootstrap, to tell a program that was
 * loaded wrong from one that went wrong later. */
#include <signal.h>

unsigned recomp_dsp_pc_ring_get(int is_gp, uint32_t *out, unsigned max);
void recomp_dsp_disasm_ring_dump(FILE *f);
extern int g_recomp_dsp_disasm;
extern int g_recomp_dsp_cur_is_gp;

static MCPXAPUState *g_diag_apu;

static void dsp_diag_write(const char *name, const uint32_t *words,
                           size_t count)
{
    FILE *f = fopen(name, "wb");
    if (!f)
        return;
    fwrite(words, sizeof(uint32_t), count, f);
    fclose(f);
    fprintf(stderr, "[DSPDIAG] wrote %s (%u words)\n", name, (unsigned)count);
}

static void dsp_diag_dump_one(const char *who, DSPState *dsp, int is_gp)
{
    uint32_t pcs[256];
    unsigned n = recomp_dsp_pc_ring_get(is_gp, pcs, 256);
    char name[64];

    dsp_sync_to_vm(dsp);
    fprintf(stderr, "[DSPDIAG] %s pc=0x%04X cur_inst=0x%06X sr=0x%06X "
            "sp=0x%02X ssh=0x%06X ssl=0x%06X\n", who, dsp->core.pc,
            dsp->core.cur_inst, dsp->core.registers[DSP_REG_SR],
            dsp->core.registers[DSP_REG_SP],
            dsp->core.stack[0][dsp->core.registers[DSP_REG_SP] & 0xF],
            dsp->core.stack[1][dsp->core.registers[DSP_REG_SP] & 0xF]);
    fprintf(stderr, "[DSPDIAG] %s last %u PCs (oldest first):", who, n);
    for (unsigned i = 0; i < n; i++)
        fprintf(stderr, "%s%04X", (i % 16) ? " " : "\n[DSPDIAG]   ", pcs[i]);
    fprintf(stderr, "\n");
    snprintf(name, sizeof(name), "dsp_%s_pram_abort.bin", who);
    dsp_diag_write(name, dsp->core.pram, DSP_PRAM_SIZE);
    snprintf(name, sizeof(name), "dsp_%s_xram_abort.bin", who);
    dsp_diag_write(name, dsp->core.xram, DSP_XRAM_SIZE);
    /* Opt-in exact scalar/register/Y-memory snapshot for continuing the
     * first failing EP instruction in the isolated DSP56300 FFI harness.
     * The legacy P/X dumps and normal runtime behaviour remain unchanged. */
    {
        const char *full = getenv("RECOMP_DSP_FULL_DUMP");
        if (full && *full && *full != '0') {
            FILE *f;
            snprintf(name, sizeof(name), "dsp_%s_core_abort.bin", who);
            f = fopen(name, "wb");
            if (f) {
                fwrite(&dsp->core, 1, sizeof(dsp->core), f);
                fclose(f);
                fprintf(stderr, "[DSPDIAG] wrote %s (%u bytes)\n", name,
                        (unsigned)sizeof(dsp->core));
            }
        }
    }
}

static void dsp_diag_on_abort(int sig)
{
    (void)sig;
    if (g_diag_apu) {
        dsp_diag_dump_one("gp", g_diag_apu->gp.dsp, 1);
        dsp_diag_dump_one("ep", g_diag_apu->ep.dsp, 0);
    }
    if (g_recomp_dsp_disasm)
        recomp_dsp_disasm_ring_dump(stderr);
    fflush(stderr);
}

static void dsp_diag_after_bootstrap(DSPState *dsp)
{
    static unsigned boots;
    char name[64];
    dsp_sync_to_vm(dsp);
    snprintf(name, sizeof(name), "dsp_gp_pram_boot%u.bin", ++boots);
    dsp_diag_write(name, dsp->core.pram, DSP_PRAM_SIZE);
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    static int last_known_dsp_pref = -1;
    static int last_known_jit_pref = -1;

    /* The surround path owns the GP/EP and the monitor point (EP). */
    if (apu_surround_active())
        return;

    if (last_known_dsp_pref != (int)g_config.audio.use_dsp) {
        if (g_config.audio.use_dsp) {
            d->monitor.point = MCPX_APU_DEBUG_MON_GP_OR_EP;
            d->gp.realtime = true;
            d->ep.realtime = true;
        } else {
            d->monitor.point = MCPX_APU_DEBUG_MON_VP;
            d->gp.realtime = false;
            d->ep.realtime = false;
        }
        last_known_dsp_pref = g_config.audio.use_dsp;
    }

    if (last_known_jit_pref != (int)g_config.audio.use_dsp_jit) {
        dsp_set_engine(d->gp.dsp, g_config.audio.use_dsp_jit);
        dsp_set_engine(d->ep.dsp, g_config.audio.use_dsp_jit);
        last_known_jit_pref = g_config.audio.use_dsp_jit;
    }
}

static void scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                              unsigned int max_sge, uint8_t *ptr, uint32_t addr,
                              size_t len, bool dir)
{
    unsigned int page_entry = addr / TARGET_PAGE_SIZE;
    unsigned int offset_in_page = addr % TARGET_PAGE_SIZE;
    unsigned int bytes_to_copy = TARGET_PAGE_SIZE - offset_in_page;

    while (len > 0) {
        assert(page_entry <= max_sge);

        uint32_t prd_address = ldl_le_phys(&address_space_memory,
                                           sge_base + page_entry * 8 + 0);

        hwaddr paddr = prd_address + offset_in_page;

        if (bytes_to_copy > len) {
            bytes_to_copy = (unsigned int)len;
        }

        /* SGE entries are guest addresses in this runtime (apu_phys_ptr). */
        if (!apu_phys_valid(paddr, bytes_to_copy)) {
            static int reported;
            if (reported++ < 8)
                fprintf(stderr, "[APU] DSP scatter-gather outside guest RAM: "
                        "sge_base=0x%08llX entry=%u paddr=0x%08llX len=%u\n",
                        (unsigned long long)sge_base, page_entry,
                        (unsigned long long)paddr, bytes_to_copy);
        } else if (dir) {
            memcpy(apu_phys_ptr(paddr), ptr, bytes_to_copy);
        } else {
            memcpy(ptr, apu_phys_ptr(paddr), bytes_to_copy);
        }

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration, we are page aligned */
        page_entry += 1;
        bytes_to_copy = TARGET_PAGE_SIZE;
        offset_in_page = 0;
    }
}

static void gp_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    scatter_gather_rw(d, d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE],
                      ptr, addr, len, dir);
}

static void ep_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                          bool dir)
{
    MCPXAPUState *d = opaque;
    scatter_gather_rw(d, d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE],
                      ptr, addr, len, dir);
}

static uint32_t circular_scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                                           unsigned int max_sge, uint8_t *ptr,
                                           uint32_t base, uint32_t end,
                                           uint32_t cur, size_t len, bool dir)
{
    while (len > 0) {
        unsigned int bytes_to_copy = end - cur;

        if (bytes_to_copy > len) {
            bytes_to_copy = (unsigned int)len;
        }

        assert((cur >= base) && ((cur + bytes_to_copy) <= end));
        scatter_gather_rw(d, sge_base, max_sge, ptr, cur, bytes_to_copy, dir);

        ptr += bytes_to_copy;
        len -= bytes_to_copy;

        /* After the first iteration we might have to wrap */
        cur += bytes_to_copy;
        if (cur >= end) {
            assert(cur == end);
            cur = base;
        }
    }

    return cur;
}

static void gp_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        assert(index < GP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPOFCUR0 + 0x10 * index;
    } else {
        assert(index < GP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_GPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    if (dir)
        surround_probe_fifo("GP", index, ptr, len);

    /* DSP hangs if current >= end; but forces current >= base */
    assert(cur < end);
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_GPFADDR], d->regs[NV_PAPU_GPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static bool ep_sink_samples(MCPXAPUState *d, uint8_t *ptr, size_t len)
{
    if (d->monitor.point == MCPX_APU_DEBUG_MON_AC97) {
        return false;
    } else if ((d->monitor.point == MCPX_APU_DEBUG_MON_EP) ||
        (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP)) {
        assert(len == sizeof(d->monitor.frame_buf));
        memcpy(d->monitor.frame_buf, ptr, len);
    }

    return true;
}

static void ep_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        assert(index < EP_OUTPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPOFCUR0 + 0x10 * index;
    } else {
        assert(index < EP_INPUT_FIFO_COUNT);
        base = GET_MASK(d->regs[NV_PAPU_EPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    /* Inspect before EP0's host-monitor sink substitutes silence. */
    if (dir)
        surround_probe_fifo("EP", index, ptr, len);

    if (dir && index == 0) {
        bool did_sink = ep_sink_samples(d, ptr, len);
        if (did_sink) {
            /* Since we are sinking, push silence out */
            assert(len <= sizeof(ep_silence));
            ptr = (uint8_t*)ep_silence;
        }
    }

    /* DSP hangs if current >= end; but forces current >= base */
    if (cur >= end) {
        cur = cur % (end - base);
    }
    if (cur < base) {
        cur = base;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_EPFADDR], d->regs[NV_PAPU_EPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

static void proc_rst_write(DSPState *dsp, uint32_t oldval, uint32_t val)
{
    if (!(val & NV_PAPU_GPRST_GPRST) || !(val & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_reset(dsp);
    } else if (
        (!(oldval & NV_PAPU_GPRST_GPRST) || !(oldval & NV_PAPU_GPRST_GPDSPRST))
        && ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST))) {
        dsp_bootstrap(dsp);
    }
}

/* Global Processor - programmable DSP.  addr is relative to APU+0x30000. */
uint64_t mcpx_apu_gp_read(MCPXAPUState *d, hwaddr addr, unsigned int size)
{
    uint64_t r = 0;

    if (apu_surround_active())
        return apu_surround_gp_read(d, addr);

    assert(size == 4);
    assert(addr % 4 == 0);

    if (IN_RANGE(addr, NV_PAPU_GPXMEM, 0x1000)) {
        r = dsp_read_memory(d->gp.dsp, 'X', (uint32_t)(addr - NV_PAPU_GPXMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPMIXBUF, 0x400)) {
        r = dsp_read_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE +
                            (uint32_t)(addr - NV_PAPU_GPMIXBUF) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPYMEM, 0x800)) {
        r = dsp_read_memory(d->gp.dsp, 'Y', (uint32_t)(addr - NV_PAPU_GPYMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPPMEM, 0x1000)) {
        r = dsp_read_memory(d->gp.dsp, 'P', (uint32_t)(addr - NV_PAPU_GPPMEM) / 4);
    } else {
        r = d->gp.regs[addr];
    }
    return r;
}

void mcpx_apu_gp_write(MCPXAPUState *d, hwaddr addr, uint64_t val,
                       unsigned int size)
{
    if (apu_surround_active()) {
        apu_surround_gp_write(d, addr, val);
        return;
    }
    apu_lock_for_mmio(d);

    assert(size == 4);
    assert(addr % 4 == 0);

    if (IN_RANGE(addr, NV_PAPU_GPXMEM, 0x1000)) {
        dsp_write_memory(d->gp.dsp, 'X', (uint32_t)(addr - NV_PAPU_GPXMEM) / 4,
                         (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_GPMIXBUF, 0x400)) {
        dsp_write_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE +
                         (uint32_t)(addr - NV_PAPU_GPMIXBUF) / 4, (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_GPYMEM, 0x800)) {
        dsp_write_memory(d->gp.dsp, 'Y', (uint32_t)(addr - NV_PAPU_GPYMEM) / 4,
                         (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_GPPMEM, 0x1000)) {
        dsp_write_memory(d->gp.dsp, 'P', (uint32_t)(addr - NV_PAPU_GPPMEM) / 4,
                         (uint32_t)val);
    } else if (addr == NV_PAPU_GPRST) {
        fprintf(stderr, "[APU] GP reset register 0x%X -> 0x%X\n",
                d->gp.regs[NV_PAPU_GPRST], (unsigned)val);
        bool diag_boot = dsp_trace_on() && (val & 3) == 3
                         && (d->gp.regs[NV_PAPU_GPRST] & 3) != 3;
        if (diag_boot) {
            /* Which scratch the GP boots from (Update 343: these are guest
             * addresses, see apu_phys_ptr). */
            uint32_t sa = d->regs[NV_PAPU_GPSADDR];
            uint32_t p0 = ldl_le_phys(&address_space_memory, sa);
            fprintf(stderr, "[DSPBOOT] GPSADDR=0x%08X GPSMAXSGE=%u "
                    "sge0=0x%08X sge1=0x%08X scratch word0=0x%08X\n",
                    sa, d->regs[NV_PAPU_GPSMAXSGE], p0,
                    ldl_le_phys(&address_space_memory, sa + 8),
                    apu_phys_valid(p0, 4)
                        ? ldl_le_phys(&address_space_memory, p0) : 0u);
        }
        proc_rst_write(d->gp.dsp, d->gp.regs[NV_PAPU_GPRST], (uint32_t)val);
        d->gp.regs[NV_PAPU_GPRST] = (uint32_t)val;
        if (diag_boot)
            dsp_diag_after_bootstrap(d->gp.dsp);
    } else {
        dsp_trace_reg("GP", 1, addr, val);
        d->gp.regs[addr] = (uint32_t)val;
    }

    qemu_mutex_unlock(&d->lock);
}

/* Encode Processor - encoding DSP.  addr is relative to APU+0x50000. */
uint64_t mcpx_apu_ep_read(MCPXAPUState *d, hwaddr addr, unsigned int size)
{
    uint64_t r = 0;

    if (apu_surround_active())
        return apu_surround_ep_read(d, addr);

    assert(size == 4);
    assert(addr % 4 == 0);

    if (IN_RANGE(addr, NV_PAPU_EPXMEM, 0xC00)) {
        r = dsp_read_memory(d->ep.dsp, 'X', (uint32_t)(addr - NV_PAPU_EPXMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_EPYMEM, 0x100)) {
        r = dsp_read_memory(d->ep.dsp, 'Y', (uint32_t)(addr - NV_PAPU_EPYMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_EPPMEM, 0x1000)) {
        r = dsp_read_memory(d->ep.dsp, 'P', (uint32_t)(addr - NV_PAPU_EPPMEM) / 4);
    } else {
        r = d->ep.regs[addr];
    }
    return r;
}

void mcpx_apu_ep_write(MCPXAPUState *d, hwaddr addr, uint64_t val,
                       unsigned int size)
{
    if (apu_surround_active()) {
        apu_surround_ep_write(d, addr, val);
        return;
    }
    apu_lock_for_mmio(d);

    assert(size == 4);
    assert(addr % 4 == 0);

    if (IN_RANGE(addr, NV_PAPU_EPXMEM, 0xC00)) {
        dsp_write_memory(d->ep.dsp, 'X', (uint32_t)(addr - NV_PAPU_EPXMEM) / 4,
                         (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_EPYMEM, 0x100)) {
        dsp_write_memory(d->ep.dsp, 'Y', (uint32_t)(addr - NV_PAPU_EPYMEM) / 4,
                         (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_EPPMEM, 0x1000)) {
        dsp_write_memory(d->ep.dsp, 'P', (uint32_t)(addr - NV_PAPU_EPPMEM) / 4,
                         (uint32_t)val);
    } else if (addr == NV_PAPU_EPRST) {
        fprintf(stderr, "[APU] EP reset register 0x%X -> 0x%X\n",
                d->ep.regs[NV_PAPU_EPRST], (unsigned)val);
        if (dsp_trace_on() && (val & 3) == 3
                && (d->ep.regs[NV_PAPU_EPRST] & 3) != 3) {
            /* Where the EP boots from, and its module table (P:$0100..,
             * scratch words 0x100..) -- Update 344: module 6's length
             * word P:$010D differs from xemu. */
            uint32_t sa = d->regs[NV_PAPU_EPSADDR];
            uint32_t p0 = ldl_le_phys(&address_space_memory, sa);
            fprintf(stderr, "[DSPBOOT] EPSADDR=0x%08X EPSMAXSGE=%u sge0=0x%08X "
                    "sge1=0x%08X\n", sa, d->regs[NV_PAPU_EPSMAXSGE], p0,
                    ldl_le_phys(&address_space_memory, sa + 8));
            if (apu_phys_valid(p0 + 0x100 * 4, 0x30 * 4)) {
                fprintf(stderr, "[DSPBOOT] EP scratch 0x100..0x12F @0x%08X:",
                        p0 + 0x100 * 4);
                for (unsigned i = 0; i < 0x30; i++)
                    fprintf(stderr, "%s%06X", (i % 16) ? " " : "\n[DSPBOOT]   ",
                            ldl_le_phys(&address_space_memory,
                                        p0 + (0x100 + i) * 4));
                fprintf(stderr, "\n");
            }
        }
        proc_rst_write(d->ep.dsp, d->ep.regs[NV_PAPU_EPRST], (uint32_t)val);
        d->ep.regs[NV_PAPU_EPRST] = (uint32_t)val;
        d->ep_frame_div = 0; /* FIXME (upstream): Still unsure about frame sync */
    } else {
        dsp_trace_reg("EP", 1, addr, val);
        d->ep.regs[addr] = (uint32_t)val;
    }

    qemu_mutex_unlock(&d->lock);
}

void mcpx_apu_dsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    surround_probe_bins(mixbins);
    /* Write VP results to the GP DSP MIXBUF */
    for (int mixbin = 0; mixbin < NUM_MIXBINS; mixbin++) {
        uint32_t base = GP_DSP_MIXBUF_BASE + mixbin * NUM_SAMPLES_PER_FRAME;
        for (int sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
            dsp_write_memory(d->gp.dsp, 'X', base + sample,
                             float_to_24b(mixbins[mixbin][sample]));
        }
    }

    bool ep_enabled = (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPRST) &&
                      (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST);

    /* Run GP */
    if ((d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPRST) &&
        (d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        dsp_start_frame(d->gp.dsp);
        dsp_set_halt_requested(d->gp.dsp, false);
        dsp_set_cycle_count(d->gp.dsp, 0);
        do {
            g_recomp_dsp_cur_is_gp = 1; /* diagnostics only */
            dsp_run(d->gp.dsp, 1000);
        } while (!dsp_get_halt_requested(d->gp.dsp) && d->gp.realtime);
        g_dbg.gp.cycles = dsp_get_cycle_count(d->gp.dsp);

        {
            static unsigned frames;
            if (frames++ % 3000 == 0 && dsp_trace_on()) {
                dsp_sync_to_vm(d->gp.dsp);
                fprintf(stderr, "[DSPRUN] GP frame %u pc=0x%04X cycles=%u "
                        "halt=%d\n", frames, d->gp.dsp->core.pc,
                        g_dbg.gp.cycles,
                        (int)dsp_get_halt_requested(d->gp.dsp));
            }
        }

        if ((d->monitor.point == MCPX_APU_DEBUG_MON_GP) ||
            (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP && !ep_enabled)) {
            int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                uint32_t l = dsp_read_memory(d->gp.dsp, 'X', 0x1400 + i);
                d->monitor.frame_buf[off + i][0] = l >> 8;
                uint32_t r =
                    dsp_read_memory(d->gp.dsp, 'X', 0x1400 + 1 * 0x20 + i);
                d->monitor.frame_buf[off + i][1] = r >> 8;
            }
        }
    }

    /* Run EP */
    if ((d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPRST) &&
        (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST)) {
        if (d->ep_frame_div % 8 == 0) {
            dsp_start_frame(d->ep.dsp);
            dsp_set_halt_requested(d->ep.dsp, false);
            dsp_set_cycle_count(d->ep.dsp, 0);
            do {
                g_recomp_dsp_cur_is_gp = 0; /* diagnostics only */
                dsp_run(d->ep.dsp, 1000);
            } while (!dsp_get_halt_requested(d->ep.dsp) && d->ep.realtime);
            g_dbg.ep.cycles = dsp_get_cycle_count(d->ep.dsp);
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* xemu's audio.use_dsp setting: run the GP/EP until their program
     * halts every frame. Off by default there too. */
    const char *use_dsp = getenv("RECOMP_APU_USE_DSP");
    g_config.audio.use_dsp = use_dsp && *use_dsp && *use_dsp != '0';
    g_config.audio.use_dsp_jit = false;

    d->gp.dsp = dsp_init(d, gp_scratch_rw, gp_fifo_rw, true);
    dsp_set_halt_requested(d->gp.dsp, false);
    dsp_set_cycle_count(d->gp.dsp, 0);

    d->ep.dsp = dsp_init(d, ep_scratch_rw, ep_fifo_rw, false);
    dsp_set_halt_requested(d->ep.dsp, false);
    dsp_set_cycle_count(d->ep.dsp, 0);

    /* Until DSP is more performant, a switch to decide whether or not we should
     * use the full audio pipeline or not.
     */
    mcpx_apu_update_dsp_preference(d);

    /* RECOMP_AUDIO_SURROUND=1 (docs Update 611): GP/EP on the JIT core with
     * AC-3 output; takes over the MMIO and frame paths below when active. */
    apu_surround_init(d);

    if (dsp_trace_on()) {
        const char *dis = getenv("RECOMP_DSP_DISASM");
        g_recomp_dsp_disasm = dis && *dis && *dis != '0';
        g_diag_apu = d;
        signal(SIGABRT, dsp_diag_on_abort);
    }

    fprintf(stderr, "[APU] DSP GP/EP: DSP56300 interpreter (xemu f9b14039), "
                    "realtime=%d\n", (int)g_config.audio.use_dsp);
}
