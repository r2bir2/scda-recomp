/*
 * Stand-in for the QEMU-generated "trace.h" of xemu's hw/xbox/mcpx/apu/dsp
 * (src/apu/dsp_jit). The trace events are disabled unless a QEMU trace
 * backend enables them; here they compile away.
 */
#pragma once
#define trace_dsp_read_peripheral(address, value) ((void)0)
#define trace_dsp_write_peripheral(address, value) ((void)0)
#define trace_dsp_dma_node(core, addr, next, control, count, dsp_offset, \
                           scratch_offset, scratch_base, scratch_size) ((void)0)
