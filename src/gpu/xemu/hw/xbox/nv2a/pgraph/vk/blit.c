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

#include "hw/xbox/nv2a/nv2a_int.h"
#include "renderer.h"

void pgraph_vk_image_blit(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    ContextSurfaces2DState *context_surfaces = &pg->context_surfaces_2d;
    ImageBlitState *image_blit = &pg->image_blit;
    BetaState *beta = &pg->beta;

    pgraph_vk_surface_update(d, false, true, true);

    assert(context_surfaces->object_instance == image_blit->context_surfaces);

    unsigned int bytes_per_pixel;
    switch (context_surfaces->color_format) {
        case NV062_SET_COLOR_FORMAT_LE_Y8:
            bytes_per_pixel = 1;
            break;
        case NV062_SET_COLOR_FORMAT_LE_R5G6B5:
            bytes_per_pixel = 2;
            break;
        case NV062_SET_COLOR_FORMAT_LE_A8R8G8B8:
        case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
        case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
        case NV062_SET_COLOR_FORMAT_LE_Y32:
            bytes_per_pixel = 4;
            break;
        default:
            fprintf(stderr, "Unknown blit surface format: 0x%x\n",
                    context_surfaces->color_format);
            assert(false);
            break;
    }

    hwaddr source_dma_len, dest_dma_len;

    uint8_t *source = (uint8_t *)nv_dma_map(
        d, context_surfaces->dma_image_source, &source_dma_len);
    assert(context_surfaces->source_offset < source_dma_len);
    source += context_surfaces->source_offset;

    uint8_t *dest = (uint8_t *)nv_dma_map(d, context_surfaces->dma_image_dest,
                                          &dest_dma_len);
    assert(context_surfaces->dest_offset < dest_dma_len);
    dest += context_surfaces->dest_offset;

    hwaddr source_addr = source - d->vram_ptr;
    hwaddr dest_addr = dest - d->vram_ptr;

    /* RECOMP_VK_BLIT_TRACE=1 (bloom ghost diagnosis, docs Update 652): the
     * first 200 image blits with their rectangles and pitches. */
    {
        static int trace = -1, traced;
        if (trace < 0) {
            const char *e = getenv("RECOMP_VK_BLIT_TRACE");
            trace = e && e[0] == '1';
        }
        extern volatile int g_nv2a_draw_trace;
        if (trace && (traced++ < 200 || g_nv2a_draw_trace)) {
            fprintf(stderr, "[VK-BLIT] frame=%d src=%08llX dst=%08llX "
                    "in=%u,%u out=%u,%u size=%ux%u pitch=%u/%u fmt=%X op=%X\n",
                    pg->frame_time, (unsigned long long)source_addr,
                    (unsigned long long)dest_addr, image_blit->in_x,
                    image_blit->in_y, image_blit->out_x, image_blit->out_y,
                    image_blit->width, image_blit->height,
                    context_surfaces->source_pitch,
                    context_surfaces->dest_pitch, context_surfaces->color_format,
                    image_blit->operation);
        }
    }

    /* Diagnostic only: the source readback precedes the GPU-copy decision.
     * Attribute sampled SURFACE_DOWN waits without changing blit semantics. */
    static int trace_blit_download = -1;
    if (trace_blit_download < 0) {
        const char *e = getenv("RECOMP_VK_SURF_WAIT_PROBE");
        trace_blit_download = e && e[0] == '1';
    }
    bool trace_blit_sample = trace_blit_download && pg->frame_time >= 120 &&
                             pg->frame_time % 30 == 0;
    SurfaceBinding *surf_src = pgraph_vk_surface_get(d, source_addr);
    hwaddr source_offset = image_blit->in_y * context_surfaces->source_pitch +
                           image_blit->in_x * bytes_per_pixel;
    hwaddr dest_offset = image_blit->out_y * context_surfaces->dest_pitch +
                         image_blit->out_x * bytes_per_pixel;
    hwaddr source_size =
        (image_blit->height - 1) * context_surfaces->source_pitch +
        image_blit->width * bytes_per_pixel;
    hwaddr dest_size = (image_blit->height - 1) * context_surfaces->dest_pitch +
                       image_blit->width * bytes_per_pixel;

    /* Only full-frame, disjoint, tracked A8R8G8B8 surfaces can defer
     * guest-RAM materialization. The first trial hard-coded one level's
     * addresses; the semantic guard also covers equivalent level layouts.
     * Still opt-in until image and performance A/B have been validated. */
    static int lazy_blit_opt_in = -1;
    if (lazy_blit_opt_in < 0) {
        const char *setting = getenv("RECOMP_VK_SCDA_LAZY_BLIT");
        lazy_blit_opt_in = setting && setting[0] == '1';
    }
    SurfaceBinding *lazy_dest = lazy_blit_opt_in ?
        pgraph_vk_surface_get(d, dest_addr) : NULL;
    bool lazy_copy = lazy_blit_opt_in &&
                     pgraph_vk_scale_fix_enabled(pg) && surf_src &&
                     surf_src->access_cb && surf_src->draw_dirty &&
                     surf_src->color && !surf_src->swizzle &&
                     surf_src->initialized && !surf_src->upload_pending &&
                     surf_src->shape.color_format ==
                         NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 &&
                     context_surfaces->color_format ==
                         NV062_SET_COLOR_FORMAT_LE_A8R8G8B8 &&
                     image_blit->operation == NV09F_SET_OPERATION_SRCCOPY &&
                     !source_offset && !dest_offset &&
                     image_blit->width == surf_src->width &&
                     image_blit->height == surf_src->height &&
                     context_surfaces->source_pitch == image_blit->width * 4 &&
                     context_surfaces->dest_pitch ==
                         context_surfaces->source_pitch &&
                     source_addr + surf_src->size <= dest_addr &&
                     lazy_dest && lazy_dest->access_cb && lazy_dest->color &&
                     !lazy_dest->swizzle &&
                     lazy_dest->width == surf_src->width &&
                     lazy_dest->height == surf_src->height &&
                     lazy_dest->pitch == surf_src->pitch &&
                     lazy_dest->host_fmt.vk_format ==
                         surf_src->host_fmt.vk_format;
    if (surf_src) {
        if (trace_blit_sample && surf_src->draw_dirty && !lazy_copy) {
            fprintf(stderr,
                    "[VK-BLIT-DOWNLOAD] frame=%d role=src "
                    "surface=%08llX %ux%u src=%08llX dst=%08llX "
                    "rect=%ux%u in=%u,%u out=%u,%u op=%X fmt=%X "
                    "pitch=%u,%u scale_fix=%d noop=%d upload=%d\n",
                    pg->frame_time,
                    (unsigned long long)surf_src->vram_addr,
                    surf_src->width, surf_src->height,
                    (unsigned long long)source_addr,
                    (unsigned long long)dest_addr,
                    image_blit->width, image_blit->height,
                    image_blit->in_x, image_blit->in_y,
                    image_blit->out_x, image_blit->out_y,
                    image_blit->operation, context_surfaces->color_format,
                    context_surfaces->source_pitch,
                    context_surfaces->dest_pitch,
                    pgraph_vk_scale_fix_enabled(pg),
                    pgraph_vk_scale_fix_enabled(pg) &&
                    image_blit->operation == NV09F_SET_OPERATION_SRCCOPY &&
                    context_surfaces->color_format ==
                        NV062_SET_COLOR_FORMAT_LE_A8R8G8B8 &&
                    source + image_blit->in_y *
                                 context_surfaces->source_pitch +
                             image_blit->in_x * bytes_per_pixel ==
                        dest + image_blit->out_y *
                                   context_surfaces->dest_pitch +
                               image_blit->out_x * bytes_per_pixel &&
                    context_surfaces->source_pitch ==
                        context_surfaces->dest_pitch,
                    surf_src->upload_pending);
        }
        if (!lazy_copy) {
            pgraph_vk_surface_download_if_dirty(d, surf_src);
        }
    }

    /* xboxrecomp: render-scale fixes of the xemu #949 patch v3 (docs Update
     * 486, see pgraph_vk_scale_fix_enabled). */
    bool scale_fix = pgraph_vk_scale_fix_enabled(pg);
    if (scale_fix && image_blit->operation == NV09F_SET_OPERATION_SRCCOPY &&
        context_surfaces->color_format == NV062_SET_COLOR_FORMAT_LE_A8R8G8B8 &&
        source + source_offset == dest + dest_offset &&
        context_surfaces->source_pitch == context_surfaces->dest_pitch) {
        // Keep the scaled surface; the guest RAM copy was synchronized above.
        return;
    }

    bool gpu_copy = scale_fix && surf_src &&
                    surf_src->color && !surf_src->swizzle &&
                    surf_src->initialized && !surf_src->upload_pending &&
                    surf_src->shape.color_format ==
                        NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 &&
                    context_surfaces->color_format ==
                        NV062_SET_COLOR_FORMAT_LE_A8R8G8B8 &&
                    image_blit->operation == NV09F_SET_OPERATION_SRCCOPY &&
                    !source_offset && !dest_offset &&
                    image_blit->width == surf_src->width &&
                    image_blit->height == surf_src->height &&
                    context_surfaces->source_pitch == image_blit->width * 4 &&
                    context_surfaces->dest_pitch ==
                        context_surfaces->source_pitch &&
                    (dest_addr + dest_size <= source_addr ||
                     source_addr + surf_src->size <= dest_addr);

    SurfaceBinding *surf_dest = lazy_dest ? lazy_dest :
                                pgraph_vk_surface_get(d, dest_addr);
    if (gpu_copy && (!surf_dest || !surf_dest->color)) {
        surf_dest = pgraph_vk_surface_create_blit_dest(d, surf_src, dest_addr);
        gpu_copy = surf_dest != NULL;
    }
    if (gpu_copy && surf_dest) {
        gpu_copy = surf_dest->color && !surf_dest->swizzle &&
                   surf_dest->width == surf_src->width &&
                   surf_dest->height == surf_src->height &&
                   surf_dest->pitch == surf_src->pitch &&
                   surf_dest->host_fmt.vk_format ==
                       surf_src->host_fmt.vk_format;
    }
    if (lazy_copy && !gpu_copy) {
        /* No GPU replacement means the original CPU copy is required. */
        pgraph_vk_surface_download_if_dirty(d, surf_src);
        lazy_copy = false;
    }
    if (surf_dest) {
        if (image_blit->height < surf_dest->height ||
            image_blit->width < surf_dest->width) {
            if (trace_blit_sample && surf_dest->draw_dirty) {
                fprintf(stderr,
                        "[VK-BLIT-DOWNLOAD] frame=%d role=dst "
                        "surface=%08llX %ux%u src=%08llX dst=%08llX "
                        "rect=%ux%u op=%X fmt=%X gpu_copy=%d\n",
                        pg->frame_time,
                        (unsigned long long)surf_dest->vram_addr,
                        surf_dest->width, surf_dest->height,
                        (unsigned long long)source_addr,
                        (unsigned long long)dest_addr,
                        image_blit->width, image_blit->height,
                        image_blit->operation,
                        context_surfaces->color_format, gpu_copy);
            }
            pgraph_vk_surface_download_if_dirty(d, surf_dest);
        } else {
            // The blit will completely replace the surface so any pending
            // download should be discarded.
            surf_dest->download_pending = false;
            surf_dest->draw_dirty = false;
        }
        surf_dest->upload_pending = true;
        pg->draw_time++;
    }

    /* FIXME: What does hardware do in this case? */
    assert(source_addr + source_offset + source_size <=
           memory_region_size(d->vram));
    assert(dest_addr + dest_offset + dest_size <= memory_region_size(d->vram));

    uint8_t *source_row = source + source_offset;
    uint8_t *dest_row = dest + dest_offset;

    if (lazy_copy) {
        /* The tracked destination will materialize its 1x RAM image on a
         * future CPU read or RAM-backed texture use. */
        assert(gpu_copy && surf_dest);
    } else if (image_blit->operation == NV09F_SET_OPERATION_SRCCOPY) {
        // NV2A_GL_DPRINTF(false, "NV09F_SET_OPERATION_SRCCOPY");
        for (unsigned int y = 0; y < image_blit->height; y++) {
            memmove(dest_row, source_row, image_blit->width * bytes_per_pixel);
            source_row += context_surfaces->source_pitch;
            dest_row += context_surfaces->dest_pitch;
        }
    } else if (image_blit->operation == NV09F_SET_OPERATION_BLEND_AND) {
        // NV2A_GL_DPRINTF(false, "NV09F_SET_OPERATION_BLEND_AND");
        uint32_t max_beta_mult = 0x7f80;
        uint32_t beta_mult = beta->beta >> 16;
        uint32_t inv_beta_mult = max_beta_mult - beta_mult;
        for (unsigned int y = 0; y < image_blit->height; y++) {
            for (unsigned int x = 0; x < image_blit->width; x++) {
                for (unsigned int ch = 0; ch < 3; ch++) {
                    uint32_t a = source_row[x * 4 + ch] * beta_mult;
                    uint32_t b = dest_row[x * 4 + ch] * inv_beta_mult;
                    dest_row[x * 4 + ch] = (a + b) / max_beta_mult;
                }
            }
            source_row += context_surfaces->source_pitch;
            dest_row += context_surfaces->dest_pitch;
        }
    } else {
        fprintf(stderr, "Unknown blit operation: 0x%x\n",
                image_blit->operation);
        assert(false && "Unknown blit operation");
    }

    NV2A_DPRINTF("  - 0x%tx -> 0x%tx\n", source_addr, dest_addr);

    bool needs_alpha_patching;
    uint8_t alpha_override;
    switch (context_surfaces->color_format) {
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8:
        needs_alpha_patching = true;
        alpha_override = 0xff;
        break;
    case NV062_SET_COLOR_FORMAT_LE_X8R8G8B8_Z8R8G8B8:
        needs_alpha_patching = true;
        alpha_override = 0;
        break;
    default:
        needs_alpha_patching = false;
        alpha_override = 0;
    }

    if (needs_alpha_patching) {
        dest_row = dest + dest_offset;
        for (unsigned int y = 0; y < image_blit->height; y++) {
            for (unsigned int x = 0; x < image_blit->width; x++) {
                dest_row[x * 4 + 3] = alpha_override;
            }
            dest_row += context_surfaces->dest_pitch;
        }
    }

    dest_addr += dest_offset;
    memory_region_set_client_dirty(d->vram, dest_addr, dest_size,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, dest_addr, dest_size,
                                   DIRTY_MEMORY_NV2A_TEX);

    if (gpu_copy) {
        pgraph_vk_surface_copy_blit(d, surf_src, surf_dest, !lazy_copy);
    }
    if (trace_blit_sample && (gpu_copy || lazy_copy)) {
        fprintf(stderr,
                "[VK-BLIT-RESULT] frame=%d src=%08llX dst=%08llX "
                "gpu_copy=%d lazy=%d dest_surface=%d initialized=%d "
                "upload=%d dirty=%d pending=%d\n",
                pg->frame_time,
                (unsigned long long)source_addr,
                (unsigned long long)dest_addr,
                gpu_copy, lazy_copy, surf_dest != NULL,
                surf_dest ? surf_dest->initialized : 0,
                surf_dest ? surf_dest->upload_pending : 0,
                surf_dest ? surf_dest->draw_dirty : 0,
                surf_dest ? surf_dest->download_pending : 0);
    }
}
