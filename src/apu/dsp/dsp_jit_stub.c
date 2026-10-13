/*
 * Local to xboxrecomp (not from xemu).
 *
 * xemu's dsp.c can switch to a Rust/FFI JIT backend (dsp_jit.c). That backend
 * is not part of this port. dsp.c still names jit_dsp_ops (pointer compare in
 * dsp_set_engine) and dsp_jit_init (only reached when
 * g_config.audio.use_dsp_jit is true). The ops table therefore only needs an
 * identity; selecting the JIT is a configuration error, not a fallback.
 */
#include "qemu/osdep.h"
#include "dsp_internal.h"

const DSPOps jit_dsp_ops = { 0 };

void dsp_jit_init(DSPState *dsp)
{
    (void)dsp;
    fprintf(stderr, "[APU] FATAL: DSP JIT backend requested but not built "
                    "(g_config.audio.use_dsp_jit must be false)\n");
    abort();
}
