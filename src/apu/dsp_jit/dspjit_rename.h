/*
 * Forced include (/FI) for the xbox_apu_dspjit library only.
 *
 * The imported xemu surround-branch DSP files (dsp.c, dma.c) export the same
 * names as the older C-interpreter DSP in src/apu/dsp/ (dsp_init, DSPState,
 * dsp_dma_read, ...). Both live in one executable -- the interpreter stays
 * the stereo default -- so this lib's copies are renamed to dspjit_* here,
 * leaving the imported files themselves unmodified in that respect.
 * Consumers use dspjit_api.h, never the imported headers.
 */
#pragma once

#define DSPState                   DSPJitState
#define DspCoreState               DSPJitCoreState
#define DSPDMAState                DSPJitDMAState
#define dsp_scratch_rw_func        dspjit_scratch_rw_func
#define dsp_fifo_rw_func           dspjit_fifo_rw_func

#define dsp_init                   dspjit_init
#define dsp_destroy                dspjit_destroy
#define dsp_reset                  dspjit_reset
#define dsp_step                   dspjit_step
#define dsp_run                    dspjit_run
#define dsp_bootstrap              dspjit_bootstrap
#define dsp_start_frame            dspjit_start_frame
#define dsp_frame_start_pending    dspjit_frame_start_pending
#define dsp_frame_tick             dspjit_frame_tick
#define dsp_get_registers          dspjit_get_registers
#define dsp_get_pc_sp              dspjit_get_pc_sp
#define dsp_read_memory            dspjit_read_memory
#define dsp_write_memory           dspjit_write_memory
#define dsp_get_halt_requested     dspjit_get_halt_requested
#define dsp_set_halt_requested     dspjit_set_halt_requested
#define dsp_get_cycle_count        dspjit_get_cycle_count
#define dsp_set_cycle_count        dspjit_set_cycle_count
#define dsp_invalidate_opcache     dspjit_invalidate_opcache
#define dsp_jit_get_stats          dspjit_jit_get_stats
#define dsp_jit_dump_block_profile dspjit_jit_dump_block_profile
#define dsp_sync_to_vm             dspjit_sync_to_vm
#define dsp_sync_from_vm           dspjit_sync_from_vm
#define dsp_dma_read               dspjit_dma_read
#define dsp_dma_write              dspjit_dma_write
#define dsp_dma_completion_seen    dspjit_dma_completion_seen
#define g_dsp_gp_halts             g_dspjit_gp_halts
#define g_dsp_ep_halts             g_dspjit_ep_halts
#define g_dsp_dma_p_writes         g_dspjit_dma_p_writes
#define mcpx_apu_ep_snapshot_on_eol_clear dspjit_ep_eol_clear_hook
