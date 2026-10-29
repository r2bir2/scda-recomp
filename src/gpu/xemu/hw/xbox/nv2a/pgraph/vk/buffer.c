/*
 * Geforce NV2A PGRAPH Vulkan Renderer
 *
 * Copyright (c) 2024 Matt Borgerson
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

#include "renderer.h"
#include <vulkan/vulkan_core.h>

static void create_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkBufferCreateInfo buffer_create_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer->buffer_size,
        .usage = buffer->usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult result = vmaCreateBuffer(r->allocator, &buffer_create_info,
                                      &buffer->alloc_info, &buffer->buffer,
                                      &buffer->allocation, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[VK] buffer %d (%llu MiB) could not be created: %d\n",
                (int)(buffer - r->storage_buffers),
                (unsigned long long)(buffer->buffer_size >> 20), (int)result);
    }
    VK_CHECK(result);
}

static void destroy_buffer(PGRAPHState *pg, StorageBuffer *buffer)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vmaDestroyBuffer(r->allocator, buffer->buffer, buffer->allocation);
    buffer->buffer = VK_NULL_HANDLE;
    buffer->allocation = VK_NULL_HANDLE;
}

void pgraph_vk_init_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    // FIXME: Profile buffer sizes

    VmaAllocationCreateInfo host_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                 VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
    };
    VmaAllocationCreateInfo device_alloc_create_info = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT
    };

    r->storage_buffers[BUFFER_STAGING_DST] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .buffer_size = 4096 * 4096 * 4,
    };

    r->storage_buffers[BUFFER_STAGING_SRC] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_STAGING_DST].buffer_size,
    };

    /* xboxrecomp (docs Update 673): the two depth/stencil conversion buffers
     * were a fixed 800 MiB each, device-local. On a 2 GiB card (GTX 960M)
     * that left almost no VRAM, and the first display-image allocation
     * failed with VK_ERROR_OUT_OF_DEVICE_MEMORY. Size them to an eighth of
     * the largest device-local heap, between 128 MiB and the old 800 MiB
     * (a conversion needs about 5 bytes per scaled pixel: 128 MiB still
     * holds a 2048x2048 surface at render scale 2). */
    VkDeviceSize compute_size = (VkDeviceSize)(1024 * 10) * (1024 * 10) * 8;
    /* docs Update 675: the inline vertex ring (and its staging twin) was a
     * fixed 1279 MiB -- ten maximal batches. It is flushed whenever it runs
     * full (ensure_buffer_space), so it only has to hold one batch; on a
     * shared-memory GPU (Mali, 7.5 GB board) the full size failed with
     * VK_ERROR_OUT_OF_DEVICE_MEMORY. A sixteenth of the device-local heap,
     * at least one batch, at most the old size. */
    const VkDeviceSize one_batch = (VkDeviceSize)NV2A_VERTEXSHADER_ATTRIBUTES *
                                   NV2A_MAX_BATCH_LENGTH * 4 * sizeof(float);
    VkDeviceSize inline_size = one_batch * 10;
    {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(r->physical_device, &mp);
        VkDeviceSize vram = 0;
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
            if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
                mp.memoryHeaps[i].size > vram) {
                vram = mp.memoryHeaps[i].size;
            }
        }
        /* integrated GPUs (Mali: one 7.5 GB pool for everything) take half
         * as much again -- two instances meet during a title relaunch and
         * ran the board out of memory (docs Update 676) */
        VkPhysicalDeviceProperties dev_props;
        vkGetPhysicalDeviceProperties(r->physical_device, &dev_props);
        if (dev_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            vram /= 4;
        VkDeviceSize want = vram / 8;
        if (want < 128ull * 1024 * 1024)
            want = 128ull * 1024 * 1024;
        if (want < compute_size)
            compute_size = want;
        VkDeviceSize want_inline = vram / 16;
        if (want_inline < one_batch)
            want_inline = one_batch;
        if (want_inline < inline_size)
            inline_size = want_inline;
        const char *e = getenv("RECOMP_VK_CONVERSION_MB"); /* test override */
        if (e && atoi(e) > 0)
            compute_size = (VkDeviceSize)atoi(e) << 20;
        fprintf(stderr, "[VK] conversion buffers %llu MiB each, inline vertex "
                "ring %llu MiB (VRAM %llu MiB)\n",
                (unsigned long long)(compute_size >> 20),
                (unsigned long long)(inline_size >> 20),
                (unsigned long long)(vram >> 20));
    }

    r->storage_buffers[BUFFER_COMPUTE_DST] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = compute_size,
    };

    r->storage_buffers[BUFFER_COMPUTE_SRC] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .buffer_size = r->storage_buffers[BUFFER_COMPUTE_DST].buffer_size,
    };

    r->storage_buffers[BUFFER_INDEX] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .buffer_size = sizeof(pg->inline_elements) * 100,
    };

    r->storage_buffers[BUFFER_INDEX_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_INDEX].buffer_size,
    };

    // FIXME: Don't assume that we can render with host mapped buffer
    r->storage_buffers[BUFFER_VERTEX_RAM] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        /* TRANSFER_DST: in-stream updates (draw.c, docs Update 651) */
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .buffer_size = memory_region_size(d->vram),
    };

    r->bitmap_size = memory_region_size(d->vram) / 4096;
    r->uploaded_bitmap = bitmap_new(r->bitmap_size);
    bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);

    r->storage_buffers[BUFFER_VERTEX_INLINE] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .buffer_size = inline_size,
    };

    r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_VERTEX_INLINE].buffer_size,
    };

    r->storage_buffers[BUFFER_UNIFORM] = (StorageBuffer){
        .alloc_info = device_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .buffer_size = 32 * 1024 * 1024, /* was 8 MiB (docs Update 641) */
    };

    r->storage_buffers[BUFFER_UNIFORM_STAGING] = (StorageBuffer){
        .alloc_info = host_alloc_create_info,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .buffer_size = r->storage_buffers[BUFFER_UNIFORM].buffer_size,
    };

    for (int i = 0; i < BUFFER_COUNT; i++) {
        create_buffer(pg, &r->storage_buffers[i]);
    }

    // FIXME: Add fallback path for device using host mapped memory

    int buffers_to_map[] = { BUFFER_VERTEX_RAM,
                             BUFFER_INDEX_STAGING,
                             BUFFER_VERTEX_INLINE_STAGING,
                             BUFFER_UNIFORM_STAGING };

    for (int i = 0; i < ARRAY_SIZE(buffers_to_map); i++) {
        VK_CHECK(vmaMapMemory(
            r->allocator, r->storage_buffers[buffers_to_map[i]].allocation,
            (void **)&r->storage_buffers[buffers_to_map[i]].mapped));
    }
}

void pgraph_vk_finalize_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (r->storage_buffers[i].mapped) {
            vmaUnmapMemory(r->allocator, r->storage_buffers[i].allocation);
        }
        destroy_buffer(pg, &r->storage_buffers[i]);
    }

    g_free(r->uploaded_bitmap);
    r->uploaded_bitmap = NULL;
}

bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size,
                                    VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b = &r->storage_buffers[index];
    return (ROUND_UP(b->buffer_offset, alignment) + size) <= b->buffer_size;
}

VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkDeviceSize total_size = 0;
    for (int i = 0; i < count; i++) {
        total_size += sizes[i];
    }
    assert(pgraph_vk_buffer_has_space_for(pg, index, total_size, alignment));

    StorageBuffer *b = &r->storage_buffers[index];
    VkDeviceSize starting_offset = ROUND_UP(b->buffer_offset, alignment);

    assert(b->mapped);

    for (int i = 0; i < count; i++) {
        b->buffer_offset = ROUND_UP(b->buffer_offset, alignment);
        memcpy(b->mapped + b->buffer_offset, data[i], sizes[i]);
        b->buffer_offset += sizes[i];
    }

    return starting_offset;
}
