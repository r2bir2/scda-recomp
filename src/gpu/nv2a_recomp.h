/* Public entry points of the xemu NV2A attachment (src/gpu/nv2a_recomp.c). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Start the emulated NV2A: aperture_host is the host address of guest VA
 * 0xFD000000 (16 MiB, committed), vram_host that of guest VA 0xF0000000
 * (the flat view of Xbox physical RAM), vram_size its length. Traps the
 * aperture; call once, before the title touches the GPU. */
bool nv2a_recomp_attach(uint8_t *aperture_host, uint8_t *vram_host,
                        uint64_t vram_size);

uint64_t nv2a_recomp_mmio_read(uint32_t offset, unsigned size);
void nv2a_recomp_mmio_write(uint32_t offset, uint64_t val, unsigned size);

/* F11-only diagnostic hook called by PGRAPH for a malformed transform
 * constant.  It traces the later CPU refill of that physical pushbuffer page. */
void nv2a_recomp_watch_pushbuffer_page(uint32_t physical_address);
