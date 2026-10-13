/*
 * MCPX APU surround path: GP/EP on the DSP56300 JIT core, EP AC-3 output
 * decoded to 5.1 (docs Update 611). Opt-in: RECOMP_AUDIO_SURROUND=1.
 *
 * Adapted from the xemu surround branch (xemu PR #3047,
 * mborgerson/xemu feat/surround at 5b1c1404c55a8748143ab37e4141b6779f656703):
 *   hw/xbox/mcpx/apu/dsp/gp_ep.c  -> scatter-gather, FIFOs, GP/EP MMIO,
 *                                    the two DSP worker threads and the
 *                                    frame begin/gp/end split
 *   hw/xbox/mcpx/apu/spdif.c      -> IEC 61937 parser + liba52 decode
 *   hw/xbox/mcpx/apu/monitor.c    -> the auto monitor point (5.1 while the
 *                                    title encodes, else EP FIFO #0 stereo)
 * The DSP core and its DMA engine are xemu's dsp.c/dma.c in src/apu/dsp_jit
 * (renamed dspjit_*), the core itself dsp56300.dll.
 *
 * Local adaptations:
 *  - SGE entries are guest addresses here (apu_phys_ptr/apu_phys_valid, as
 *    in apu_dsp.c); an entry outside guest memory ends the transfer.
 *  - A FIFO cursor at or past END (upstream: assert) is reported and the
 *    transfer skipped instead of aborting the game.
 *  - Win32 threads/locks for the DSP workers; the frame thread's lock and
 *    lock_waiters are apu_core.c's (apu_lock_for_mmio).
 *  - Debug/trace infrastructure (SCHED report, EP snapshot, block profile,
 *    MIX_INJECT) is not ported; a compact [APU-SURROUND] status line every
 *    10 s and optional raw dumps replace it.
 *  - Output goes to XAudio2 as 6-channel PCM (apu_core.c), not SDL.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2026 Matt Borgerson
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

#include "apu_state.h"
#include "fpconv.h"
#include "apu_surround.h"
#include "apu_xaudio2.h"
#include "apu_passthrough.h"
#include "dsp_jit/dspjit_api.h"
#include "a52.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Guest logical processor, as xbox_GuestCpu (RECOMP_GUEST_CPU, docs Update
 * 646c); read here so this library links without the kernel. */
static DWORD_PTR apu_guest_mask(void)
{
    const char *e = getenv("RECOMP_GUEST_CPU");
    int lp = e ? atoi(e) : 0;
    return (DWORD_PTR)1 << ((lp > 0 && lp < 64) ? lp : 0);
}

#define IN_RANGE(a, base, words) ((a) >= (base) && (a) < (base) + (words) * 4)

static int s_active;
/* RECOMP_AUDIO_SURROUND=passthrough: the EP's IEC 61937 stream goes to an
 * S/PDIF/HDMI output unchanged (apu_passthrough.c, docs Update 612). */
static int s_passthrough;
/* RECOMP_AUDIO_SURROUND=stereo (docs Update 616): the same GP/EP path with
 * the console set to stereo -- the EP's analog output (FIFO #0) is played
 * through XAudio2 as is, the mix a console's stereo output carries (GP
 * effects, 3D panning), instead of the VP monitor tap. */
static int s_stereo;
static DSPJitState *s_gp, *s_ep;
static const int16_t ep_silence[256][2] = { 0 };

int apu_surround_active(void)
{
    return s_active;
}

int mcpx_apu_surround_active(void)
{
    return s_active && !s_stereo;
}

int apu_surround_six_channels(void)
{
    return s_active && !s_stereo && !s_passthrough;
}

/* Status counters for the [APU-SURROUND] line. Fields the workers touch
 * are updated with interlocked adds; the rest are frame-thread only. */
static struct {
    uint64_t frames, kicks, exit_wall, gp_late_finish;
    volatile __int64 waiter_breaks, gp_catchup, ep_catchup;
    uint64_t fifo1_writes, fifo1_bytes, fifo0_blocks, sge_errors;
    uint64_t gp_halts_prev, ep_halts_prev;
    int64_t last_report_us;
} g_stat;

/* ============================================================
 * Scatter-gather (gp_ep.c)
 * ============================================================ */

/* Walk an SGE table for a scratch or FIFO transfer: one 8-byte entry per
 * page in guest memory, the first word being the page's address. An entry
 * past `max_sge` or a page outside guest memory ends the transfer. */
static void scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                              unsigned int max_sge, uint8_t *ptr, uint32_t addr,
                              size_t len, bool dir)
{
    unsigned int page_entry = addr / TARGET_PAGE_SIZE;
    unsigned int offset_in_page = addr % TARGET_PAGE_SIZE;
    unsigned int bytes_to_copy = TARGET_PAGE_SIZE - offset_in_page;

    (void)d;
    while (len > 0) {
        if (page_entry > max_sge) {
            return;
        }
        hwaddr entry = sge_base + page_entry * 8;
        if (!apu_phys_valid(entry, 4)) {
            g_stat.sge_errors++;
            return;
        }
        uint32_t prd_address = ldl_le_phys(address_space_memory, entry);
        hwaddr paddr = prd_address + offset_in_page;

        if (bytes_to_copy > len) {
            bytes_to_copy = (unsigned int)len;
        }
        if (!apu_phys_valid(paddr, bytes_to_copy)) {
            g_stat.sge_errors++;
            return;
        }
        if (dir) {
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

void dspjit_ep_eol_clear_hook(DSPJitState *dsp)
{
    (void)dsp; /* xemu's MCPX_EP_SNAPSHOT trigger; not ported */
}

/* The FIFO descriptor walk as probed on the EP (the DMA corpus, bank
 * b12): the cursor is used where it stands, below the base included,
 * advances by the transfer, and wraps to the base when it reaches the end
 * from below. BASE and END hold 256-byte units; the cursor keeps every bit. */
#define FIFO_BOUND_MASK 0xffff00

static uint32_t circular_scatter_gather_rw(MCPXAPUState *d, hwaddr sge_base,
                                           unsigned int max_sge, uint8_t *ptr,
                                           uint32_t base, uint32_t end,
                                           uint32_t cur, size_t len, bool dir)
{
    base &= FIFO_BOUND_MASK;
    end &= FIFO_BOUND_MASK;
    while (len > 0) {
        size_t bytes_to_copy = len;
        bool crosses = false;

        if (cur < end) {
            crosses = len >= end - cur;
            bytes_to_copy = MIN(len, (size_t)(end - cur));
        }

        scatter_gather_rw(d, sge_base, max_sge, ptr, cur, bytes_to_copy, dir);

        ptr += bytes_to_copy;
        len -= bytes_to_copy;
        cur = crosses ? base : cur + (uint32_t)bytes_to_copy;
    }

    return cur;
}

/* ============================================================
 * S/PDIF tap: IEC 61937 parser + AC-3 decode (spdif.c)
 * ============================================================ */

#define SPDIF_MAX_BURST 4096   /* AC-3 frames are at most 3840 bytes */
#define SPDIF_PCM_FRAMES 16384 /* ~341 ms of decoded 5.1 */

enum {
    IEC_PA0, IEC_PA1, IEC_PB0, IEC_PB1, IEC_PC0, IEC_PC1, IEC_PD0, IEC_PD1,
    IEC_PAYLOAD,
};

static struct {
    int state;
    unsigned type;
    unsigned length; /* payload bytes */
    unsigned pos;
    uint8_t burst[SPDIF_MAX_BURST];

    int16_t pcm[SPDIF_PCM_FRAMES][6]; /* FL FR FC LFE BL BR */
    unsigned head, tail;              /* frames; head == tail is empty */

    a52_state_t *a52;
    bool decoding;
    /* Output pulls so far, and the pull at which the last valid burst was
     * seen: the stream is present while the gap is short. */
    unsigned pulls, last_valid_pull;

    uint64_t frames, rejected, underruns, overruns;
} s_spdif;

/* AC-3 CRC-16 (x^16 + x^15 + x^2 + 1). crc2 at the end of a frame is
 * chosen so the CRC of everything after the syncword is zero. */
static uint16_t ac3_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x8005)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void pcm_push(const int16_t v[6])
{
    unsigned next = (s_spdif.head + 1) % SPDIF_PCM_FRAMES;
    if (next == s_spdif.tail) {
        /* Full: drop the oldest so the output stays near real time. */
        s_spdif.tail = (s_spdif.tail + 1) % SPDIF_PCM_FRAMES;
        s_spdif.overruns++;
    }
    memcpy(s_spdif.pcm[s_spdif.head], v, sizeof(s_spdif.pcm[0]));
    s_spdif.head = next;
}

static int16_t to_s16(float v)
{
    v *= 32767.0f;
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (int16_t)lrintf(v);
}

static void decode_burst(void)
{
    /* Burst type 1 is AC-3 (IEC 61937-3); the payload is big-endian bytes
     * packed into little-endian 16-bit words. */
    if (s_spdif.type != 1 || s_spdif.length < 8 || (s_spdif.length & 1)) {
        s_spdif.rejected++;
        return;
    }
    uint8_t *f = s_spdif.burst;
    for (unsigned i = 0; i + 1 < s_spdif.length; i += 2) {
        uint8_t t = f[i];
        f[i] = f[i + 1];
        f[i + 1] = t;
    }

    if (!s_spdif.a52) {
        s_spdif.a52 = a52_init(0);
        if (!s_spdif.a52) {
            s_spdif.rejected++;
            return;
        }
        /* Decode what the encoder produced, without the receiver-side
         * dynamic range compression liba52 applies by default. */
        a52_dynrng(s_spdif.a52, NULL, NULL);
    }
    int flags, sample_rate, bit_rate;
    int frame_len = a52_syncinfo(f, &flags, &sample_rate, &bit_rate);
    if (frame_len <= 0 || (unsigned)frame_len > s_spdif.length ||
        ac3_crc16(f + 2, frame_len - 2) != 0) {
        s_spdif.rejected++;
        return;
    }
    s_spdif.last_valid_pull = s_spdif.pulls;
    if (!s_spdif.decoding) {
        return;
    }
    /* Ask for discrete 3/2 + LFE; liba52 hands back the layout it will
     * actually produce (it never upmixes), with LFE first when present,
     * then the full-bandwidth channels in bitstream order. */
    flags = A52_3F2R | A52_LFE | A52_ADJUST_LEVEL;
    level_t level = 1;
    if (a52_frame(s_spdif.a52, f, &flags, &level, 0)) {
        s_spdif.rejected++;
        return;
    }
    enum { FL, FR, FC, LFE, BL, BR, SURR_MONO = 8 };
    static const int8_t layout[16][5] = {
        [A52_CHANNEL] = { FL, FR, -1, -1, -1 },
        [A52_MONO] = { FC, -1, -1, -1, -1 },
        [A52_STEREO] = { FL, FR, -1, -1, -1 },
        [A52_3F] = { FL, FC, FR, -1, -1 },
        [A52_2F1R] = { FL, FR, SURR_MONO, -1, -1 },
        [A52_3F1R] = { FL, FC, FR, SURR_MONO, -1 },
        [A52_2F2R] = { FL, FR, BL, BR, -1 },
        [A52_3F2R] = { FL, FC, FR, BL, BR },
        [A52_CHANNEL1] = { FC, -1, -1, -1, -1 },
        [A52_CHANNEL2] = { FC, -1, -1, -1, -1 },
        [A52_DOLBY] = { FL, FR, -1, -1, -1 },
        [11] = { -1, -1, -1, -1, -1 }, [12] = { -1, -1, -1, -1, -1 },
        [13] = { -1, -1, -1, -1, -1 }, [14] = { -1, -1, -1, -1, -1 },
        [15] = { -1, -1, -1, -1, -1 },
    };
    const int8_t *map = layout[flags & A52_CHANNEL_MASK];
    bool has_lfe = (flags & A52_LFE) != 0;
    for (int blk = 0; blk < 6; blk++) {
        if (a52_block(s_spdif.a52)) {
            s_spdif.rejected++;
            return;
        }
        const sample_t *out = a52_samples(s_spdif.a52);
        for (int i = 0; i < 256; i++) {
            int16_t v[6] = { 0 };
            const sample_t *ch = out;
            if (has_lfe) {
                v[LFE] = to_s16(ch[i]);
                ch += 256;
            }
            for (int k = 0; k < 5 && map[k] >= 0; k++, ch += 256) {
                int16_t x = to_s16(ch[i]);
                if (map[k] == SURR_MONO) {
                    v[BL] = x;
                    v[BR] = x;
                } else {
                    v[map[k]] = x;
                }
            }
            pcm_push(v);
        }
    }
    s_spdif.frames++;
}

static void spdif_feed(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = buf[i];
        switch (s_spdif.state) {
        case IEC_PA0:
            s_spdif.state = (b == 0x72) ? IEC_PA1 : IEC_PA0;
            break;
        case IEC_PA1:
            s_spdif.state = (b == 0xF8) ? IEC_PB0 : (b == 0x72) ? IEC_PA1 : IEC_PA0;
            break;
        case IEC_PB0:
            s_spdif.state = (b == 0x1F) ? IEC_PB1 : (b == 0x72) ? IEC_PA1 : IEC_PA0;
            break;
        case IEC_PB1:
            s_spdif.state = (b == 0x4E) ? IEC_PC0 : (b == 0x72) ? IEC_PA1 : IEC_PA0;
            break;
        case IEC_PC0:
            s_spdif.type = b & 0x1F;
            s_spdif.state = IEC_PC1;
            break;
        case IEC_PC1:
            s_spdif.state = IEC_PD0;
            break;
        case IEC_PD0:
            s_spdif.length = b;
            s_spdif.state = IEC_PD1;
            break;
        case IEC_PD1:
            s_spdif.length = (s_spdif.length | ((unsigned)b << 8)) / 8;
            s_spdif.pos = 0;
            if (s_spdif.length == 0 || s_spdif.length > SPDIF_MAX_BURST) {
                s_spdif.rejected++;
                s_spdif.state = IEC_PA0;
            } else {
                s_spdif.state = IEC_PAYLOAD;
            }
            break;
        case IEC_PAYLOAD:
            s_spdif.burst[s_spdif.pos++] = b;
            if (s_spdif.pos == s_spdif.length) {
                decode_burst();
                s_spdif.state = IEC_PA0;
            }
            break;
        }
    }
}

/* Bursts arrive at 31.25/s, one per six pulls; a gap of 32 pulls (~170 ms)
 * means the encoder stopped. */
#define SPDIF_PRESENT_PULLS 32

static bool spdif_stream_present(void)
{
    return s_spdif.last_valid_pull &&
           s_spdif.pulls - s_spdif.last_valid_pull < SPDIF_PRESENT_PULLS;
}

static void spdif_set_decoding(bool on)
{
    if (on && !s_spdif.decoding) {
        /* Start from an empty ring: what it holds is from the last time. */
        s_spdif.head = s_spdif.tail = 0;
    }
    s_spdif.decoding = on;
}

/* RECOMP_SURROUND_DUMP_DIR=<dir>: raw EP FIFO #1 (IEC 61937, the same
 * format as xemu's MCPX_EP_FIFO1_DUMP) and the 6-channel output PCM, for
 * scratchpad/audio_surround_sources_20261005/analyze_surround_capture.py. */
static FILE *s_dump_fifo1, *s_dump_pcm;

static void dump_open(void)
{
    const char *dir = getenv("RECOMP_SURROUND_DUMP_DIR");
    char path[600];
    if (!dir || !*dir) {
        return;
    }
    snprintf(path, sizeof(path), "%s\\ep_fifo1_%lu.iec61937", dir,
             (unsigned long)GetCurrentProcessId());
    s_dump_fifo1 = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s\\out_6ch_%lu.s16le", dir,
             (unsigned long)GetCurrentProcessId());
    s_dump_pcm = fopen(path, "wb");
    fprintf(stderr, "[APU-SURROUND] dumps in %s (fifo1 %s, pcm %s)\n", dir,
            s_dump_fifo1 ? "ok" : "failed", s_dump_pcm ? "ok" : "failed");
}

/* Passthrough: the output pull without PCM (presence/stat bookkeeping). */
void apu_surround_passthrough_block(void)
{
    s_spdif.pulls++;
    spdif_set_decoding(false);
}

int apu_surround_passthrough(void)
{
    return s_passthrough && apu_passthrough_active();
}

void apu_surround_output_block(const int16_t stereo[256][2],
                               int16_t out[256][6])
{
    s_spdif.pulls++;
    bool surround = spdif_stream_present();
    spdif_set_decoding(surround);

    if (surround) {
        unsigned avail = (s_spdif.head + SPDIF_PCM_FRAMES - s_spdif.tail) %
                         SPDIF_PCM_FRAMES;
        if (avail < 256) {
            s_spdif.underruns++;
        }
        for (unsigned i = 0; i < 256; i++) {
            if (s_spdif.tail == s_spdif.head) {
                memset(out[i], 0, sizeof(out[i]));
            } else {
                memcpy(out[i], s_spdif.pcm[s_spdif.tail], sizeof(out[i]));
                s_spdif.tail = (s_spdif.tail + 1) % SPDIF_PCM_FRAMES;
            }
        }
    } else {
        /* EP FIFO #0, the analog output: stereo on the front pair. */
        for (unsigned i = 0; i < 256; i++) {
            out[i][0] = stereo[i][0];
            out[i][1] = stereo[i][1];
            out[i][2] = out[i][3] = out[i][4] = out[i][5] = 0;
        }
    }
    if (s_dump_pcm) {
        fwrite(out, sizeof(int16_t) * 6, 256, s_dump_pcm);
    }
}

/* ============================================================
 * FIFOs (gp_ep.c)
 * ============================================================ */

static void gp_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    uint32_t base;
    uint32_t end;
    hwaddr cur_reg;
    if (dir) {
        if (index >= GP_OUTPUT_FIFO_COUNT) {
            return;
        }
        base = GET_MASK(d->regs[NV_PAPU_GPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPOFCUR0 + 0x10 * index;
    } else {
        if (index >= GP_INPUT_FIFO_COUNT) {
            return;
        }
        base = GET_MASK(d->regs[NV_PAPU_GPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_GPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_GPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    /* A cursor at or past END leaves the engine running with no EOL and
     * kills the console on the next boot (upstream asserts). */
    if (cur >= (end & FIFO_BOUND_MASK)) {
        g_stat.sge_errors++;
        return;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_GPFADDR], d->regs[NV_PAPU_GPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

/* EP output FIFO #0 is the analog stereo output - with Dolby Digital on, a
 * surround-encoded downmix - as 256 S16 stereo frames per EP frame. It goes
 * to the monitor frame buffer and silence to the guest's ring. A write of
 * any other size is not that output and goes to the ring as data. */
static bool ep_sink_samples(MCPXAPUState *d, uint8_t *ptr, size_t len)
{
    if (len != sizeof(d->monitor.frame_buf)) {
        return false;
    }
    memcpy(d->monitor.frame_buf, ptr, len);
    g_stat.fifo0_blocks++;
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
        if (index >= EP_OUTPUT_FIFO_COUNT) {
            return;
        }
        base = GET_MASK(d->regs[NV_PAPU_EPOFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPOFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPOFCUR0 + 0x10 * index;
    } else {
        if (index >= EP_INPUT_FIFO_COUNT) {
            return;
        }
        base = GET_MASK(d->regs[NV_PAPU_EPIFBASE0 + 0x10 * index],
                        NV_PAPU_GPOFBASE0_VALUE);
        end = GET_MASK(d->regs[NV_PAPU_EPIFEND0 + 0x10 * index],
                       NV_PAPU_GPOFEND0_VALUE);
        cur_reg = NV_PAPU_EPIFCUR0 + 0x10 * index;
    }

    uint32_t cur = GET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE);

    /* With Dolby Digital enabled, FIFO #1 carries the EP AC-3 encoder's
     * IEC 61937 bitstream. Runs on the EP worker; the output block reads
     * the decoded ring on the frame thread after the cores are joined. */
    if (dir && index == 1) {
        if (s_dump_fifo1) {
            fwrite(ptr, 1, len, s_dump_fifo1);
        }
        g_stat.fifo1_writes++;
        g_stat.fifo1_bytes += len;
        if (s_passthrough) {
            apu_passthrough_feed(ptr, len);
        }
        spdif_feed(ptr, len); /* CRC statistics; decodes only for 5.1 PCM */
    }

    if (dir && index == 0) {
        if (ep_sink_samples(d, ptr, len)) {
            /* Since we are sinking, push silence out */
            ptr = (uint8_t *)ep_silence;
        }
    }

    if (cur >= (end & FIFO_BOUND_MASK)) {
        g_stat.sge_errors++;
        return;
    }

    cur = circular_scatter_gather_rw(d,
        d->regs[NV_PAPU_EPFADDR], d->regs[NV_PAPU_EPFMAXSGE],
        ptr, base, end, cur, len, dir);

    SET_MASK(d->regs[cur_reg], NV_PAPU_GPOFCUR0_VALUE, cur);
}

/* ============================================================
 * GP/EP MMIO (gp_ep.c)
 * ============================================================ */

static void proc_rst_write(DSPJitState *dsp, uint32_t oldval, uint32_t val)
{
    if (!(val & NV_PAPU_GPRST_GPRST) || !(val & NV_PAPU_GPRST_GPDSPRST)) {
        dspjit_reset(dsp);
    } else if (
        (!(oldval & NV_PAPU_GPRST_GPRST) || !(oldval & NV_PAPU_GPRST_GPDSPRST))
        && ((val & NV_PAPU_GPRST_GPRST) && (val & NV_PAPU_GPRST_GPDSPRST))) {
        dspjit_bootstrap(dsp);
    }
}

uint64_t apu_surround_gp_read(MCPXAPUState *d, uint64_t addr)
{
    if (IN_RANGE(addr, NV_PAPU_GPXMEM, 0x1000)) {
        return dspjit_read_memory(s_gp, 'X', (uint32_t)(addr - NV_PAPU_GPXMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPMIXBUF, 0x400)) {
        return dspjit_read_memory(s_gp, 'X', GP_DSP_MIXBUF_BASE +
                                  (uint32_t)(addr - NV_PAPU_GPMIXBUF) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPYMEM, 0x800)) {
        return dspjit_read_memory(s_gp, 'Y', (uint32_t)(addr - NV_PAPU_GPYMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_GPPMEM, 0x1000)) {
        return dspjit_read_memory(s_gp, 'P', (uint32_t)(addr - NV_PAPU_GPPMEM) / 4);
    }
    return d->gp.regs[addr];
}

void apu_surround_gp_write(MCPXAPUState *d, uint64_t addr, uint64_t val)
{
    apu_lock_for_mmio(d);

    if (IN_RANGE(addr, NV_PAPU_GPXMEM, 0x1000)) {
        dspjit_write_memory(s_gp, 'X', (uint32_t)(addr - NV_PAPU_GPXMEM) / 4,
                            (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_GPMIXBUF, 0x400)) {
        /* The mixbuffer is the VP's; host writes here are dropped. */
    } else if (IN_RANGE(addr, NV_PAPU_GPYMEM, 0x800)) {
        dspjit_write_memory(s_gp, 'Y', (uint32_t)(addr - NV_PAPU_GPYMEM) / 4,
                            (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_GPPMEM, 0x1000)) {
        dspjit_write_memory(s_gp, 'P', (uint32_t)(addr - NV_PAPU_GPPMEM) / 4,
                            (uint32_t)val);
    } else if (addr == NV_PAPU_GPRST) {
        fprintf(stderr, "[APU-SURROUND] GP reset register 0x%X -> 0x%X\n",
                d->gp.regs[NV_PAPU_GPRST], (unsigned)val);
        proc_rst_write(s_gp, d->gp.regs[NV_PAPU_GPRST], (uint32_t)val);
        d->gp.regs[NV_PAPU_GPRST] = (uint32_t)val;
    } else if (addr < 0x10000) {
        d->gp.regs[addr] = (uint32_t)val;
    }

    qemu_mutex_unlock(&d->lock);
}

uint64_t apu_surround_ep_read(MCPXAPUState *d, uint64_t addr)
{
    if (IN_RANGE(addr, NV_PAPU_EPXMEM, 0xC00)) {
        return dspjit_read_memory(s_ep, 'X', (uint32_t)(addr - NV_PAPU_EPXMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_EPYMEM, 0x100)) {
        return dspjit_read_memory(s_ep, 'Y', (uint32_t)(addr - NV_PAPU_EPYMEM) / 4);
    } else if (IN_RANGE(addr, NV_PAPU_EPPMEM, 0x1000)) {
        return dspjit_read_memory(s_ep, 'P', (uint32_t)(addr - NV_PAPU_EPPMEM) / 4);
    }
    return d->ep.regs[addr];
}

void apu_surround_ep_write(MCPXAPUState *d, uint64_t addr, uint64_t val)
{
    apu_lock_for_mmio(d);

    if (IN_RANGE(addr, NV_PAPU_EPXMEM, 0xC00)) {
        dspjit_write_memory(s_ep, 'X', (uint32_t)(addr - NV_PAPU_EPXMEM) / 4,
                            (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_EPYMEM, 0x100)) {
        dspjit_write_memory(s_ep, 'Y', (uint32_t)(addr - NV_PAPU_EPYMEM) / 4,
                            (uint32_t)val);
    } else if (IN_RANGE(addr, NV_PAPU_EPPMEM, 0x1000)) {
        dspjit_write_memory(s_ep, 'P', (uint32_t)(addr - NV_PAPU_EPPMEM) / 4,
                            (uint32_t)val);
    } else if (addr == NV_PAPU_EPRST) {
        fprintf(stderr, "[APU-SURROUND] EP reset register 0x%X -> 0x%X\n",
                d->ep.regs[NV_PAPU_EPRST], (unsigned)val);
        proc_rst_write(s_ep, d->ep.regs[NV_PAPU_EPRST], (uint32_t)val);
        d->ep.regs[NV_PAPU_EPRST] = (uint32_t)val;
        d->ep_frame_div = 0; /* FIXME (upstream): Still unsure about frame sync */
    } else if (addr < 0x10000) {
        d->ep.regs[addr] = (uint32_t)val;
    }

    qemu_mutex_unlock(&d->lock);
}

/* ============================================================
 * DSP workers and the frame (gp_ep.c)
 * ============================================================ */

/* Cycles handed to a core per run call: how often the run loop is
 * re-entered, and so how promptly a worker notices a waiting guest accessor
 * or the end of its frame. */
#define DSP_SLICE_CYCLES 1000

/* One worker thread per DSP core. On silicon the GP and EP are concurrent
 * cores that hand data to each other through scratch buffers in guest RAM.
 * The frame thread remains the barrier: it fills the GP mixbuf, releases
 * both workers, and waits for both before reading any result. */
typedef struct DSPWorker {
    MCPXAPUState *d;
    bool is_gp;
    HANDLE thread;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE start_cond, done_cond;
    bool created;
    bool start_requested;
    bool busy;
    /* Frame thread -> worker */
    uint32_t budget;
    /* Worker -> frame thread, published before done_cond is signalled */
    bool wall_bound;
} DSPWorker;

static DSPWorker g_gp_worker = { .is_gp = true };
static DSPWorker g_ep_worker = { .is_gp = false };

static DSPJitState *worker_dsp(DSPWorker *w)
{
    return w->is_gp ? s_gp : s_ep;
}

/* A core that halted while its next frame start is already pending owes
 * that frame: let it continue within its budget instead of parking until
 * the next frame, which would leave it a frame behind for good. */
static bool core_resume_owed_frame(DSPJitState *dsp, volatile __int64 *counter)
{
    if (dspjit_get_halt_requested(dsp) && dspjit_frame_start_pending(dsp)) {
        dspjit_set_halt_requested(dsp, false);
        _InterlockedIncrement64(counter);
        return true;
    }
    return false;
}

/* Run one core until it signals frame-complete, exhausts its cycle budget, or
 * has held the thread long enough that the frame should end. */
static void run_one_core(DSPWorker *w)
{
    MCPXAPUState *d = w->d;
    DSPJitState *dsp = worker_dsp(w);
    const int64_t slice_us = 2000;
    int64_t t0 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    w->wall_bound = false;
    while (dspjit_get_cycle_count(dsp) < w->budget) {
        if (dspjit_get_halt_requested(dsp) &&
            !core_resume_owed_frame(dsp, w->is_gp ? &g_stat.gp_catchup
                                                  : &g_stat.ep_catchup)) {
            break;
        }
        dspjit_run(dsp, DSP_SLICE_CYCLES);
        if (qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t0 >= slice_us) {
            w->wall_bound = true;
            break;
        }
        /* A guest-side accessor is blocked on the APU lock the frame thread
         * is holding while it waits for us. Stop between slices so it can be
         * handed over; the core keeps its state and resumes. */
        if (qatomic_read(&d->lock_waiters) > 0) {
            _InterlockedIncrement64(&g_stat.waiter_breaks);
            break;
        }
    }
}

static DWORD WINAPI dsp_worker_thread(LPVOID opaque)
{
    DSPWorker *w = opaque;

    /* Off host core 0, which the guest threads are pinned to (as the APU
     * frame thread, apu_core.c). */
    {
        DWORD_PTR proc = 0, sys = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)
                && (proc & ~apu_guest_mask()))
            SetThreadAffinityMask(GetCurrentThread(), proc & ~apu_guest_mask());
    }
    EnterCriticalSection(&w->lock);
    for (;;) {
        while (!w->start_requested) {
            SleepConditionVariableCS(&w->start_cond, &w->lock, INFINITE);
        }
        w->start_requested = false;
        LeaveCriticalSection(&w->lock);

        run_one_core(w);

        EnterCriticalSection(&w->lock);
        w->busy = false;
        WakeConditionVariable(&w->done_cond);
    }
}

static void dsp_worker_init(DSPWorker *w, MCPXAPUState *d)
{
    if (w->created) {
        return;
    }
    w->d = d;
    InitializeCriticalSection(&w->lock);
    InitializeConditionVariable(&w->start_cond);
    InitializeConditionVariable(&w->done_cond);
    w->thread = CreateThread(NULL, 0, dsp_worker_thread, w, 0, NULL);
    w->created = true;
}

static void dsp_worker_start(DSPWorker *w, uint32_t budget)
{
    EnterCriticalSection(&w->lock);
    w->budget = budget;
    w->busy = true;
    w->start_requested = true;
    WakeConditionVariable(&w->start_cond);
    LeaveCriticalSection(&w->lock);
}

static void dsp_worker_wait(DSPWorker *w)
{
    EnterCriticalSection(&w->lock);
    while (w->busy) {
        SleepConditionVariableCS(&w->done_cond, &w->lock, INFINITE);
    }
    LeaveCriticalSection(&w->lock);
}

/* Carried from begin to end. */
static struct {
    bool gp_enabled, ep_enabled, ran;
    uint32_t gp_budget, ep_budget;
    /* The GP did not reach frame-complete by the end of the previous frame:
     * its output slice for that frame is still unwritten. */
    bool gp_late;
} g_frame;

/* Join whatever was released. The workers stop between slices when a guest
 * accessor is waiting on the APU lock this thread holds; ending the frame
 * there would leave the stopped core's output unproduced, so while a core is
 * unfinished for any reason but wall time, hand the lock over with both
 * workers idle, then re-release the unfinished cores. */
static void join_dsps(MCPXAPUState *d, bool gp_active, bool ep_active)
{
    if (gp_active) {
        dsp_worker_wait(&g_gp_worker);
    }
    if (ep_active) {
        dsp_worker_wait(&g_ep_worker);
    }

    for (;;) {
        bool gp_more = gp_active && !g_gp_worker.wall_bound &&
                       !dspjit_get_halt_requested(s_gp) &&
                       dspjit_get_cycle_count(s_gp) < g_frame.gp_budget;
        bool ep_more = ep_active && !g_ep_worker.wall_bound &&
                       !dspjit_get_halt_requested(s_ep) &&
                       dspjit_get_cycle_count(s_ep) < g_frame.ep_budget;
        if ((!gp_more && !ep_more) || d->pause_requested) {
            break;
        }
        while (!d->pause_requested && qatomic_read(&d->lock_waiters) > 0) {
            qemu_cond_timedwait(&d->cond, &d->lock, 1);
        }
        if (d->pause_requested) {
            break;
        }
        if (gp_more) {
            dsp_worker_start(&g_gp_worker, g_frame.gp_budget);
        }
        if (ep_more) {
            dsp_worker_start(&g_ep_worker, g_frame.ep_budget);
        }
        if (gp_more) {
            dsp_worker_wait(&g_gp_worker);
        }
        if (ep_more) {
            dsp_worker_wait(&g_ep_worker);
        }
    }
}

static void status_report(MCPXAPUState *d)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (!g_stat.last_report_us) {
        g_stat.last_report_us = now;
        return;
    }
    if (now - g_stat.last_report_us < 10000000) {
        return;
    }
    double span = (now - g_stat.last_report_us) / 1e6;
    fprintf(stderr, "[APU-SURROUND] %.0f frames/s kicks/s=%.0f halts/s gp=%.0f "
            "ep=%.0f | wall-bound=%llu waiter-breaks=%lld catchup gp=%lld "
            "ep=%lld gp-late=%llu | fifo0=%llu fifo1 %llu wr %llu B | "
            "ac3 frames=%llu bad=%llu under=%llu over=%llu present=%d | "
            "sge-err=%llu GPRST=%X EPRST=%X\n",
            g_stat.frames / span, g_stat.kicks / span,
            (g_dspjit_gp_halts - g_stat.gp_halts_prev) / span,
            (g_dspjit_ep_halts - g_stat.ep_halts_prev) / span,
            (unsigned long long)g_stat.exit_wall,
            (long long)g_stat.waiter_breaks, (long long)g_stat.gp_catchup,
            (long long)g_stat.ep_catchup,
            (unsigned long long)g_stat.gp_late_finish,
            (unsigned long long)g_stat.fifo0_blocks,
            (unsigned long long)g_stat.fifo1_writes,
            (unsigned long long)g_stat.fifo1_bytes,
            (unsigned long long)s_spdif.frames,
            (unsigned long long)s_spdif.rejected,
            (unsigned long long)s_spdif.underruns,
            (unsigned long long)s_spdif.overruns, (int)spdif_stream_present(),
            (unsigned long long)g_stat.sge_errors,
            d->gp.regs[NV_PAPU_GPRST], d->ep.regs[NV_PAPU_EPRST]);
    g_stat.gp_halts_prev = g_dspjit_gp_halts;
    g_stat.ep_halts_prev = g_dspjit_ep_halts;
    g_stat.frames = g_stat.kicks = g_stat.exit_wall = g_stat.gp_late_finish = 0;
    g_stat.waiter_breaks = g_stat.gp_catchup = g_stat.ep_catchup = 0;
    g_stat.fifo0_blocks = g_stat.fifo1_writes = g_stat.fifo1_bytes = 0;
    g_stat.last_report_us = now;
    if (s_dump_fifo1) {
        fflush(s_dump_fifo1);
    }
    if (s_dump_pcm) {
        fflush(s_dump_pcm);
    }
}

void apu_surround_frame_begin(MCPXAPUState *d)
{
    bool ep_enabled = (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPRST) &&
                      (d->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST);
    bool gp_enabled = (d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPRST) &&
                      (d->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPDSPRST);

    g_frame.gp_enabled = gp_enabled;
    g_frame.ep_enabled = ep_enabled;
    g_frame.ran = gp_enabled || ep_enabled;

    if (gp_enabled) {
        dspjit_start_frame(s_gp);
        dspjit_set_halt_requested(s_gp, false);
        dspjit_frame_tick(s_gp);
    }
    if (ep_enabled) {
        /* The kick (start-frame interrupt + run-to-idle) fires every 8th
         * frame as on hardware; between kicks an EP that has not idled keeps
         * executing its per-frame slice, and an idled EP stays parked. */
        if (d->ep_frame_div % 8 == 0) {
            dspjit_start_frame(s_ep);
            dspjit_set_halt_requested(s_ep, false);
            g_stat.kicks++;
        }
        dspjit_frame_tick(s_ep);
    }

    if (gp_enabled || ep_enabled) {
        /* Hardware budget: one 32-sample frame at 48 kHz on the 133.3 MHz
         * core; the EP gets the same per-frame slice every SE frame. */
        g_frame.gp_budget = DSPJIT_FRAME_CYCLES;
        g_frame.ep_budget = DSPJIT_FRAME_CYCLES;
        g_stat.frames++;
        dsp_worker_init(&g_gp_worker, d);
        dsp_worker_init(&g_ep_worker, d);
        /* The EP runs from here, overlapped with the VP; the GP waits for
         * the VP (apu_surround_frame_gp). */
        if (ep_enabled) {
            dsp_worker_start(&g_ep_worker, g_frame.ep_budget);
        }
    }
}

/* The VP has filled this frame's mixbins: hand them to the GP and release
 * it. The GP runs after the VP, not overlapped with it: its DMA writes
 * effect returns to scratch pages VP voices read in the same frame. */
void apu_surround_frame_gp(MCPXAPUState *d, float (*mixbins)[32])
{
    (void)d;
    if (!g_frame.gp_enabled) {
        return;
    }

    /* A GP that did not reach frame-complete last frame is parked part way
     * through it, still reading the mixbuf that frame was given: finish the
     * late frame first, on its own mixbuf, bounded by the GP's budget. */
    if (g_frame.gp_late) {
        g_stat.gp_late_finish++;
        int64_t c0 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        while (!dspjit_get_halt_requested(s_gp) &&
               dspjit_get_cycle_count(s_gp) < g_frame.gp_budget &&
               qemu_clock_get_us(QEMU_CLOCK_REALTIME) - c0 < 2000) {
            dspjit_run(s_gp, DSP_SLICE_CYCLES);
        }
    }

    /* The VP's mixbins into the mixbuffer (GP X:$0C00-$0FFF, seen by the
     * core at X:$1400). Only a frame that mixed a voice writes it: with the
     * engine running and no voices, silicon leaves a GP program's own mix
     * there alone. */
    if (mixbins) {
        for (int mixbin = 0; mixbin < NUM_MIXBINS; mixbin++) {
            uint32_t base = GP_DSP_MIXBUF_BASE + mixbin * NUM_SAMPLES_PER_FRAME;
            for (int sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
                dspjit_write_memory(s_gp, 'X', base + sample,
                                    float_to_24b(mixbins[mixbin][sample]));
            }
        }
    }

    dsp_worker_start(&g_gp_worker, g_frame.gp_budget);
}

/* Join the cores and read their results. */
void apu_surround_frame_end(MCPXAPUState *d)
{
    if (!g_frame.ran) {
        return;
    }
    join_dsps(d, g_frame.gp_enabled, g_frame.ep_enabled);
    if ((g_frame.gp_enabled && g_gp_worker.wall_bound) ||
        (g_frame.ep_enabled && g_ep_worker.wall_bound)) {
        g_stat.exit_wall++;
    }
    g_frame.gp_late = g_frame.gp_enabled && !dspjit_get_halt_requested(s_gp);
    if (g_frame.gp_enabled) {
        g_dbg.gp.cycles = dspjit_get_cycle_count(s_gp);
    }
    if (g_frame.ep_enabled) {
        g_dbg.ep.cycles = dspjit_get_cycle_count(s_ep);
    }
    status_report(d);
}

/* ============================================================
 * Init
 * ============================================================ */

int apu_surround_init(MCPXAPUState *d)
{
    const char *e = getenv("RECOMP_AUDIO_SURROUND");
    if (!e || !*e || *e == '0') {
        return 0;
    }
    if (!dsp56300_dll_load()) {
        fprintf(stderr, "[APU-SURROUND] requested but unavailable: staying on "
                        "the stereo path\n");
        return 0;
    }

    s_gp = dspjit_init(d, gp_scratch_rw, gp_fifo_rw, true);
    dspjit_set_halt_requested(s_gp, false);
    dspjit_set_cycle_count(s_gp, 0);

    s_ep = dspjit_init(d, ep_scratch_rw, ep_fifo_rw, false);
    dspjit_set_halt_requested(s_ep, false);
    dspjit_set_cycle_count(s_ep, 0);

    if (!s_gp || !s_ep) {
        fprintf(stderr, "[APU-SURROUND] DSP core creation failed: staying on "
                        "the stereo path\n");
        return 0;
    }

    /* The EP's output is the monitor's input from now on. */
    d->monitor.point = MCPX_APU_DEBUG_MON_EP;
    s_stereo = !_stricmp(e, "stereo");
    if (!_stricmp(e, "passthrough") || !_stricmp(e, "ac3")) {
        const char *m = getenv("RECOMP_SMOKE_MUTE");
        s_passthrough = apu_passthrough_start(m && m[0] == '1');
        if (!s_passthrough) {
            fprintf(stderr, "[APU-SURROUND] passthrough unavailable: decoding "
                            "to 5.1 PCM instead\n");
        }
    }
    if (!s_passthrough && !s_stereo) {
        xa2_set_channels(6); /* monitor init (xa2_init) follows dsp init */
    }
    dump_open();
    s_active = 1;
    fprintf(stderr, "[APU-SURROUND] active: GP/EP on the DSP56300 JIT, "
                    "%s\n", s_stereo ? "XC_AUDIO stereo, EP analog output" :
                    s_passthrough ? "XC_AUDIO Surround+AC3, Dolby Digital passthrough" :
                    "XC_AUDIO Surround+AC3, 6-channel PCM output");
    return 1;
}

