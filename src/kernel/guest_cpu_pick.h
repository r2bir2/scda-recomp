/* Guest CPU choice from a CPU topology (docs Update 647b).
 *
 * Pure function so the rule can be checked against other processors'
 * layouts (tools/kernel_audit/test_guest_cpu_pick.c); xbox_memory_layout.c
 * feeds it the real topology from GetSystemCpuSetInformation and
 * GetLogicalProcessorInformationEx.
 *
 *   1. only the fastest cores (highest efficiency class: Intel P-cores,
 *      never E-cores);
 *   2. among their L3 caches the largest (an X3D part's V-Cache CCD), on a
 *      tie one without CPU 0 (second CCD / CCX), else the later one;
 *   3. in that cache the last physical core other than core 0, its first
 *      logical processor.
 */
#pragma once
#include <stdint.h>

typedef struct { int lp, core, eff; } GuestCpuLp;
typedef struct { uint64_t mask; uint32_t size; } GuestCpuL3;

static inline int guest_cpu_pick(const GuestCpuLp *lps, int nlp,
                                 const GuestCpuL3 *l3, int nl3)
{
    int maxeff = 0, core0 = -1, k;
    uint64_t fast = 0, group;

    if (nlp <= 0)
        return 0;
    for (k = 0; k < nlp; k++) {
        if (lps[k].eff > maxeff)
            maxeff = lps[k].eff;
        if (lps[k].lp == 0)
            core0 = lps[k].core;
    }
    for (k = 0; k < nlp; k++)
        if (lps[k].eff == maxeff && lps[k].lp < 64)
            fast |= (uint64_t)1 << lps[k].lp;

    group = fast;
    {
        uint64_t best = 0;
        uint32_t best_size = 0;
        int best_has0 = 1;
        for (k = 0; k < nl3; k++) {
            uint64_t m = l3[k].mask & fast;
            int has0 = (m & 1) != 0;
            if (m && (!best || l3[k].size > best_size ||
                      (l3[k].size == best_size &&
                       (has0 < best_has0 || (has0 == best_has0 && m > best))))) {
                best = m;
                best_size = l3[k].size;
                best_has0 = has0;
            }
        }
        if (best)
            group = best;
    }

    {
        int pick = -1, pick_core = -1, pass;
        for (pass = 0; pass < 2 && pick < 0; pass++)
            for (k = 0; k < nlp; k++) {
                if (lps[k].lp >= 64 || !(group & ((uint64_t)1 << lps[k].lp)) ||
                    (pass == 0 && lps[k].core == core0))
                    continue;
                if (lps[k].core > pick_core ||
                    (lps[k].core == pick_core && lps[k].lp < pick)) {
                    pick_core = lps[k].core;
                    pick = lps[k].lp;
                }
            }
        return pick >= 0 ? pick : 0;
    }
}
