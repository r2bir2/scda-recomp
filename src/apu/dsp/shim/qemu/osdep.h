/*
 * Minimal stand-in for QEMU's "qemu/osdep.h", local to xboxrecomp.
 *
 * Provides exactly what the imported xemu DSP files use from it (see
 * src/apu/dsp/PROVENANCE.md): the C standard headers, ARRAY_SIZE, MIN and the
 * two GLib allocation helpers. Nothing else of QEMU is pulled in.
 */
#pragma once

/* QEMU refuses to build with NDEBUG ("building with NDEBUG is not
 * supported"): its code, including the DSP interpreter, treats assert() as
 * a runtime check (e.g. read_memory_p's bound on the P address). Keep that
 * semantics here; with asserts compiled out an out-of-range PC silently
 * fetched host pointers from the neighbouring opcode cache as instructions
 * (docs Update 343). */
#undef NDEBUG
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* POSIX type used by dsp_dma.c; MSVC's CRT does not define it. */
#if defined(_MSC_VER) && !defined(_SSIZE_T_DEFINED)
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef g_new0
#define g_new0(type, n) ((type *)calloc((n), sizeof(type)))
#endif
#ifndef g_free
#define g_free(p) free(p)
#endif
