/*
 * Vulkan-renderer side of the nv2a_recomp.c glue. The GL and Vulkan
 * renderers each define their own SurfaceBinding, so lookups into the
 * Vulkan surface cache live in this separate translation unit. Local code
 * for xboxrecomp, see src/gpu/PROVENANCE.md.
 */
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/pgraph/vk/renderer.h"

#include <windows.h>

int64_t nv2a_recomp_vk_now_us(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER ticks;
    if (!frequency.QuadPart) {
        QueryPerformanceFrequency(&frequency);
    }
    QueryPerformanceCounter(&ticks);
    return (int64_t)((ticks.QuadPart / frequency.QuadPart) * 1000000 +
                     ((ticks.QuadPart % frequency.QuadPart) * 1000000) /
                         frequency.QuadPart);
}

/* RECOMP_VK_TRACE_XFER=1 (docs Update 489): each distinct GPU<->RAM
 * transfer (surface read-back, texture upload) with its first host call
 * stack, then a count per kind every 600 transfers -- to find what the
 * renderer moves through guest RAM every frame. Host frames are exe
 * offsets; resolve them with scratchpad/tools/resolve_map.py. */
void nv2a_recomp_vk_trace_xfer(const char *what, hwaddr addr, unsigned w,
                               unsigned h, unsigned fmt, int color, int extra)
{
    static int on = -1;
    static struct { const char *what; hwaddr addr; unsigned w, h, fmt;
                    unsigned long n; } seen[128];
    static int nseen;
    static unsigned long total;
    int i;

    if (on < 0) {
        const char *e = getenv("RECOMP_VK_TRACE_XFER");
        on = e && e[0] == '1';
    }
    if (!on)
        return;
    total++;
    for (i = 0; i < nseen; i++)
        if (seen[i].what == what && seen[i].addr == addr && seen[i].w == w &&
            seen[i].h == h && seen[i].fmt == fmt)
            break;
    if (i == nseen && nseen < (int)ARRAY_SIZE(seen)) {
        void *frames[8];
        USHORT nf = RtlCaptureStackBackTrace(1, 8, frames, NULL);
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        char stack[256];
        int len = 0;
        for (USHORT f = 0; f < nf && len < (int)sizeof(stack) - 24; f++)
            len += snprintf(stack + len, sizeof(stack) - len, " %llX",
                            (unsigned long long)((uintptr_t)frames[f] - base));
        seen[nseen].what = what;
        seen[nseen].addr = addr;
        seen[nseen].w = w;
        seen[nseen].h = h;
        seen[nseen].fmt = fmt;
        seen[nseen].n = 0;
        nseen++;
        fprintf(stderr, "[VK-XFER] new %s addr=%08llX %ux%u fmt=0x%X color=%d "
                "extra=%d frame=%d stack:%s\n", what, (unsigned long long)addr,
                w, h, fmt, color, extra, g_nv2a->pgraph.frame_time, stack);
    }
    if (i < nseen)
        seen[i].n++;
    if (total % 600 == 0) {
        fprintf(stderr, "[VK-XFER] after %lu transfers (frame %d):", total,
                g_nv2a->pgraph.frame_time);
        for (i = 0; i < nseen; i++)
            if (seen[i].n)
                fprintf(stderr, " %s@%08llX=%lu", seen[i].what,
                        (unsigned long long)seen[i].addr, seen[i].n);
        fputc('\n', stderr);
        for (i = 0; i < nseen; i++)
            seen[i].n = 0;
    }
}

/* Pitch of the Vulkan surface containing addr, for the guest-RAM scan-out
 * fallback (update_scanout_fallback). Caller holds pfifo.lock and
 * pgraph.lock. */
bool nv2a_recomp_vk_surface_pitch(NV2AState *d, hwaddr addr, uint32_t *pitch)
{
    SurfaceBinding *surface;

    if (!d->pgraph.vk_renderer_state)
        return false;
    surface = pgraph_vk_surface_get_within(d, addr);
    if (!surface)
        return false;
    *pitch = surface->pitch;
    return true;
}
