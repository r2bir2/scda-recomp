/*
 * Opt-in surround audio path (docs Update 611): GP and EP run on the
 * DSP56300 JIT core (dsp56300.dll) with xemu's surround-branch DMA/FIFO/
 * frame model; the EP's AC-3 output (EP FIFO #1, IEC 61937) is decoded to
 * 5.1 PCM. Enabled by RECOMP_AUDIO_SURROUND=1; without it, or without the
 * DLL, nothing here runs and the stereo interpreter path stays as before.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct MCPXAPUState MCPXAPUState;

/* Called from mcpx_apu_dsp_init. Returns 1 when the surround path is on. */
int apu_surround_init(MCPXAPUState *d);
int apu_surround_active(void);
/* 5.1 PCM output (not passthrough, not the stereo variant). */
int apu_surround_six_channels(void);

uint64_t apu_surround_gp_read(MCPXAPUState *d, uint64_t addr);
void apu_surround_gp_write(MCPXAPUState *d, uint64_t addr, uint64_t val);
uint64_t apu_surround_ep_read(MCPXAPUState *d, uint64_t addr);
void apu_surround_ep_write(MCPXAPUState *d, uint64_t addr, uint64_t val);

/* One SE frame, as xemu's se_frame: begin (EP runs from here), the VP fills
 * the mixbins, gp (hands them to the GP; NULL when no voice mixed), end
 * (joins both cores). */
void apu_surround_frame_begin(MCPXAPUState *d);
void apu_surround_frame_gp(MCPXAPUState *d, float (*mixbins)[32]);
void apu_surround_frame_end(MCPXAPUState *d);

/* One 256-sample output block (every 8th frame): the decoded 5.1 stream
 * while the title encodes AC-3, else the EP's stereo output (`stereo`,
 * the monitor frame buffer) on FL/FR. Order FL FR FC LFE BL BR. */
void apu_surround_output_block(const int16_t stereo[256][2],
                               int16_t out[256][6]);

/* RECOMP_AUDIO_SURROUND=passthrough with an output that took the Dolby
 * Digital stream (docs Update 612): no PCM output; the output pull. */
int apu_surround_passthrough(void);
void apu_surround_passthrough_block(void);

/* For the kernel's XC_AUDIO answer: Surround + AC-3 while this path runs. */
int mcpx_apu_surround_active(void);
