# Provenance of hle/tgp

Source: SCDA-XBERecomp by TGP482 and Joshhhuaaa (https://github.com/TGP482/SCDA-XBERecomp),
MIT license (see `LICENSE` in this folder), local checkout `C:\VScode\SCDA-XBERecomp_TGP482`,
commit `fdc8e1fb722a8c3ed8f8b497740696500af97764` (2026-10-13). Copied 2026-10-26.

| File | Source path | Local changes |
|---|---|---|
| `gpu/state.h` | `src/gpu/state.h` | none |
| `gpu/backend.h` | `src/gpu/backend.h` | none |
| `gpu/texture.h`, `gpu/texture.cpp` | `src/gpu/` | none |
| `gpu/vertex.h`, `gpu/vertex.cpp` | `src/gpu/` | none (non-Windows hosts get `hle/compat_posix/dxgiformat.h`) |
| `gpu/shaders.h`, `gpu/shaders.cpp` | `src/gpu/` | `Hash()`: `_umul128` instead of `unsigned __int128` under MSVC (same result) |
| `xdk/d3d8.h`, `xdk/d3d8.cpp` | `src/xdk/` | `#include <thread>`; `Lock2DSurface` as `HLE_BEFORE` instead of an `X86_FUNC` override calling `GUEST_ORIG`; `D3DDevice_SetLight`/`D3DDevice_LightEnable` overrides inside `#if 0` (not in this image) |

Their runtime calls (`kernel::`, `mem::`, `app::`, `LOG*`, `HLE_*`, `SYM()`) resolve to
`hle/compat` (this runtime's glue, see `hle/hle_glue.cpp`). Symbol addresses come from their
`config/scda_v2.toml` and `config/xdk_symbols.toml` via `tools/hle/d3d_symbols.py`
(`hle/compat/image_symbols.h`, `projects/scda/hle_d3d_functions.json`).

Plan and state of the HLE renderer: `agent_handoff/HLE_RENDERER_PLAN.md`.
