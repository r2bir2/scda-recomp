/*
 * QEMU Geforce NV2A profiling helpers
 *
 * Copyright (c) 2020-2024 Matt Borgerson
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

#include "hw/xbox/nv2a/nv2a_int.h"
#include <stdio.h>
#include <stdlib.h>

NV2AStats g_nv2a_stats;

/* Opt-in per-guest-frame pacing diagnostic. All three callbacks run on the
 * PFIFO/PGRAPH thread; the normal renderer pays only the cached flag check.
 * Counters and waits are reset at each FLIP_STALL, not at a wall-clock tick. */
static struct {
    uint64_t main_us, aux_us, main_max_us, aux_max_us;
    uint64_t main_reason_us[9], aux_reason_us[4];
    unsigned main_calls, aux_calls;
    int64_t last_stall_us;
} s_recomp_frame_pace;

int nv2a_recomp_frame_pace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_NV2A_FRAME_PACE");
        enabled = e && e[0] == '1';
    }
    return enabled != 0;
}

void nv2a_recomp_frame_pace_wait(int aux, int reason, uint64_t elapsed_us)
{
    if (!nv2a_recomp_frame_pace_enabled())
        return;
    if (aux) {
        s_recomp_frame_pace.aux_calls++;
        s_recomp_frame_pace.aux_us += elapsed_us;
        s_recomp_frame_pace.aux_max_us =
            MAX(s_recomp_frame_pace.aux_max_us, elapsed_us);
        if (reason < 0 || reason >= 4)
            reason = 0;
        s_recomp_frame_pace.aux_reason_us[reason] += elapsed_us;
    } else {
        s_recomp_frame_pace.main_calls++;
        s_recomp_frame_pace.main_us += elapsed_us;
        s_recomp_frame_pace.main_max_us =
            MAX(s_recomp_frame_pace.main_max_us, elapsed_us);
        if (reason >= 0 && reason < 9)
            s_recomp_frame_pace.main_reason_us[reason] += elapsed_us;
    }
}

void nv2a_profile_increment(void)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    const int64_t fps_update_interval = 250000;
    g_nv2a_stats.last_flip_time = now;

    static int64_t frame_count = 0;
    frame_count++;

    static int64_t ts = 0;
    int64_t delta = now - ts;
    if (delta >= fps_update_interval) {
        g_nv2a_stats.increment_fps = frame_count * 1000000 / delta;
        ts = now;
        frame_count = 0;
    }
}

void nv2a_profile_flip_stall(unsigned frame)
{
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t render_time = (now-g_nv2a_stats.last_flip_time)/1000;

    if (nv2a_recomp_frame_pace_enabled()) {
        int64_t interval_us = s_recomp_frame_pace.last_stall_us ?
            now - s_recomp_frame_pace.last_stall_us : 0;
        s_recomp_frame_pace.last_stall_us = now;
        /* Window RECOMP_NV2A_FRAME_PACE_RANGE=<from>,<to> (docs Update 639;
         * default 120..799). */
        static int pace_from = -1, pace_to;
        if (pace_from < 0) {
            const char *e = getenv("RECOMP_NV2A_FRAME_PACE_RANGE");
            pace_from = 120;
            pace_to = 800;
            if (e)
                sscanf(e, "%d,%d", &pace_from, &pace_to);
        }
        if ((int)frame >= pace_from && (int)frame < pace_to) {
            const int *c = g_nv2a_stats.frame_working.counters;
            fprintf(stderr,
                    "[GPU-FRAME-PACE] t_ms=%lld frame=%u interval_us=%lld "
                    "main_us=%llu main_calls=%u main_max_us=%llu "
                    "aux_us=%llu aux_calls=%u aux_max_us=%llu "
                    "main_create_us=%llu main_down_us=%llu "
                    "main_flip_us=%llu aux_download_us=%llu "
                    "aux_upload_us=%llu aux_display_us=%llu "
                    "inline=%d arrays=%d passes=%d down=%d up=%d "
                    "tex=%d shader_gen=%d\n",
                    (long long)(now / 1000), frame, (long long)interval_us,
                    (unsigned long long)s_recomp_frame_pace.main_us,
                    s_recomp_frame_pace.main_calls,
                    (unsigned long long)s_recomp_frame_pace.main_max_us,
                    (unsigned long long)s_recomp_frame_pace.aux_us,
                    s_recomp_frame_pace.aux_calls,
                    (unsigned long long)s_recomp_frame_pace.aux_max_us,
                    (unsigned long long)s_recomp_frame_pace.main_reason_us[1],
                    (unsigned long long)s_recomp_frame_pace.main_reason_us[2],
                    (unsigned long long)s_recomp_frame_pace.main_reason_us[6],
                    (unsigned long long)s_recomp_frame_pace.aux_reason_us[1],
                    (unsigned long long)s_recomp_frame_pace.aux_reason_us[2],
                    (unsigned long long)s_recomp_frame_pace.aux_reason_us[3],
                    c[NV2A_PROF_INLINE_ELEMENTS], c[NV2A_PROF_DRAW_ARRAYS],
                    c[NV2A_PROF_PIPELINE_RENDERPASSES],
                    c[NV2A_PROF_SURF_DOWNLOAD], c[NV2A_PROF_SURF_UPLOAD],
                    c[NV2A_PROF_SURF_TO_TEX], c[NV2A_PROF_SHADER_GEN]);
        }
        s_recomp_frame_pace.main_us = s_recomp_frame_pace.aux_us = 0;
        s_recomp_frame_pace.main_max_us = s_recomp_frame_pace.aux_max_us = 0;
        s_recomp_frame_pace.main_calls = s_recomp_frame_pace.aux_calls = 0;
        memset(s_recomp_frame_pace.main_reason_us, 0,
               sizeof s_recomp_frame_pace.main_reason_us);
        memset(s_recomp_frame_pace.aux_reason_us, 0,
               sizeof s_recomp_frame_pace.aux_reason_us);
    }

    g_nv2a_stats.frame_working.mspf = render_time;
    g_nv2a_stats.frame_history[g_nv2a_stats.frame_ptr] =
        g_nv2a_stats.frame_working;
    g_nv2a_stats.frame_ptr =
        (g_nv2a_stats.frame_ptr + 1) % NV2A_PROF_NUM_FRAMES;
    g_nv2a_stats.frame_count++;
    memset(&g_nv2a_stats.frame_working, 0, sizeof(g_nv2a_stats.frame_working));
}

const char *nv2a_profile_get_counter_name(unsigned int cnt)
{
    const char *default_names[NV2A_PROF__COUNT] = {
        #define _X(x) stringify(x),
        NV2A_PROF_COUNTERS_XMAC
        #undef _X
    };

    assert(cnt < NV2A_PROF__COUNT);
    return default_names[cnt] + 10; /* 'NV2A_PROF_' */
}

int nv2a_profile_get_counter_value(unsigned int cnt)
{
    assert(cnt < NV2A_PROF__COUNT);
    unsigned int idx = (g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1) %
                       NV2A_PROF_NUM_FRAMES;
    return g_nv2a_stats.frame_history[idx].counters[cnt];
}
