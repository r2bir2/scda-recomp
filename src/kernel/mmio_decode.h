/* Emulate the host instruction that faulted on a trapped device aperture.
 *
 * Device registers (NV2A, MCPX ACI) are kept PAGE_NOACCESS; a vectored
 * exception handler hands the faulting context here together with the
 * register offset. The generated code touches registers only through plain
 * loads, stores and a few read-modify-write forms (mov/movzx/movsx, test,
 * ALU with immediate or register), which this decodes, performs through the
 * device callbacks and steps over. 8-byte accesses are split into two 4-byte
 * ones, as the 32-bit bus does. */
#pragma once

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

/* Faulting host pc of a CONTEXT (xbox_winnt.h defines it on hosts where the
 * runtime's own NT vocabulary is used; with the Windows SDK it is Rip). */
#ifndef XBOX_CONTEXT_PC
#if defined(__aarch64__) || defined(_M_ARM64)
#define XBOX_CONTEXT_PC(c) ((c)->Pc)
#else
#define XBOX_CONTEXT_PC(c) ((c)->Rip)
#endif
#endif

typedef uint64_t (*mmio_read_fn)(void *opaque, uint32_t off, unsigned size);
typedef void (*mmio_write_fn)(void *opaque, uint32_t off, uint64_t val,
                              unsigned size);

/* Returns false (context untouched) for an instruction it does not know. */
bool mmio_decode_and_run(PCONTEXT ctx, uint32_t off, mmio_read_fn rd,
                         mmio_write_fn wr, void *opaque);
