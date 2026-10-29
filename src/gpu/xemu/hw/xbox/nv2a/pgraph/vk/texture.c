/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
 *
 * Based on GL implementation:
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/xbox/nv2a/pgraph/s3tc.h"
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/fast-hash.h"
#include "qemu/lru.h"
#include "renderer.h"

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode);

static const VkImageType dimensionality_to_vk_image_type[] = {
    0,
    VK_IMAGE_TYPE_1D,
    VK_IMAGE_TYPE_2D,
    VK_IMAGE_TYPE_3D,
};
static const VkImageViewType dimensionality_to_vk_image_view_type[] = {
    0,
    VK_IMAGE_VIEW_TYPE_1D,
    VK_IMAGE_VIEW_TYPE_2D,
    VK_IMAGE_VIEW_TYPE_3D,
};

static VkSamplerAddressMode lookup_texture_address_mode(int idx)
{
    assert(0 < idx && idx < ARRAY_SIZE(pgraph_texture_addr_vk_map));
    return pgraph_texture_addr_vk_map[idx];
}

// FIXME: Move to common
// FIXME: We can shrink the size of this structure
// FIXME: Use simple allocator
typedef struct TextureLevel {
    unsigned int width, height, depth;
    hwaddr vram_addr;
    void *decoded_data;
    size_t decoded_size;
} TextureLevel;

typedef struct TextureLayer {
    TextureLevel levels[16];
} TextureLayer;

typedef struct TextureLayout {
    TextureLayer layers[6];
} TextureLayout;

// FIXME: Move to common
static enum S3TC_DECOMPRESS_FORMAT kelvin_format_to_s3tc_format(int color_format)
{
    switch (color_format) {
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5:
        return S3TC_DECOMPRESS_FORMAT_DXT1;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT23_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT3;
    case NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT45_A8R8G8B8:
        return S3TC_DECOMPRESS_FORMAT_DXT5;
    default:
        assert(false);
    }
}

// FIXME: Move to common
static void memcpy_image(void *dst, void *src, int min_stride, int dst_stride, int src_stride, int height)
{
    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t *src_ptr = (uint8_t *)src;

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, min_stride);
        src_ptr += src_stride;
        dst_ptr += dst_stride;
    }
}

// FIXME: Move to common
static size_t get_cubemap_layer_size(PGRAPHState *pg, TextureShape s)
{
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];
    bool is_compressed =
        pgraph_is_texture_format_compressed(pg, s.color_format);
    unsigned int block_size;

    unsigned int w = s.width, h = s.height;
    size_t length = 0;

    if (!f.linear && s.border) {
        w = MAX(16, w * 2);
        h = MAX(16, h * 2);
    }

    if (is_compressed) {
        block_size =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5 ?
                8 :
                16;
    }

    for (int level = 0; level < s.levels; level++) {
        if (is_compressed) {
            length += w / 4 * h / 4 * block_size;
        } else {
            length += w * h * f.bytes_per_pixel;
        }

        w /= 2;
        h /= 2;
    }

    return ROUND_UP(length, NV2A_CUBEMAP_FACE_ALIGNMENT);
}

// FIXME: Move to common
// FIXME: More refactoring
// FIXME: Possible parallelization of decoding
// FIXME: Bounds checking
static TextureLayout *get_texture_layout(PGRAPHState *pg, int texture_idx)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    TextureShape s = pgraph_get_texture_shape(pg, texture_idx);
    BasicColorFormatInfo f = kelvin_color_format_info_map[s.color_format];

    NV2A_VK_DGROUP_BEGIN("Texture %d: cubemap=%d, dimensionality=%d, color_format=0x%x, levels=%d, width=%d, height=%d, depth=%d border=%d, min_mipmap_level=%d, max_mipmap_level=%d, pitch=%d",
        texture_idx,
        s.cubemap,
        s.dimensionality,
        s.color_format,
        s.levels,
        s.width,
        s.height,
        s.depth,
        s.border,
        s.min_mipmap_level,
        s.max_mipmap_level,
        s.pitch
        );

    // Sanity checks on below assumptions
    if (f.linear) {
        assert(s.dimensionality == 2);
    }
    if (s.cubemap) {
        assert(s.dimensionality == 2);
        assert(!f.linear);
    }
    assert(s.dimensionality > 1);

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    /* xboxrecomp: uint8_t * instead of void * -- MSVC has no GNU void *
     * arithmetic (the += steps below). */
    uint8_t *texture_data_ptr = (uint8_t *)d->vram_ptr + texture_vram_offset;

    size_t texture_palette_data_size;
    const hwaddr texture_palette_vram_offset =
        pgraph_get_texture_palette_phys_addr_length(pg, texture_idx,
                                                    &texture_palette_data_size);
    void *palette_data_ptr = (char *)d->vram_ptr + texture_palette_vram_offset;

    unsigned int adjusted_width = s.width, adjusted_height = s.height,
                 adjusted_pitch = s.pitch, adjusted_depth = s.depth;

    if (!f.linear && s.border) {
        adjusted_width = MAX(16, adjusted_width * 2);
        adjusted_height = MAX(16, adjusted_height * 2);
        adjusted_pitch = adjusted_width * (s.pitch / s.width);
        adjusted_depth = MAX(16, s.depth * 2);
    }

    TextureLayout *layout = g_malloc0(sizeof(TextureLayout));

    if (f.linear) {
        assert(s.pitch % f.bytes_per_pixel == 0 && "Can't handle strides unaligned to pixels");

        size_t converted_size;
        uint8_t *converted = pgraph_convert_texture_data(
            s, texture_data_ptr, palette_data_ptr, adjusted_width,
            adjusted_height, 1, adjusted_pitch, 0, &converted_size);

        if (!converted) {
            int dst_stride = adjusted_width * f.bytes_per_pixel;
            assert(adjusted_width <= s.width);
            converted_size = dst_stride * adjusted_height;
            converted = g_malloc(converted_size);
            memcpy_image(converted, texture_data_ptr, adjusted_width * f.bytes_per_pixel, dst_stride,
                         adjusted_pitch, adjusted_height);
        }

        assert(s.levels == 1);
        layout->layers[0].levels[0] = (TextureLevel){
            .width = adjusted_width,
            .height = adjusted_height,
            .depth = 1,
            .decoded_size = converted_size,
            .decoded_data = converted,
        };

        NV2A_VK_DGROUP_END();
        return layout;
    }

    bool is_compressed = pgraph_is_texture_format_compressed(pg, s.color_format);
    size_t block_size = 0;
    if (is_compressed) {
        bool is_dxt1 =
            s.color_format == NV097_SET_TEXTURE_FORMAT_COLOR_L_DXT1_A1R5G5B5;
        block_size = is_dxt1 ? 8 : 16;
    }

    if (s.dimensionality == 2) {
        hwaddr layer_size = s.cubemap ? get_cubemap_layer_size(pg, s) : 0;
        const int num_layers = s.cubemap ? 6 : 1;
        for (int layer = 0; layer < num_layers; layer++) {
            unsigned int width = adjusted_width, height = adjusted_height;
            texture_data_ptr = (char *)d->vram_ptr + texture_vram_offset +
                               layer * layer_size;

            for (int level = 0; level < s.levels; level++) {
                NV2A_VK_DPRINTF("Layer %d Level %d @ %x", layer, level, (int)((char*)texture_data_ptr - (char*)d->vram_ptr));

                width = MAX(width, 1);
                height = MAX(height, 1);
                if (is_compressed) {
                    // https://docs.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression#virtual-size-versus-physical-size
                    unsigned int tex_width = width, tex_height = height;
                    unsigned int physical_width = (width + 3) & ~3,
                                 physical_height = (height + 3) & ~3;

                    size_t converted_size = width * height * 4;
                    uint8_t *converted = s3tc_decompress_2d(
                        kelvin_format_to_s3tc_format(s.color_format),
                        texture_data_ptr, width, height);
                    assert(converted);

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.

                        // glPixelStorei(GL_UNPACK_SKIP_PIXELS, 4);
                        // glPixelStorei(GL_UNPACK_SKIP_ROWS, 4);
                        tex_width = s.width;
                        tex_height = s.height;
                        // if (physical_width == width) {
                        //     glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        // }

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr +=
                        physical_width / 4 * physical_height / 4 * block_size;
                } else {
                    unsigned int pitch = width * f.bytes_per_pixel;
                    unsigned int tex_width = width, tex_height = height;

                    size_t converted_size = height * pitch;
                    uint8_t *unswizzled = (uint8_t*)g_malloc(height * pitch);
                    unswizzle_rect(texture_data_ptr, width, height,
                                   unswizzled, pitch, f.bytes_per_pixel);

                    uint8_t *converted = pgraph_convert_texture_data(
                        s, unswizzled, palette_data_ptr, width, height, 1,
                        pitch, 0, &converted_size);

                    if (converted) {
                        g_free(unswizzled);
                    } else {
                        converted = unswizzled;
                    }

                    if (s.cubemap && adjusted_width != s.width) {
                        // FIXME: Consider preserving the border.
                        // There does not seem to be a way to reference the border
                        // texels in a cubemap, so they are discarded.
                        // glPixelStorei(GL_UNPACK_ROW_LENGTH, adjusted_width);
                        tex_width = s.width;
                        tex_height = s.height;
                        // pixel_data += 4 * f.bytes_per_pixel + 4 * pitch;

                        // FIXME: Crop by 4 pixels on each side
                    }

                    layout->layers[layer].levels[level] = (TextureLevel){
                        .width = tex_width,
                        .height = tex_height,
                        .depth = 1,
                        .decoded_size = converted_size,
                        .decoded_data = converted,
                    };

                    texture_data_ptr += width * height * f.bytes_per_pixel;
                }

                width /= 2;
                height /= 2;
            }
        }
    } else if (s.dimensionality == 3) {
        assert(!f.linear);
        unsigned int width = adjusted_width, height = adjusted_height,
                     depth = adjusted_depth;

        for (int level = 0; level < s.levels; level++) {
            if (is_compressed) {
                width = MAX(width, 1);
                height = MAX(height, 1);
                unsigned int physical_width = (width + 3) & ~3,
                             physical_height = (height + 3) & ~3;
                depth = MAX(depth, 1);

                size_t converted_size = width * height * depth * 4;
                uint8_t *converted = s3tc_decompress_3d(
                    kelvin_format_to_s3tc_format(s.color_format),
                    texture_data_ptr, width, height, depth);
                assert(converted);

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += physical_width / 4 * physical_height / 4 * depth * block_size;
            } else {
                width = MAX(width, 1);
                height = MAX(height, 1);
                depth = MAX(depth, 1);

                unsigned int row_pitch = width * f.bytes_per_pixel;
                unsigned int slice_pitch = row_pitch * height;

                size_t unswizzled_size = slice_pitch * depth;
                uint8_t *unswizzled = g_malloc(unswizzled_size);
                unswizzle_box(texture_data_ptr, width, height, depth,
                              unswizzled, row_pitch, slice_pitch,
                              f.bytes_per_pixel);

                size_t converted_size;
                uint8_t *converted = pgraph_convert_texture_data(
                    s, unswizzled, palette_data_ptr, width, height, depth,
                    row_pitch, slice_pitch, &converted_size);

                if (converted) {
                    g_free(unswizzled);
                } else {
                    converted = unswizzled;
                    converted_size = unswizzled_size;
                }

                layout->layers[0].levels[level] = (TextureLevel){
                    .width = width,
                    .height = height,
                    .depth = depth,
                    .decoded_size = converted_size,
                    .decoded_data = converted,
                };

                texture_data_ptr += width * height * depth * f.bytes_per_pixel;
            }

            width /= 2;
            height /= 2;
            depth /= 2;
        }
    }

    NV2A_VK_DGROUP_END();
    return layout;
}

struct pgraph_texture_possibly_dirty_struct {
    hwaddr addr, end;
};

static void mark_textures_possibly_dirty_visitor(Lru *lru, LruNode *node, void *opaque)
{
    struct pgraph_texture_possibly_dirty_struct *test = opaque;

    TextureBinding *tnode = container_of(node, TextureBinding, node);
    if (tnode->possibly_dirty) {
        return;
    }

    uintptr_t k_tex_addr = tnode->key.texture_vram_offset;
    uintptr_t k_tex_end = k_tex_addr + tnode->key.texture_length - 1;
    bool overlapping = !(test->addr > k_tex_end || k_tex_addr > test->end);

    if (tnode->key.palette_length > 0) {
        uintptr_t k_pal_addr = tnode->key.palette_vram_offset;
        uintptr_t k_pal_end = k_pal_addr + tnode->key.palette_length - 1;
        overlapping |= !(test->addr > k_pal_end || k_pal_addr > test->end);
    }

    tnode->possibly_dirty |= overlapping;
}

void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d,
    hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size) - 1;
    addr &= TARGET_PAGE_MASK;
    assert(end <= memory_region_size(d->vram));

    struct pgraph_texture_possibly_dirty_struct test = {
        .addr = addr,
        .end = end,
    };

    lru_visit_active(&d->pgraph.vk_renderer_state->texture_cache,
                     mark_textures_possibly_dirty_visitor,
                     &test);
}

static bool check_texture_dirty(NV2AState *d, hwaddr addr, hwaddr size)
{
    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    assert(end < memory_region_size(d->vram));
    return memory_region_test_and_clear_dirty(d->vram, addr, end - addr,
                                              DIRTY_MEMORY_NV2A_TEX);
}

// Check if any of the pages spanned by the a texture are dirty.
static bool check_texture_possibly_dirty(NV2AState *d,
                                         hwaddr texture_vram_offset,
                                         unsigned int length,
                                         hwaddr palette_vram_offset,
                                         unsigned int palette_length)
{
    /* xboxrecomp (docs Update 487): there is no page dirty log here;
     * check_texture_dirty answers "first look at this exact range in this
     * frame" (nv2a_recomp_region_dirty), which every other texture over the
     * same memory also gets on its own first look. Marking the overlapping
     * textures -- a walk over the whole texture cache per texture per frame,
     * ~17 % of the PFIFO thread -- therefore adds nothing and is skipped. */
    bool possibly_dirty = false;
    if (check_texture_dirty(d, texture_vram_offset, length)) {
        possibly_dirty = true;
    }
    if (palette_length && check_texture_dirty(d, palette_vram_offset,
                                                     palette_length)) {
        possibly_dirty = true;
    }
    return possibly_dirty;
}

/* xboxrecomp (docs Update 652): the dirty answer above is per (address,
 * length) and consumed by the first query of a frame -- but two cached
 * textures can cover exactly the same range with different keys (SCDA
 * samples its 640x480 frame copy 04DD9000 with two filters). The first got
 * re-hashed, the second kept an image from seconds earlier: the
 * tone-mapped frame showed an old camera view while the bloom of the
 * current one was added -- the "ghost" copy of every bright light. Each
 * range now carries a generation, bumped whenever the region reports a
 * change; a binding that verified an older generation re-checks its hash.
 * RECOMP_VK_TEXTURE_RANGE_GEN=0 restores the old behaviour. */
#define RANGE_GEN_SLOTS 2048
static struct { hwaddr addr, len; uint32_t gen; bool used; }
    s_range_gen[RANGE_GEN_SLOTS];

static uint32_t *texture_range_gen(hwaddr addr, hwaddr len)
{
    unsigned h = (unsigned)((addr >> 6) ^ (addr >> 17) ^ len) & (RANGE_GEN_SLOTS - 1);
    for (unsigned probe = 0; probe < 16; probe++) {
        unsigned i = (h + probe) & (RANGE_GEN_SLOTS - 1);
        if (!s_range_gen[i].used || (s_range_gen[i].addr == addr &&
                                     s_range_gen[i].len == len)) {
            s_range_gen[i].used = true;
            s_range_gen[i].addr = addr;
            s_range_gen[i].len = len;
            return &s_range_gen[i].gen;
        }
    }
    /* table full around here: reuse the home slot, bumping its generation
     * so every binding that shared it re-verifies (conservative) */
    s_range_gen[h].addr = addr;
    s_range_gen[h].len = len;
    s_range_gen[h].gen++;
    return &s_range_gen[h].gen;
}

static bool range_gen_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_VK_TEXTURE_RANGE_GEN");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* check_texture_possibly_dirty for one binding: also true when another
 * texture over the same range already consumed this frame's marker and
 * this binding has not been verified since. */
static bool binding_possibly_dirty(NV2AState *d, TextureBinding *b,
                                   hwaddr texture_vram_offset,
                                   unsigned int length,
                                   hwaddr palette_vram_offset,
                                   unsigned int palette_length)
{
    bool fresh = check_texture_possibly_dirty(d, texture_vram_offset, length,
                                              palette_vram_offset,
                                              palette_length);
    if (!range_gen_enabled())
        return fresh;
    uint32_t *gen = texture_range_gen(texture_vram_offset, length);
    if (fresh)
        (*gen)++;
    return fresh || (b && b->verified_gen != *gen);
}

static void binding_mark_verified(TextureBinding *b)
{
    if (range_gen_enabled())
        b->verified_gen = *texture_range_gen(b->key.texture_vram_offset,
                                             b->key.texture_length);
}

// FIXME: Make sure we update sampler when data matches. Should we add filtering
// options to the textureshape?
static void upload_texture_image(PGRAPHState *pg, int texture_idx,
                                 TextureBinding *binding)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &binding->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_TEX_UPLOAD);
    /* xboxrecomp diagnostic (docs Update 489), RECOMP_VK_TRACE_XFER=1 */
    nv2a_recomp_vk_trace_xfer("tex-upload", binding->key.texture_vram_offset,
                              state->width, state->height, state->color_format,
                              -1, state->levels);

    g_autofree TextureLayout *layout = get_texture_layout(pg, texture_idx);
    const int num_layers = state->cubemap ? 6 : 1;

    /* xboxrecomp diagnostic (docs Update 622): RECOMP_TEX_DUMP=<hex>[,<hex>]
     * writes the decoded levels of these textures (as uploaded to the host)
     * and 64 KiB of the raw guest bytes, once per address, to the working
     * directory: texdump_<addr>_l<layer>_m<level>_<w>x<h>.bin. */
    {
        /* RECOMP_TEX_DUMP=all: level 0 of every texture uploaded once
         * (up to 600, 2D only) for finding a texture by its picture. */
        const char *e = getenv("RECOMP_TEX_DUMP");
        static hwaddr dumped[600];
        static int n_dumped;
        hwaddr addr = binding->key.texture_vram_offset;
        bool want = e && !strcmp(e, "all") && !state->cubemap, done = false;
        for (const char *p = want ? NULL : e; p && *p;) {
            char *end;
            unsigned long long v = strtoull(p, &end, 16);
            if (end == p)
                break;
            want |= v == addr;
            p = *end == ',' ? end + 1 : end;
        }
        for (int i = 0; i < n_dumped; i++)
            done |= dumped[i] == addr;
        if (want && !done && n_dumped < 600) {
            NV2AState *d = container_of(pg, NV2AState, pgraph);
            char name[128];
            FILE *f;
            dumped[n_dumped++] = addr;
            snprintf(name, sizeof(name), "texraw_%08llX.bin",
                     (unsigned long long)addr);
            bool all = !strcmp(e, "all");
            if (!all && (f = fopen(name, "wb"))) {
                fwrite((uint8_t *)d->vram_ptr + addr, 1, 0x10000, f);
                fclose(f);
            }
            for (int l = 0; l < num_layers; l++)
                for (int m = 0; m < (all ? 1 : (int)state->levels); m++) {
                    TextureLevel *lv = &layout->layers[l].levels[m];
                    snprintf(name, sizeof(name), "texdump_%08llX_l%d_m%d_%ux%u.bin",
                             (unsigned long long)addr, l, m, lv->width,
                             lv->height);
                    if ((f = fopen(name, "wb"))) {
                        fwrite(lv->decoded_data, 1, lv->decoded_size, f);
                        fclose(f);
                    }
                }
            fprintf(stderr, "[TEX-DUMP] %08llX fmt=0x%X vk=%d %ux%u levels=%u "
                    "cubemap=%d border=%d pitch=%u\n",
                    (unsigned long long)addr, state->color_format,
                    (int)vkf.vk_format, state->width, state->height,
                    state->levels, state->cubemap, state->border, state->pitch);
        }
    }

    // Calculate decoded texture data size
    size_t texture_data_size = 0;
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            size_t size = layer->levels[level_idx].decoded_size;
            assert(size);
            texture_data_size += size;
        }
    }

    assert(texture_data_size <=
           r->storage_buffers[BUFFER_STAGING_SRC].buffer_size);

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;

    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                          (void *)&mapped_memory_ptr));

    int num_regions = num_layers * state->levels;
    g_autofree VkBufferImageCopy *regions =
        g_malloc0_n(num_regions, sizeof(VkBufferImageCopy));

    VkBufferImageCopy *region = regions;
    VkDeviceSize buffer_offset = 0;

    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        NV2A_VK_DPRINTF("Layer %d", layer_idx);
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            TextureLevel *level = &layer->levels[level_idx];
            NV2A_VK_DPRINTF(" - Level %d, w=%d h=%d d=%d @ %08" HWADDR_PRIx,
                            level_idx, level->width, level->height,
                            level->depth, buffer_offset);
            memcpy(mapped_memory_ptr + buffer_offset, level->decoded_data,
                   level->decoded_size);
            *region = (VkBufferImageCopy){
                .bufferOffset = buffer_offset,
                .bufferRowLength = 0, // Tightly packed
                .bufferImageHeight = 0,
                .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .imageSubresource.mipLevel = level_idx,
                .imageSubresource.baseArrayLayer = layer_idx,
                .imageSubresource.layerCount = 1,
                .imageOffset = (VkOffset3D){ 0, 0, 0 },
                .imageExtent =
                    (VkExtent3D){ level->width, level->height, level->depth },
            };
            buffer_offset += level->decoded_size;
            region++;
        }
    }
    assert(buffer_offset <= r->storage_buffers[BUFFER_STAGING_SRC].buffer_size);

    vmaFlushAllocation(r->allocator,
                       r->storage_buffers[BUFFER_STAGING_SRC].allocation, 0,
                       VK_WHOLE_SIZE);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_SRC].allocation);

    // FIXME: Use nondraw. Need to fill and copy tex buffer at once
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_STAGING_SRC].buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    binding->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           binding->image, binding->current_layout,
                           num_regions, regions);

    pgraph_vk_transition_image_layout(pg, cmd, binding->image, vkf.vk_format,
                                      binding->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    binding->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_4);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd, VK_AUX_OTHER);

    // Release decoded texture data
    for (int layer_idx = 0; layer_idx < num_layers; layer_idx++) {
        TextureLayer *layer = &layout->layers[layer_idx];
        for (int level_idx = 0; level_idx < state->levels; level_idx++) {
            g_free(layer->levels[level_idx].decoded_data);
        }
    }
    /* xboxrecomp: g_autofree is a no-op under MSVC */
    g_free(regions);
    g_free(layout);
}

static void copy_zeta_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                       TextureBinding *texture)
{
    assert(!surface->color);

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    bool use_compute_to_convert_depth_stencil =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool compute_needs_finish = use_compute_to_convert_depth_stencil &&
                                pgraph_vk_compute_needs_finish(r);
    if (compute_needs_finish) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    size_t copied_image_size =
        scaled_width * scaled_height * surface->host_fmt.host_bytes_per_pixel;
    size_t stencil_buffer_offset = 0;
    size_t stencil_buffer_size = 0;

    int num_regions = 0;
    VkBufferImageCopy regions[2];
    regions[num_regions++] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0, // Tightly packed
        .bufferImageHeight = 0, // Tightly packed
        .imageSubresource.aspectMask = surface->color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){0, 0, 0},
        .imageExtent = (VkExtent3D){scaled_width, scaled_height, 1},
    };

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        stencil_buffer_offset =
            ROUND_UP(scaled_width * scaled_height * 4,
                     r->device_props.limits.minStorageBufferOffsetAlignment);
        stencil_buffer_size = scaled_width * scaled_height;
        copied_image_size += stencil_buffer_size;

        regions[num_regions++] = (VkBufferImageCopy){
            .bufferOffset = stencil_buffer_offset,
            .bufferRowLength = 0, // Tightly packed
            .bufferImageHeight = 0, // Tightly packed
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.mipLevel = 0,
            .imageSubresource.baseArrayLayer = 0,
            .imageSubresource.layerCount = 1,
            .imageOffset = (VkOffset3D){0, 0, 0},
            .imageExtent = (VkExtent3D){scaled_width, scaled_height, 1},
        };
    }
    StorageBuffer *dst_storage_buffer = &r->storage_buffers[BUFFER_COMPUTE_DST];
    assert(dst_storage_buffer->buffer_size >= copied_image_size);

    if (surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM) {
        /* xboxrecomp (docs Update 490, see
         * check_surface_to_texture_compatiblity): D16 depth -> buffer ->
         * R16_UNORM texture, bit for bit what the guest-RAM round trip
         * produced, without leaving the GPU. */
        pgraph_vk_transition_image_layout(
            pg, cmd, surface->image, surface->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cmd, surface->image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst_storage_buffer->buffer, 1, regions);
        pgraph_vk_transition_image_layout(
            pg, cmd, surface->image, surface->host_fmt.vk_format,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        VkBufferMemoryBarrier d16_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = dst_storage_buffer->buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &d16_barrier, 0, NULL);

        pgraph_vk_transition_image_layout(pg, cmd, texture->image,
                                          vkf.vk_format,
                                          texture->current_layout,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        VkBufferImageCopy d16_region = {
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
        };
        vkCmdCopyBufferToImage(cmd, dst_storage_buffer->buffer,
                               texture->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &d16_region);

        VkBufferMemoryBarrier d16_post_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = dst_storage_buffer->buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &d16_post_barrier, 0, NULL);

        pgraph_vk_transition_image_layout(
            pg, cmd, texture->image, vkf.vk_format, texture->current_layout,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        pgraph_vk_end_debug_marker(r, cmd);
        pgraph_vk_end_nondraw_commands(pg, cmd);

        texture->draw_time = surface->draw_time;
        return;
    }

    assert(use_compute_to_convert_depth_stencil && "Unimplemented");

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    vkCmdCopyImageToBuffer(
        cmd, surface->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_storage_buffer->buffer,
        num_regions, regions);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    size_t packed_image_size = scaled_width * scaled_height * 4;

    VkBufferMemoryBarrier pre_pack_src_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                         1, &pre_pack_src_barrier, 0, NULL);

    VkBufferMemoryBarrier pre_pack_dst_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                         1, &pre_pack_dst_barrier, 0, NULL);

    pgraph_vk_pack_depth_stencil(
        pg, surface, cmd,
        r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
        r->storage_buffers[BUFFER_COMPUTE_SRC].buffer, false);

    VkBufferMemoryBarrier post_pack_src_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_pack_src_barrier, 0, NULL);

    VkBufferMemoryBarrier post_pack_dst_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
        .size = packed_image_size
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_pack_dst_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    regions[0] = (VkBufferImageCopy){
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
    };
    vkCmdCopyBufferToImage(
        cmd, r->storage_buffers[BUFFER_COMPUTE_SRC].buffer, texture->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, regions);

    VkBufferMemoryBarrier post_copy_src_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_copy_src_barrier, 0, NULL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

/* A linear depth texture can cover a larger pitched RAM area than the active
 * depth surface.  The ordinary readback updates only the rendered rectangle;
 * bytes outside it remain the guest-RAM contents.  Reproduce that composition
 * on the GPU instead of reading the active rectangle back to the CPU. */
static void copy_partial_z16_surface_to_texture(PGRAPHState *pg,
                                                 SurfaceBinding *surface,
                                                 TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *dst = &r->storage_buffers[BUFFER_COMPUTE_DST];
    VkColorFormatInfo vkf =
        kelvin_color_format_vk_map[texture->key.state.color_format];
    unsigned int scaled_width = surface->width;
    unsigned int scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    assert(surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM);
    assert(vkf.vk_format == VK_FORMAT_R16_UNORM);
    assert(dst->buffer_size >= (size_t)surface->width * surface->height * 2);

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, VK_FORMAT_D16_UNORM,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkImage source_image = surface->image;
    if (pg->surface_scale_factor != 1) {
        pgraph_vk_transition_image_layout(
            pg, cmd, surface->image_scratch, VK_FORMAT_D16_UNORM,
            surface->image_scratch_current_layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        surface->image_scratch_current_layout =
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkImageBlit blit = {
            .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                .layerCount = 1 },
            .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                .layerCount = 1 },
            .srcOffsets[1] = { (int32_t)scaled_width,
                               (int32_t)scaled_height, 1 },
            .dstOffsets[1] = { (int32_t)surface->width,
                               (int32_t)surface->height, 1 },
        };
        vkCmdBlitImage(cmd, surface->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       surface->image_scratch,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);
        pgraph_vk_transition_image_layout(
            pg, cmd, surface->image_scratch, VK_FORMAT_D16_UNORM,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        surface->image_scratch_current_layout =
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        source_image = surface->image_scratch;
    }

    VkBufferMemoryBarrier before = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst->buffer,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &before, 0, NULL);
    VkBufferImageCopy region = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                              .layerCount = 1 },
        .imageExtent = { surface->width, surface->height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, source_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->buffer,
                           1, &region);
    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, VK_FORMAT_D16_UNORM,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkBufferMemoryBarrier written = before;
    written.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    written.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &written, 0, NULL);
    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(cmd, dst->buffer, texture->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    before.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &before, 0, NULL);
    pgraph_vk_transition_image_layout(
        pg, cmd, texture->image, vkf.vk_format, texture->current_layout,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);
    texture->draw_time = surface->draw_time;

    static int last_trace_frame = -1;
    if (last_trace_frame != pg->frame_time && pg->frame_time % 30 == 0) {
        last_trace_frame = pg->frame_time;
        fprintf(stderr, "[VK-Z16-PARTIAL] frame=%d addr=%08llX %ux%u -> %ux%u\n",
                pg->frame_time, (unsigned long long)surface->vram_addr,
                surface->width, surface->height, texture->key.state.width,
                texture->key.state.height);
    }
}

static bool can_copy_partial_z16_surface(PGRAPHState *pg,
                                         const SurfaceBinding *surface,
                                         const TextureShape *shape,
                                         size_t texture_length)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_VK_Z16_PARTIAL_TEX");
        enabled = e && e[0] == '1';
    }
    return enabled && surface && !surface->color && !surface->swizzle &&
           surface->shape.zeta_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 &&
           surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM &&
           shape->color_format ==
               NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED &&
           !shape->cubemap && shape->levels == 1 &&
           shape->dimensionality == 2 &&
           surface->pitch == shape->pitch &&
           surface->width < shape->width &&
           surface->height < shape->height &&
           shape->pitch == shape->width * 2 &&
           surface->size <= texture_length &&
           pg->surface_scale_factor >= 2 && /* any upscale (docs Update 635) */
           surface->image_scratch != VK_NULL_HANDLE &&
           pg->vk_renderer_state->storage_buffers[BUFFER_COMPUTE_DST].buffer_size >=
               (size_t)surface->width * surface->height * 2;
}

/* The GPU overlay replaces exactly this rectangle, so changes to its stale
 * RAM copy must not invalidate the 512x512 base image every frame.  Hash only
 * the right strip and bottom rows, which really come from guest RAM. */
static uint64_t hash_partial_z16_ram_border(const uint8_t *data,
                                             const SurfaceBinding *surface,
                                             const TextureShape *shape)
{
    size_t pitch = shape->pitch;
    size_t active_bytes = (size_t)surface->width * 2;
    uint64_t hash = fast_hash(data + (size_t)surface->height * pitch,
                              (size_t)(shape->height - surface->height) * pitch);
    for (unsigned int y = 0; y < surface->height; y++) {
        uint64_t row_hash = fast_hash(data + (size_t)y * pitch + active_bytes,
                                      pitch - active_bytes);
        hash ^= row_hash + UINT64_C(0x9E3779B97F4A7C15) +
                (hash << 6) + (hash >> 2);
    }
    return hash;
}

// FIXME: Should be able to skip the copy and sample the original surface image
/* RECOMP_VK_BLOOM_PREFILTER=<n> (docs Update 656): 0 off, 1 the
 * native copy averages a box of ~1.3 guest px, 2 (default) ~2.7 px (steadier
 * small lights). Only for large frames read into the bloom chain at a
 * render scale above 1. */
static int bloom_prefilter_level(void)
{
    static int level = -1;
    if (level < 0) {
        const char *e = getenv("RECOMP_VK_BLOOM_PREFILTER");
        level = e ? atoi(e) : 2;
        if (level < 0)
            level = 0;
        if (level > 3)
            level = 3;
    }
    return level;
}

/* Scratch images for the prefilter's halving steps, kept per step and
 * recreated when the size changes. */
static VkImage bloom_scratch_image(PGRAPHState *pg, int step, unsigned int w,
                                   unsigned int h, VkFormat fmt)
{
    static struct { VkImage img; VmaAllocation alloc; unsigned int w, h;
                    VkFormat fmt; } s[4];
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (s[step].img && (s[step].w != w || s[step].h != h ||
                        s[step].fmt != fmt)) {
        /* Only on a render-scale change. The old image may still be in a
         * recorded command buffer and this runs while one is being
         * recorded, so it is left allocated rather than freed here. */
        s[step].img = VK_NULL_HANDLE;
    }
    if (!s[step].img) {
        VkImageCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .extent = { w, h, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .format = fmt,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VmaAllocationCreateInfo alloc = {
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        VK_CHECK(vmaCreateImage(r->allocator, &info, &alloc, &s[step].img,
                                &s[step].alloc, NULL));
        s[step].w = w;
        s[step].h = h;
        s[step].fmt = fmt;
    }
    return s[step].img;
}

static bool native_small_target(PGRAPHState *pg, const SurfaceBinding *s);

/* Scratch images for the small-target supersample, one per size and format
 * (the chain's sizes stay fixed per render scale, so this never grows past
 * a handful of entries). */
static VkImage small_ss_scratch(PGRAPHState *pg, unsigned int w,
                                unsigned int h, VkFormat fmt)
{
    static struct { VkImage img; VmaAllocation alloc; unsigned int w, h;
                    VkFormat fmt; } s[32];
    PGRAPHVkState *r = pg->vk_renderer_state;
    int i;
    for (i = 0; i < 32 && s[i].img; i++) {
        if (s[i].w == w && s[i].h == h && s[i].fmt == fmt)
            return s[i].img;
    }
    if (i == 32)
        return VK_NULL_HANDLE;
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = fmt,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo alloc = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VK_CHECK(vmaCreateImage(r->allocator, &info, &alloc, &s[i].img,
                            &s[i].alloc, NULL));
    s[i].w = w;
    s[i].h = h;
    s[i].fmt = fmt;
    return s[i].img;
}

/* Native-size copy of a small upscaled render target, supersampled (docs
 * Update 666). SCDA's glow masks (e.g. the red rim of the ice boulders in
 * Iceland) are drawn into 256x256 targets and blurred down to 32x32; taking
 * one texel per guest pixel (Update 660) keeps the console's hard 256-grid
 * edges, which step visibly while the camera moves. Here the upscaled
 * target is averaged down in exact 2:1 steps plus one linear step, so
 * edges keep their sub-texel position. Row and column 0 still take the
 * covered texel (i+0.5)*s: their left/top part stays unwritten upscaled
 * (Update 660), and an average would pull that stale texel back in. All
 * writes to the texture are disjoint regions. Returns false when it could
 * not run (caller falls back to the per-pixel copy). */
static bool small_target_supersample(PGRAPHState *pg, VkCommandBuffer cmd,
                                     SurfaceBinding *surface,
                                     TextureBinding *texture,
                                     unsigned int sw, unsigned int sh)
{
    unsigned int w = surface->width, h = surface->height;
    unsigned int s = sw / w;
    VkFormat fmt = surface->host_fmt.vk_format;
    if (w < 2 || h < 2 || s < 2 || sw != w * s || sh != h * s)
        return false;

    VkImage cur = surface->image;
    unsigned int cw = sw, ch = sh;
    while (cw / 2 >= w && ch / 2 >= h) {
        unsigned int nw = cw / 2, nh = ch / 2;
        VkImage dst = small_ss_scratch(pg, nw, nh, fmt);
        if (!dst)
            return false;
        pgraph_vk_transition_image_layout(pg, cmd, dst, fmt,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit half = {
            .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .srcSubresource.layerCount = 1,
            .srcOffsets[1] = { (int32_t)cw, (int32_t)ch, 1 },
            .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .dstSubresource.layerCount = 1,
            .dstOffsets[1] = { (int32_t)nw, (int32_t)nh, 1 },
        };
        vkCmdBlitImage(cmd, cur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &half,
                       VK_FILTER_LINEAR);
        pgraph_vk_transition_image_layout(pg, cmd, dst, fmt,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cur = dst;
        cw = nw;
        ch = nh;
    }
    if (cw != w || ch != h) {
        VkImage dst = small_ss_scratch(pg, w, h, fmt);
        if (!dst)
            return false;
        pgraph_vk_transition_image_layout(pg, cmd, dst, fmt,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit last = {
            .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .srcSubresource.layerCount = 1,
            .srcOffsets[1] = { (int32_t)cw, (int32_t)ch, 1 },
            .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .dstSubresource.layerCount = 1,
            .dstOffsets[1] = { (int32_t)w, (int32_t)h, 1 },
        };
        vkCmdBlitImage(cmd, cur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &last,
                       VK_FILTER_LINEAR);
        pgraph_vk_transition_image_layout(pg, cmd, dst, fmt,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cur = dst;
    }

    VkImageBlit parts[3] = {
        { /* interior: the average, 1:1 */
            .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .srcSubresource.layerCount = 1,
            .srcOffsets = { { 1, 1, 0 }, { (int32_t)w, (int32_t)h, 1 } },
            .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .dstSubresource.layerCount = 1,
            .dstOffsets = { { 1, 1, 0 }, { (int32_t)w, (int32_t)h, 1 } },
        },
        { /* column 0 (with the corner): covered texel per guest pixel */
            .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .srcSubresource.layerCount = 1,
            .srcOffsets = { { 0, 0, 0 }, { (int32_t)s, (int32_t)sh, 1 } },
            .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .dstSubresource.layerCount = 1,
            .dstOffsets = { { 0, 0, 0 }, { 1, (int32_t)h, 1 } },
        },
        { /* row 0 without the corner */
            .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .srcSubresource.layerCount = 1,
            .srcOffsets = { { (int32_t)s, 0, 0 }, { (int32_t)sw, (int32_t)s, 1 } },
            .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .dstSubresource.layerCount = 1,
            .dstOffsets = { { 1, 0, 0 }, { (int32_t)w, 1, 1 } },
        },
    };
    vkCmdBlitImage(cmd, cur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   texture->image, texture->current_layout, 1, &parts[0],
                   VK_FILTER_NEAREST);
    vkCmdBlitImage(cmd, surface->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   texture->image, texture->current_layout, 2, &parts[1],
                   VK_FILTER_NEAREST);
    static int logged;
    if (!logged) {
        logged = 1;
        fprintf(stderr, "[VK] small render targets: supersampled native copy "
                "(%ux%u from %ux%u)\n", w, h, sw, sh);
    }
    return true;
}

static void copy_surface_to_texture(PGRAPHState *pg, SurfaceBinding *surface,
                                    TextureBinding *texture)
{
    if (!surface->color) {
        copy_zeta_surface_to_texture(pg, surface, texture);
        return;
    }

    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape *state = &texture->key.state;
    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state->color_format];

    nv2a_profile_inc_counter(NV2A_PROF_SURF_TO_TEX);

    trace_nv2a_pgraph_surface_render_to_texture(
        surface->vram_addr, surface->width, surface->height);

    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    int prefilter = bloom_prefilter_level();
    if (texture->key.scale == 1 && pg->surface_scale_factor > 1 &&
        prefilter > 0 && (surface->width > 256 || surface->height > 256)) {
        /* Bloom source prefilter (docs Update 656): the frame the chain's
         * first downsample reads is averaged down in exact 2:1 steps and
         * scaled back to guest size, so 1-2 px lights spread over the game's
         * 2.5 px sample spacing instead of blinking in and out of it. */
        unsigned int cw = surface->width, ch = surface->height;
        pgraph_apply_scaling_factor(pg, &cw, &ch);
        unsigned int target_w = surface->width >> (prefilter - 1);
        VkImage cur = surface->image;
        VkFormat fmt = surface->host_fmt.vk_format;
        int step = 0;
        while (cw > target_w && cw >= 4 && ch >= 4 && step < 4) {
            unsigned int nw = cw / 2, nh = ch / 2;
            VkImage dst = bloom_scratch_image(pg, step, nw, nh, fmt);
            pgraph_vk_transition_image_layout(pg, cmd, dst, fmt,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit half = {
                .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .srcSubresource.layerCount = 1,
                .srcOffsets[1] = { (int32_t)cw, (int32_t)ch, 1 },
                .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .dstSubresource.layerCount = 1,
                .dstOffsets[1] = { (int32_t)nw, (int32_t)nh, 1 },
            };
            vkCmdBlitImage(cmd, cur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &half,
                           VK_FILTER_LINEAR);
            pgraph_vk_transition_image_layout(pg, cmd, dst, fmt,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cur = dst;
            cw = nw;
            ch = nh;
            step++;
        }
        VkImageBlit up = {
            .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .srcSubresource.layerCount = 1,
            .srcOffsets[1] = { (int32_t)cw, (int32_t)ch, 1 },
            .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .dstSubresource.layerCount = 1,
            .dstOffsets[1] = { (int32_t)surface->width,
                               (int32_t)surface->height, 1 },
        };
        vkCmdBlitImage(cmd, cur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       texture->image, texture->current_layout, 1, &up,
                       VK_FILTER_LINEAR);
    } else if (texture->key.scale == 1 && pg->surface_scale_factor > 1) {
        /* Native-size texture of an upscaled render target (docs Update
         * 654, see native_small_target): scale it down on the GPU. */
        unsigned int sw = surface->width, sh = surface->height;
        pgraph_apply_scaling_factor(pg, &sw, &sh);
        /* Small targets: take each guest pixel's centre texel instead of
         * averaging its block (docs Update 660). SCDA's bloom quads start at
         * the first pixel centre; the console counts that pixel in, but
         * upscaled the left/top half of it stays unwritten (one host texel
         * at 2x, two at 4x). LINEAR averaged that stale texel into texel 0
         * at even scales -- the blur spread it into a band that flickered
         * along the left/top screen edge (green veil without cross-type
         * discard). NEAREST samples (i+0.5)*s, a covered texel at every
         * scale, i.e. what the console renders. RECOMP_VK_NATIVE_SMALL_
         * NEAREST=0 restores the average. */
        static int nearest = -1;
        if (nearest < 0) {
            const char *e = getenv("RECOMP_VK_NATIVE_SMALL_NEAREST");
            nearest = !(e && e[0] == '0');
        }
        /* RECOMP_VK_NATIVE_SMALL_SS=0 (docs Update 666): per-pixel copy
         * instead of the supersample. */
        static int supersample = -1;
        if (supersample < 0) {
            const char *e = getenv("RECOMP_VK_NATIVE_SMALL_SS");
            supersample = !(e && e[0] == '0');
        }
        bool small = native_small_target(pg, surface);
        if (small && nearest && supersample &&
            small_target_supersample(pg, cmd, surface, texture, sw, sh)) {
            goto copied;
        }
        VkFilter filter = nearest && small ?
                              VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        VkImageBlit blit = {
            .srcSubresource.aspectMask = surface->host_fmt.aspect,
            .srcSubresource.layerCount = 1,
            .srcOffsets[1] = { (int32_t)sw, (int32_t)sh, 1 },
            .dstSubresource.aspectMask = surface->host_fmt.aspect,
            .dstSubresource.layerCount = 1,
            .dstOffsets[1] = { (int32_t)surface->width,
                               (int32_t)surface->height, 1 },
        };
        vkCmdBlitImage(cmd, surface->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image,
                       texture->current_layout, 1, &blit, filter);
    } else {
        VkImageCopy region = {
            .srcSubresource.aspectMask = surface->host_fmt.aspect,
            .srcSubresource.layerCount = 1,
            .dstSubresource.aspectMask = surface->host_fmt.aspect,
            .dstSubresource.layerCount = 1,
            .extent.width = surface->width,
            .extent.height = surface->height,
            .extent.depth = 1,
        };
        pgraph_apply_scaling_factor(pg, &region.extent.width,
                                    &region.extent.height);
        vkCmdCopyImage(cmd, surface->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image,
                       texture->current_layout, 1, &region);
    }
copied:

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    pgraph_vk_transition_image_layout(pg, cmd, texture->image, vkf.vk_format,
                                      texture->current_layout,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    texture->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    texture->draw_time = surface->draw_time;
}

/* RECOMP_VK_NATIVE_SMALL_RT=<max> (docs Update 654, default 256, 0 = off):
 * a swizzled colour render target up to max x max is sampled as a texture
 * at the guest's own size even when rendering is upscaled. SCDA's bloom
 * chain (256..16, swizzled) downsamples with four taps one texel apart --
 * at 3x those taps skip two of every three texels, so small bright lights
 * flickered as a dotted grid and figures got a speckled rim. Sampling the
 * native-size copy restores the console's filtering; the large frame stays
 * upscaled. */
static bool native_small_target(PGRAPHState *pg, const SurfaceBinding *s)
{
    static int max = -1;
    if (max < 0) {
        const char *e = getenv("RECOMP_VK_NATIVE_SMALL_RT");
        max = e ? atoi(e) : 256;
    }
    return max > 0 && pg->surface_scale_factor > 1 && s->color &&
           s->swizzle && s->width <= (unsigned)max &&
           s->height <= (unsigned)max;
}

static bool check_surface_to_texture_compatiblity(const SurfaceBinding *surface,
                                                  const TextureShape *shape)
{
    // FIXME: Better checks/handling on formats and surface-texture compat

    if ((!surface->swizzle && surface->pitch != shape->pitch) ||
        surface->width != shape->width ||
        surface->height != shape->height) {
        return false;
    }

    int surface_fmt = surface->shape.color_format;
    int texture_fmt = shape->color_format;

    if (!surface->color) {
        if (surface->shape.zeta_format == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) {
            return true;
        }
        /* xboxrecomp (docs Update 490): a Z16 depth buffer sampled as a
         * 16-bit fixed depth texture (SCDA's 512x512 shadow map) stays on
         * the GPU; upstream sent it through guest RAM every frame (read-back
         * + texture upload, two queue drains). RECOMP_VK_Z16_TEX=0: off. */
        static int z16_tex = -1;
        if (z16_tex < 0) {
            const char *e = getenv("RECOMP_VK_Z16_TEX");
            z16_tex = !(e && e[0] == '0');
        }
        if (z16_tex &&
            surface->shape.zeta_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16 &&
            surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM &&
            !shape->cubemap && shape->levels == 1 &&
            (texture_fmt == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_DEPTH_Y16_FIXED ||
             texture_fmt ==
                 NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_DEPTH_Y16_FIXED)) {
            return true;
        }
        return false;
    }

    if (shape->cubemap) {
        // FIXME: Support rendering surface to cubemap face
        return false;
    }

    if (shape->levels > 1) {
        // FIXME: Support rendering surface to mip levels
        return false;
    }

    switch (surface_fmt) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8: switch(texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_X8R8G8B8: return true;
        default: break;
        }
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8: switch (texture_fmt) {
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R8G8B8A8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8: return true;
        case NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8: return true;
        default: break;
        }
        break;
    default:
        break;
    }

    trace_nv2a_pgraph_surface_texture_compat_failed(
        surface_fmt, texture_fmt);
    return false;
}

static void create_dummy_texture(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = 16,
        .extent.height = 16,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = VK_FORMAT_R8_UNORM,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = 0,
    };

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VkImage texture_image;
    VmaAllocation texture_allocation;

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &texture_image,
                            &texture_allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_R },
    };
    VkImageView texture_image_view;
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &texture_image_view));

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    };

    VkSampler texture_sampler;
    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &texture_sampler));

    // Copy texture data to mapped device buffer
    uint8_t *mapped_memory_ptr;
    size_t texture_data_size =
        image_create_info.extent.width * image_create_info.extent.height;

    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_SRC].allocation,
                          (void *)&mapped_memory_ptr));
    memset(mapped_memory_ptr, 0xff, texture_data_size);

    vmaFlushAllocation(r->allocator,
                       r->storage_buffers[BUFFER_STAGING_SRC].allocation, 0,
                       VK_WHOLE_SIZE);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_SRC].allocation);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_GREEN, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, texture_image, VK_FORMAT_R8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .imageSubresource.mipLevel = 0,
        .imageSubresource.baseArrayLayer = 0,
        .imageSubresource.layerCount = 1,
        .imageOffset = (VkOffset3D){ 0, 0, 0 },
        .imageExtent = (VkExtent3D){ image_create_info.extent.width,
                                     image_create_info.extent.height, 1 },
    };
    vkCmdCopyBufferToImage(cmd, r->storage_buffers[BUFFER_STAGING_SRC].buffer,
                           texture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &region);

    pgraph_vk_transition_image_layout(pg, cmd, texture_image,
                                      VK_FORMAT_R8_UNORM,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd, VK_AUX_OTHER);

    r->dummy_texture = (TextureBinding){
        .key.scale = 1.0,
        .image = texture_image,
        .current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .allocation = texture_allocation,
        .image_view = texture_image_view,
        .sampler = texture_sampler,
    };
}

static void destroy_dummy_texture(PGRAPHVkState *r)
{
    texture_cache_release_node_resources(r, &r->dummy_texture);
}

static void set_texture_label(PGRAPHState *pg, TextureBinding *texture)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    g_autofree gchar *label = g_strdup_printf(
        "Texture %" HWADDR_PRIx "h fmt:%02xh %dx%dx%d lvls:%d",
        texture->key.texture_vram_offset, texture->key.state.color_format,
        texture->key.state.width, texture->key.state.height,
        texture->key.state.depth, texture->key.state.levels);

    VkDebugUtilsObjectNameInfoEXT name_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = (uint64_t)texture->image,
        .pObjectName = label,
    };

    if (r->debug_utils_extension_enabled) {
        vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
    }
    vmaSetAllocationName(r->allocator, texture->allocation, label);
    g_free(label); /* xboxrecomp: g_autofree is a no-op under MSVC */
}

static bool is_linear_filter_supported_for_format(PGRAPHVkState *r,
                                                  int kelvin_format)
{
    return r->texture_format_properties[kelvin_format].optimalTilingFeatures &
           VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}

static void create_texture(PGRAPHState *pg, int texture_idx)
{
    NV2A_VK_DGROUP_BEGIN("Creating texture %d", texture_idx);

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureShape state = pgraph_get_texture_shape(pg, texture_idx); // FIXME: Check for pad issues
    BasicColorFormatInfo f_basic = kelvin_color_format_info_map[state.color_format];

    const hwaddr texture_vram_offset = pgraph_get_texture_phys_addr(pg, texture_idx);
    size_t texture_palette_data_size;
    const hwaddr texture_palette_vram_offset =
        pgraph_get_texture_palette_phys_addr_length(pg, texture_idx,
                                                    &texture_palette_data_size);

    size_t texture_length = pgraph_get_texture_length(pg, &state);

    uint32_t filter =
        pgraph_reg_r(pg, NV_PGRAPH_TEXFILTER0 + texture_idx * 4);
    uint32_t address =
        pgraph_reg_r(pg, NV_PGRAPH_TEXADDRESS0 + texture_idx * 4);
    uint32_t border_color_pack32 =
        pgraph_reg_r(pg, NV_PGRAPH_BORDERCOLOR0 + texture_idx * 4);

    TextureKey key;
    memset(&key, 0, sizeof(key));
    key.state = state;
    key.texture_vram_offset = texture_vram_offset;
    key.texture_length = texture_length;
    key.palette_vram_offset = texture_palette_vram_offset;
    key.palette_length = texture_palette_data_size;
    key.scale = 1;

    // FIXME: Separate sampler from texture
    key.filter = filter;
    key.address = address;
    key.border_color = border_color_pack32;

    bool is_indexed = (state.color_format ==
            NV097_SET_TEXTURE_FORMAT_COLOR_SZ_I8_A8R8G8B8);

    bool possibly_dirty = false;
    bool possibly_dirty_checked = false;
    bool surface_to_texture = false;
    bool partial_z16_surface = false;

    // Check active surfaces to see if this texture was a render target
    SurfaceBinding *surface = pgraph_vk_surface_get(d, texture_vram_offset);
    if (surface && state.levels == 1) {
        surface_to_texture =
            check_surface_to_texture_compatiblity(surface, &state);
        if (!surface_to_texture) {
            partial_z16_surface = can_copy_partial_z16_surface(
                pg, surface, &state, texture_length);
        }

        if ((surface_to_texture || partial_z16_surface) &&
            surface->upload_pending) {
            pgraph_vk_upload_surface_data(d, surface, false);
        }
    }

    /* Diagnostic only: does the observed 640x480 SRCCOPY destination stay
     * on the GPU when later bound as a texture, or fall back to guest RAM? */
    if (texture_vram_offset == 0x04DD9000 &&
        pg->frame_time >= 120 && pg->frame_time % 30 == 0) {
        static int trace_enabled = -1;
        static int trace_frame = -1;
        static int trace_count;
        if (trace_enabled < 0) {
            const char *e = getenv("RECOMP_VK_SURF_WAIT_PROBE");
            trace_enabled = e && e[0] == '1';
        }
        if (trace_enabled) {
            if (trace_frame != pg->frame_time) {
                trace_frame = pg->frame_time;
                trace_count = 0;
            }
            if (trace_count++ < 8) {
                fprintf(stderr,
                        "[VK-BLIT-TEXTURE] frame=%d addr=%08llX "
                        "surface=%d compatible=%d partial_z16=%d "
                        "texture=%ux%u fmt=%X levels=%u len=%llu "
                        "surface=%ux%u upload=%d dirty=%d "
                        "surface_draw=%llu draw=%llu\n",
                        pg->frame_time,
                        (unsigned long long)texture_vram_offset,
                        surface != NULL, surface_to_texture,
                        partial_z16_surface, state.width, state.height,
                        state.color_format, state.levels,
                        (unsigned long long)texture_length,
                        surface ? surface->width : 0,
                        surface ? surface->height : 0,
                        surface ? surface->upload_pending : 0,
                        surface ? surface->draw_dirty : 0,
                        (unsigned long long)(surface ? surface->draw_time : 0),
                        (unsigned long long)pg->draw_time);
            }
        }
    }

    /* Diagnostic only: establish which exact shape/format check sends a
     * Z16 render target back through guest RAM instead of the GPU path. */
    if (surface && !surface_to_texture && !surface->color &&
        surface->shape.zeta_format == NV097_SET_SURFACE_FORMAT_ZETA_Z16) {
        static int trace_enabled = -1;
        static int last_frame = -1;
        if (trace_enabled < 0) {
            const char *e = getenv("RECOMP_VK_Z16_MISMATCH_TRACE");
            trace_enabled = e && e[0] == '1';
        }
        if (trace_enabled && last_frame != pg->frame_time) {
            last_frame = pg->frame_time;
            fprintf(stderr,
                    "[VK-Z16-MISMATCH] frame=%d addr=%08llX "
                    "surface=%ux%u pitch=%u swizzle=%d host_fmt=%d "
                    "texture=%ux%u pitch=%u fmt=0x%X levels=%u cubemap=%d "
                    "length=%llu clip=%ux%u+%u+%u\n",
                    pg->frame_time, (unsigned long long)texture_vram_offset,
                    surface->width, surface->height, surface->pitch,
                    surface->swizzle, surface->host_fmt.vk_format,
                    state.width, state.height, state.pitch,
                    state.color_format, state.levels, state.cubemap,
                    (unsigned long long)texture_length,
                    pg->surface_shape.clip_width,
                    pg->surface_shape.clip_height,
                    pg->surface_shape.clip_x,
                    pg->surface_shape.clip_y);
        }
    }

    if (!surface_to_texture && !partial_z16_surface) {
        // FIXME: Restructure to support rendering surfaces to cubemap faces

        // Writeback any surfaces which this texture may index
        pgraph_vk_download_surfaces_in_range_if_dirty(
            pg, texture_vram_offset, texture_length, "texture-range");
    }

    /* also any colour surface sampled while drawing INTO such a small target
     * (the chain's first downsample of the 640x480 frame): at 3x its taps,
     * 7.5 px apart, point-sampled the fine frame and small lights twinkled
     * as a grid in motion; the native copy matches the console's footprint
     * (docs Update 655). RECOMP_VK_NATIVE_SMALL_SRC=0: only the targets. */
    static int native_src = -1;
    if (native_src < 0) {
        const char *e = getenv("RECOMP_VK_NATIVE_SMALL_SRC");
        native_src = !(e && e[0] == '0');
    }
    bool native_small = surface_to_texture && surface->color &&
        (native_small_target(pg, surface) ||
         (native_src && r->color_binding &&
          native_small_target(pg, r->color_binding)));
    if (surface_to_texture && pg->surface_scale_factor > 1 && !native_small) {
        key.scale = pg->surface_scale_factor;
    }

    uint64_t key_hash = fast_hash((void*)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->texture_cache, key_hash, &key);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    bool binding_found = snode->image != VK_NULL_HANDLE;

    if (binding_found) {
        NV2A_VK_DPRINTF("Cache hit");
        r->texture_bindings[texture_idx] = snode;
        possibly_dirty |= snode->possibly_dirty;
    } else {
        possibly_dirty = true;
    }

    if (!surface_to_texture && !possibly_dirty_checked) {
        possibly_dirty |= binding_possibly_dirty(
            d, binding_found ? snode : NULL, texture_vram_offset,
            texture_length, texture_palette_vram_offset,
            texture_palette_data_size);
    }

    // Calculate hash of texture data, if necessary
    void *texture_data = (char*)d->vram_ptr + texture_vram_offset;
    void *palette_data = (char*)d->vram_ptr + texture_palette_vram_offset;

    uint64_t content_hash = 0;
    if (!surface_to_texture && possibly_dirty) {
        content_hash = partial_z16_surface ?
            hash_partial_z16_ram_border(texture_data, surface, &state) :
            fast_hash(texture_data, texture_length);
        if (is_indexed) {
            content_hash ^= fast_hash(palette_data, texture_palette_data_size);
        }
    }

    if (binding_found) {
        if (surface_to_texture) {
            // FIXME: Add draw time tracking
            if (surface->draw_time != snode->draw_time) {
                copy_surface_to_texture(pg, surface, snode);
            }
        } else {
            bool base_uploaded = false;
            if (possibly_dirty && content_hash != snode->hash) {
                /* The cached image may still be referenced by preceding
                 * draws in the main command buffer.  Before the auxiliary
                 * full-RAM upload, complete those draws; the common
                 * per-frame partial copy below stays in the main buffer. */
                if (partial_z16_surface) {
                    pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
                }
                upload_texture_image(pg, texture_idx, snode);
                snode->hash = content_hash;
                base_uploaded = true;
            }
            if (partial_z16_surface &&
                (base_uploaded || surface->draw_time != snode->draw_time)) {
                copy_partial_z16_surface_to_texture(pg, surface, snode);
            }
            /* xboxrecomp (docs Update 487): the content is verified now, so
             * the mark is consumed, as gl/texture.c does. Upstream vk never
             * cleared it, re-hashing a once-marked texture on every use
             * (~27 % of the PFIFO thread in a level). */
            snode->possibly_dirty = false;
            if (possibly_dirty)
                binding_mark_verified(snode);
        }

        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");

    memcpy(&snode->key, &key, sizeof(key));
    snode->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    snode->possibly_dirty = false;
    snode->hash = content_hash;
    binding_mark_verified(snode);

    VkColorFormatInfo vkf = kelvin_color_format_vk_map[state.color_format];
    assert(vkf.vk_format != 0);
    assert(0 < state.dimensionality);
    assert(state.dimensionality < ARRAY_SIZE(dimensionality_to_vk_image_type));
    assert(state.dimensionality <
           ARRAY_SIZE(dimensionality_to_vk_image_view_type));

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = dimensionality_to_vk_image_type[state.dimensionality],
        .extent.width = state.width, // FIXME: Use adjusted size?
        .extent.height = state.height,
        .extent.depth = state.depth,
        .mipLevels = f_basic.linear ? 1 : state.levels,
        .arrayLayers = state.cubemap ? 6 : 1,
        .format = vkf.vk_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .flags = (state.cubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
    };

    if (surface_to_texture && !native_small) {
        pgraph_apply_scaling_factor(pg, &image_create_info.extent.width,
                                        &image_create_info.extent.height);
    }

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &snode->image,
                            &snode->allocation, NULL));

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = snode->image,
        .viewType = state.cubemap ?
            VK_IMAGE_VIEW_TYPE_CUBE :
            dimensionality_to_vk_image_view_type[state.dimensionality],
        .format = vkf.vk_format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = image_create_info.mipLevels,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = image_create_info.arrayLayers,
        .components = vkf.component_map,
    };

    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &snode->image_view));


    void *sampler_next_struct = NULL;

    VkSamplerCustomBorderColorCreateInfoEXT custom_border_color_create_info;
    VkBorderColor vk_border_color;

    bool is_integer_type = vkf.vk_format == VK_FORMAT_R32_UINT;

    if (r->custom_border_color_extension_enabled) {
        vk_border_color = is_integer_type ? VK_BORDER_COLOR_INT_CUSTOM_EXT :
                                            VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        custom_border_color_create_info =
            (VkSamplerCustomBorderColorCreateInfoEXT){
                .sType =
                    VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                .format = image_view_create_info.format,
                .pNext = sampler_next_struct
            };
        if (is_integer_type) {
            float rgba[4];
            pgraph_argb_pack32_to_rgba_float(border_color_pack32, rgba);
            for (int i = 0; i < 4; i++) {
                custom_border_color_create_info.customBorderColor.uint32[i] =
                    (uint32_t)((double)rgba[i] * (double)0xffffffff);
            }
        } else {
            pgraph_argb_pack32_to_rgba_float(
                border_color_pack32,
                custom_border_color_create_info.customBorderColor.float32);
        }
        sampler_next_struct = &custom_border_color_create_info;
    } else {
        // FIXME: Handle custom color in shader
        if (is_integer_type) {
            vk_border_color = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0x00000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        } else if (border_color_pack32 == 0xff000000) {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        } else {
            vk_border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        }
    }

    if (filter & NV_PGRAPH_TEXFILTER0_ASIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_ASIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_RSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_RSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_GSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_GSIGNED");
    if (filter & NV_PGRAPH_TEXFILTER0_BSIGNED)
        NV2A_UNIMPLEMENTED("NV_PGRAPH_TEXFILTER0_BSIGNED");

    VkFilter vk_min_filter, vk_mag_filter;
    unsigned int mag_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MAG);
    assert(mag_filter < ARRAY_SIZE(pgraph_texture_mag_filter_vk_map));

    unsigned int min_filter = GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIN);
    assert(min_filter < ARRAY_SIZE(pgraph_texture_min_filter_vk_map));

    if (is_linear_filter_supported_for_format(r, state.color_format)) {
        vk_mag_filter = pgraph_texture_min_filter_vk_map[mag_filter];
        vk_min_filter = pgraph_texture_min_filter_vk_map[min_filter];
    } else {
        vk_mag_filter = vk_min_filter = VK_FILTER_NEAREST;
    }

    bool mipmap_en =
        !f_basic.linear &&
        !(min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_LOD0 ||
          min_filter == NV_PGRAPH_TEXFILTER0_MIN_CONVOLUTION_2D_LOD0);

    bool mipmap_nearest =
        f_basic.linear || image_create_info.mipLevels == 1 ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_BOX_NEARESTLOD ||
        min_filter == NV_PGRAPH_TEXFILTER0_MIN_TENT_NEARESTLOD;

    /* xboxrecomp (docs Update 630): the NV2A mipmap LOD bias
     * (TEXFILTER bits 0-12, signed fixed point with 8 fractional bits; SCDA
     * uses 0x1F81 = -0.496) is not applied upstream; applied by default
     * since Update 669 (RECOMP_TEX_LOD_BIAS=0: off; it was opt-in before).
     * RECOMP_TEX_MIP_LINEAR=1 (diagnostic) blends mip levels
     * even where the title asks for the nearest one. */
    /* RECOMP_TEX_FILTER (option TextureFiltering, docs Update 669): 0 = as on
     * the console (nearest mip level, with the console's LOD bias -- walls
     * switched to the blurrier level visibly earlier without it), 1 = mip
     * levels blended (no hard sharp/blurry line), 2 (default) = 1 plus 16x
     * anisotropic filtering (surfaces seen at a shallow angle stay sharp).
     * The diagnostic switches below still override. */
    static int tex_filter = -1;
    if (tex_filter < 0) {
        const char *e = getenv("RECOMP_TEX_FILTER");
        tex_filter = e && e[0] >= '0' && e[0] <= '2' ? e[0] - '0' : 2;
        fprintf(stderr, "[VK] texture filtering %d%s\n", tex_filter,
                tex_filter == 2 && !r->sampler_anisotropy_enabled ?
                    " (no anisotropy on this GPU)" : "");
    }
    bool mipped = mipmap_en && !f_basic.linear && image_create_info.mipLevels > 1;
    bool aniso = tex_filter == 2 && mipped && r->sampler_anisotropy_enabled &&
                 vk_min_filter == VK_FILTER_LINEAR;
    if (tex_filter >= 1 && mipped)
        mipmap_nearest = false;
    float lod_bias = 0.0f;
    {
        static int bias_on = -1, mip_linear = -1;
        if (bias_on < 0) {
            const char *e = getenv("RECOMP_TEX_LOD_BIAS");
            bias_on = e ? e[0] == '1' : 1;
            e = getenv("RECOMP_TEX_MIP_LINEAR");
            mip_linear = e && e[0] == '1';
        }
        if (bias_on) {
            int raw = (int)GET_MASK(filter, NV_PGRAPH_TEXFILTER0_MIPMAP_LOD_BIAS);
            if (raw & 0x1000)
                raw -= 0x2000;
            lod_bias = raw / 256.0f;
        }
        if (mip_linear && !f_basic.linear && image_create_info.mipLevels > 1)
            mipmap_nearest = false;
    }

    VkSamplerCreateInfo sampler_create_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = vk_mag_filter,
        .minFilter = vk_min_filter,
        .addressModeU = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRU)),
        .addressModeV = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRV)),
        .addressModeW = lookup_texture_address_mode(
            GET_MASK(address, NV_PGRAPH_TEXADDRESS0_ADDRP)),
        .anisotropyEnable = aniso ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = aniso ?
            MIN(16.0f, r->device_props.limits.maxSamplerAnisotropy) : 1.0f,
        .borderColor = vk_border_color,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .mipmapMode = mipmap_nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST :
                                       VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .minLod = mipmap_en ? MIN(state.min_mipmap_level, state.levels - 1) : 0.0,
        .maxLod = mipmap_en ? MIN(state.max_mipmap_level, state.levels - 1) : 0.0,
        .mipLodBias = lod_bias,
        .pNext = sampler_next_struct,
    };

    VK_CHECK(vkCreateSampler(r->device, &sampler_create_info, NULL,
                             &snode->sampler));

    set_texture_label(pg, snode);

    r->texture_bindings[texture_idx] = snode;

    if (surface_to_texture) {
        copy_surface_to_texture(pg, surface, snode);
    } else {
        upload_texture_image(pg, texture_idx, snode);
        snode->draw_time = 0;
        if (partial_z16_surface) {
            copy_partial_z16_surface_to_texture(pg, surface, snode);
        }
    }

    NV2A_VK_DGROUP_END();
}

static bool check_textures_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!r->texture_bindings[i] || pg->texture_dirty[i]) {
            return true;
        }
    }
    return false;
}

static void update_timestamps(PGRAPHVkState *r)
{
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i]) {
            r->texture_bindings[i]->submit_time = r->submit_count;
        }
    }
}

/* A clean register stage can still sample a surface whose draw_time changed,
 * or RAM written by the CPU.  Only bypass the key/cache reconstruction when
 * neither source can have changed the bound image.  Keep this opt-in until
 * the full-scene A/B and visual checks are complete. */
static bool texture_clean_fast_path(NV2AState *d, int stage)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    TextureBinding *binding = r->texture_bindings[stage];
    if (!binding || binding == &r->dummy_texture ||
        binding->image == VK_NULL_HANDLE || binding->possibly_dirty) {
        return false;
    }

    const TextureKey *key = &binding->key;
    if (!key->texture_length) {
        return false;
    }
    /* This is intentionally stricter than create_texture's overlap test.
     * Any active GPU surface touching texture OR palette takes the old path. */
    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        uint64_t start = surface->vram_addr;
        uint64_t end = start + surface->size;
        uint64_t tex_start = key->texture_vram_offset;
        uint64_t tex_end = tex_start + key->texture_length;
        if (tex_start < end && start < tex_end) {
            return false;
        }
        if (key->palette_length) {
            uint64_t pal_start = key->palette_vram_offset;
            uint64_t pal_end = pal_start + key->palette_length;
            if (pal_start < end && start < pal_end) {
                return false;
            }
        }
    }

    if (binding_possibly_dirty(d, binding, key->texture_vram_offset,
                               key->texture_length,
                               key->palette_vram_offset,
                               key->palette_length)) {
        /* The region query consumes its dirty marker.  Preserve it for the
         * full create_texture path, which must now hash/upload the bytes. */
        binding->possibly_dirty = true;
        return false;
    }
    return true;
}

void pgraph_vk_bind_textures(NV2AState *d)
{
    NV2A_VK_DGROUP_BEGIN("%s", __func__);

    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    static int timing_enabled = -1, sampled_frame = -1;
    static uint64_t phase_us[3], calls, dirty_calls;
    static uint64_t dirty_stages, clean_stages;
    static uint64_t clean_same, clean_changed, clean_marked;
    static uint64_t clean_fast_skipped;
    static int clean_fast_enabled = -1;
    static int clean_fast_verify = -1;
    static uint64_t clean_fast_verified, clean_fast_mismatched;
    int64_t phase_start = 0;
    if (timing_enabled < 0) {
        const char *setting = getenv("RECOMP_VK_DRAW_CPU_SAMPLE");
        timing_enabled = setting && setting[0] == '1';
    }
    if (clean_fast_enabled < 0) {
        const char *setting = getenv("RECOMP_VK_TEXTURE_CLEAN_FAST");
        clean_fast_enabled = setting && setting[0] == '1';
    }
    if (clean_fast_verify < 0) {
        const char *setting = getenv("RECOMP_VK_TEXTURE_CLEAN_FAST_VERIFY");
        clean_fast_verify = setting && setting[0] == '1';
    }
    if (timing_enabled && pg->frame_time >= 120 &&
        pg->frame_time % 30 == 0) {
        if (sampled_frame != pg->frame_time) {
            if (sampled_frame >= 0) {
                fprintf(stderr,
                        "[VK-TEXTURE-PHASE] frame=%d calls=%llu dirty=%llu "
                        "dirty_stages=%llu clean_stages=%llu "
                        "clean_same=%llu clean_changed=%llu clean_marked=%llu "
                        "clean_fast_skipped=%llu fast_verified=%llu "
                        "fast_mismatched=%llu "
                        "check=%llu_us create=%llu_us timestamp=%llu_us\n",
                        sampled_frame, (unsigned long long)calls,
                        (unsigned long long)dirty_calls,
                        (unsigned long long)dirty_stages,
                        (unsigned long long)clean_stages,
                        (unsigned long long)clean_same,
                        (unsigned long long)clean_changed,
                        (unsigned long long)clean_marked,
                        (unsigned long long)clean_fast_skipped,
                        (unsigned long long)clean_fast_verified,
                        (unsigned long long)clean_fast_mismatched,
                        (unsigned long long)phase_us[0],
                        (unsigned long long)phase_us[1],
                        (unsigned long long)phase_us[2]);
            }
            memset(phase_us, 0, sizeof phase_us);
            calls = dirty_calls = dirty_stages = clean_stages = 0;
            clean_same = clean_changed = clean_marked = 0;
            clean_fast_skipped = 0;
            clean_fast_verified = clean_fast_mismatched = 0;
            sampled_frame = pg->frame_time;
        }
        calls++;
        phase_start = nv2a_recomp_vk_now_us();
    }

    // FIXME: Check for modifications on bind fastpath (CPU hook)
    // FIXME: Mark textures that are sourced from surfaces so we can track them

    r->texture_bindings_changed = false;

    bool textures_dirty = check_textures_dirty(pg);
    if (phase_start) {
        int64_t now = nv2a_recomp_vk_now_us();
        phase_us[0] += (uint64_t)(now - phase_start);
        phase_start = now;
        if (textures_dirty) dirty_calls++;
    }
    if (!textures_dirty) {
        NV2A_VK_DPRINTF("Not dirty");
        NV2A_VK_DGROUP_END();
        update_timestamps(r);
        if (phase_start) {
            phase_us[2] += (uint64_t)(nv2a_recomp_vk_now_us() - phase_start);
        }
        return;
    }

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (!pgraph_is_texture_enabled(pg, i)) {
            r->texture_bindings[i] = &r->dummy_texture;
            continue;
        }

        bool stage_dirty = pg->texture_dirty[i];
        TextureBinding *before = phase_start && !stage_dirty ?
            r->texture_bindings[i] : NULL;
        VkImage before_image = before ? before->image : VK_NULL_HANDLE;
        uint64_t before_hash = before ? before->hash : 0;
        unsigned int before_draw_time = before ? before->draw_time : 0;
        bool before_marked = before && before->possibly_dirty;
        if (phase_start) {
            if (stage_dirty) dirty_stages++;
            else clean_stages++;
        }

        if (clean_fast_enabled && !stage_dirty &&
            texture_clean_fast_path(d, i)) {
            if (phase_start) {
                clean_same++;
                clean_fast_skipped++;
            }
            if (phase_start && clean_fast_verify) {
                TextureBinding *binding = r->texture_bindings[i];
                VkImage old_image = binding->image;
                uint64_t old_hash = binding->hash;
                unsigned old_draw_time = binding->draw_time;
                create_texture(pg, i);
                clean_fast_verified++;
                if (r->texture_bindings[i] != binding ||
                    binding->image != old_image ||
                    binding->hash != old_hash ||
                    binding->draw_time != old_draw_time) {
                    clean_fast_mismatched++;
                    fprintf(stderr,
                            "[VK-TEXTURE-FAST-MISMATCH] frame=%d stage=%d "
                            "addr=%08llX hash=%016llX->%016llX "
                            "draw=%u->%u binding_changed=%d\n",
                            pg->frame_time, i,
                            (unsigned long long)
                                binding->key.texture_vram_offset,
                            (unsigned long long)old_hash,
                            (unsigned long long)binding->hash,
                            old_draw_time, binding->draw_time,
                            r->texture_bindings[i] != binding);
                }
            }
            continue;
        }

        create_texture(pg, i);

        if (phase_start && !stage_dirty) {
            TextureBinding *after = r->texture_bindings[i];
            if (after == before && after && after->image == before_image &&
                after->hash == before_hash &&
                after->draw_time == before_draw_time) {
                clean_same++;
                if (before_marked) clean_marked++;
            } else {
                clean_changed++;
                if (clean_changed <= 8) {
                    hwaddr addr = pgraph_get_texture_phys_addr(pg, i);
                    SurfaceBinding *surface = pgraph_vk_surface_get(
                        container_of(pg, NV2AState, pgraph), addr);
                    fprintf(stderr,
                            "[VK-TEXTURE-CLEAN-CHANGE] frame=%d stage=%d "
                            "addr=%08llX before=%p after=%p "
                            "image_changed=%d hash=%016llX->%016llX "
                            "draw=%u->%u marked=%d surface=%p "
                            "surface_draw=%d upload=%d dirty=%d\n",
                            pg->frame_time, i, (unsigned long long)addr,
                            (void *)before, (void *)after,
                            after && after->image != before_image,
                            (unsigned long long)before_hash,
                            (unsigned long long)(after ? after->hash : 0),
                            before_draw_time, after ? after->draw_time : 0,
                            before_marked, (void *)surface,
                            surface ? surface->draw_time : -1,
                            surface ? surface->upload_pending : 0,
                            surface ? surface->draw_dirty : 0);
                }
            }
        }

        pg->texture_dirty[i] = false; // FIXME: Move to renderer?
    }
    if (phase_start) {
        int64_t now = nv2a_recomp_vk_now_us();
        phase_us[1] += (uint64_t)(now - phase_start);
        phase_start = now;
    }

    r->texture_bindings_changed = true;
    update_timestamps(r);
    if (phase_start) {
        phase_us[2] += (uint64_t)(nv2a_recomp_vk_now_us() - phase_start);
    }
    NV2A_VK_DGROUP_END();
}

static void texture_cache_entry_init(Lru *lru, LruNode *node, void *state)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);

    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
    snode->image_view = VK_NULL_HANDLE;
    snode->sampler = VK_NULL_HANDLE;
}

static void texture_cache_release_node_resources(PGRAPHVkState *r, TextureBinding *snode)
{
    vkDestroySampler(r->device, snode->sampler, NULL);
    snode->sampler = VK_NULL_HANDLE;

    vkDestroyImageView(r->device, snode->image_view, NULL);
    snode->image_view = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, snode->image, snode->allocation);
    snode->image = VK_NULL_HANDLE;
    snode->allocation = VK_NULL_HANDLE;
}

static bool texture_cache_entry_pre_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);

    // FIXME: Simplify. We don't really need to check bindings


    // Currently bound
    for (int i = 0; i < ARRAY_SIZE(r->texture_bindings); i++) {
        if (r->texture_bindings[i] == snode) {
            return false;
        }
    }

    // Used in command buffer
    if (r->in_command_buffer && snode->submit_time == r->submit_count) {
        return false;
    }

    return true;
}

static void texture_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, texture_cache);
    TextureBinding *snode = container_of(node, TextureBinding, node);
    texture_cache_release_node_resources(r, snode);
}

static bool texture_cache_entry_compare(Lru *lru, LruNode *node, void *key)
{
    TextureBinding *snode = container_of(node, TextureBinding, node);
    return memcmp(&snode->key, key, sizeof(TextureKey));
}

static void texture_cache_init(PGRAPHVkState *r)
{
    const size_t texture_cache_size = 1024;
    lru_init(&r->texture_cache);
    r->texture_cache_entries = g_malloc_n(texture_cache_size, sizeof(TextureBinding));
    assert(r->texture_cache_entries != NULL);
    for (int i = 0; i < texture_cache_size; i++) {
        lru_add_free(&r->texture_cache, &r->texture_cache_entries[i].node);
    }
    r->texture_cache.init_node = texture_cache_entry_init;
    r->texture_cache.compare_nodes = texture_cache_entry_compare;
    r->texture_cache.pre_node_evict = texture_cache_entry_pre_evict;
    r->texture_cache.post_node_evict = texture_cache_entry_post_evict;
}

static void texture_cache_finalize(PGRAPHVkState *r)
{
    lru_flush(&r->texture_cache);
    g_free(r->texture_cache_entries);
    r->texture_cache_entries = NULL;
}

void pgraph_vk_trim_texture_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Allow specifying some amount to trim by

    int num_to_evict = r->texture_cache.num_used / 4;
    int num_evicted = 0;

    while (num_to_evict-- && lru_try_evict_one(&r->texture_cache)) {
        num_evicted += 1;
    }

    NV2A_VK_DPRINTF("Evicted %d textures, %d remain", num_evicted, r->texture_cache.num_used);
}

void pgraph_vk_init_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    texture_cache_init(r);
    create_dummy_texture(pg);

    r->texture_format_properties = g_malloc0_n(
        ARRAY_SIZE(kelvin_color_format_vk_map), sizeof(VkFormatProperties));
    for (int i = 0; i < ARRAY_SIZE(kelvin_color_format_vk_map); i++) {
        vkGetPhysicalDeviceFormatProperties(
            r->physical_device, kelvin_color_format_vk_map[i].vk_format,
            &r->texture_format_properties[i]);
    }
}

void pgraph_vk_finalize_textures(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_command_buffer);

    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        r->texture_bindings[i] = NULL;
    }

    destroy_dummy_texture(r);
    texture_cache_finalize(r);

    assert(r->texture_cache.num_used == 0);

    g_free(r->texture_format_properties);
    r->texture_format_properties = NULL;
}
