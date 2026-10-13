# libsamplerate (vendored)

- Source: https://github.com/libsndfile/libsamplerate, tag `0.2.2`,
  commit `c96f5e3de9c4488f4e6c97f59f5245f22fda22f7` (2021-09-05).
- License: BSD-2-Clause, see `COPYING` (unchanged).
- Files: `src/*.c`, `src/*.h` and `include/samplerate.h` of that tag, copied
  unmodified. No `config.h`; the CMake target in `src/apu/CMakeLists.txt`
  defines what upstream's configure step would (fast sinc converter on,
  x86 has no clip-mode behaviour).
- Why (docs Update 429): xemu's voice processor resamples every APU voice
  from its pitch rate to 48 kHz through libsamplerate's callback API
  (`SRC_SINC_FASTEST`). The port had stubbed it, so voices below 48 kHz
  played too fast (24 kHz 2x, 11 kHz ~4.4x). Added with the user's consent
  (2026-09-29).
