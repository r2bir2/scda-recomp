/* 2026-09-03 (continued 26): minimal, fast basic-block tracer for the
 * v0.7.x-merge regression A/B comparison (OLD checkpoint build vs NEW
 * merged build, same XBE/game data). Deliberately bypasses
 * scda_trace_pc()'s large diagnostic if-chain -- that chain is fast only
 * because of its narrow runtime gate, and this needs to run across a much
 * wider PC range (enough to bracket the actual divergence) without the
 * per-call overhead of walking dozens of unrelated address comparisons.
 *
 * One compact, fixed-width line per basic-block entry:
 *   <pc> <esp> <ebx> <esi> <edi> <ebp>
 * to a dedicated file (SCDA_AB_TRACE_PATH env var, default
 * scda_ab_trace.log next to the exe) -- never stderr/stdout, so it never
 * mixes with scda_runtime.log. Buffered, and the buffer is sized up front
 * so this doesn't itself become the bottleneck.
 *
 * Enabled only when the SCDA_AB_TRACE CMake option is ON (see
 * projects/scda/CMakeLists.txt); RECOMP_TRACE_HANDLER is swapped to this
 * function instead of scda_trace_pc for that build, and the compile-time
 * trace range widens to bracket early boot.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"

static FILE *s_ab_trace_file;
static char s_ab_trace_buf[1 << 20]; /* 1 MB stdio buffer */

static void scda_ab_trace_close(void)
{
    if (s_ab_trace_file) {
        fclose(s_ab_trace_file);
        s_ab_trace_file = NULL;
    }
}

void scda_ab_trace_pc(uint32_t xbox_va)
{
    if (!s_ab_trace_file) {
        const char *path = getenv("SCDA_AB_TRACE_PATH");
        if (!path) path = "scda_ab_trace.log";
        s_ab_trace_file = fopen(path, "w");
        if (!s_ab_trace_file) return;
        setvbuf(s_ab_trace_file, s_ab_trace_buf, _IOFBF, sizeof(s_ab_trace_buf));
        atexit(scda_ab_trace_close);
    }
    fprintf(s_ab_trace_file, "%08X %08X %08X %08X %08X %08X\n",
            xbox_va, g_esp, g_ebx, g_esi, g_edi, g_ebp);
}
