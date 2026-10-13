/*
 * Minimal stand-in for QEMU's "qemu/osdep.h" for src/apu/dsp_jit (local to
 * xboxrecomp): the C headers and the few GLib/QEMU helpers the imported
 * xemu surround-branch dsp.c/dma.c use. Keeps assert() active, as QEMU does.
 */
#pragma once

#undef NDEBUG
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define g_new(type, n)  ((type *)malloc((size_t)(n) * sizeof(type)))
#define g_new0(type, n) ((type *)calloc((n), sizeof(type)))
#define g_realloc(p, n) realloc((p), (n))
#define g_free(p)       free(p)
