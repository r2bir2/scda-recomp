/*
 * Stand-in for xemu's "ui/xemu-settings.h", local to xboxrecomp.
 *
 * The single definition is in src/apu/apu_dsp.c. The DSP code reads:
 *   use_dsp      run GP/EP until the program halts each frame ("realtime");
 *                off by default, as in xemu, which then runs 1000 cycles per
 *                APU frame
 *   use_dsp_jit  the Rust JIT backend is not part of this port; must stay
 *                false (dsp_jit_stub.c aborts if it is ever selected)
 */
#pragma once

#include <stdbool.h>

typedef struct XemuSettingsAudio {
    /* read by the VP (src/apu/apu_vp.c) */
    float volume_limit;
    bool hrtf;
    struct {
        int num_workers;
    } vp;
    /* read by the DSP (src/apu/dsp, src/apu/apu_dsp.c) */
    bool use_dsp;
    bool use_dsp_jit;
} XemuSettingsAudio;

typedef struct XemuSettings {
    XemuSettingsAudio audio;
} XemuSettings;

/* Its own name: the GPU (src/gpu) has xemu's full `struct config g_config`,
 * and two different objects under one symbol overlapped -- the small APU
 * definition won and GPU settings were written past its end (found linking
 * the ARM port with -fcommon, docs Update 677). */
#define g_config g_apu_config
extern XemuSettings g_config;
