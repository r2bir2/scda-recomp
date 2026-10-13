# Provenance of src/apu/dsp

MCPX APU DSP (GP/EP, DSP56300) emulation taken from xemu.

- Upstream: https://github.com/xemu-project/xemu
- Commit:   `f9b14039e5bb56ae2d8f028e31e7cc19f13f7e12` (2026-09-14)
- Path:     `hw/xbox/mcpx/apu/dsp/` (and `interp/`)
- Fetched:  2026-09-26, raw files from raw.githubusercontent.com at that
  commit. Only the C interpreter backend is used; the Rust/FFI JIT backend
  (`dsp_jit.c`, `dsp56300.h`) is NOT included.
- `LICENSE.xemu` is the top-level QEMU/xemu LICENSE file from that commit.

## Licences (per file header)

| File | Licence | Status |
|---|---|---|
| `dsp.c`, `dsp.h`, `dsp_c.c`, `dsp_internal.h`, `debug.h` | GPL v2 or later | copied unmodified unless listed below |
| `dsp_dma.c`, `dsp_dma.h`, `dsp_dma_regs.h` | LGPL v2 or later | copied unmodified unless listed below |
| `interp/dsp_cpu.c`, `interp/dsp_cpu.h`, `interp/dsp_cpu_regs.h`, `interp/dsp_dis.c.inc`, `interp/dsp_emu.c.inc`, `interp/debug.c` | GPL v2 or later | copied unmodified unless listed below |
| `upstream_ref/gp_ep.c`, `upstream_ref/gp_ep.h` | LGPL v2 or later | unmodified reference copy; the adapted version lives in `src/apu/apu_dsp.c` |

The files under `shim/`, `dsp_jit_stub.c` and `dsp_diag.c` are local code
written for this project (they replace QEMU/xemu infrastructure headers; the
two `.c` files are a JIT stub that aborts if ever selected, and an execution
history for diagnostics). The empty directories `shim/hw/pci` and
`shim/hw/xbox/mcpx/apu` are unused leftovers from the initial setup.

**Licence note:** GPL v2+ code is now part of the source tree. The licence
compatibility of the combined project must be clarified separately before
anything built from it is published or distributed. Do not remove any of the
licence headers above.

## Local modifications to upstream files

(keep this list complete)

- **No upstream file under `src/apu/dsp/` is modified.** All adaptation lives
  outside them:
- `src/apu/apu_dsp.c` -- adapted from `gp_ep.c` (LGPL v2+ header kept):
  GNU case ranges -> explicit range checks (MSVC); scatter-gather entries
  resolved as guest addresses via `apu_phys_ptr`/`apu_phys_valid` instead of
  64 MB physical offsets (this runtime's `MmGetPhysicalAddress` returns the
  guest VA; contiguous memory lives at 0x80000000/0x84000000); out-of-range
  transfers are reported and skipped instead of hitting a MemoryRegion
  assert; `g_config` defined here (JIT always off, `use_dsp` from
  `RECOMP_APU_USE_DSP`); GP/EP MMIO exported as functions for
  `apu_core.c`'s dispatcher; opt-in diagnostics (`RECOMP_DSP_TRACE`,
  `RECOMP_DSP_DISASM`: register/boot logs, PC/disassembly history, dumps on
  abort).
- `shim/qemu/osdep.h` keeps `assert()` active (`#undef NDEBUG`), as QEMU
  refuses to build with NDEBUG and the interpreter relies on asserts as
  runtime checks.
- `shim/trace.h` routes `trace_dsp56k_execute_instruction(_disasm)` into
  `dsp_diag.c`; the disassembly hook is only enabled by `RECOMP_DSP_DISASM`.
- `src/apu/apu_shim.h` (existing file, not from this directory): APU
  "physical" accessors resolve guest addresses (`apu_phys_ptr`), no 64 MB
  mask; same for the ADPCM fetch in `src/apu/apu_vp.c`.
