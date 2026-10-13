/*
 * MCPX APU Core - Standalone extraction from xemu
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

#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "apu_surround.h"
#include "apu_passthrough.h"
#include "fpconv.h"

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * IRQ handling
 * ============================================================ */

/* The APU's interrupt line (IRQ 5 on the Xbox, xemu xbox_pci.c). The kernel
 * installs a handler that runs the title's connected ISR; without one (the
 * standalone tests) the line goes nowhere, as before. docs Update 347. */
static void (*g_apu_irq_handler)(int asserted);

void mcpx_apu_set_irq_handler(void (*handler)(int asserted))
{
    g_apu_irq_handler = handler;
}

static void apu_irq_line(int asserted)
{
    if (g_apu_irq_handler)
        g_apu_irq_handler(asserted);
}

static void update_irq(MCPXAPUState *d)
{
    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        apu_irq_line(1);
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        apu_irq_line(0);
    }
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100);
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    /* RECOMP_APU_TRAP_TRACE: what DirectSound's ISR sees when it decodes a
     * front-end trap (it reads FECTL, FEDECMETH, FEDECPARAM). docs Update 347. */
    if (addr == NV_PAPU_FEDECMETH) {
        static int on = -1;
        static unsigned n;
        if (on < 0) {
            const char *e = getenv("RECOMP_APU_TRAP_TRACE");
            on = (e && *e && *e != '0') ? 1 : 0;
        }
        if (on && (n++ < 30 || (n & (n - 1)) == 0))
            fprintf(stderr, "[APU-TRAP] ISR read #%u FEDECMETH=0x%X FEDECPARAM=%u "
                    "FECTL=0x%08X ISTS=0x%08X\n", n, (unsigned)r,
                    d->regs[NV_PAPU_FEDECPARAM], d->regs[NV_PAPU_FECTL],
                    d->regs[NV_PAPU_ISTS]);
    }

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)r);
     */
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)val);
     */
    (void)size;

    switch (addr) {
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        qatomic_set(&d->regs[addr], (uint32_t)val);
        /* Starting the APU has to start the frame thread.
         *
         * The thread idles on pause_requested, which init sets and only the
         * test tone ever cleared -- so a title that enabled the APU through
         * these registers got an APU that stayed asleep. Nothing then advanced
         * the front end, and a title waiting on a notify completion (the
         * FEMEMDATA magic write, which is how completion reaches guest memory)
         * waited forever. Wreckless hangs exactly there during DirectSound
         * init, and because it initialises its whole engine behind a
         * successful DirectSound create, that hang is not confined to audio.
         *
         * Resume whenever the write is not switching the block off; the thread
         * re-checks FECTL itself and idles again if it is halted or trapped. */
        {
            uint32_t sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
            uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            bool running =
                ((sectl & NV_PAPU_SECTL_XCNTMODE) != NV_PAPU_SECTL_XCNTMODE_OFF)
                && ((fectl & NV_PAPU_FECTL_FEMETHMODE)
                    != NV_PAPU_FECTL_FEMETHMODE_HALTED);
            if (running && d->pause_requested) {
                d->pause_requested = false;
                fprintf(stderr, "[APU] started by the title"
                                " (SECTL=%08X FECTL=%08X)\n", sectl, fectl);
            }
        }
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* On Linux, waveOut* are inert stubs from win32_compat.h: the APU's
 * waveOut fallback path stays inactive and never produces audio. */

/* Ring of waveOut buffers for double-buffering */
#define WAVEOUT_NUM_BUFS 4
#define WAVEOUT_BUF_SAMPLES 2048  /* ~42.7ms at 48kHz, matches 8-frame delivery rate */
#define MIXER_FRAME_SAMPLES 256  /* Internal mixing frame size (matches frame_buf) */

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };

/* Automated smoke runs must never play surprise audio. Keep APU, mixer and
 * backend pacing active; silence only the final PCM handed to Windows. */
static bool smoke_output_muted(void)
{
    static int muted = -1;
    if (muted < 0) {
        const char *e = getenv("RECOMP_SMOKE_MUTE");
        muted = e && e[0] == '1';
        if (muted)
            fprintf(stderr, "[APU] smoke output muted (RECOMP_SMOKE_MUTE=1)\n");
    }
    return muted != 0;
}

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;

    /* Dolby Digital passthrough owns the output device exclusively (docs
     * Update 612): no XAudio2/waveOut stream next to it. */
    if (apu_surround_passthrough()) {
        fprintf(stderr, "[APU] Output: Dolby Digital passthrough (no PCM stream)\n");
        return;
    }

    /* Try XAudio2 first (lower latency) */
    if (xa2_init()) {
        xa2_set_silent(smoke_output_muted());
        fprintf(stderr, "[APU] Using XAudio2 audio backend\n");
        return;
    }
    fprintf(stderr, "[APU] XAudio2 unavailable, falling back to waveOut\n");

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

/* Output-gap diagnosis (docs Update 617): where the frame thread spent its
 * time between two 256-sample output blocks. A block normally follows the
 * previous one within ~5 ms; a gap longer than the card's queue (32 ms) is
 * an audible dropout. Always on, logged only for gaps over 20 ms. */
static struct {
    int64_t last_submit_us;
    int64_t throttle_us, trap_us, lock_us, frame_us, handover_us, vp_us;
    int64_t spin_us_base;
    unsigned frames, traps, spin_waits_base;
} s_gap;

static void gap_lock(MCPXAPUState *d)
{
    int64_t t = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    qemu_mutex_lock(&d->lock);
    s_gap.lock_us += qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t;
}

static void gap_block_submitted(void)
{
    static unsigned logged;
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t gap = s_gap.last_submit_us ? now - s_gap.last_submit_us : 0;
    if (gap > 20000 && (logged++ < 100 || logged % 50 == 0))
        fprintf(stderr, "[APU-GAP] %.1f ms between output blocks: throttle "
                "%.1f, trap-wait %.1f (%u), lock %.1f, frames %.1f (%u; VP %.1f, "
                "of it voice-lock spin %.1f in %u waits), handover %.1f ms; "
                "queue now %d\n", gap / 1000.0,
                s_gap.throttle_us / 1000.0, s_gap.trap_us / 1000.0, s_gap.traps,
                s_gap.lock_us / 1000.0, s_gap.frame_us / 1000.0, s_gap.frames,
                s_gap.vp_us / 1000.0,
                (g_apu_vp_spin_us - s_gap.spin_us_base) / 1000.0,
                g_apu_vp_spin_waits - s_gap.spin_waits_base,
                s_gap.handover_us / 1000.0, xa2_queued());
    memset(&s_gap, 0, sizeof(s_gap));
    s_gap.last_submit_us = now;
    s_gap.spin_us_base = g_apu_vp_spin_us;
    s_gap.spin_waits_base = g_apu_vp_spin_waits;
}

/* RECOMP_MUTE_IN_BACKGROUND=1 (scda_options.txt MuteInBackground, docs
 * Update 614): silence while no window of this process is in front. Checked
 * once per 256-sample block. */
static bool background_muted(void)
{
    static int on = -1;
    DWORD pid = 0;
    HWND fg;
    if (on < 0) {
        const char *e = getenv("RECOMP_MUTE_IN_BACKGROUND");
        on = e && *e && *e != '0';
    }
    if (!on)
        return false;
    fg = GetForegroundWindow();
    if (fg)
        GetWindowThreadProcessId(fg, &pid);
    return pid != GetCurrentProcessId();
}

void mcpx_apu_monitor_frame(MCPXAPUState *d, bool apu_frame_ready)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    if (apu_surround_passthrough()) {
        apu_passthrough_set_muted(background_muted());
        apu_surround_passthrough_block();
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        return;
    }

    /* XAudio2 path: render and submit a buffer */
    if (xa2_is_active()) {
        static uint64_t apu_blocks, apu_nonzero, mix_blocks;
        static DWORD last_report;
        if (apu_frame_ready) {
            apu_blocks++;
            for (int i = 0; i < MIXER_FRAME_SAMPLES; i++)
                if (d->monitor.frame_buf[i][0] || d->monitor.frame_buf[i][1]) {
                    apu_nonzero++;
                    break;
                }
        }
        if (g_mixer_active_count) mix_blocks++;
        if (getenv("RECOMP_AUDIO_DIAG") && GetTickCount() - last_report >= 1000) {
            fprintf(stderr, "[AUDIO-SRC] apu_blocks=%llu nonzero_blocks=%llu "
                    "mixer_active=%d mixer_blocks=%llu point=%d SECTL=%08X "
                    "GPRST=%08X EPRST=%08X\n",
                    (unsigned long long)apu_blocks, (unsigned long long)apu_nonzero,
                    g_mixer_active_count, (unsigned long long)mix_blocks,
                    d->monitor.point, d->regs[NV_PAPU_SECTL],
                    d->gp.regs[NV_PAPU_GPRST], d->ep.regs[NV_PAPU_EPRST]);
            last_report = GetTickCount();
        }
        /* Eight 32-sample APU frames form exactly one 256-sample output
         * block. Keep the VP/GP/EP samples already written to frame_buf:
         * clearing it here discarded all game audio. */
        if (!apu_frame_ready || g_audio_muted || background_muted())
            memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));

        if (g_test_tone.active && !g_audio_muted) {
            for (int i = 0; i < MIXER_FRAME_SAMPLES; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                d->monitor.frame_buf[i][0] = s;
                d->monitor.frame_buf[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }

        if (!g_audio_muted)
            mixer_render(d->monitor.frame_buf, MIXER_FRAME_SAMPLES);

        if (apu_surround_six_channels()) {
            /* 5.1 decoded from the EP's AC-3 stream, else the EP's stereo
             * output on the front pair (docs Update 611). */
            int16_t out6[MIXER_FRAME_SAMPLES][6];
            apu_surround_output_block(d->monitor.frame_buf, out6);
            if (g_audio_muted || background_muted())
                memset(out6, 0, sizeof(out6));
            xa2_submit_samples(&out6[0][0], MIXER_FRAME_SAMPLES);
            gap_block_submitted();
            memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
            return;
        }
        xa2_submit_samples((const int16_t *)d->monitor.frame_buf,
                           MIXER_FRAME_SAMPLES);
        gap_block_submitted();
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        return;
    }

    if (!g_waveout.initialized) return;

    int idx = g_waveout.next_buf;
    WAVEHDR *hdr = &g_waveout.hdrs[idx];

    /* Wait if this buffer is still playing (with timeout) */
    int wait_loops = 0;
    while (!(hdr->dwFlags & WHDR_DONE) && (hdr->dwFlags & WHDR_INQUEUE)) {
        qemu_mutex_unlock(&d->lock);
        Sleep(1);
        qemu_mutex_lock(&d->lock);
        if (++wait_loops > 50) break;
    }

    /* Fill the large waveOut buffer by rendering multiple 256-sample frames */
    int16_t *out = (int16_t *)g_waveout.bufs[idx];
    int remaining = WAVEOUT_BUF_SAMPLES;
    int out_offset = 0;

    while (remaining > 0) {
        int chunk = (remaining < MIXER_FRAME_SAMPLES) ? remaining : MIXER_FRAME_SAMPLES;

        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));

        /* Test tone (skip if muted) */
        if (g_test_tone.active && !g_audio_muted) {
            for (int i = 0; i < chunk; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                d->monitor.frame_buf[i][0] = s;
                d->monitor.frame_buf[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }

        /* Mix software voices (skip if muted) */
        if (!g_audio_muted)
            mixer_render(d->monitor.frame_buf, chunk);

        /* Copy to waveOut buffer */
        memcpy(out + out_offset * 2, d->monitor.frame_buf, chunk * 2 * sizeof(int16_t));
        out_offset += chunk;
        remaining -= chunk;
    }

    /* Submit to waveOut */
    if (smoke_output_muted())
        memset(out, 0, sizeof(g_waveout.bufs[idx]));
    hdr->dwFlags &= ~WHDR_DONE;
    waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR));

    g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
    g_waveout.frames_written++;
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

/* RECOMP_AUDIO_TRAP_SILENCE=1: old behaviour, a silent block per pass while
 * the front end is trapped (A/B for docs Update 507). */
static int audio_wait_on_trap(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_AUDIO_TRAP_SILENCE") == NULL;
    return on;
}

/* Blocks queued at the output: XAudio2's queue, or the Dolby Digital
 * passthrough ring (docs Update 612); -1 when neither runs. */
static int output_queued(void)
{
    if (apu_surround_passthrough())
        return apu_passthrough_queued_blocks();
    return xa2_queued();
}

static void throttle(MCPXAPUState *d)
{
    static uint64_t throttle_calls, resets, late_calls, late_us, max_late_us;
    static int64_t report_at;
    static HANDLE highres_timer;
    static bool timer_checked;
    if (d->ep_frame_div % 8) {
        return;
    }

    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    /* Pace by the sound card, not the host clock (docs Update 506), as
     * xemu does with its audio FIFO: produce the next 256-sample block as
     * soon as fewer than ~6 blocks (32 ms) are queued in XAudio2, wait
     * otherwise. The host-clock pacing below kept only 3 blocks (16 ms):
     * any audio-thread delay emptied the queue, and the drift between the
     * host clock and the card's clock dropped a block now and then --
     * crackle in music and ambience, even in the pause menu.
     * RECOMP_AUDIO_TIMER=1 restores the host-clock pacing (A/B);
     * RECOMP_AUDIO_QUEUE=<blocks> changes the target. */
    {
        static int mode = -1, target = 6;
        if (mode < 0) {
            const char *q = getenv("RECOMP_AUDIO_QUEUE");
            mode = getenv("RECOMP_AUDIO_TIMER") == NULL;
            if (q && atoi(q) >= 2 && atoi(q) <= 24)
                target = atoi(q);
            else if (apu_surround_passthrough())
                /* A gap in the Dolby Digital stream tears an AC-3 burst
                 * (the receiver mutes), so ride out JIT warm-up and load
                 * stalls with ~85 ms instead of 32 (docs Update 612). */
                target = 16;
        }
        if (mode && output_queued() >= 0) {
            int waits = 0;
            if (!timer_checked) {
                highres_timer = CreateWaitableTimerExW(NULL, NULL,
                    0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                    TIMER_ALL_ACCESS);
                timer_checked = true;
                fprintf(stderr, "[AUDIO-PACE] paced by the sound card, target %d "
                        "blocks (%d ms)\n", target, target * 16 / 3);
            }
            /* A stalled device must not stop the APU: give up after ~200 ms
             * and let xa2_submit_samples drop the block. */
            while (!d->pause_requested && output_queued() >= target && waits < 200) {
                qemu_mutex_unlock(&d->lock);
                if (highres_timer) {
                    LARGE_INTEGER due;
                    due.QuadPart = -10000;   /* 1 ms */
                    SetWaitableTimerEx(highres_timer, &due, 0, NULL, NULL, NULL, 0);
                    WaitForSingleObject(highres_timer, INFINITE);
                } else {
                    Sleep(1);
                }
                gap_lock(d);
                waits++;
            }
            throttle_calls++;
            d->next_frame_time_us = 0;
            return;
        }
    }

    if (d->next_frame_time_us == 0 ||
        now_us - d->next_frame_time_us > EP_FRAME_US) {
        if (d->next_frame_time_us) resets++;
        d->next_frame_time_us = now_us;
    }

    if (!timer_checked) {
        /* SleepConditionVariableCS has system-timer granularity and routinely
         * overshoots this 5.33-ms audio deadline by 5-15 ms. A high-resolution
         * waitable timer keeps the same real-time pace without busy spinning. */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        highres_timer = CreateWaitableTimerExW(NULL, NULL,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        timer_checked = true;
        fprintf(stderr, "[AUDIO-PACE] high-resolution timer %s\n",
                highres_timer ? "active" : "unavailable");
    }
    while (!d->pause_requested) {
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t remaining_us = d->next_frame_time_us - now_us;
        if (remaining_us <= 0)
            break;
        if (highres_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -remaining_us * 10; /* relative, 100-ns units */
            if (SetWaitableTimerEx(highres_timer, &due, 0, NULL, NULL, NULL, 0)) {
                qemu_mutex_unlock(&d->lock);
                WaitForSingleObject(highres_timer, INFINITE);
                qemu_mutex_lock(&d->lock);
                continue;
            }
        }
        qemu_cond_timedwait(&d->cond, &d->lock,
                            (int)((remaining_us + 999) / 1000));
    }
    now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (now_us > d->next_frame_time_us) {
        uint64_t late = (uint64_t)(now_us - d->next_frame_time_us);
        late_calls++;
        late_us += late;
        if (late > max_late_us) max_late_us = late;
    }
    throttle_calls++;
    if (getenv("RECOMP_AUDIO_DIAG") && now_us - report_at >= 2000000) {
        fprintf(stderr, "[AUDIO-PACE] calls=%llu resets=%llu late=%llu "
                "avg_late_us=%.0f max_late_us=%llu\n",
                (unsigned long long)throttle_calls, (unsigned long long)resets,
                (unsigned long long)late_calls,
                late_calls ? (double)late_us / late_calls : 0.0,
                (unsigned long long)max_late_us);
        throttle_calls = resets = late_calls = late_us = max_late_us = 0;
        report_at = now_us;
    }
    d->next_frame_time_us += EP_FRAME_US;

    d->sleep_acc_us += (int)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - now_us);
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    int64_t t0 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t t1, t2;
    if (apu_surround_active()) {
        /* xemu surround branch se_frame: the EP runs across the frame, the
         * GP once the VP has filled the mixbins (docs Update 611). */
        apu_surround_frame_begin(d);
        mcpx_apu_vp_frame(d, mixbins);
        t1 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        s_gap.vp_us += t1 - t0;
        apu_surround_frame_gp(d, g_apu_vp_frame_voices ? mixbins : NULL);
        apu_surround_frame_end(d);
    } else {
        mcpx_apu_vp_frame(d, mixbins);
        t1 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        s_gap.vp_us += t1 - t0;
        mcpx_apu_dsp_frame(d, mixbins);
    }
    t2 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    mcpx_apu_monitor_frame(d, true);
    int64_t t3 = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (getenv("RECOMP_AUDIO_DIAG")) {
        static uint64_t vp_us, dsp_us, monitor_us, frames;
        static int64_t report_at;
        vp_us += t1 - t0;
        dsp_us += t2 - t1;
        monitor_us += t3 - t2;
        frames++;
        if (t3 - report_at >= 2000000) {
            fprintf(stderr, "[AUDIO-COST] frames=%llu vp_us=%.1f dsp_us=%.1f "
                    "monitor_us=%.1f budget_us=666.7\n",
                    (unsigned long long)frames, (double)vp_us / frames,
                    (double)dsp_us / frames, (double)monitor_us / frames);
            frames = vp_us = dsp_us = monitor_us = 0;
            report_at = t3;
        }
    }

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

/* Guest logical processor, as xbox_GuestCpu (RECOMP_GUEST_CPU, docs Update
 * 646c); read here so this library links without the kernel. */
static DWORD_PTR apu_guest_mask(void)
{
    const char *e = getenv("RECOMP_GUEST_CPU");
    int lp = e ? atoi(e) : 0;
    return (DWORD_PTR)1 << ((lp > 0 && lp < 64) ? lp : 0);
}

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);

    /* The APU is separate hardware running alongside the CPU: keep this
     * thread off host core 0, which the guest threads are pinned to (see
     * xbox_PinGuestThread in src/kernel/xbox_memory_layout.c; not called
     * directly so this library still links without the kernel). */
    {
        DWORD_PTR proc = 0, sys = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)
                && (proc & ~apu_guest_mask()))
            SetThreadAffinityMask(GetCurrentThread(), proc & ~apu_guest_mask());
    }
    qemu_mutex_lock(&d->lock);

    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        /* Always run the audio output loop — the software mixer and test tone
         * need continuous frame delivery regardless of APU register state.
         * The VP/DSP pipeline (se_frame) only runs when registers allow it. */
        {
            int64_t t = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            throttle(d);
            s_gap.throttle_us += qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t;
        }

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);

        /* As xemu's mcpx_apu_frame_thread: a stopped/trapped/halted front end
         * and anything the last frame flagged (SE2FE_IDLE_VOICE trap, notifier
         * status) go through update_irq, which sets ISTS and drives the IRQ
         * line. This block was missing, so ISTS never showed the FE trap and
         * DirectSound's ISR never learned a voice had gone idle (docs Update
         * 347: IDirectSoundStream_Flush then waited forever on 0x8000). */
        if (xcntmode == NV_PAPU_SECTL_XCNTMODE_OFF ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED)) {
            d->set_irq = true;
        }
        if (d->set_irq) {
            update_irq(d);
            d->set_irq = false;
        }

        bool apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED);

        if (apu_active && !g_test_tone.active) {
            /* Full pipeline: VP voices → DSP → monitor → waveOut */
            int64_t t = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            se_frame(d);
            s_gap.frame_us += qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t;
            s_gap.frames++;
        } else if (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF && !g_test_tone.active &&
                   !g_mixer_active_count && audio_wait_on_trap()) {
            /* Front end trapped/halted while the APU runs: DirectSound's ISR
             * has to service it (SE2FE idle voice, notifier ...). As xemu's
             * frame thread: wait for that, produce no frame -- the sound
             * card's queue (Update 506) bridges the pause. Emitting a silent
             * 256-sample block per pass here instead put 5 ms gaps into
             * music and ambience whenever the guest answered a trap late:
             * the crackle / on-off of docs Update 507. */
            int64_t t = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            qemu_cond_timedwait(&d->cond, &d->lock, 1);
            s_gap.trap_us += qemu_clock_get_us(QEMU_CLOCK_REALTIME) - t;
            s_gap.traps++;
        } else {
            /* Lightweight: just monitor frame (test tone + software mixer) */
            mcpx_apu_monitor_frame(d, false);
            d->ep_frame_div++;
        }

        /* A guest thread is waiting for d->lock (apu_lock_for_mmio): let it
         * in before the next frame. Bounded, so a stream of writers cannot
         * stall audio either. */
        if (qatomic_read(&d->lock_waiters)) {
            DWORD t0 = GetTickCount();
            int64_t th = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
            qemu_mutex_unlock(&d->lock);
            while (qatomic_read(&d->lock_waiters) && GetTickCount() - t0 < 5)
                Sleep(0);
            qemu_mutex_lock(&d->lock);
            s_gap.handover_us += qemu_clock_get_us(QEMU_CLOCK_REALTIME) - th;
        }
    }

    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    /* As xemu's mcpx_apu_reset_locked (upstream FIXME: reset DSP state). */
    if (d->gp.dsp)
        dsp_invalidate_opcache(d->gp.dsp);
    if (d->ep.dsp)
        dsp_invalidate_opcache(d->ep.dsp);
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Init DSP (GP/EP - stubbed) */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Init monitor (waveOut output) */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    mcpx_apu_wait_for_idle(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    qemu_mutex_lock(&d->lock);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    qemu_thread_join(&d->apu_thread);
    mcpx_apu_vp_finalize(d);
    mcpx_apu_monitor_finalize(d);

    free(d);
    g_state = NULL;
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    } else if (addr >= 0x30000 && addr < 0x40000) {
        /* GP DSP window (xemu: "mcpx-apu-gp", 0x10000 at +0x30000) */
        if (is_write) {
            mcpx_apu_gp_write(d, addr - 0x30000, val, size);
        }
    } else if (addr >= 0x50000 && addr < 0x60000) {
        /* EP DSP window (xemu: "mcpx-apu-ep", 0x10000 at +0x50000) */
        if (is_write) {
            mcpx_apu_ep_write(d, addr - 0x50000, val, size);
        }
    }
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    } else if (addr >= 0x30000 && addr < 0x40000) {
        return mcpx_apu_gp_read(d, addr - 0x30000, size);
    } else if (addr >= 0x50000 && addr < 0x60000) {
        return mcpx_apu_ep_read(d, addr - 0x50000, size);
    }
    return 0;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    if (!d) return;
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
}

/* ============================================================
 * APU Test Tone - Direct waveOut sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that waveOut
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Make sure waveOut is running - enable SECTL and resume APU thread */
    qemu_mutex_lock(&d->lock);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers to waveOut
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    g_mixer_voices[slot].active = 0;
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || v->pcm_bytes == 0) return;
    v->looping = looping;
    v->play_offset = 0;
    v->active = 1;
    InterlockedIncrement((volatile LONG *)&g_mixer_active_count);

    /* Wake up APU thread if it was paused */
    extern MCPXAPUState *g_state;
    if (g_state) {
        qemu_mutex_lock(&g_state->lock);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * play_offset is stored as a 16.16 fixed-point source frame position. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint32_t inc = (uint32_t)(((uint64_t)voice->sample_rate << 16) / 48000);
        uint32_t pos = voice->play_offset; /* 16.16 fixed-point */
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos = 0;
                    src_frame = 0;
                } else {
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = 0;
            } else if (voice->active) {
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
}
