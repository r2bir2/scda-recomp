/* Local stand-in for QEMU's VGA state (see src/gpu/PROVENANCE.md).
 *
 * The NV2A's legacy VGA aliases (PRMCIO/PRMVIO) only need to hold what the
 * title writes and hand it back; nothing here scans out VGA memory. The
 * display geometry the GL display path asks for comes from get_params /
 * get_resolution, filled in by nv2a_recomp.c. */
#pragma once

#include <stdint.h>
#include "hw/hw.h"
#include "ui/console.h"
#include "hw/display/vga_regs.h"

typedef struct VGADisplayParams {
    uint32_t line_offset;
    uint32_t start_addr;
    uint32_t line_compare;
} VGADisplayParams;

typedef struct VGACommonState VGACommonState;
struct VGACommonState {
    MemoryRegion vram;
    uint8_t *vram_ptr;
    uint32_t vram_size_mb;
    uint8_t sr_index, sr[256];
    uint8_t gr_index, gr[256];
    uint8_t ar_index, ar[21];
    int ar_flip_flop;
    uint8_t cr_index, cr[256];
    uint8_t msr, fcr, st00, st01;
    int (*get_bpp)(VGACommonState *s);
    void (*get_params)(VGACommonState *s, VGADisplayParams *params);
    void (*get_resolution)(VGACommonState *s, int *pwidth, int *pheight);
    const GraphicHwOps *hw_ops;
    void *con;
};

void vga_invalidate_scanlines(VGACommonState *s, int y1, int y2);
uint32_t vga_ioport_read(void *opaque, uint32_t addr);
void vga_ioport_write(void *opaque, uint32_t addr, uint32_t val);
