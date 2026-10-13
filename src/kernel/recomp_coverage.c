/*
 * Basic-block coverage recording for differential debugging (docs Updates
 * 460/465/466).
 *
 * RECOMP_TRACE_PC runs at every translated basic block; while
 * g_recomp_cov_on is set it calls recomp_cov_hit(). A 1-bit-per-VA map
 * (1 MB, cache friendly -- a 32 MB counter array froze the game while
 * recording) marks executed blocks, and every block's FIRST execution is
 * appended to an ordered list. xbox_coverage_toggle() (bound to a hotkey by
 * the game project) starts a recording; it ends by itself after
 * RECOMP_COV_SECONDS (default 6), on a second toggle, or right before a
 * title relaunch. The file coverage_<pid>_<n>.txt lists "VA order" per
 * executed block, sorted by VA; the order column is the first-execution
 * rank, so a diff of a working and a broken run can show the EARLIEST
 * block that only one of them reached, before any follow-up code.
 *
 * Unsynchronised on purpose: a lost mark only drops one block from the
 * set; the order list uses an interlocked index.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define COV_LO 0x00010000u
#define COV_HI 0x00800000u   /* covers .text and the XDK library sections */
#define COV_ORDER_MAX (1u << 20)

volatile int32_t g_recomp_cov_on;
static uint8_t *s_cov_bits;
static uint32_t *s_cov_order;
static volatile LONG s_cov_order_n;
static int s_cov_run;
static DWORD s_cov_start_tick;

void recomp_cov_hit(uint32_t xbox_va)
{
    uint32_t i;
    uint8_t m;
    if (xbox_va < COV_LO || xbox_va >= COV_HI || !s_cov_bits)
        return;
    i = xbox_va - COV_LO;
    m = (uint8_t)(1u << (i & 7));
    if (s_cov_bits[i >> 3] & m)
        return;
    s_cov_bits[i >> 3] |= m;
    {
        LONG n = InterlockedIncrement(&s_cov_order_n) - 1;
        if ((uint32_t)n < COV_ORDER_MAX)
            s_cov_order[n] = xbox_va;
    }
}

static void coverage_write(void);

/* Stops the recording and writes the file; only the caller that flips the
 * flag from 1 to 0 writes, so the auto-stop timer, the hotkey and the
 * relaunch hook cannot race each other. */
void xbox_coverage_stop_if_running(void)
{
    if (InterlockedCompareExchange((volatile LONG *)&g_recomp_cov_on, 0, 1) == 1)
        coverage_write();
}

/* Auto-stop (docs Update 465): the user cannot time a second key press
 * around a level-section relaunch, so a recording ends by itself after
 * RECOMP_COV_SECONDS (default 6) -- or earlier on relaunch/hotkey. */
static DWORD WINAPI coverage_timer(LPVOID arg)
{
    int run = (int)(intptr_t)arg;
    const char *e = getenv("RECOMP_COV_SECONDS");
    DWORD ms = (DWORD)((e && atoi(e) > 0 ? atoi(e) : 6) * 1000);
    Sleep(ms);
    if (run == s_cov_run)
        xbox_coverage_stop_if_running();
    return 0;
}

void xbox_coverage_toggle(void)
{
    if (!g_recomp_cov_on) {
        if (!s_cov_bits) {
            s_cov_bits = (uint8_t *)calloc((COV_HI - COV_LO) / 8, 1);
            s_cov_order = (uint32_t *)malloc(COV_ORDER_MAX * sizeof(uint32_t));
            if (!s_cov_bits || !s_cov_order) {
                fprintf(stderr, "[COVERAGE] cannot allocate the block map\n");
                return;
            }
        } else {
            memset(s_cov_bits, 0, (COV_HI - COV_LO) / 8);
        }
        InterlockedExchange(&s_cov_order_n, 0);
        s_cov_run++;
        s_cov_start_tick = GetTickCount();
        InterlockedExchange((volatile LONG *)&g_recomp_cov_on, 1);
        fprintf(stderr, "[COVERAGE] recording %d started (tick=%lu)\n", s_cov_run,
                (unsigned long)s_cov_start_tick);
        fflush(stderr);
        {
            HANDLE t = CreateThread(NULL, 0, coverage_timer,
                                    (LPVOID)(intptr_t)s_cov_run, 0, NULL);
            if (t)
                CloseHandle(t);
        }
        return;
    }
    xbox_coverage_stop_if_running();
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = ((const uint32_t *)a)[0], y = ((const uint32_t *)b)[0];
    return x < y ? -1 : x > y;
}

static void coverage_write(void)
{
    char path[MAX_PATH];
    FILE *f;
    uint32_t i, n, *pairs;
    Sleep(50);  /* let blocks already past the flag check finish marking */
    n = (uint32_t)s_cov_order_n;
    if (n > COV_ORDER_MAX)
        n = COV_ORDER_MAX;
    snprintf(path, sizeof(path), "coverage_%lu_%d.txt",
             (unsigned long)GetCurrentProcessId(), s_cov_run);
    f = fopen(path, "w");
    pairs = (uint32_t *)malloc((size_t)n * 2 * sizeof(uint32_t));
    if (!f || !pairs) {
        fprintf(stderr, "[COVERAGE] cannot write %s\n", path);
        if (f)
            fclose(f);
        free(pairs);
        return;
    }
    for (i = 0; i < n; i++) {
        pairs[2 * i] = s_cov_order[i];
        pairs[2 * i + 1] = i + 1;   /* first-execution rank */
    }
    qsort(pairs, n, 2 * sizeof(uint32_t), cmp_u32);
    fprintf(f, "# coverage run %d, %lu ms, column 2 = first-execution rank\n",
            s_cov_run, (unsigned long)(GetTickCount() - s_cov_start_tick));
    for (i = 0; i < n; i++)
        fprintf(f, "%08X %u\n", pairs[2 * i], pairs[2 * i + 1]);
    fclose(f);
    free(pairs);
    fprintf(stderr, "[COVERAGE] recording %d stopped: %u blocks -> %s\n",
            s_cov_run, n, path);
    fflush(stderr);
}
