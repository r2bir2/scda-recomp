# Provenance of src/apu/dsp_jit

GP/EP DSP for the opt-in surround path (`RECOMP_AUDIO_SURROUND=1`,
`src/apu/apu_surround.c`, docs Update 611).

## xemu surround branch files

- Upstream: https://github.com/mborgerson/xemu/tree/feat/surround
  (xemu PR https://github.com/xemu-project/xemu/pull/3047, draft as of 2026-10-05)
- Commit: `5b1c1404c55a8748143ab37e4141b6779f656703`
- Path: `hw/xbox/mcpx/apu/dsp/`
- Copied 2026-10-05 from the local checkout
  `scratchpad/audio_surround_sources_20261005/xemu_surround`.

| File | Licence | Local change |
|---|---|---|
| `dsp.c`, `dsp.h`, `debug.h`, `mem.h`, `ep_yrom.h` | GPL v2 or later | none |
| `dma.c`, `dma.h`, `dma_regs.h` | LGPL v2 or later | `dma.c`: GNU case range `0x0 ... 0x3` written out (MSVC) |

All exported names are renamed to `dspjit_*` by `dspjit_rename.h`, which the
build force-includes (`/FI`) for this library only, so these files can live
next to the older interpreter copies in `src/apu/dsp/`. Consumers include
`dspjit_api.h`. `shim/` stands in for the few QEMU headers they use.

## dsp56300 (the DSP56300 core)

- Upstream: https://github.com/mborgerson/dsp56300, tag `v1.0.0`
  (`aab3649fc452daf03da8b5e36fdc0c308d874101`), MIT (`LICENSE.dsp56300`).
- `dsp56300.h`: the C API header from the release archive
  `dsp56300-1.0.0-x86_64-pc-windows-gnu.tar.gz` (the asset xemu's
  `subprojects/dsp56300/meson.build` downloads), unmodified.
- The archive's static library is Rust/windows-gnu and cannot be linked by
  MSVC. `build_dsp56300_dll.ps1` wraps it into `dsp56300.dll` with MinGW GCC
  (exports in `dsp56300.def`); `dsp56300_dll.c` loads that DLL at runtime from
  the executable's directory. Without the DLL the surround path stays off.

Local files: `dspjit_rename.h`, `dspjit_api.h`, `dsp56300_dll.c`,
`dsp56300.def`, `build_dsp56300_dll.ps1`, `shim/*`, this file.
