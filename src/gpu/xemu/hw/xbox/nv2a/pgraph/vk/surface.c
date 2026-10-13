/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024-2025 Matt Borgerson
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
#include "hw/xbox/nv2a/pgraph/swizzle.h"
#include "qemu/compiler.h"
#include "ui/xemu-settings.h"
#include "renderer.h"

const int num_invalid_surfaces_to_keep = 10;  // FIXME: Make automatic
const int max_surface_frame_time_delta = 5;

/* Diagnostic only: identify why a dirty surface is read back.  This does
 * not alter synchronization; enable only for a bounded SCDA capture. */
static void trace_surface_readback_reason(PGRAPHState *pg,
                                          const SurfaceBinding *surface,
                                          const char *reason)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_VK_TRACE_EVICT");
        enabled = e && e[0] == '1';
    }
    if (enabled && qatomic_read(&surface->draw_dirty)) {
        fprintf(stderr,
                "[VK-EVICT] frame=%d reason=%s addr=%08llX %ux%u "
                "age=%d tracked=%d color=%d fmt=0x%X\n",
                pg->frame_time, reason,
                (unsigned long long)surface->vram_addr,
                surface->width, surface->height,
                pg->frame_time - surface->frame_time,
                surface->access_cb != NULL, surface->color,
                surface->color ? surface->shape.color_format :
                                 surface->shape.zeta_format);
    }
}

/* Diagnostic only: attribute the remaining SURFACE_DOWN submits to their
 * surface and call site.  Sample one frame in 30 to keep logging overhead
 * outside the measured steady-state profile.  This does not skip a wait. */
static void finish_surface_down_with_probe(PGRAPHState *pg,
                                           const SurfaceBinding *surface,
                                           const char *cause)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_VK_SURF_WAIT_PROBE");
        enabled = e && e[0] == '1';
    }
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool sample = enabled && pg->frame_time >= 120 &&
                  pg->frame_time % 30 == 0;
    bool was_open = r->in_command_buffer;
    int64_t start = sample ? nv2a_recomp_vk_now_us() : 0;
    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_DOWN);
    if (sample) {
        fprintf(stderr,
                "[VK-SURF-WAIT] frame=%d cause=%s addr=%08llX "
                "size=%ux%u color=%d open_main=%d draw_time=%llu "
                "cmd_start=%llu wall_us=%lld\n",
                pg->frame_time, cause,
                (unsigned long long)surface->vram_addr,
                surface->width, surface->height, surface->color,
                was_open, (unsigned long long)surface->draw_time,
                (unsigned long long)r->command_buffer_start_time,
                (long long)(nv2a_recomp_vk_now_us() - start));
    }
}

static void trace_surface_download_source(PGRAPHState *pg,
                                          const SurfaceBinding *surface,
                                          const char *source)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_VK_SURF_WAIT_PROBE");
        enabled = e && e[0] == '1';
    }
    bool will_download = !strcmp(source, "pending") ?
                         surface->download_pending : surface->draw_dirty;
    if (enabled && will_download && pg->frame_time >= 120 &&
        pg->frame_time % 30 == 0) {
        fprintf(stderr,
                "[VK-DOWNLOAD-SOURCE] frame=%d source=%s addr=%08llX "
                "size=%ux%u color=%d dirty=%d pending=%d\n",
                pg->frame_time, source,
                (unsigned long long)surface->vram_addr,
                surface->width, surface->height, surface->color,
                surface->draw_dirty, surface->download_pending);
    }
}

/* Opt-in diagnostic: one frame in 30, including aux-submit/queue idle and
 * host copies.  A finish-only sample omits most of the transfer path. */
static bool sample_surface_total(PGRAPHState *pg)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_VK_SURF_TOTAL_PROBE");
        enabled = e && e[0] == '1';
    }
    return enabled && pg->frame_time >= 120 && pg->frame_time % 30 == 0;
}

static void swizzle_downloaded_surface(PGRAPHState *pg, const uint8_t *linear,
                                        SurfaceBinding *surface,
                                        uint8_t *guest_pixels)
{
    static int timing_enabled = -1;
    static uint64_t count, total_us, max_us;
    if (timing_enabled < 0) {
        const char *e = getenv("RECOMP_VK_STAGE_TIMING");
        timing_enabled = e && e[0] == '1';
    }
    int64_t start = timing_enabled ? nv2a_recomp_vk_now_us() : 0;
    swizzle_rect(linear, surface->width, surface->height, guest_pixels,
                 surface->pitch, surface->fmt.bytes_per_pixel);
    if (timing_enabled) {
        uint64_t elapsed = (uint64_t)(nv2a_recomp_vk_now_us() - start);
        count++;
        total_us += elapsed;
        max_us = MAX(max_us, elapsed);
        if (count == 330) {
            fprintf(stderr, "[VK-SWIZZLE] frame=%d calls=%llu total_ms=%.3f "
                    "avg_us=%.1f max_us=%llu\n", pg->frame_time,
                    (unsigned long long)count, total_us / 1000.0,
                    (double)total_us / count, (unsigned long long)max_us);
            count = total_us = max_us = 0;
        }
    }
}

/* A tracked surface can remain GPU-resident until it is reused, overlaps a
 * newer surface, or the guest CPU touches it (the access callback then
 * synchronizes it).  The upstream five-frame expiry still applies to
 * untracked surfaces.  Opt-in while validating the longer residency against
 * SCDA's aliased shadow-map/mip-chain workload; default behavior is intact. */
static int tracked_surface_frame_time_delta(void)
{
    static int delta;
    if (!delta) {
        const char *setting = getenv("RECOMP_VK_SURFACE_TTL");
        long requested = setting ? strtol(setting, NULL, 10) : 0;
        delta = requested >= 5 && requested <= 120 ? (int)requested :
                max_surface_frame_time_delta;
        if (delta != max_surface_frame_time_delta)
            fprintf(stderr, "[GPU] tracked Vulkan surface lifetime: %d frames "
                    "(RECOMP_VK_SURFACE_TTL)\n", delta);
    }
    return delta;
}

void pgraph_vk_set_surface_scale_factor(NV2AState *d, unsigned int scale)
{
    g_config.display.quality.surface_scale = scale < 1 ? 1 : scale;

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, true);
    qemu_mutex_unlock(&d->pfifo.lock);

    // FIXME: It's just flush
    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);
    qatomic_set(&d->pgraph.vk_renderer_state->download_dirty_surfaces_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.vk_renderer_state->dirty_surfaces_download_complete);

    qemu_mutex_lock(&d->pgraph.lock);
    qemu_event_reset(&d->pgraph.flush_complete);
    qatomic_set(&d->pgraph.flush_pending, true);
    qemu_mutex_unlock(&d->pgraph.lock);
    qemu_mutex_lock(&d->pfifo.lock);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
    qemu_event_wait(&d->pgraph.flush_complete);

    qemu_mutex_lock(&d->pfifo.lock);
    qatomic_set(&d->pfifo.halt, false);
    pfifo_kick(d);
    qemu_mutex_unlock(&d->pfifo.lock);
}

unsigned int pgraph_vk_get_surface_scale_factor(NV2AState *d)
{
    return d->pgraph.surface_scale_factor; // FIXME: Move internal to renderer
}

void pgraph_vk_reload_surface_scale_factor(PGRAPHState *pg)
{
    int factor = g_config.display.quality.surface_scale;
    pg->surface_scale_factor = MAX(factor, 1);
}

// FIXME: Move to common
static void get_surface_dimensions(PGRAPHState const *pg, unsigned int *width,
                                   unsigned int *height)
{
    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    if (swizzle) {
        *width = 1 << pg->surface_shape.log_width;
        *height = 1 << pg->surface_shape.log_height;
    } else {
        *width = pg->surface_shape.clip_width;
        *height = pg->surface_shape.clip_height;
    }
}

// FIXME: Move to common
static bool framebuffer_dirty(PGRAPHState const *pg)
{
    bool shape_changed = memcmp(&pg->surface_shape, &pg->last_surface_shape,
                                sizeof(SurfaceShape)) != 0;
    if (!shape_changed || (!pg->surface_shape.color_format
            && !pg->surface_shape.zeta_format)) {
        return false;
    }
    return true;
}

static void memcpy_image(void *dst, void const *src, int dst_stride,
                         int src_stride, int height)
{
    if (dst_stride == src_stride) {
        memcpy(dst, src, dst_stride * height);
        return;
    }

    uint8_t *dst_ptr = (uint8_t *)dst;
    uint8_t const *src_ptr = (uint8_t *)src;

    size_t copy_stride = MIN(src_stride, dst_stride);

    for (int i = 0; i < height; i++) {
        memcpy(dst_ptr, src_ptr, copy_stride);
        dst_ptr += dst_stride;
        src_ptr += src_stride;
    }
}

void pgraph_vk_download_surfaces_in_range_if_dirty(PGRAPHState *pg, hwaddr start,
                                                   hwaddr size, const char *reason)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    SurfaceBinding *surface;

    hwaddr end = start + size - 1;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        hwaddr surf_end = surface->vram_addr + surface->size - 1;
        bool overlapping = !(surface->vram_addr >= end || start >= surf_end);
        if (overlapping) {
            trace_surface_readback_reason(pg, surface, reason);
            pgraph_vk_surface_download_if_dirty(
                container_of(pg, NV2AState, pgraph), surface);
        }
    }
}

static void download_surface_to_buffer(NV2AState *d, SurfaceBinding *surface,
                                       uint8_t *pixels)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    bool sample_total = sample_surface_total(pg);
    int64_t total_start = sample_total ? nv2a_recomp_vk_now_us() : 0;

    nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
    /* xboxrecomp diagnostic (docs Update 489), RECOMP_VK_TRACE_XFER=1:
     * which surfaces are read back each frame and who asks. */
    nv2a_recomp_vk_trace_xfer("download", surface->vram_addr, surface->width,
                              surface->height,
                              surface->color ? surface->shape.color_format :
                                               surface->shape.zeta_format,
                              surface->color, surface->swizzle);

    bool use_compute_to_convert_depth_stencil_format =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool no_conversion_necessary =
        surface->color || use_compute_to_convert_depth_stencil_format ||
        surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM;

    assert(no_conversion_necessary);

    bool compute_needs_finish = (use_compute_to_convert_depth_stencil_format &&
                                 pgraph_vk_compute_needs_finish(r));

    if (r->in_command_buffer &&
        surface->draw_time >= r->command_buffer_start_time) {
        finish_surface_down_with_probe(pg, surface, "download");
    } else if (compute_needs_finish) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    bool downscale = (pg->surface_scale_factor != 1);

    trace_nv2a_pgraph_surface_download(
        surface->color ? "COLOR" : "ZETA",
        surface->swizzle ? "sz" : "lin", surface->vram_addr,
        surface->width, surface->height, surface->pitch,
        surface->fmt.bytes_per_pixel);

    // Read surface into memory
    uint8_t *gl_read_buf = pixels;

    uint8_t *swizzle_buf = pixels;
    if (surface->swizzle) {
        // FIXME: Swizzle in shader
        assert(pg->surface_scale_factor == 1 || downscale);
        swizzle_buf = (uint8_t *)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
    }

    unsigned int scaled_width = surface->width,
                 scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    int num_copy_regions = 1;
    VkBufferImageCopy copy_regions[2];
    copy_regions[0] = (VkBufferImageCopy){
        .imageSubresource.aspectMask = surface->color ?
                                           VK_IMAGE_ASPECT_COLOR_BIT :
                                           VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.layerCount = 1,
    };

    VkImage surface_image_loc;
    if (downscale && !use_compute_to_convert_depth_stencil_format) {
        copy_regions[0].imageExtent =
            (VkExtent3D){ surface->width, surface->height, 1 };

        if (surface->image_scratch_current_layout !=
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            pgraph_vk_transition_image_layout(
                pg, cmd, surface->image_scratch, surface->host_fmt.vk_format,
                surface->image_scratch_current_layout,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            surface->image_scratch_current_layout =
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }

        VkImageBlit blit_region = {
            .srcSubresource.aspectMask = surface->host_fmt.aspect,
            .srcSubresource.mipLevel = 0,
            .srcSubresource.baseArrayLayer = 0,
            .srcSubresource.layerCount = 1,
            .srcOffsets[0] = (VkOffset3D){0, 0, 0},
            .srcOffsets[1] = (VkOffset3D){scaled_width, scaled_height, 1},

            .dstSubresource.aspectMask = surface->host_fmt.aspect,
            .dstSubresource.mipLevel = 0,
            .dstSubresource.baseArrayLayer = 0,
            .dstSubresource.layerCount = 1,
            .dstOffsets[0] = (VkOffset3D){0, 0, 0},
            .dstOffsets[1] = (VkOffset3D){surface->width, surface->height, 1},
        };

        vkCmdBlitImage(cmd, surface->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       surface->image_scratch,
                       surface->image_scratch_current_layout, 1, &blit_region,
                       surface->color ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

        pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                                          surface->host_fmt.vk_format,
                                          surface->image_scratch_current_layout,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        surface->image_scratch_current_layout =
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        surface_image_loc = surface->image_scratch;
    } else {
        copy_regions[0].imageExtent =
            (VkExtent3D){ scaled_width, scaled_height, 1 };
        surface_image_loc = surface->image;
    }

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        size_t depth_size = scaled_width * scaled_height * 4;
        copy_regions[num_copy_regions++] = (VkBufferImageCopy){
            .bufferOffset = ROUND_UP(
                depth_size,
                r->device_props.limits.minStorageBufferOffsetAlignment),
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 },
        };
    }

    //
    // Copy image to staging buffer, or to compute_dst if we need to pack it
    //

    size_t downloaded_image_size = surface->host_fmt.host_bytes_per_pixel *
                                   surface->width * surface->height;
    assert((downloaded_image_size) <=
           r->storage_buffers[BUFFER_STAGING_DST].buffer_size);

    int copy_buffer_idx = use_compute_to_convert_depth_stencil_format ?
                             BUFFER_COMPUTE_DST :
                             BUFFER_STAGING_DST;
    VkBuffer copy_buffer = r->storage_buffers[copy_buffer_idx].buffer;

    {
        VkBufferMemoryBarrier pre_copy_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_copy_dst_barrier, 0, NULL);
    }
    vkCmdCopyImageToBuffer(cmd, surface_image_loc,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy_buffer,
                           num_copy_regions, copy_regions);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    // FIXME: Verify output of depth stencil conversion
    // FIXME: Track current layout and only transition when required

    if (use_compute_to_convert_depth_stencil_format) {
        size_t bytes_per_pixel = 4;
        size_t packed_size =
            downscale ? (surface->width * surface->height * bytes_per_pixel) :
                        (scaled_width * scaled_height * bytes_per_pixel);

        //
        // Pack the depth-stencil image into compute_src buffer
        //

        VkBufferMemoryBarrier pre_compute_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_compute_src_barrier, 0, NULL);

        VkBuffer pack_buffer = r->storage_buffers[BUFFER_COMPUTE_SRC].buffer;

        VkBufferMemoryBarrier pre_compute_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
                             1, &pre_compute_dst_barrier, 0, NULL);

        pgraph_vk_pack_depth_stencil(pg, surface, cmd, copy_buffer, pack_buffer,
                                     downscale);

        VkBufferMemoryBarrier post_compute_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_compute_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_compute_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = packed_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_compute_dst_barrier, 0, NULL);

        //
        // Copy packed image over to staging buffer for host download
        //

        copy_buffer = r->storage_buffers[BUFFER_STAGING_DST].buffer;

        VkBufferMemoryBarrier pre_copy_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &pre_copy_dst_barrier, 0, NULL);

        VkBufferCopy buffer_copy_region = {
            .size = packed_size,
        };
        vkCmdCopyBuffer(cmd, pack_buffer, copy_buffer, 1, &buffer_copy_region);

        VkBufferMemoryBarrier post_copy_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = pack_buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_copy_src_barrier, 0, NULL);
    }

    //
    // Download image data to host
    //

    VkBufferMemoryBarrier post_copy_dst_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                         &post_copy_dst_barrier, 0, NULL);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_1);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd, VK_AUX_DOWNLOAD);

    void *mapped_memory_ptr = NULL;
    VK_CHECK(vmaMapMemory(r->allocator,
                          r->storage_buffers[BUFFER_STAGING_DST].allocation,
                          &mapped_memory_ptr));

    vmaInvalidateAllocation(r->allocator,
                            r->storage_buffers[BUFFER_STAGING_DST].allocation,
                            0, VK_WHOLE_SIZE);

    memcpy_image(gl_read_buf, mapped_memory_ptr, surface->pitch,
                 surface->width * surface->fmt.bytes_per_pixel,
                 surface->height);

    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_DST].allocation);

    if (surface->swizzle) {
        // FIXME: Swizzle in shader
        swizzle_downloaded_surface(pg, swizzle_buf, surface, pixels);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
        g_free(swizzle_buf);
    }
    if (sample_total) {
        fprintf(stderr,
                "[VK-SURF-TOTAL] frame=%d op=download addr=%08llX "
                "size=%ux%u color=%d swizzle=%d scale=%d wall_us=%lld\n",
                pg->frame_time, (unsigned long long)surface->vram_addr,
                surface->width, surface->height, surface->color,
                surface->swizzle, pg->surface_scale_factor,
                (long long)(nv2a_recomp_vk_now_us() - total_start));
    }
}

static void download_surface(NV2AState *d, SurfaceBinding *surface, bool force)
{
    if (!(surface->download_pending || force)) {
        return;
    }

    // FIXME: Respect write enable at last TOU?

    trace_surface_readback_reason(&d->pgraph, surface, "actual");

    download_surface_to_buffer(d, surface, d->vram_ptr + surface->vram_addr);

    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_VGA);
    memory_region_set_client_dirty(d->vram, surface->vram_addr,
                                   surface->pitch * surface->height,
                                   DIRTY_MEMORY_NV2A_TEX);

    surface->download_pending = false;
    surface->draw_dirty = false;
    /* xboxrecomp: RAM equals the GPU copy again (docs Update 492) */
    nv2a_recomp_access_watch(surface->access_cb, 1);
}

void pgraph_vk_wait_for_surface_download(SurfaceBinding *surface)
{
    NV2AState *d = g_nv2a;

    if (qatomic_read(&surface->draw_dirty)) {
        qemu_mutex_lock(&d->pfifo.lock);
        qemu_event_reset(&d->pgraph.vk_renderer_state->downloads_complete);
        qatomic_set(&surface->download_pending, true);
        qatomic_set(&d->pgraph.vk_renderer_state->downloads_pending, true);
        pfifo_kick(d);
        qemu_mutex_unlock(&d->pfifo.lock);
        qemu_event_wait(&d->pgraph.vk_renderer_state->downloads_complete);
    }
}

void pgraph_vk_process_pending_downloads(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    SurfaceBinding *surface;

    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        trace_surface_download_source(&d->pgraph, surface, "pending");
        download_surface(d, surface, false);
    }

    qatomic_set(&r->downloads_pending, false);
    qemu_event_set(&r->downloads_complete);
}

void pgraph_vk_download_dirty_surfaces(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH(surface, &r->surfaces, entry) {
        trace_surface_download_source(&d->pgraph, surface, "pre-save");
        pgraph_vk_surface_download_if_dirty(d, surface);
    }

    qatomic_set(&r->download_dirty_surfaces_pending, false);
    qemu_event_set(&r->dirty_surfaces_download_complete);
}

static void surface_access_callback(void *opaque, MemoryRegion *mr, hwaddr addr,
                                    hwaddr len, bool write)
{
    SurfaceBinding *e = opaque;
    assert(addr >= e->vram_addr);
    hwaddr offset = addr - e->vram_addr;
    assert(offset < e->size);

    if (qatomic_read(&e->draw_dirty)) {
        trace_nv2a_pgraph_surface_cpu_access(e->vram_addr, offset);
        pgraph_vk_wait_for_surface_download(e);
    }

    if (write && !qatomic_read(&e->upload_pending)) {
        trace_nv2a_pgraph_surface_cpu_access(e->vram_addr, offset);
        qatomic_set(&e->upload_pending, true);
    }
}

static void register_cpu_access_callback(NV2AState *d, SurfaceBinding *surface)
{
    if (tcg_enabled()) {
        qemu_mutex_unlock(&d->pgraph.lock);
        bql_lock();
        mem_access_callback_insert(qemu_get_cpu(0),
            d->vram, surface->vram_addr, surface->size,
            &surface->access_cb, &surface_access_callback,
            surface);
        bql_unlock();
        qemu_mutex_lock(&d->pgraph.lock);
    }
}

static void unregister_cpu_access_callback(NV2AState *d,
                                           SurfaceBinding const *surface)
{
    if (tcg_enabled()) {
        qemu_mutex_unlock(&d->pgraph.lock);
        bql_lock();
        mem_access_callback_remove_by_ref(qemu_get_cpu(0), surface->access_cb);
        bql_unlock();
        qemu_mutex_lock(&d->pgraph.lock);
    }
}

static void bind_surface(PGRAPHVkState *r, SurfaceBinding *surface)
{
    if (surface->color) {
        r->color_binding = surface;
    } else {
        r->zeta_binding = surface;
    }

    r->framebuffer_dirty = true;
}

static void unbind_surface(NV2AState *d, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (color) {
        if (r->color_binding) {
            r->color_binding = NULL;
            r->framebuffer_dirty = true;
        }
    } else {
        if (r->zeta_binding) {
            r->zeta_binding = NULL;
            r->framebuffer_dirty = true;
        }
    }
}

static void invalidate_surface_internal(NV2AState *d, SurfaceBinding *surface,
                                        bool defer_until_finish)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;
    bool deferred = defer_until_finish && r->in_command_buffer;

    trace_nv2a_pgraph_surface_invalidated(surface->vram_addr);

    if (!deferred) {
        // Generic invalidation still waits before an image may be reused.
        finish_surface_down_with_probe(&d->pgraph, surface, "invalidate");
        assert((!r->in_command_buffer ||
                surface->draw_time < r->command_buffer_start_time) &&
               "Surface evicted while in use!");
    }

    if (surface == r->color_binding) {
        assert(d->pgraph.surface_color.buffer_dirty);
        unbind_surface(d, true);
    }
    if (surface == r->zeta_binding) {
        assert(d->pgraph.surface_zeta.buffer_dirty);
        unbind_surface(d, false);
    }

    unregister_cpu_access_callback(d, surface);

    QTAILQ_REMOVE(&r->surfaces, surface, entry);
    if (deferred) {
        /* The old VkImage may still be referenced by the open main command
         * buffer.  Keep it out of the reuse/prune pool until that buffer's
         * fence completes.  No guest-RAM readback is pending for this path. */
        QTAILQ_INSERT_HEAD(&r->deferred_invalid_surfaces, surface, entry);
    } else {
        QTAILQ_INSERT_HEAD(&r->invalid_surfaces, surface, entry);
    }
}

static void invalidate_surface(NV2AState *d, SurfaceBinding *surface)
{
    invalidate_surface_internal(d, surface, false);
}

void pgraph_vk_retire_deferred_invalid_surfaces(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    SurfaceBinding *surface;
    while ((surface = QTAILQ_FIRST(&r->deferred_invalid_surfaces)) != NULL) {
        QTAILQ_REMOVE(&r->deferred_invalid_surfaces, surface, entry);
        QTAILQ_INSERT_HEAD(&r->invalid_surfaces, surface, entry);
    }
}

/* Several disjoint, swizzled A8R8G8B8 mip surfaces may be displaced by one
 * aliased target.  Their readbacks all need to reach guest RAM, but they do
 * not need four separate queue-idle waits.  Keep this path opt-in until the
 * SCDA same-scene image/FPS A/B is complete. */
#define MAX_BATCHED_OVERLAP_SURFACES 8
static bool batch_overlapping_color_downloads(NV2AState *d,
                                               const SurfaceBinding *incoming)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_VK_BATCH_OVERLAP");
        enabled = e && e[0] == '1';
    }
    if (!enabled || pg->surface_scale_factor != 2) {
        return false;
    }

    SurfaceBinding *batch[MAX_BATCHED_OVERLAP_SURFACES];
    VkDeviceSize offsets[MAX_BATCHED_OVERLAP_SURFACES];
    size_t count = 0;
    VkDeviceSize total = 0;
    VkDeviceSize alignment = MAX((VkDeviceSize)4,
        (VkDeviceSize)r->device_props.limits.optimalBufferCopyOffsetAlignment);
    uintptr_t incoming_end = incoming->vram_addr + incoming->size - 1;
    SurfaceBinding *s;
    QTAILQ_FOREACH(s, &r->surfaces, entry) {
        uintptr_t end = s->vram_addr + s->size - 1;
        if (s->vram_addr > incoming_end || incoming->vram_addr > end ||
            !qatomic_read(&s->draw_dirty)) {
            continue;
        }
        if (count == MAX_BATCHED_OVERLAP_SURFACES || !s->color || !s->swizzle ||
            s->shape.color_format != NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8 ||
            s->fmt.bytes_per_pixel != 4 ||
            s->host_fmt.host_bytes_per_pixel != 4 ||
            s->host_fmt.aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
            s->image_scratch == VK_NULL_HANDLE ||
            s->pitch < s->width * 4) {
            return false;
        }
        total = ((total + alignment - 1) / alignment) * alignment;
        offsets[count] = total;
        total += (VkDeviceSize)s->width * s->height * 4;
        if (total > r->storage_buffers[BUFFER_STAGING_DST].buffer_size) {
            return false;
        }
        batch[count++] = s;
    }
    if (count < 2) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        if (r->in_command_buffer &&
            batch[i]->draw_time >= r->command_buffer_start_time) {
            finish_surface_down_with_probe(pg, batch[i], "batch");
            break;
        }
    }

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);
    VkBuffer staging = r->storage_buffers[BUFFER_STAGING_DST].buffer;
    VkBufferMemoryBarrier before = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = staging,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &before, 0, NULL);

    for (size_t i = 0; i < count; i++) {
        s = batch[i];
        trace_surface_readback_reason(pg, s, "actual-batch");
        nv2a_profile_inc_counter(NV2A_PROF_SURF_DOWNLOAD);
        nv2a_recomp_vk_trace_xfer("download", s->vram_addr, s->width,
                                  s->height, s->shape.color_format,
                                  s->color, s->swizzle);
        trace_nv2a_pgraph_surface_download("COLOR", "sz", s->vram_addr,
                                           s->width, s->height, s->pitch, 4);

        unsigned int scaled_width = s->width, scaled_height = s->height;
        pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);
        pgraph_vk_transition_image_layout(pg, cmd, s->image,
            s->host_fmt.vk_format, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (s->image_scratch_current_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            pgraph_vk_transition_image_layout(pg, cmd, s->image_scratch,
                s->host_fmt.vk_format, s->image_scratch_current_layout,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            s->image_scratch_current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        }
        VkImageBlit blit = {
            .srcSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .layerCount = 1 },
            .dstSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                .layerCount = 1 },
            .srcOffsets[1] = { (int32_t)scaled_width,
                               (int32_t)scaled_height, 1 },
            .dstOffsets[1] = { (int32_t)s->width, (int32_t)s->height, 1 },
        };
        vkCmdBlitImage(cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s->image_scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
        pgraph_vk_transition_image_layout(pg, cmd, s->image_scratch,
            s->host_fmt.vk_format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        s->image_scratch_current_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkBufferImageCopy region = {
            .bufferOffset = offsets[i],
            .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .layerCount = 1 },
            .imageExtent = { s->width, s->height, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, s->image_scratch,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               staging, 1, &region);
        pgraph_vk_transition_image_layout(pg, cmd, s->image,
            s->host_fmt.vk_format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    VkBufferMemoryBarrier after = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = staging,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                         &after, 0, NULL);
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_1);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd, VK_AUX_DOWNLOAD);

    void *mapped = NULL;
    VK_CHECK(vmaMapMemory(r->allocator,
        r->storage_buffers[BUFFER_STAGING_DST].allocation, &mapped));
    vmaInvalidateAllocation(r->allocator,
        r->storage_buffers[BUFFER_STAGING_DST].allocation, 0, VK_WHOLE_SIZE);
    for (size_t i = 0; i < count; i++) {
        s = batch[i];
        uint8_t *linear = g_malloc(s->size);
        memcpy_image(linear, (uint8_t *)mapped + offsets[i], s->pitch,
                     s->width * 4, s->height);
        swizzle_downloaded_surface(pg, linear, s,
                                   d->vram_ptr + s->vram_addr);
        g_free(linear);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
        memory_region_set_client_dirty(d->vram, s->vram_addr,
                                       s->pitch * s->height, DIRTY_MEMORY_VGA);
        memory_region_set_client_dirty(d->vram, s->vram_addr,
                                       s->pitch * s->height, DIRTY_MEMORY_NV2A_TEX);
        s->download_pending = false;
        s->draw_dirty = false;
        nv2a_recomp_access_watch(s->access_cb, 1);
    }
    vmaUnmapMemory(r->allocator,
                   r->storage_buffers[BUFFER_STAGING_DST].allocation);
    return true;
}

/* RECOMP_VK_CROSS_TYPE_DISCARD=1 (docs Update 638): when guest memory that
 * held a depth (zeta) surface becomes a colour target, or the other way
 * round, the old contents mean nothing to the new user -- on hardware the
 * new target starts with the other type's bit pattern, which no title can
 * use. SCDA's shadow maps and its bloom chain share 04C99000.. and paid ~5
 * ms of readbacks/uploads per frame for that. Such old surfaces are retired
 * without a readback, and the new target is not filled from guest RAM. */
static bool cross_type_discard_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_VK_CROSS_TYPE_DISCARD");
        enabled = e && e[0] == '1';
    }
    return enabled;
}

/* Guest-RAM ranges whose last owner was retired by a cross-type discard
 * (docs Update 641): RAM there holds stale data, not what the GPU drew.
 * A colour/zeta target created inside such a range within the next frame
 * need not be filled from RAM (that upload was another full GPU wait per
 * bloom mip). Short-lived on purpose: the CPU's later writes into that
 * memory are not tracked once the old surface is gone. */
#define STALE_RANGES 8
static struct { hwaddr addr, end; int frame; } s_stale[STALE_RANGES];

static void note_stale_range(PGRAPHState *pg, const SurfaceBinding *s)
{
    static int next;
    s_stale[next].addr = s->vram_addr;
    s_stale[next].end = s->vram_addr + s->size;
    s_stale[next].frame = pg->frame_time;
    next = (next + 1) % STALE_RANGES;
}

static bool target_in_stale_range(PGRAPHState *pg, const SurfaceBinding *t)
{
    for (int i = 0; i < STALE_RANGES; i++) {
        if (s_stale[i].end && pg->frame_time - s_stale[i].frame <= 1 &&
            t->vram_addr >= s_stale[i].addr &&
            t->vram_addr + t->size <= s_stale[i].end) {
            return true;
        }
    }
    return false;
}

static void invalidate_overlapping_surfaces(NV2AState *d,
                                            SurfaceBinding const *surface)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    batch_overlapping_color_downloads(d, surface);

    uintptr_t e_end = surface->vram_addr + surface->size - 1;

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        uintptr_t s_end = s->vram_addr + s->size - 1;
        bool overlapping =
            !(s->vram_addr > e_end || surface->vram_addr > s_end);
        if (overlapping) {
            trace_nv2a_pgraph_surface_evict_overlapping(
                s->vram_addr, s->width, s->height,
                s->pitch);
            trace_surface_readback_reason(&d->pgraph, s, "overlap");
            if (cross_type_discard_enabled() && s->color != surface->color) {
                /* No readback, and no full GPU wait to retire the image:
                 * it is released after the open command buffer's fence. */
                note_stale_range(&d->pgraph, s);
                invalidate_surface_internal(d, s, true);
            } else {
                pgraph_vk_surface_download_if_dirty(d, s);
                invalidate_surface(d, s);
            }
        }
    }
}

static void surface_put(NV2AState *d, SurfaceBinding *surface)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    assert(pgraph_vk_surface_get(d, surface->vram_addr) == NULL);

    invalidate_overlapping_surfaces(d, surface);
    register_cpu_access_callback(d, surface);

    QTAILQ_INSERT_HEAD(&r->surfaces, surface, entry);
}

/* xboxrecomp: the render-scale fixes of the xemu #949 patch v3
 * (r3538987, github.com/xemu-project/xemu/issues/949, 2026-09-21; docs
 * Update 486), active when rendering above 1x. RECOMP_VK_SCALE_FIX=0 turns
 * them off for comparison. */
bool pgraph_vk_scale_fix_enabled(PGRAPHState *pg)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_VK_SCALE_FIX");
        enabled = !(e && e[0] == '0');
    }
    return enabled && pg->surface_scale_factor > 1;
}

static void create_surface_image(PGRAPHState *pg, SurfaceBinding *surface);

/* A full-size SRCCOPY blit between two A8R8G8B8 surfaces at render scale
 * > 1: give the destination its own scaled surface so the copy can stay on
 * the GPU instead of going through the 1x guest-RAM copy (pixelation). */
SurfaceBinding *pgraph_vk_surface_create_blit_dest(NV2AState *d,
                                                   SurfaceBinding *source,
                                                   hwaddr dest_addr)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    SurfaceBinding *other;

    SurfaceBinding *existing = pgraph_vk_surface_get(d, dest_addr);
    if (existing && !existing->color && !existing->swizzle &&
        existing->vram_addr == dest_addr && existing->size == source->size &&
        existing->width == source->width &&
        existing->height == source->height &&
        existing->pitch == source->pitch) {
        existing->download_pending = false;
        existing->draw_dirty = false;
        if (existing == r->zeta_binding) {
            pg->surface_zeta.buffer_dirty = true;
        }
        invalidate_surface(d, existing);
    }

    QTAILQ_FOREACH(other, &r->surfaces, entry) {
        if (dest_addr < other->vram_addr + other->size &&
            other->vram_addr < dest_addr + source->size) {
            return NULL;
        }
    }

    SurfaceBinding *dest = g_new0(SurfaceBinding, 1);

    dest->vram_addr = dest_addr;
    dest->shape = source->shape;
    dest->dma_addr = dest_addr;
    dest->dma_len = source->size;
    dest->color = true;
    dest->width = source->width;
    dest->height = source->height;
    dest->pitch = source->pitch;
    dest->size = source->size;
    dest->fmt = source->fmt;
    dest->host_fmt = source->host_fmt;
    dest->frame_time = pg->frame_time;

    create_surface_image(pg, dest);
    surface_put(d, dest);
    return dest;
}

void pgraph_vk_surface_copy_blit(NV2AState *d, SurfaceBinding *source,
                                 SurfaceBinding *dest, bool ram_current)
{
    PGRAPHState *pg = &d->pgraph;
    VkCommandBuffer cmd = pgraph_vk_begin_nondraw_commands(pg);
    VkFormat format = source->host_fmt.vk_format;

    pgraph_vk_transition_image_layout(pg, cmd, source->image, format,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    pgraph_vk_transition_image_layout(pg, cmd, dest->image, format,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageCopy region = {
        .srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .srcSubresource.layerCount = 1,
        .dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .dstSubresource.layerCount = 1,
        .extent.width = source->width * pg->surface_scale_factor,
        .extent.height = source->height * pg->surface_scale_factor,
        .extent.depth = 1,
    };
    vkCmdCopyImage(cmd, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dest->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region);

    pgraph_vk_transition_image_layout(pg, cmd, source->image, format,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    pgraph_vk_transition_image_layout(pg, cmd, dest->image, format,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    pgraph_vk_end_nondraw_commands(pg, cmd);

    dest->initialized = true;
    dest->upload_pending = false;
    dest->download_pending = false;
    dest->draw_dirty = !ram_current;
    dest->frame_time = pg->frame_time;
    dest->draw_time = pg->draw_time;
    /* If the CPU copy was deferred, protect reads as well as writes.  A
     * guest CPU read then materializes the 1x image through the existing
     * surface_access_callback / pending-download path. */
    nv2a_recomp_access_watch(dest->access_cb, ram_current ? 1 : 2);

    /* One-shot diagnostic: verify that a deferred destination readback
     * would reproduce the bytes currently written by the CPU blit.  Keep
     * the normal copy and surface state unchanged; never use this probe as
     * an implicit optimization switch. */
    static int blit_equal_probe = -1;
    static bool blit_equal_sampled;
    if (blit_equal_probe < 0) {
        const char *setting = getenv("RECOMP_VK_BLIT_EQ_PROBE");
        blit_equal_probe = setting && setting[0] == '1';
    }
    if (blit_equal_probe && ram_current && !blit_equal_sampled &&
        pg->frame_time >= 120 &&
        ((source->vram_addr == 0x04914000 &&
          dest->vram_addr == 0x04DD9000) ||
         (source->vram_addr == 0x0496C000 &&
          dest->vram_addr == 0x04E31000))) {
        blit_equal_sampled = true;
        uint8_t *readback = g_malloc(dest->size);
        download_surface_to_buffer(d, dest, readback);
        const uint8_t *ram = d->vram_ptr + dest->vram_addr;
        size_t first = dest->size;
        size_t mismatches = 0;
        for (size_t i = 0; i < dest->size; i++) {
            if (readback[i] != ram[i]) {
                if (first == dest->size) {
                    first = i;
                }
                mismatches++;
            }
        }
        fprintf(stderr,
                "[VK-BLIT-EQUALITY] frame=%d src=%08llX dst=%08llX "
                "bytes=%zu mismatches=%zu first=%zu gpu=%02X ram=%02X\n",
                pg->frame_time,
                (unsigned long long)source->vram_addr,
                (unsigned long long)dest->vram_addr,
                dest->size, mismatches, first,
                first < dest->size ? readback[first] : 0,
                first < dest->size ? ram[first] : 0);
        g_free(readback);
    }
}

SurfaceBinding *pgraph_vk_surface_get(NV2AState *d, hwaddr addr)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (surface->vram_addr == addr) {
            return surface;
        }
    }

    return NULL;
}

SurfaceBinding *pgraph_vk_surface_get_within(NV2AState *d, hwaddr addr)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *surface;
    QTAILQ_FOREACH (surface, &r->surfaces, entry) {
        if (addr >= surface->vram_addr &&
            addr < (surface->vram_addr + surface->size)) {
            return surface;
        }
    }

    return NULL;
}

static void set_surface_label(PGRAPHState *pg, SurfaceBinding const *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    /* RECOMP_VK_SURF_CREATE_TRACE=1 (bloom ghost diagnosis, docs Update
     * 652): the first 300 created surfaces with size, AA mode and layout. */
    {
        static int trace = -1, traced;
        if (trace < 0) {
            const char *e = getenv("RECOMP_VK_SURF_CREATE_TRACE");
            trace = e && e[0] == '1';
        }
        if (trace && traced++ < 300) {
            fprintf(stderr, "[VK-SURF-NEW] frame=%d addr=%08llX %s fmt=%02X "
                    "%ux%u pitch=%u aa=%u %s\n", pg->frame_time,
                    (unsigned long long)surface->vram_addr,
                    surface->color ? "color" : "zeta",
                    surface->color ? surface->shape.color_format :
                                     surface->shape.zeta_format,
                    surface->width, surface->height, surface->pitch,
                    surface->shape.anti_aliasing,
                    surface->swizzle ? "swizzled" : "linear");
        }
    }

    g_autofree gchar *label = g_strdup_printf(
        "Surface %" HWADDR_PRIx "h fmt:%s,%02xh %dx%d aa:%d",
        surface->vram_addr, surface->color ? "Color" : "Zeta",
        surface->color ? surface->shape.color_format :
                         surface->shape.zeta_format,
        surface->width, surface->height, pg->surface_shape.anti_aliasing);

    VkDebugUtilsObjectNameInfoEXT name_info = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = VK_OBJECT_TYPE_IMAGE,
        .objectHandle = (uint64_t)surface->image,
        .pObjectName = label,
    };

    if (r->debug_utils_extension_enabled) {
        vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
    }
    vmaSetAllocationName(r->allocator, surface->allocation, label);

    if (surface->image_scratch) {
        g_autofree gchar *label_scratch =
            g_strdup_printf("%s (scratch)", label);
        name_info.objectHandle = (uint64_t)surface->image_scratch;
        name_info.pObjectName = label_scratch;
        if (r->debug_utils_extension_enabled) {
            vkSetDebugUtilsObjectNameEXT(r->device, &name_info);
        }
        vmaSetAllocationName(r->allocator, surface->allocation_scratch,
                             label_scratch);
        g_free(label_scratch); /* xboxrecomp: g_autofree is a no-op */
    }
    g_free(label); /* xboxrecomp: g_autofree is a no-op under MSVC */
}

static void create_surface_image(PGRAPHState *pg, SurfaceBinding *surface)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int width = surface->width, height = surface->height;
    pgraph_apply_scaling_factor(pg, &width, &height);

    assert(!surface->image);
    assert(!surface->image_scratch);

    NV2A_VK_DPRINTF(
        "Creating new surface image width=%d height=%d @ %08" HWADDR_PRIx,
        width, height, surface->vram_addr);

    VkImageCreateInfo image_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.width = width,
        .extent.height = height,
        .extent.depth = 1,
        .mipLevels = 1,
        .arrayLayers = 1,
        .format = surface->host_fmt.vk_format,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | surface->host_fmt.usage,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    VmaAllocationCreateInfo alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &surface->image,
                            &surface->allocation, NULL));

    VK_CHECK(vmaCreateImage(r->allocator, &image_create_info,
                            &alloc_create_info, &surface->image_scratch,
                            &surface->allocation_scratch, NULL));
    surface->image_scratch_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewCreateInfo image_view_create_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = surface->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = surface->host_fmt.vk_format,
        .subresourceRange.aspectMask = surface->host_fmt.aspect,
        .subresourceRange.levelCount = 1,
        .subresourceRange.layerCount = 1,
    };
    VK_CHECK(vkCreateImageView(r->device, &image_view_create_info, NULL,
                               &surface->image_view));

    // FIXME: Go right into main command buffer
    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_UNDEFINED,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_3);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd, VK_AUX_OTHER);
    nv2a_profile_inc_counter(NV2A_PROF_SURF_CREATE);
}

static void migrate_surface_image(SurfaceBinding *dst, SurfaceBinding *src)
{
    dst->image = src->image;
    dst->image_view = src->image_view;
    dst->allocation = src->allocation;
    dst->image_scratch = src->image_scratch;
    dst->image_scratch_current_layout = src->image_scratch_current_layout;
    dst->allocation_scratch = src->allocation_scratch;

    src->image = VK_NULL_HANDLE;
    src->image_view = VK_NULL_HANDLE;
    src->allocation = VK_NULL_HANDLE;
    src->image_scratch = VK_NULL_HANDLE;
    src->image_scratch_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    src->allocation_scratch = VK_NULL_HANDLE;
}

static void destroy_surface_image(PGRAPHVkState *r, SurfaceBinding *surface)
{
    vkDestroyImageView(r->device, surface->image_view, NULL);
    surface->image_view = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, surface->image, surface->allocation);
    surface->image = VK_NULL_HANDLE;
    surface->allocation = VK_NULL_HANDLE;

    vmaDestroyImage(r->allocator, surface->image_scratch,
                    surface->allocation_scratch);
    surface->image_scratch = VK_NULL_HANDLE;
    surface->allocation_scratch = VK_NULL_HANDLE;
}

static bool check_invalid_surface_is_compatibile(SurfaceBinding *surface,
                                                 SurfaceBinding *target)
{
    return surface->host_fmt.vk_format == target->host_fmt.vk_format &&
           surface->width == target->width &&
           surface->height == target->height &&
           surface->host_fmt.usage == target->host_fmt.usage;
}

static SurfaceBinding *
get_any_compatible_invalid_surface(PGRAPHVkState *r, SurfaceBinding *target)
{
    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE(surface, &r->invalid_surfaces, entry, next) {
        if (check_invalid_surface_is_compatibile(surface, target)) {
            QTAILQ_REMOVE(&r->invalid_surfaces, surface, entry);
            return surface;
        }
    }

    return NULL;
}

static void prune_invalid_surfaces(PGRAPHVkState *r, int keep)
{
    int num_surfaces = 0;

    SurfaceBinding *surface, *next;
    QTAILQ_FOREACH_SAFE(surface, &r->invalid_surfaces, entry, next) {
        num_surfaces += 1;
        if (num_surfaces > keep) {
            QTAILQ_REMOVE(&r->invalid_surfaces, surface, entry);
            destroy_surface_image(r, surface);
            g_free(surface);
        }
    }
}

static void expire_old_surfaces(NV2AState *d)
{
    PGRAPHVkState *r = d->pgraph.vk_renderer_state;

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        int last_used = d->pgraph.frame_time - s->frame_time;
        int max_age = s->access_cb ? tracked_surface_frame_time_delta() :
                      max_surface_frame_time_delta;
        if (last_used >= max_age) {
            trace_nv2a_pgraph_surface_evict_reason("old", s->vram_addr);
            trace_surface_readback_reason(&d->pgraph, s, "age");
            pgraph_vk_surface_download_if_dirty(d, s);
            invalidate_surface(d, s);
        }
    }
}

static bool check_surface_compatibility(SurfaceBinding const *s1,
                                        SurfaceBinding const *s2, bool strict)
{
    bool format_compatible =
        (s1->color == s2->color) &&
        (s1->host_fmt.vk_format == s2->host_fmt.vk_format) &&
        (s1->pitch == s2->pitch);
    if (!format_compatible) {
        return false;
    }

    if (!strict) {
        return (s1->width >= s2->width) && (s1->height >= s2->height);
    } else {
        return (s1->width == s2->width) && (s1->height == s2->height);
    }
}

void pgraph_vk_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface)
{
    if (surface->draw_dirty) {
        download_surface(d, surface, true);
    }
}

void pgraph_vk_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                   bool force)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!(surface->upload_pending || force)) {
        return;
    }
    bool sample_total = sample_surface_total(pg);
    int64_t total_start = sample_total ? nv2a_recomp_vk_now_us() : 0;

    nv2a_profile_inc_counter(NV2A_PROF_SURF_UPLOAD);

    /* Diagnostic only: attribute SURFACE_CREATE-labelled waits to the RAM
     * upload which actually invokes them.  Preserve the existing fence. */
    static int upload_wait_probe = -1;
    if (upload_wait_probe < 0) {
        const char *setting = getenv("RECOMP_VK_SURF_WAIT_PROBE");
        upload_wait_probe = setting && setting[0] == '1';
    }
    bool sample_upload_wait = upload_wait_probe && pg->frame_time >= 120 &&
                              pg->frame_time % 30 == 0;
    bool upload_open_main = r->in_command_buffer;
    int64_t upload_wait_start = sample_upload_wait ?
                                nv2a_recomp_vk_now_us() : 0;
    pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_CREATE); // FIXME: SURFACE_UP
    if (sample_upload_wait) {
        fprintf(stderr,
                "[VK-SURF-UPLOAD-WAIT] frame=%d addr=%08llX size=%ux%u "
                "color=%d swizzle=%d open_main=%d draw_time=%llu "
                "wall_us=%lld\n",
                pg->frame_time,
                (unsigned long long)surface->vram_addr,
                surface->width, surface->height,
                surface->color, surface->swizzle, upload_open_main,
                (unsigned long long)surface->draw_time,
                (long long)(nv2a_recomp_vk_now_us() - upload_wait_start));
    }

    trace_nv2a_pgraph_surface_upload(
                 surface->color ? "COLOR" : "ZETA",
                 surface->swizzle ? "sz" : "lin", surface->vram_addr,
                 surface->width, surface->height, surface->pitch,
                 surface->fmt.bytes_per_pixel);

    surface->upload_pending = false;
    surface->draw_time = pg->draw_time;
    /* xboxrecomp: the GPU copy is loaded from RAM (docs Update 492) */
    nv2a_recomp_access_watch(surface->access_cb, 1);

    uint8_t *data = d->vram_ptr;
    uint8_t *buf = data + surface->vram_addr;

    g_autofree uint8_t *swizzle_buf = NULL;
    uint8_t *gl_read_buf = NULL;

    if (surface->swizzle) {
        swizzle_buf = (uint8_t*)g_malloc(surface->size);
        gl_read_buf = swizzle_buf;
        unswizzle_rect(data + surface->vram_addr,
                       surface->width, surface->height,
                       swizzle_buf,
                       surface->pitch,
                       surface->fmt.bytes_per_pixel);
        nv2a_profile_inc_counter(NV2A_PROF_SURF_SWIZZLE);
    } else {
        gl_read_buf = buf;
    }

    //
    // Upload image data from host to staging buffer
    //

    StorageBuffer *copy_buffer = &r->storage_buffers[BUFFER_STAGING_SRC];
    size_t uploaded_image_size = surface->height * surface->width *
                                 surface->fmt.bytes_per_pixel;
    assert(uploaded_image_size <= copy_buffer->buffer_size);

    void *mapped_memory_ptr = NULL;
    VK_CHECK(vmaMapMemory(r->allocator, copy_buffer->allocation,
                          &mapped_memory_ptr));

    bool use_compute_to_convert_depth_stencil_format =
        surface->host_fmt.vk_format == VK_FORMAT_D24_UNORM_S8_UINT ||
        surface->host_fmt.vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    bool no_conversion_necessary =
        surface->color || surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM ||
        use_compute_to_convert_depth_stencil_format;
    assert(no_conversion_necessary);

    memcpy_image(mapped_memory_ptr, gl_read_buf,
                 surface->width * surface->fmt.bytes_per_pixel, surface->pitch,
                 surface->height);
    /* xboxrecomp: g_autofree is a no-op under MSVC; last use was above. */
    g_free(swizzle_buf);
    swizzle_buf = NULL;
    gl_read_buf = NULL;

    vmaFlushAllocation(r->allocator, copy_buffer->allocation, 0, VK_WHOLE_SIZE);
    vmaUnmapMemory(r->allocator, copy_buffer->allocation);

    VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
    pgraph_vk_begin_debug_marker(r, cmd, RGBA_RED, __func__);

    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer->buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &host_barrier, 0, NULL);

    // Set up image copy regions (which may be modified by compute unpack)

    VkBufferImageCopy regions[2];
    int num_regions = 0;

    regions[num_regions++] = (VkBufferImageCopy){
        .imageSubresource.aspectMask = surface->color ?
                                           VK_IMAGE_ASPECT_COLOR_BIT :
                                           VK_IMAGE_ASPECT_DEPTH_BIT,
        .imageSubresource.layerCount = 1,
        .imageExtent = (VkExtent3D){ surface->width, surface->height, 1 },
    };

    if (surface->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
        regions[num_regions++] = (VkBufferImageCopy){
            .imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .imageSubresource.layerCount = 1,
            .imageExtent = (VkExtent3D){ surface->width, surface->height, 1 },
        };
    }


    unsigned int scaled_width = surface->width, scaled_height = surface->height;
    pgraph_apply_scaling_factor(pg, &scaled_width, &scaled_height);

    if (use_compute_to_convert_depth_stencil_format) {

        //
        // Copy packed image buffer to compute_dst for unpacking
        //

        size_t packed_size = uploaded_image_size;
        VkBufferCopy buffer_copy_region = {
            .size = packed_size,
        };
        vkCmdCopyBuffer(cmd, copy_buffer->buffer,
                        r->storage_buffers[BUFFER_COMPUTE_DST].buffer, 1,
                        &buffer_copy_region);

        size_t num_pixels = scaled_width * scaled_height;
        size_t unpacked_depth_image_size = num_pixels * 4;
        size_t unpacked_stencil_image_size = num_pixels;
        size_t unpacked_size =
            unpacked_depth_image_size + unpacked_stencil_image_size;

        VkBufferMemoryBarrier post_copy_src_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = copy_buffer->buffer,
            .size = VK_WHOLE_SIZE
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_copy_src_barrier, 0, NULL);

        //
        // Unpack depth-stencil image into compute_src
        //

        VkBufferMemoryBarrier pre_unpack_src_barrier = {
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
                             1, &pre_unpack_src_barrier, 0, NULL);

        StorageBuffer *unpack_buffer = &r->storage_buffers[BUFFER_COMPUTE_SRC];

        VkBufferMemoryBarrier pre_unpack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = unpack_buffer->buffer,
            .size = unpacked_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1,
                             &pre_unpack_dst_barrier, 0, NULL);

        pgraph_vk_unpack_depth_stencil(
            pg, surface, cmd, r->storage_buffers[BUFFER_COMPUTE_DST].buffer,
            unpack_buffer->buffer);

        VkBufferMemoryBarrier post_unpack_src_barrier = {
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
                             &post_unpack_src_barrier, 0, NULL);

        VkBufferMemoryBarrier post_unpack_dst_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = unpack_buffer->buffer,
            .size = unpacked_size
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                             &post_unpack_dst_barrier, 0, NULL);

        // Already scaled during compute. Adjust copy regions.
        regions[0].imageExtent = (VkExtent3D){ scaled_width, scaled_height, 1 };
        regions[1].imageExtent = regions[0].imageExtent;
        regions[1].bufferOffset =
            ROUND_UP(unpacked_depth_image_size,
                     r->device_props.limits.minStorageBufferOffsetAlignment);

        copy_buffer = unpack_buffer;
    }

    //
    // Copy image data from buffer to staging image
    //

    if (surface->image_scratch_current_layout !=
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                                          surface->host_fmt.vk_format,
                                          surface->image_scratch_current_layout,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        surface->image_scratch_current_layout =
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }

    vkCmdCopyBufferToImage(cmd, copy_buffer->buffer, surface->image_scratch,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, num_regions,
                           regions);

    VkBufferMemoryBarrier post_copy_src_buffer_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = copy_buffer->buffer,
        .size = VK_WHOLE_SIZE
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &post_copy_src_buffer_barrier, 0, NULL);

    //
    // Copy staging image to final image
    //

    pgraph_vk_transition_image_layout(pg, cmd, surface->image_scratch,
                                      surface->host_fmt.vk_format,
                                      surface->image_scratch_current_layout,
                                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    surface->image_scratch_current_layout =
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    bool upscale = pg->surface_scale_factor > 1 &&
                   !use_compute_to_convert_depth_stencil_format;

    if (upscale) {
        VkImageBlit blitRegion = {
            .srcSubresource.aspectMask = surface->host_fmt.aspect,
            .srcSubresource.mipLevel = 0,
            .srcSubresource.baseArrayLayer = 0,
            .srcSubresource.layerCount = 1,
            .srcOffsets[0] = (VkOffset3D){0, 0, 0},
            .srcOffsets[1] = (VkOffset3D){surface->width, surface->height, 1},

            .dstSubresource.aspectMask = surface->host_fmt.aspect,
            .dstSubresource.mipLevel = 0,
            .dstSubresource.baseArrayLayer = 0,
            .dstSubresource.layerCount = 1,
            .dstOffsets[0] = (VkOffset3D){0, 0, 0},
            .dstOffsets[1] = (VkOffset3D){scaled_width, scaled_height, 1},
        };

        vkCmdBlitImage(cmd, surface->image_scratch,
                       surface->image_scratch_current_layout, surface->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitRegion,
                       surface->color ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else {
        // Note: We should be able to vkCmdCopyBufferToImage directly into
        // surface->image, but there is an apparent AMD Windows driver
        // synchronization bug we'll hit when doing this. For this reason,
        // always use a staging image.

        for (int i = 0; i < num_regions; i++) {
            VkImageAspectFlags aspect = regions[i].imageSubresource.aspectMask;
            VkImageCopy copy_region = {
                .srcSubresource.aspectMask = aspect,
                .srcSubresource.layerCount = 1,
                .dstSubresource.aspectMask = aspect,
                .dstSubresource.layerCount = 1,
                .extent = regions[i].imageExtent,
            };
            vkCmdCopyImage(cmd, surface->image_scratch,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, surface->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy_region);
        }
    }

    pgraph_vk_transition_image_layout(
        pg, cmd, surface->image, surface->host_fmt.vk_format,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        surface->color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL :
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_2);
    pgraph_vk_end_debug_marker(r, cmd);
    pgraph_vk_end_single_time_commands(pg, cmd, VK_AUX_UPLOAD);

    surface->initialized = true;
    if (sample_total) {
        fprintf(stderr,
                "[VK-SURF-TOTAL] frame=%d op=upload addr=%08llX "
                "size=%ux%u color=%d swizzle=%d scale=%d wall_us=%lld\n",
                pg->frame_time, (unsigned long long)surface->vram_addr,
                surface->width, surface->height, surface->color,
                surface->swizzle, pg->surface_scale_factor,
                (long long)(nv2a_recomp_vk_now_us() - total_start));
    }
}

static void compare_surfaces(SurfaceBinding const *a, SurfaceBinding const *b)
{
    #define DO_CMP(fld) \
        if (a->fld != b->fld) \
            trace_nv2a_pgraph_surface_compare_mismatch( \
                #fld, (long int)a->fld, (long int)b->fld);
    DO_CMP(shape.clip_x)
    DO_CMP(shape.clip_width)
    DO_CMP(shape.clip_y)
    DO_CMP(shape.clip_height)
    DO_CMP(fmt.bytes_per_pixel)
    DO_CMP(host_fmt.vk_format)
    DO_CMP(color)
    DO_CMP(swizzle)
    DO_CMP(vram_addr)
    DO_CMP(width)
    DO_CMP(height)
    DO_CMP(pitch)
    DO_CMP(size)
    DO_CMP(dma_addr)
    DO_CMP(dma_len)
    DO_CMP(frame_time)
    DO_CMP(draw_time)
    #undef DO_CMP
}

static void populate_surface_binding_target_sized(NV2AState *d, bool color,
                                                  unsigned int width,
                                                  unsigned int height,
                                                  SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    Surface *surface;
    hwaddr dma_address;
    BasicSurfaceFormatInfo fmt;
    SurfaceFormatInfo host_fmt;

    if (color) {
        surface = &pg->surface_color;
        dma_address = pg->dma_color;
        assert(pg->surface_shape.color_format != 0);
        assert(pg->surface_shape.color_format <
               ARRAY_SIZE(kelvin_surface_color_format_vk_map));
        fmt = kelvin_surface_color_format_map[pg->surface_shape.color_format];
        host_fmt = kelvin_surface_color_format_vk_map[pg->surface_shape.color_format];
        if (host_fmt.host_bytes_per_pixel == 0) {
            fprintf(stderr, "nv2a: unimplemented color surface format 0x%x\n",
                    pg->surface_shape.color_format);
            abort();
        }
    } else {
        surface = &pg->surface_zeta;
        dma_address = pg->dma_zeta;
        assert(pg->surface_shape.zeta_format != 0);
        assert(pg->surface_shape.zeta_format <
               ARRAY_SIZE(r->kelvin_surface_zeta_vk_map));
        fmt = kelvin_surface_zeta_format_map[pg->surface_shape.zeta_format];
        host_fmt = r->kelvin_surface_zeta_vk_map[pg->surface_shape.zeta_format];
        // FIXME: Support float 16,24b float format surface
    }

    DMAObject dma = nv_dma_load(d, dma_address);
    // There's a bunch of bugs that could cause us to hit this function
    // at the wrong time and get a invalid dma object.
    // Check that it's sane.
    assert(dma.dma_class == NV_DMA_IN_MEMORY_CLASS);
    // assert(dma.address + surface->offset != 0);
    assert(surface->offset <= dma.limit);
    assert(surface->offset + surface->pitch * height <= dma.limit + 1);
    assert(surface->pitch % fmt.bytes_per_pixel == 0);
    assert((dma.address & ~0x07FFFFFF) == 0);

    target->shape = (color || !r->color_binding) ? pg->surface_shape :
                                                   r->color_binding->shape;
    target->fmt = fmt;
    target->host_fmt = host_fmt;
    target->color = color;
    target->swizzle =
        (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);
    target->vram_addr = dma.address + surface->offset;
    target->width = width;
    target->height = height;
    target->pitch = surface->pitch;
    target->size = height * MAX(surface->pitch, width * fmt.bytes_per_pixel);
    target->upload_pending = true;
    target->download_pending = false;
    target->draw_dirty = false;
    target->dma_addr = dma.address;
    target->dma_len = dma.limit;
    target->frame_time = pg->frame_time;
    target->draw_time = pg->draw_time;
    target->cleared = false;

    target->initialized = false;
}

static void populate_surface_binding_target(NV2AState *d, bool color,
                                            SurfaceBinding *target)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    unsigned int width, height;

    if (color || !r->color_binding) {
        get_surface_dimensions(pg, &width, &height);
        pgraph_apply_anti_aliasing_factor(pg, &width, &height);

        // Since we determine surface dimensions based on the clipping
        // rectangle, make sure to include the surface offset as well.
        if (pg->surface_type != NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE) {
            width += pg->surface_shape.clip_x;
            height += pg->surface_shape.clip_y;
        }
    } else {
        width = r->color_binding->width;
        height = r->color_binding->height;
    }

    populate_surface_binding_target_sized(d, color, width, height, target);
}

/* An old depth image cannot contribute to a larger target if that target is
 * about to receive a complete depth clear.  Limit this optimization to the
 * observed linear Z16 reshape and verify the actual clear bits and rectangle,
 * not merely pg->clearing.  The old image is still synchronized/inactivated
 * normally; only the redundant RAM readback and new RAM upload are omitted. */
static bool z16_target_will_be_fully_cleared(PGRAPHState *pg,
                                              const SurfaceBinding *old,
                                              const SurfaceBinding *target)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_VK_Z16_FULL_CLEAR_REUSE");
        enabled = e && e[0] == '1';
    }
    if (!enabled || !pg->clearing ||
        !(pg->vk_renderer_state->clear_parameter & NV097_CLEAR_SURFACE_Z) ||
        pg->surface_scale_factor < 2 || old->color || target->color ||
        old->swizzle || target->swizzle ||
        old->host_fmt.vk_format != VK_FORMAT_D16_UNORM ||
        target->host_fmt.vk_format != VK_FORMAT_D16_UNORM ||
        old->vram_addr != target->vram_addr ||
        old->pitch != target->pitch ||
        target->pitch != target->width * 2 ||
        old->width >= target->width || old->height >= target->height) {
        return false;
    }

    uint32_t clear_x = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clear_y = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);
    return GET_MASK(clear_x, NV_PGRAPH_CLEARRECTX_XMIN) == 0 &&
           GET_MASK(clear_y, NV_PGRAPH_CLEARRECTY_YMIN) == 0 &&
           GET_MASK(clear_x, NV_PGRAPH_CLEARRECTX_XMAX) >=
               target->width - 1 &&
           GET_MASK(clear_y, NV_PGRAPH_CLEARRECTY_YMAX) >=
               target->height - 1;
}

static void update_surface_part(NV2AState *d, bool upload, bool color)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    SurfaceBinding target;
    memset(&target, 0, sizeof(target));
    populate_surface_binding_target(d, color, &target);

    Surface *pg_surface = color ? &pg->surface_color : &pg->surface_zeta;

    /* xboxrecomp: QEMU's dirty log is not available; surfaces get their own
     * CPU-write check (nv2a_recomp.c, src/gpu/PROVENANCE.md), as in
     * gl/surface.c. */
    bool mem_dirty = !tcg_enabled() &&
                     nv2a_recomp_surface_cpu_dirty(target.vram_addr,
                                                   target.size);

    SurfaceBinding *current_binding = color ? r->color_binding
                                            : r->zeta_binding;

    if (!current_binding ||
        (upload && (pg_surface->buffer_dirty || mem_dirty))) {
        // FIXME: We don't need to be so aggressive flushing the command list
        // pgraph_vk_finish(pg, VK_FINISH_REASON_SURFACE_CREATE);
        pgraph_vk_ensure_not_in_render_pass(pg);

        unbind_surface(d, color);

        SurfaceBinding *surface = pgraph_vk_surface_get(d, target.vram_addr);
        if (surface != NULL) {
            // FIXME: Support same color/zeta surface target? In the mean time,
            // if the surface we just found is currently bound, just unbind it.
            SurfaceBinding *other = (color ? r->zeta_binding
                                           : r->color_binding);
            if (surface == other) {
                NV2A_UNIMPLEMENTED("Same color & zeta surface offset");
                unbind_surface(d, !color);
            }
        }

        trace_nv2a_pgraph_surface_target(
            color ? "COLOR" : "ZETA", target.vram_addr,
            target.swizzle ? "sz" : "ln",
            pg->surface_shape.anti_aliasing,
            pg->surface_shape.clip_x,
            pg->surface_shape.clip_width, pg->surface_shape.clip_y,
            pg->surface_shape.clip_height);

        bool should_create = true;
        bool discard_old_for_full_z_clear = false;

        if (surface != NULL) {
            bool is_compatible =
                check_surface_compatibility(surface, &target, false);

            void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                             const char *layout, uint32_t anti_aliasing,
                             uint32_t clip_x, uint32_t clip_width,
                             uint32_t clip_y, uint32_t clip_height,
                             uint32_t pitch) =
                surface->color ? trace_nv2a_pgraph_surface_match_color :
                               trace_nv2a_pgraph_surface_match_zeta;

            trace_fn(surface->vram_addr, surface->width, surface->height,
                     surface->swizzle ? "sz" : "ln", surface->shape.anti_aliasing,
                     surface->shape.clip_x, surface->shape.clip_width,
                     surface->shape.clip_y, surface->shape.clip_height,
                     surface->pitch);

            assert(!(target.swizzle && pg->clearing));

#if 0
            if (surface->swizzle != target.swizzle) {
                // Clears should only be done on linear surfaces. Avoid
                // synchronization by allowing (1) a surface marked swizzled to
                // be cleared under the assumption the entire surface is
                // destined to be cleared and (2) a fully cleared linear surface
                // to be marked swizzled. Strictly match size to avoid
                // pathological cases.
                is_compatible &= (pg->clearing || surface->cleared) &&
                    check_surface_compatibility(surface, &target, true);
                if (is_compatible) {
                    trace_nv2a_pgraph_surface_migrate_type(
                        target.swizzle ? "swizzled" : "linear");
                }
            }
#endif

            if (is_compatible && color &&
                !check_surface_compatibility(surface, &target, true)) {
                SurfaceBinding zeta_entry;
                populate_surface_binding_target_sized(
                    d, !color, surface->width, surface->height, &zeta_entry);
                hwaddr color_end = surface->vram_addr + surface->size;
                hwaddr zeta_end = zeta_entry.vram_addr + zeta_entry.size;
                is_compatible &= surface->vram_addr >= zeta_end ||
                                 zeta_entry.vram_addr >= color_end;
            }

            if (is_compatible && !color && r->color_binding) {
                is_compatible &= (surface->width == r->color_binding->width) &&
                                 (surface->height == r->color_binding->height);
            }

            if (is_compatible) {
                // FIXME: Refactor
                pg->surface_binding_dim.width = surface->width;
                pg->surface_binding_dim.clip_x = surface->shape.clip_x;
                pg->surface_binding_dim.clip_width = surface->shape.clip_width;
                pg->surface_binding_dim.height = surface->height;
                pg->surface_binding_dim.clip_y = surface->shape.clip_y;
                pg->surface_binding_dim.clip_height = surface->shape.clip_height;
                surface->upload_pending |= mem_dirty;
                pg->surface_zeta.buffer_dirty |= color;
                should_create = false;
            } else {
                discard_old_for_full_z_clear =
                    z16_target_will_be_fully_cleared(pg, surface, &target) ||
                    (cross_type_discard_enabled() &&
                     surface->color != target.color);
                /* Diagnostic only: decide whether the recurring smaller
                 * D16 target is replaced by a full clear or by a draw that
                 * really needs the old contents. */
                if (!color && !surface->color &&
                    surface->host_fmt.vk_format == VK_FORMAT_D16_UNORM &&
                    target.host_fmt.vk_format == VK_FORMAT_D16_UNORM &&
                    surface->vram_addr == target.vram_addr &&
                    surface->width < target.width &&
                    surface->height < target.height) {
                    static int reshape_trace = -1;
                    if (reshape_trace < 0) {
                        const char *e = getenv("RECOMP_VK_Z16_RESHAPE_TRACE");
                        reshape_trace = e && e[0] == '1';
                    }
                    if (reshape_trace) {
                        uint32_t clear_x = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
                        uint32_t clear_y = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);
                        fprintf(stderr,
                                "[VK-Z16-RESHAPE] frame=%d addr=%08llX "
                                "old=%ux%u new=%ux%u pitch=%u "
                                "clearing=%d dirty=%d upload=%d "
                                "clearreg=%08X,%08X clip=%ux%u+%u+%u\n",
                                pg->frame_time,
                                (unsigned long long)surface->vram_addr,
                                surface->width, surface->height,
                                target.width, target.height, target.pitch,
                                pg->clearing, surface->draw_dirty,
                                surface->upload_pending, clear_x, clear_y,
                                pg->surface_shape.clip_width,
                                pg->surface_shape.clip_height,
                                pg->surface_shape.clip_x,
                                pg->surface_shape.clip_y);
                    }
                }
                trace_nv2a_pgraph_surface_evict_reason(
                    "incompatible", surface->vram_addr);
                compare_surfaces(surface, &target);
                trace_surface_readback_reason(pg, surface, "incompatible");
                if (!discard_old_for_full_z_clear) {
                    pgraph_vk_surface_download_if_dirty(d, surface);
                } else if (pg->frame_time % 30 == 0) {
                    fprintf(stderr,
                            "[VK-Z16-FULL-CLEAR] frame=%d addr=%08llX "
                            "%ux%u -> %ux%u\n",
                            pg->frame_time,
                            (unsigned long long)surface->vram_addr,
                            surface->width, surface->height,
                            target.width, target.height);
                }
                static int defer_enabled = -1;
                if (defer_enabled < 0) {
                    const char *e = getenv("RECOMP_VK_Z16_DEFER_INVALIDATION");
                    defer_enabled = e && e[0] == '1';
                }
                /* Discards need no readback, so with cross-type discard on
                 * they also skip the full GPU wait (docs Update 641). */
                bool defer_retirement = discard_old_for_full_z_clear &&
                                         (defer_enabled ||
                                          cross_type_discard_enabled());
                if (cross_type_discard_enabled() &&
                    surface->color != target.color) {
                    note_stale_range(pg, surface);
                }
                if (defer_retirement && pg->frame_time % 30 == 0) {
                    fprintf(stderr,
                            "[VK-Z16-DEFER] frame=%d open_main=%d\n",
                            pg->frame_time, r->in_command_buffer);
                }
                invalidate_surface_internal(d, surface, defer_retirement);
            }
        }

        if (should_create) {
            if (discard_old_for_full_z_clear ||
                (cross_type_discard_enabled() && !mem_dirty &&
                 target_in_stale_range(pg, &target))) {
                /* create_surface_image transitions the new VkImage to the
                 * attachment layout.  The immediately following full Z clear
                 * initializes every depth pixel before any draw can read it. */
                target.upload_pending = false;
                target.initialized = true;
            }
            surface = get_any_compatible_invalid_surface(r, &target);
            if (surface) {
                migrate_surface_image(&target, surface);
            } else {
                surface = g_malloc(sizeof(SurfaceBinding));
                create_surface_image(pg, &target);
            }

            *surface = target;
            set_surface_label(pg, surface);
            surface_put(d, surface);

            // FIXME: Refactor
            pg->surface_binding_dim.width = target.width;
            pg->surface_binding_dim.clip_x = target.shape.clip_x;
            pg->surface_binding_dim.clip_width = target.shape.clip_width;
            pg->surface_binding_dim.height = target.height;
            pg->surface_binding_dim.clip_y = target.shape.clip_y;
            pg->surface_binding_dim.clip_height = target.shape.clip_height;

            if (color && r->zeta_binding &&
                (r->zeta_binding->width != target.width ||
                 r->zeta_binding->height != target.height)) {
                pg->surface_zeta.buffer_dirty = true;
            }
        }

        void (*trace_fn)(uint32_t addr, uint32_t width, uint32_t height,
                         const char *layout, uint32_t anti_aliasing,
                         uint32_t clip_x, uint32_t clip_width, uint32_t clip_y,
                         uint32_t clip_height, uint32_t pitch) =
            color ? (should_create ? trace_nv2a_pgraph_surface_create_color :
                                     trace_nv2a_pgraph_surface_hit_color) :
                    (should_create ? trace_nv2a_pgraph_surface_create_zeta :
                                     trace_nv2a_pgraph_surface_hit_zeta);
        trace_fn(surface->vram_addr, surface->width, surface->height,
                 surface->swizzle ? "sz" : "ln", surface->shape.anti_aliasing,
                 surface->shape.clip_x, surface->shape.clip_width,
                 surface->shape.clip_y, surface->shape.clip_height, surface->pitch);

        bind_surface(r, surface);
        pg_surface->buffer_dirty = false;
    }

    if (!upload && pg_surface->draw_dirty) {
        if (!tcg_enabled()) {
            // FIXME: Cannot monitor for reads/writes; flush now
            SurfaceBinding *bound = color ? r->color_binding : r->zeta_binding;
            if (bound) {
                trace_surface_download_source(pg, bound, "post-draw-nontcg");
                download_surface(d, bound, true);
            } else {
                /* xboxrecomp (docs Update 463, as in gl/surface.c): a
                 * surface-shape change can leave draw_dirty set with no
                 * binding; the unbound surface is still in r->surfaces with
                 * its own draw_dirty, so flush every dirty surface of this
                 * kind instead of passing NULL. */
                SurfaceBinding *s;
                QTAILQ_FOREACH(s, &r->surfaces, entry) {
                    if (s->color == color) {
                        pgraph_vk_surface_download_if_dirty(d, s);
                    }
                }
            }
        }

        pg_surface->write_enabled_cache = false;
        pg_surface->draw_dirty = false;
    }
}

// FIXME: Move to common?
void pgraph_vk_surface_update(NV2AState *d, bool upload, bool color_write,
                              bool zeta_write)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    pg->surface_shape.z_format =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                 NV_PGRAPH_SETUPRASTER_Z_FORMAT);

    color_write = color_write &&
            (pg->clearing || pgraph_color_write_enabled(pg));
    zeta_write = zeta_write && (pg->clearing || pgraph_zeta_write_enabled(pg));

    if (upload) {
        bool fb_dirty = framebuffer_dirty(pg);
        if (fb_dirty) {
            memcpy(&pg->last_surface_shape, &pg->surface_shape,
                   sizeof(SurfaceShape));
            pg->surface_color.buffer_dirty = true;
            pg->surface_zeta.buffer_dirty = true;
        }

        if (pg->surface_color.buffer_dirty) {
            unbind_surface(d, true);
        }

        if (color_write) {
            update_surface_part(d, true, true);
        }

        if (pg->surface_zeta.buffer_dirty) {
            unbind_surface(d, false);
        }

        if (zeta_write) {
            update_surface_part(d, true, false);
        }
    } else {
        if ((color_write || pg->surface_color.write_enabled_cache)
            && pg->surface_color.draw_dirty) {
            update_surface_part(d, false, true);
        }
        if ((zeta_write || pg->surface_zeta.write_enabled_cache)
            && pg->surface_zeta.draw_dirty) {
            update_surface_part(d, false, false);
        }
    }

    if (upload) {
        pg->draw_time++;
    }

    bool swizzle = (pg->surface_type == NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE);

    if (r->color_binding) {
        r->color_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_vk_upload_surface_data(d, r->color_binding, false);
            r->color_binding->draw_time = pg->draw_time;
            r->color_binding->swizzle = swizzle;
        }
    }

    if (r->zeta_binding) {
        r->zeta_binding->frame_time = pg->frame_time;
        if (upload) {
            pgraph_vk_upload_surface_data(d, r->zeta_binding, false);
            r->zeta_binding->draw_time = pg->draw_time;
            r->zeta_binding->swizzle = swizzle;
        }
    }

    // Sanity check color and zeta dimensions match
    if (r->color_binding && r->zeta_binding) {
        assert(r->color_binding->width == r->zeta_binding->width);
        assert(r->color_binding->height == r->zeta_binding->height);
    }

    expire_old_surfaces(d);
    prune_invalid_surfaces(r, num_invalid_surfaces_to_keep);
}

static bool check_format_and_usage_supported(PGRAPHVkState *r, VkFormat format,
                                             VkImageUsageFlags usage)
{
    VkPhysicalDeviceImageFormatInfo2 pdif2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
    };
    VkResult result = vkGetPhysicalDeviceImageFormatProperties2(
        r->physical_device, &pdif2, &props);
    return result == VK_SUCCESS;
}

static bool check_surface_internal_formats_supported(
    PGRAPHVkState *r, const SurfaceFormatInfo *fmts, size_t count)
{
    bool all_supported = true;
    for (int i = 0; i < count; i++) {
        const SurfaceFormatInfo *f = &fmts[i];
        if (f->host_bytes_per_pixel) {
            all_supported &=
                check_format_and_usage_supported(r, f->vk_format, f->usage);
        }
    }
    return all_supported;
}

void pgraph_vk_init_surfaces(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Make sure all surface format types are supported. We don't expect issue
    // with these, and therefore have no fallback mechanism.
    bool color_formats_supported = check_surface_internal_formats_supported(
        r, kelvin_surface_color_format_vk_map,
        ARRAY_SIZE(kelvin_surface_color_format_vk_map));
    assert(color_formats_supported);

    // Check if the device supports preferred VK_FORMAT_D24_UNORM_S8_UINT
    // format, fall back to D32_SFLOAT_S8_UINT otherwise.
    r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z16] = zeta_d16;
    if (check_surface_internal_formats_supported(r, &zeta_d24_unorm_s8_uint,
                                                 1)) {
        r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] =
            zeta_d24_unorm_s8_uint;
    } else if (check_surface_internal_formats_supported(
                   r, &zeta_d32_sfloat_s8_uint, 1)) {
        r->kelvin_surface_zeta_vk_map[NV097_SET_SURFACE_FORMAT_ZETA_Z24S8] =
            zeta_d32_sfloat_s8_uint;
    } else {
        assert(!"No suitable depth-stencil format supported");
    }

    QTAILQ_INIT(&r->surfaces);
    QTAILQ_INIT(&r->invalid_surfaces);
    QTAILQ_INIT(&r->deferred_invalid_surfaces);

    r->downloads_pending = false;
    qemu_event_init(&r->downloads_complete, false);
    qemu_event_init(&r->dirty_surfaces_download_complete, false);

    r->color_binding = NULL;
    r->zeta_binding = NULL;
    r->framebuffer_dirty = true;

    pgraph_vk_reload_surface_scale_factor(pg); // FIXME: Move internal
}

void pgraph_vk_finalize_surfaces(PGRAPHState *pg)
{
    pgraph_vk_surface_flush(container_of(pg, NV2AState, pgraph));
}

void pgraph_vk_surface_flush(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // Clear last surface shape to force recreation of buffers at next draw
    pg->surface_color.draw_dirty = false;
    pg->surface_zeta.draw_dirty = false;
    memset(&pg->last_surface_shape, 0, sizeof(pg->last_surface_shape));
    unbind_surface(d, true);
    unbind_surface(d, false);

    SurfaceBinding *s, *next;
    QTAILQ_FOREACH_SAFE(s, &r->surfaces, entry, next) {
        // FIXME: We should download all surfaces to ram, but need to
        //        investigate corruption issue
        trace_surface_download_source(pg, s, "flush");
        pgraph_vk_surface_download_if_dirty(d, s);
        invalidate_surface(d, s);
    }
    if (!QTAILQ_EMPTY(&r->deferred_invalid_surfaces)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_FLUSH);
        assert(QTAILQ_EMPTY(&r->deferred_invalid_surfaces));
    }
    prune_invalid_surfaces(r, 0);

    pgraph_vk_reload_surface_scale_factor(pg);
}

/* xboxrecomp diagnostic (docs Update 627): read the surface at addr back to
 * guest RAM (guest resolution, guest format) and write it to a file with a
 * 16-byte header (width, height, pitch, bytes per pixel). Used to compare
 * depth buffers with and without an object. */
void nv2a_recomp_vk_dump_surface(NV2AState *d, hwaddr addr, const char *name)
{
    SurfaceBinding *s = pgraph_vk_surface_get(d, addr);
    if (!s) {
        /* No GPU surface: write guest RAM as a 640x480 32-bit image
         * (bloom ghost diagnosis, docs Update 652). */
        FILE *f = fopen(name, "wb");
        if (f) {
            uint32_t hdr[4] = { 640, 480, 2560, 4 };
            fwrite(hdr, sizeof(hdr), 1, f);
            fwrite((uint8_t *)d->vram_ptr + addr, 1, 2560 * 480, f);
            fclose(f);
        }
        fprintf(stderr, "[ZDUMP] no surface at %08llX, raw RAM written\n",
                (unsigned long long)addr);
        return;
    }
    /* RECOMP_DUMP_SCALED=1 (shadow teeth diagnosis, docs Update 659): a
     * linear D16 surface is written at host resolution, not downscaled, to
     * show whether the upscaled shadow map really holds finer detail. */
    static int dump_scaled = -1;
    if (dump_scaled < 0) {
        const char *e = getenv("RECOMP_DUMP_SCALED");
        dump_scaled = e && e[0] == '1';
    }
    if (dump_scaled && !s->color && !s->swizzle &&
        s->host_fmt.vk_format == VK_FORMAT_D16_UNORM) {
        PGRAPHState *pg = &d->pgraph;
        PGRAPHVkState *r = pg->vk_renderer_state;
        unsigned int sw = s->width, sh = s->height;
        pgraph_apply_scaling_factor(pg, &sw, &sh);
        if ((size_t)sw * sh * 2 <=
            r->storage_buffers[BUFFER_STAGING_DST].buffer_size) {
            if (r->in_command_buffer &&
                s->draw_time >= r->command_buffer_start_time) {
                finish_surface_down_with_probe(pg, s, "download");
            }
            VkCommandBuffer cmd = pgraph_vk_begin_single_time_commands(pg);
            pgraph_vk_transition_image_layout(
                pg, cmd, s->image, s->host_fmt.vk_format,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy region = {
                .imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .imageSubresource.layerCount = 1,
                .imageExtent = (VkExtent3D){ sw, sh, 1 },
            };
            vkCmdCopyImageToBuffer(cmd, s->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   r->storage_buffers[BUFFER_STAGING_DST].buffer,
                                   1, &region);
            pgraph_vk_transition_image_layout(
                pg, cmd, s->image, s->host_fmt.vk_format,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            VkBufferMemoryBarrier host = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = r->storage_buffers[BUFFER_STAGING_DST].buffer,
                .size = VK_WHOLE_SIZE,
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                                 &host, 0, NULL);
            pgraph_vk_end_single_time_commands(pg, cmd, VK_AUX_DOWNLOAD);
            void *mapped = NULL;
            VK_CHECK(vmaMapMemory(r->allocator,
                     r->storage_buffers[BUFFER_STAGING_DST].allocation,
                     &mapped));
            vmaInvalidateAllocation(r->allocator,
                     r->storage_buffers[BUFFER_STAGING_DST].allocation, 0,
                     VK_WHOLE_SIZE);
            FILE *f = fopen(name, "wb");
            if (f) {
                uint32_t hdr[4] = { sw, sh, sw * 2, 2 };
                fwrite(hdr, sizeof(hdr), 1, f);
                fwrite(mapped, 1, (size_t)sw * sh * 2, f);
                fclose(f);
            }
            vmaUnmapMemory(r->allocator,
                           r->storage_buffers[BUFFER_STAGING_DST].allocation);
            fprintf(stderr, "[ZDUMP] %s: surface %08llX scaled %ux%u\n", name,
                    (unsigned long long)addr, sw, sh);
            return;
        }
    }
    download_surface(d, s, true);
    FILE *f = fopen(name, "wb");
    if (f) {
        uint32_t hdr[4] = { s->width, s->height, s->pitch, s->fmt.bytes_per_pixel };
        fwrite(hdr, sizeof(hdr), 1, f);
        fwrite((uint8_t *)d->vram_ptr + s->vram_addr, 1, s->size, f);
        fclose(f);
    }
    fprintf(stderr, "[ZDUMP] %s: surface %08llX %ux%u pitch %u bpp %u\n", name,
            (unsigned long long)addr, s->width, s->height, s->pitch,
            s->fmt.bytes_per_pixel);
}
