/*
 * Consumer API of the JIT DSP core (src/apu/dsp_jit, xemu surround branch
 * dsp.c/dma.c renamed by dspjit_rename.h). Used by src/apu/apu_surround.c.
 * The core itself is mborgerson/dsp56300, loaded at runtime from
 * dsp56300.dll (dsp56300_dll.c); without that DLL nothing here is usable
 * and the APU stays on the stereo interpreter path.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct DSPJitState DSPJitState;

typedef void (*dspjit_scratch_rw_func)(
    void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir);
typedef void (*dspjit_fifo_rw_func)(
    void *opaque, uint8_t *ptr, unsigned int index, size_t len, bool dir);

/* One SE frame of the core's clock (dsp.h DSP_FRAME_CYCLES). */
#define DSPJIT_FRAME_CYCLES 88875

/* Loads dsp56300.dll once; 1 when the core is available. */
int dsp56300_dll_load(void);

DSPJitState *dspjit_init(void *rw_opaque, dspjit_scratch_rw_func scratch_rw,
                         dspjit_fifo_rw_func fifo_rw, bool is_gp);
void dspjit_reset(DSPJitState *dsp);
void dspjit_run(DSPJitState *dsp, int cycles);
void dspjit_bootstrap(DSPJitState *dsp);
void dspjit_start_frame(DSPJitState *dsp);
bool dspjit_frame_start_pending(DSPJitState *dsp);
void dspjit_frame_tick(DSPJitState *dsp);
void dspjit_get_pc_sp(DSPJitState *dsp, uint32_t *pc, uint32_t *sp,
                      uint32_t ssh[16]);
uint32_t dspjit_read_memory(DSPJitState *dsp, char space, uint32_t addr);
void dspjit_write_memory(DSPJitState *dsp, char space, uint32_t addr,
                         uint32_t value);
bool dspjit_get_halt_requested(DSPJitState *dsp);
void dspjit_set_halt_requested(DSPJitState *dsp, bool idle);
uint32_t dspjit_get_cycle_count(DSPJitState *dsp);
void dspjit_set_cycle_count(DSPJitState *dsp, uint32_t count);
void dspjit_invalidate_opcache(DSPJitState *dsp);

extern uint64_t g_dspjit_gp_halts, g_dspjit_ep_halts, g_dspjit_dma_p_writes;

/* Provided by the consumer (apu_surround.c): the EP cleared its DMA EOL
 * interrupt (xemu's MCPX_EP_SNAPSHOT node trigger). */
void dspjit_ep_eol_clear_hook(DSPJitState *dsp);
