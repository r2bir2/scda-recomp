/**
 * Xbox Texture Swizzle/Unswizzle
 *
 * Xbox textures use a Z-order curve (Morton code) swizzled memory layout
 * instead of the linear row-major layout that D3D11 expects. This provides
 * better 2D locality for texture cache hits on the NV2A GPU.
 *
 * The swizzle interleaves the bits of the X and Y coordinates:
 *   linear (x=3, y=5) = row_pitch * 5 + bpp * 3
 *   swizzled (x=3, y=5) = morton(3, 5) * bpp
 *
 * where morton(x, y) interleaves x bits into even positions and y bits
 * into odd positions:
 *   x = ...x2 x1 x0   y = ...y2 y1 y0
 *   morton = ...y2 x2 y1 x1 y0 x0
 *
 * Non-square textures are handled by masking: the larger dimension uses
 * all bits, the smaller dimension wraps within a square region.
 *
 * DXT/BC compressed formats are NOT swizzled (they use 4x4 block layout).
 * Only uncompressed formats (A8R8G8B8, R5G6B5, A8, L8, etc.) are swizzled.
 *
 * References:
 *   - xemu: hw/xbox/nv2a/pgraph/swizzle.c
 *   - Xbox Dev Wiki: https://xboxdevwiki.net/NV2A/Swizzling
 */

#ifndef D3D8_SWIZZLE_H
#define D3D8_SWIZZLE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Spread bits of v into even bit positions.
 * E.g., 0b1011 -> 0b01_00_01_01
 * This is one half of the Morton code interleave.
 */
static inline uint32_t swizzle_spread(uint32_t v)
{
    v = (v | (v << 8)) & 0x00FF00FF;
    v = (v | (v << 4)) & 0x0F0F0F0F;
    v = (v | (v << 2)) & 0x33333333;
    v = (v | (v << 1)) & 0x55555555;
    return v;
}

/**
 * Compact even-positioned bits of v back to contiguous form.
 * Inverse of swizzle_spread.
 */
static inline uint32_t swizzle_compact(uint32_t v)
{
    v &= 0x55555555;
    v = (v | (v >> 1)) & 0x33333333;
    v = (v | (v >> 2)) & 0x0F0F0F0F;
    v = (v | (v >> 4)) & 0x00FF00FF;
    v = (v | (v >> 8)) & 0x0000FFFF;
    return v;
}

/**
 * Compute the swizzled (Morton code) offset for coordinates (x, y)
 * within a texture of dimensions (width, height).
 *
 * For non-square textures, the larger dimension's bits are spread
 * and the smaller dimension is masked. This handles the "folding"
 * behavior where the swizzle pattern wraps within power-of-2 tiles.
 */
static inline uint32_t swizzle_offset(uint32_t x, uint32_t y,
                                       uint32_t width, uint32_t height)
{
    /* Build masks for X and Y based on texture dimensions.
     * For a 256x64 texture: x uses bits 0,2,4,6,8,10,12,14 (8 bits for 256)
     *                        y uses bits 1,3,5,7,9,11 (6 bits for 64)
     * But the Y bits only occupy positions where they "fit" within the
     * square tiles. */

    uint32_t x_mask = 0, y_mask = 0;
    uint32_t bit = 1;
    uint32_t w = width, h = height;

    /* Interleave bit allocation: alternate between X and Y,
     * but stop allocating bits for a dimension once it's exhausted. */
    while (w > 1 || h > 1) {
        if (w > 1) {
            x_mask |= bit;
            bit <<= 1;
            w >>= 1;
        }
        if (h > 1) {
            y_mask |= bit;
            bit <<= 1;
            h >>= 1;
        }
    }

    return (swizzle_spread(x) & x_mask) | (swizzle_spread(y) & y_mask);
}

/**
 * Generate interleaved bit masks for X and Y dimensions.
 * Dimensions must be powers of 2.
 * Based on xemu's generate_swizzle_masks algorithm.
 */
static inline void xbox_swizzle_masks(uint32_t width, uint32_t height,
                                       uint32_t *mask_x, uint32_t *mask_y)
{
    uint32_t x = 0, y = 0;
    uint32_t bit = 1, mask_bit = 1;
    while (bit < width || bit < height) {
        if (bit < width)  { x |= mask_bit; mask_bit <<= 1; }
        if (bit < height) { y |= mask_bit; mask_bit <<= 1; }
        bit <<= 1;
    }
    *mask_x = x;
    *mask_y = y;
}

/**
 * Unswizzle a texture from Xbox swizzled (Z-order/Morton) layout
 * to linear row-major layout suitable for D3D11.
 *
 * Uses the masked-increment trick from xemu (Fabian Giesen):
 *   off_x = (off_x - mask_x) & mask_x
 * This increments through only the bits belonging to X's mask,
 * avoiding per-pixel swizzle_offset() recomputation.
 *
 * @param dst       Destination buffer (linear layout).
 * @param src       Source buffer (swizzled layout).
 * @param width     Texture width in pixels (must be power of 2).
 * @param height    Texture height in pixels (must be power of 2).
 * @param bpp       Bytes per pixel (1, 2, or 4).
 */
static inline void xbox_unswizzle_rect(void *dst, const void *src,
                                        uint32_t width, uint32_t height,
                                        uint32_t bpp)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    uint32_t mask_x, mask_y;

    xbox_swizzle_masks(width, height, &mask_x, &mask_y);

    uint32_t off_y = 0;
    for (uint32_t y = 0; y < height; y++) {
        uint32_t off_x = 0;
        uint8_t *dst_row = d + y * width * bpp;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t swiz_off = (off_y + off_x) * bpp;

            switch (bpp) {
            case 1:
                dst_row[x] = s[swiz_off];
                break;
            case 2:
                ((uint16_t *)dst_row)[x] = *(const uint16_t *)(s + swiz_off);
                break;
            case 4:
                ((uint32_t *)dst_row)[x] = *(const uint32_t *)(s + swiz_off);
                break;
            default:
                memcpy(dst_row + x * bpp, s + swiz_off, bpp);
                break;
            }

            off_x = (off_x - mask_x) & mask_x;  /* masked increment */
        }
        off_y = (off_y - mask_y) & mask_y;  /* masked increment */
    }
}

/**
 * Swizzle a texture from linear row-major layout to Xbox swizzled layout.
 * (Inverse of xbox_unswizzle_rect.)
 */
static inline void xbox_swizzle_rect(void *dst, const void *src,
                                      uint32_t width, uint32_t height,
                                      uint32_t bpp)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    uint32_t mask_x, mask_y;

    xbox_swizzle_masks(width, height, &mask_x, &mask_y);

    uint32_t off_y = 0;
    for (uint32_t y = 0; y < height; y++) {
        uint32_t off_x = 0;
        const uint8_t *src_row = s + y * width * bpp;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t swiz_off = (off_y + off_x) * bpp;

            switch (bpp) {
            case 1:
                d[swiz_off] = src_row[x];
                break;
            case 2:
                *(uint16_t *)(d + swiz_off) = ((const uint16_t *)src_row)[x];
                break;
            case 4:
                *(uint32_t *)(d + swiz_off) = ((const uint32_t *)src_row)[x];
                break;
            default:
                memcpy(d + swiz_off, src_row + x * bpp, bpp);
                break;
            }

            off_x = (off_x - mask_x) & mask_x;
        }
        off_y = (off_y - mask_y) & mask_y;
    }
}

/**
 * Compute the Z-order offset for a 3D volume texture coordinate.
 * X, Y and Z consume index bits in a round-robin order until each
 * dimension's extent is exhausted (the natural 3D extension of the
 * 2D Morton layout used for regular textures on NV2A).
 */
static inline uint32_t xbox_swizzle_offset_3d(uint32_t x, uint32_t y, uint32_t z,
                                               uint32_t width, uint32_t height,
                                               uint32_t depth)
{
    uint32_t bit = 1, pos = 0;
    uint32_t w = width, h = height, d = depth;
    while (w > 1 || h > 1 || d > 1) {
        if (w > 1) { if (x & 1) pos |= bit; x >>= 1; bit <<= 1; w >>= 1; }
        if (h > 1) { if (y & 1) pos |= bit; y >>= 1; bit <<= 1; h >>= 1; }
        if (d > 1) { if (z & 1) pos |= bit; z >>= 1; bit <<= 1; d >>= 1; }
    }
    return pos;
}

/**
 * Unswizzle a volume texture (swizzled Z-order layout) into linear,
 * z-slice-major order: dst is (z*slices + linear_2d(y,x)).
 */
static inline void xbox_unswizzle_box(void *dst, const void *src,
                                      uint32_t width, uint32_t height,
                                      uint32_t depth, uint32_t bpp)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    uint32_t x, y, z;
    size_t face_size = (size_t)width * height * bpp;

    for (z = 0; z < depth; z++) {
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                uint32_t swiz = xbox_swizzle_offset_3d(x, y, z, width, height, depth);
                memcpy(d + (size_t)z * face_size + ((size_t)y * width + x) * bpp,
                       s + (size_t)swiz * bpp, bpp);
            }
        }
    }
}

/**
 * Swizzle a volume texture from linear z-slice-major layout to Xbox
 * Z-order layout. (Inverse of xbox_unswizzle_box.)
 */
static inline void xbox_swizzle_box(void *dst, const void *src,
                                    uint32_t width, uint32_t height,
                                    uint32_t depth, uint32_t bpp)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    uint32_t x, y, z;
    size_t face_size = (size_t)width * height * bpp;

    for (z = 0; z < depth; z++) {
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                uint32_t swiz = xbox_swizzle_offset_3d(x, y, z, width, height, depth);
                memcpy(d + (size_t)swiz * bpp,
                       s + (size_t)z * face_size + ((size_t)y * width + x) * bpp, bpp);
            }
        }
    }
}

/**
 * Check if an Xbox D3D8 format is swizzled (vs linear/compressed).
 * Xbox formats with "LIN_" prefix are linear; DXT formats are block-compressed.
 * All other uncompressed formats are swizzled by default.
 */
static inline int d3d8_format_is_swizzled(uint32_t fmt)
{
    /* Linear formats (0x10-0x41 range excludes the non-LIN 0x19-0x3C range) */
    switch (fmt) {
    case 0x10: case 0x11: case 0x12: case 0x13:   /* LIN_A1R5G5B5..LIN_L8 */
    case 0x14: case 0x15:                          /* LIN_X8L8V8U8, LIN_V8U8 */
    case 0x16: case 0x17: case 0x18:               /* LIN_R8B8, LIN_G8B8, LIN_L6V5U5 */
    case 0x1B: case 0x1C: case 0x1D: case 0x1E:   /* LIN_AL8..LIN_X8R8G8B8 */
    case 0x1F: case 0x20:                          /* LIN_A8, LIN_A8L8 */
    case 0x2E: case 0x2F: case 0x30: case 0x31:   /* LIN_D24S8..LIN_F16 */
    case 0x35: case 0x36: case 0x37:              /* LIN_L16, LIN_V16U16, LIN_R6G5B5 */
    case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41:  /* LIN_R5G5B5A1..LIN_R8G8B8A8 */
    case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60:  /* LIN_R16F..LIN_A32B32G32R32F */
    case 0x61: case 0x62: case 0x63:              /* LIN_G16R16, LIN_A16L16, LIN_A16B16G16R16 */
    case 0x79: case 0x7A:                          /* LIN_A32B32G32R32, LIN_G32R32 */
    case 0x67: case 0x68:                          /* LIN_L32, LIN_A32L32 */
    case 0x69: case 0x6A: case 0x6B:              /* LIN_V32U32..LIN_Q32W32V32U32 */
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71:  /* LIN_A2R10G10B10..LIN_R11G11B10 */
    case 0x72: case 0x73: case 0x74:              /* LIN_D24X8..LIN_D32 */
    case 0x75: case 0x76: case 0x77: case 0x78:   /* LIN_DXN..LIN_CTX1 */
        return 0;
    default:
        break;
    }

    /* Compressed formats (block layout, not swizzled) */
    if (fmt == 0x0C || /* DXT1 */
        fmt == 0x0E || /* DXT3 (DXT2/3) */
        fmt == 0x0F || /* DXT5 (DXT4/5) */
        fmt == 0x57 || /* DXN */
        fmt == 0x59 || /* DXT3A */
        fmt == 0x5A || /* DXT5A */
        fmt == 0x58)   /* CTX1 */
        return 0;

    /* Index/depth formats (not swizzled) */
    if (fmt == 101 || fmt == 102 ||  /* INDEX16/32 */
        fmt == 0x2A || fmt == 0x2B || fmt == 0x2C || fmt == 0x2D ||  /* D24S8/F24S8/D16/F16 */
        fmt == 0x54 || fmt == 0x55 || fmt == 0x56)  /* D24X8/D24FS8/D32 */
        return 0;

    /* YUV is linear (packed in raster order), not swizzled */
    if (fmt == 0x24 || fmt == 0x25)  /* YUY2/UYVY */
        return 0;

    /* All other formats are swizzled */
    return 1;
}

#endif /* D3D8_SWIZZLE_H */
