/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2021 Matt Borgerson
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

#include "nv2a_int.h"

/* USER - PFIFO MMIO and DMA submission area */

/* RECOMP_PFIFO_LOCK_PROBE=1 (docs Update 643): how long the title's thread
 * waits for pfifo.lock here while the PFIFO thread holds it. One line per
 * ~2 s: accesses, contended ones, total and longest wait. */
static void user_lock(NV2AState *d)
{
    static int probe = -1;
    static LONG64 n, contended, wait_ticks, max_ticks;
    static LONG64 last_print;
    LARGE_INTEGER t0, t1, f;
    if (probe < 0) {
        const char *e = getenv("RECOMP_PFIFO_LOCK_PROBE");
        probe = e && e[0] == '1';
    }
    if (!probe) {
        qemu_mutex_lock(&d->pfifo.lock);
        return;
    }
    n++;
    if (!qemu_mutex_trylock(&d->pfifo.lock))
        goto locked;
    QueryPerformanceCounter(&t0);
    qemu_mutex_lock(&d->pfifo.lock);
    QueryPerformanceCounter(&t1);
    contended++;
    wait_ticks += t1.QuadPart - t0.QuadPart;
    if (t1.QuadPart - t0.QuadPart > max_ticks)
        max_ticks = t1.QuadPart - t0.QuadPart;
locked:
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    if (!last_print)
        last_print = t1.QuadPart;
    if (t1.QuadPart - last_print > 2 * f.QuadPart) {
        double s = (double)(t1.QuadPart - last_print) / (double)f.QuadPart;
        fprintf(stderr, "[PFIFO-LOCK] %.0f accesses/s, %.0f contended/s, "
                "wait %.1f ms/s, max %.2f ms\n", n / s, contended / s,
                1000.0 * wait_ticks / (double)f.QuadPart / s,
                1000.0 * max_ticks / (double)f.QuadPart);
        n = contended = wait_ticks = max_ticks = 0;
        last_print = t1.QuadPart;
    }
}
/* The title's side of the pushbuffer handshake without pfifo.lock (recomp,
 * docs Update 643). The PFIFO thread holds that lock while it decodes the
 * pushbuffer and while it waits for the host GPU, and an SRW lock lets it
 * take the lock straight back after each method batch, so the title's
 * DMA_GET polls and DMA_PUT kicks queued behind it -- ~75 ms per second of
 * the game thread at the measured spot. On hardware these are register
 * accesses that never wait. Reads are single aligned 32-bit loads; a
 * DMA_PUT store is followed by pfifo_wake, which the PFIFO thread cannot
 * miss (pfifo.c). RECOMP_PFIFO_LOCKFREE_USER=0 restores the locked path. */
static bool user_lockfree(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PFIFO_LOCKFREE_USER");
        on = !(e && e[0] == '0');
    }
    return on;
}

uint64_t user_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    unsigned int channel_id = addr >> 16;
    assert(channel_id < NV2A_NUM_CHANNELS);

    const bool lockfree = user_lockfree();
    if (!lockfree)
        user_lock(d);

    uint32_t channel_modes = d->pfifo.regs[NV_PFIFO_MODE];

    uint64_t r = 0;
    if (channel_modes & (1 << channel_id)) {
        /* DMA Mode */

        unsigned int cur_channel_id =
            GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                     NV_PFIFO_CACHE1_PUSH1_CHID);

        if (channel_id == cur_channel_id) {
            switch (addr & 0xFFFF) {
            case NV_USER_DMA_PUT:
                r = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];
                break;
            case NV_USER_DMA_GET:
                r = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
                break;
            case NV_USER_REF:
                r = d->pfifo.regs[NV_PFIFO_CACHE1_REF];
                break;
            default:
                break;
            }
        } else {
            /* ramfc */
            assert(false);
        }
    } else {
        /* PIO Mode */
        assert(false);
    }

    if (!lockfree)
        qemu_mutex_unlock(&d->pfifo.lock);

    nv2a_reg_log_read(NV_USER, addr, size, r);
    return r;
}

void user_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_USER, addr, size, val);

    unsigned int channel_id = addr >> 16;
    assert(channel_id < NV2A_NUM_CHANNELS);

    if ((addr & 0xFFFF) == NV_USER_DMA_PUT && user_lockfree() &&
        (d->pfifo.regs[NV_PFIFO_MODE] & (1 << channel_id)) &&
        channel_id == GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                               NV_PFIFO_CACHE1_PUSH1_CHID)) {
        InterlockedExchange(
            (volatile LONG *)&d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT],
            (LONG)(uint32_t)val);
        pfifo_wake(d);
        return;
    }

    user_lock(d);

    uint32_t channel_modes = d->pfifo.regs[NV_PFIFO_MODE];
    if (channel_modes & (1 << channel_id)) {
        /* DMA Mode */
        unsigned int cur_channel_id =
            GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                     NV_PFIFO_CACHE1_PUSH1_CHID);

        if (channel_id == cur_channel_id) {
            switch (addr & 0xFFFF) {
            case NV_USER_DMA_PUT:
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = val;
                break;
            case NV_USER_DMA_GET:
                d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = val;
                break;
            case NV_USER_REF:
                d->pfifo.regs[NV_PFIFO_CACHE1_REF] = val;
                break;
            default:
                assert(false);
                break;
            }

            pfifo_kick(d);

        } else {
            /* ramfc */
            assert(false);
        }
    } else {
        /* PIO Mode */
        assert(false);
    }

    qemu_mutex_unlock(&d->pfifo.lock);

}
