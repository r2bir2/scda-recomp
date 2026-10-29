/*
 * HLE renderer core (agent_handoff/HLE_RENDERER_PLAN.md).
 *
 * Every Direct3D 8 entry point of the title goes through hle_d3d_call()
 * (hle_d3d_wrappers.c, generated). Nonzero means the HLE renderer handled
 * the call; zero runs the recompiled original, which drives the emulated
 * NV2A (the LLE path, src/gpu).
 *
 * RECOMP_HLE_D3D=1 (read once) hands the calls to the HLE renderer
 * (hle/hle_glue.cpp, hle/tgp/xdk/d3d8.cpp); otherwise they are only counted.
 * RECOMP_HLE_D3D_STATS=1 prints the busiest entry points every 5 s.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "recomp_types.h"
#include "hle_d3d_ids.h"

#if defined(_WIN32)
#include <windows.h>
#define HLE_TICK() GetTickCount()
#else
#include <time.h>
static uint32_t hle_tick(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}
#define HLE_TICK() hle_tick()
#endif

static uint64_t s_calls[HLE_D3D__COUNT];
static int s_stats = -1;

int hle_d3d_dispatch(int id);                   /* hle/hle_glue.cpp */
int hle_d3d_dispatch_named(const char *name);

int hle_d3d_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_HLE_D3D");
        on = e && e[0] == '1';
        if (on)
            fprintf(stderr, "[HLE-D3D] HLE renderer on (RECOMP_HLE_D3D=1)\n");
    }
    return on;
}

/* D3DDevice_Swap has its wrapper in recomp_manual.c (60 FPS policy) */
int hle_d3d_swap(void)
{
    return hle_d3d_enabled() && hle_d3d_dispatch_named("D3DDevice_Swap");
}

static void hle_d3d_report(void)
{
    static uint32_t last;
    static uint64_t prev[HLE_D3D__COUNT];
    uint32_t now = HLE_TICK();
    int order[HLE_D3D__COUNT], n = 0;

    if (last && now - last < 5000)
        return;
    if (!last) {
        last = now;
        return;
    }
    for (int i = 0; i < HLE_D3D__COUNT; i++)
        if (s_calls[i] != prev[i])
            order[n++] = i;
    for (int i = 1; i < n; i++)          /* by calls in this interval */
        for (int j = i; j > 0 &&
             s_calls[order[j]] - prev[order[j]] > s_calls[order[j - 1]] - prev[order[j - 1]]; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    fprintf(stderr, "[HLE-D3D] calls in %u ms (%d entry points):", now - last, n);
    for (int i = 0; i < n && i < 24; i++)
        fprintf(stderr, " %s=%llu", g_hle_d3d_info[order[i]].name,
                (unsigned long long)(s_calls[order[i]] - prev[order[i]]));
    fputc('\n', stderr);
    memcpy(prev, s_calls, sizeof prev);
    last = now;
}

int hle_d3d_call(enum hle_d3d_id id)
{
    s_calls[id]++;
    if (s_stats < 0) {
        const char *e = getenv("RECOMP_HLE_D3D_STATS");
        s_stats = e && e[0] == '1';
    }
    if (s_stats) {
        static uint32_t total;
        if ((++total & 1023u) == 0)
            hle_d3d_report();
    }
    return hle_d3d_enabled() ? hle_d3d_dispatch((int)id) : 0;
}
