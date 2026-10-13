# Provenance of src/apu/third_party/a52dec

liba52 (ATSC A/52 / AC-3 decoder) from a52dec 0.8.0, as used by xemu's
surround branch (xemu PR #3047, `subprojects/a52dec.wrap`):

- Upstream: https://git.adelielinux.org/community/a52dec.git
- Revision: `c388f3b6d911c246e0b2a7b2c436c3de2e79c74d` ("Release 0.8.0")
- Patch applied: xemu's `subprojects/packagefiles/a52dec/a52dec-bitstream-shift.patch`
  (changes `liba52/bitstream.c` only), exactly as xemu's Meson wrap applies it.
- Copied 2026-10-05 from the local xemu surround checkout
  (`scratchpad/audio_surround_sources_20261005/xemu_surround/subprojects/a52dec`).
- Only the library: `include/a52.h`, `include/attributes.h`, `include/mm_accel.h`,
  `liba52/*.c`, `liba52/*.h`. No player, libao or tests.
- `config.h` is local (MSVC; no GCC attributes, no memalign).

Licence: GPL v2 or later (`COPYING`). Used only by the opt-in surround path
(`src/apu/apu_surround.c`) to decode the EP's AC-3 output to 5.1 PCM.
