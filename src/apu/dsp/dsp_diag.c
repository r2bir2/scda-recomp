/*
 * Execution history for the DSP interpreter, local to xboxrecomp (not from
 * xemu).
 *
 * The shim trace.h routes xemu's trace_dsp56k_execute_instruction(_disasm)
 * hooks here. Keeping the last PCs costs one store per instruction and lets
 * apu_dsp.c print how a DSP reached an illegal instruction or assert
 * (docs Update 344).
 *
 * Which DSP is executing comes from g_recomp_dsp_cur_is_gp, set by
 * apu_dsp.c around each dsp_run: the interpreter's own core->is_gp is only
 * copied in by sync_from_vm and stays false for the GP otherwise.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RING 256u

int g_recomp_dsp_cur_is_gp;
static uint32_t g_ring[2][RING];
static uint32_t g_ring_pos[2];
static uint32_t g_last_pc;

void recomp_dsp_pc_ring_push(int is_gp, uint32_t pc)
{
    unsigned g = g_recomp_dsp_cur_is_gp ? 1u : 0u;
    (void)is_gp;
    g_ring[g][g_ring_pos[g]++ % RING] = pc;
    g_last_pc = pc;
}

/* Copies up to RING most recent PCs, oldest first; returns the count. */
unsigned recomp_dsp_pc_ring_get(int is_gp, uint32_t *out, unsigned max)
{
    unsigned g = is_gp ? 1u : 0u;
    unsigned total = g_ring_pos[g] < RING ? g_ring_pos[g] : RING;
    unsigned n = total < max ? total : max;
    for (unsigned i = 0; i < n; i++)
        out[i] = g_ring[g][(g_ring_pos[g] - n + i) % RING];
    return n;
}

/* Disassembly of executed instructions, only filled while
 * g_recomp_dsp_disasm is set. Each P address is recorded at most
 * TEXT_PER_PC times per DSP, so loops do not push the path that led into
 * them out of the ring. */
#define TEXT_RING 4096u
#define TEXT_LEN 100u
#define TEXT_PER_PC 3u

int g_recomp_dsp_disasm;
static char g_text[TEXT_RING][TEXT_LEN];
static unsigned g_text_pos;
static uint8_t g_text_seen[2][4096];

void recomp_dsp_disasm_ring_push(const char *text)
{
    unsigned g = g_recomp_dsp_cur_is_gp ? 1u : 0u;
    uint8_t *seen = &g_text_seen[g][g_last_pc & 0xFFFu];
    char *slot;

    if (*seen >= TEXT_PER_PC)
        return;
    (*seen)++;
    slot = g_text[g_text_pos++ % TEXT_RING];
    snprintf(slot, TEXT_LEN, "%s %s", g ? "GP" : "EP", text);
}

void recomp_dsp_disasm_ring_dump(FILE *f)
{
    unsigned n = g_text_pos < TEXT_RING ? g_text_pos : TEXT_RING;
    for (unsigned i = 0; i < n; i++) {
        const char *t = g_text[(g_text_pos - n + i) % TEXT_RING];
        size_t len = strlen(t);
        fprintf(f, "[DSPDIS] %s%s", t, (len && t[len - 1] == '\n') ? "" : "\n");
    }
}
