/*
 * Stand-in for the QEMU-generated "trace.h" of hw/xbox/mcpx/apu/dsp,
 * local to xboxrecomp.
 *
 * xemu generates these from trace-events; all of them are disabled unless a
 * QEMU trace backend enables them. Here they compile away. The optional
 * RECOMP_DSP_TRACE hooks in src/apu/apu_dsp.c are the place for runtime
 * diagnostics instead.
 */
#pragma once

#include <stdint.h>

#define TRACE_DSP56K_EXECUTE_INSTRUCTION_DISASM 0
/* Per-instruction disassembly into a ring (dsp_diag.c) only while
 * RECOMP_DSP_DISASM is set; the only user is dsp56k_execute_instruction. */
extern int g_recomp_dsp_disasm;
#define trace_event_get_state(ev) ((void)(ev), g_recomp_dsp_disasm)

static inline void trace_dsp_read_peripheral(uint32_t address, uint32_t value)
{
    (void)address;
    (void)value;
}

static inline void trace_dsp_write_peripheral(uint32_t address, uint32_t value)
{
    (void)address;
    (void)value;
}

/* Diagnostics (docs Update 344): every executed PC goes into a small ring per
 * DSP (src/apu/dsp/dsp_diag.c), dumped by apu_dsp.c when the process aborts,
 * so an assert inside the interpreter shows how the DSP got there. */
void recomp_dsp_pc_ring_push(int is_gp, uint32_t pc);

static inline void trace_dsp56k_execute_instruction(int is_gp, uint32_t pc)
{
    recomp_dsp_pc_ring_push(is_gp, pc);
}

void recomp_dsp_disasm_ring_push(const char *text);

static inline void trace_dsp56k_execute_instruction_disasm(const char *text)
{
    recomp_dsp_disasm_ring_push(text);
}
