/* Guest CPU choice for common processor layouts (docs Update 647b).
 * Build and run: cl /nologo /I src\kernel tools\kernel_audit\test_guest_cpu_pick.c
 * (any C compiler; prints PASS/FAIL per layout, exit code = failures). */
#include <stdio.h>
#include "guest_cpu_pick.h"

static int nlp;
static GuestCpuLp lps[64];

/* `cores` cores from core index `core0` with `smt` threads each, logical
 * processors numbered consecutively from `lp0`, efficiency class `eff`. */
static void add(int lp0, int core0, int cores, int smt, int eff)
{
    for (int c = 0; c < cores; c++)
        for (int t = 0; t < smt; t++) {
            lps[nlp].lp = lp0 + c * smt + t;
            lps[nlp].core = core0 + c;
            lps[nlp].eff = eff;
            nlp++;
        }
}

static uint64_t bits(int from, int count)
{
    return (count >= 64 ? ~0ull : ((1ull << count) - 1)) << from;
}

static int failures;

static void check(const char *name, const GuestCpuL3 *l3, int nl3, int want)
{
    int got = guest_cpu_pick(lps, nlp, l3, nl3);
    printf("%-44s pick %2d want %2d  %s\n", name, got, want, got == want ? "PASS" : "FAIL");
    failures += got != want;
    nlp = 0;
}

int main(void)
{
    add(0, 0, 6, 1, 0);
    { GuestCpuL3 c[] = { { bits(0, 6), 9u << 20 } };
      check("Intel i5-9600K (6C/6T, one L3)", c, 1, 5); }

    add(0, 0, 8, 2, 1);           /* P-cores */
    add(16, 8, 4, 1, 0);          /* E-cores */
    { GuestCpuL3 c[] = { { bits(0, 20), 25u << 20 } };
      check("Intel i7-12700K (8P+4E, one L3)", c, 1, 14); }

    add(0, 0, 6, 2, 0);
    { GuestCpuL3 c[] = { { bits(0, 6), 8u << 20 }, { bits(6, 6), 8u << 20 } };
      check("Ryzen 5 2600 (2 CCX x 3C/6T)", c, 2, 10); }

    add(0, 0, 6, 2, 0);
    { GuestCpuL3 c[] = { { bits(0, 12), 32u << 20 } };
      check("Ryzen 5 5600X (6C/12T, one CCD)", c, 1, 10); }

    add(0, 0, 8, 2, 0);
    { GuestCpuL3 c[] = { { bits(0, 16), 32u << 20 } };
      check("Ryzen 7 5800X (8C/16T, one CCD)", c, 1, 14); }

    add(0, 0, 16, 2, 0);
    { GuestCpuL3 c[] = { { bits(0, 16), 32u << 20 }, { bits(16, 16), 32u << 20 } };
      check("Ryzen 9 5950X (2 CCD)", c, 2, 30); }

    add(0, 0, 16, 2, 0);
    { GuestCpuL3 c[] = { { bits(0, 16), 96u << 20 }, { bits(16, 16), 32u << 20 } };
      check("Ryzen 9 7950X3D (V-Cache on CCD0)", c, 2, 14); }

    add(0, 0, 12, 2, 0);
    { GuestCpuL3 c[] = { { bits(0, 12), 96u << 20 }, { bits(12, 12), 32u << 20 } };
      check("Ryzen 9 7900X3D (V-Cache on CCD0)", c, 2, 10); }

    add(0, 0, 8, 2, 0);
    { GuestCpuL3 c[] = { { bits(0, 16), 96u << 20 } };
      check("Ryzen 7 7800X3D (one CCD, V-Cache)", c, 1, 14); }

    add(0, 0, 2, 1, 0);
    { GuestCpuL3 c[] = { { bits(0, 2), 4u << 20 } };
      check("dual core without SMT", c, 1, 1); }

    add(0, 0, 1, 1, 0);
    check("single core, no cache information", NULL, 0, 0);

    add(0, 0, 4, 2, 0);
    check("4C/8T, no cache information", NULL, 0, 6);

    return failures;
}
