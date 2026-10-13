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

static void create_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    QueueFamilyIndices indices =
        pgraph_vk_find_queue_families(r->physical_device);

    VkCommandPoolCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = indices.queue_family,
    };
    VK_CHECK(
        vkCreateCommandPool(r->device, &create_info, NULL, &r->command_pool));
}

static void destroy_command_pool(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkDestroyCommandPool(r->device, r->command_pool, NULL);
}

static void create_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = r->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = ARRAY_SIZE(r->command_buffers),
    };
    VK_CHECK(
        vkAllocateCommandBuffers(r->device, &alloc_info, r->command_buffers));

    r->command_buffer = r->command_buffers[0];
    r->aux_command_buffer = r->command_buffers[1];

    /* Chunk ring (docs Update 645): main + staging-sync buffer per slot. */
    alloc_info.commandBufferCount = RECOMP_VK_CHUNKS;
    VK_CHECK(vkAllocateCommandBuffers(r->device, &alloc_info, r->chunk_cmd));
    VK_CHECK(vkAllocateCommandBuffers(r->device, &alloc_info, r->chunk_aux));
    r->chunk_slot = 0;
    r->chunk_wait_sem = VK_NULL_HANDLE;
    r->command_buffer = r->chunk_cmd[0];
}

static void destroy_command_buffers(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    vkFreeCommandBuffers(r->device, r->command_pool,
                         ARRAY_SIZE(r->command_buffers), r->command_buffers);
    vkFreeCommandBuffers(r->device, r->command_pool, RECOMP_VK_CHUNKS,
                         r->chunk_cmd);
    vkFreeCommandBuffers(r->device, r->command_pool, RECOMP_VK_CHUNKS,
                         r->chunk_aux);

    r->command_buffer = VK_NULL_HANDLE;
    r->aux_command_buffer = VK_NULL_HANDLE;
}

VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(!r->in_aux_command_buffer);
    r->in_aux_command_buffer = true;

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->aux_command_buffer, &begin_info));

    return r->aux_command_buffer;
}

void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd,
                                        VkAuxReason reason)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    static int timing_enabled = -1;
    static uint64_t wait_count, wait_us, wait_max_us;
    if (timing_enabled < 0) {
        const char *e = getenv("RECOMP_VK_STAGE_TIMING");
        timing_enabled = e && e[0] == '1';
    }

    assert(r->in_aux_command_buffer);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    VK_CHECK(vkQueueSubmit(r->queue, 1, &submit_info, VK_NULL_HANDLE));
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT_AUX);
    bool frame_pace = nv2a_recomp_frame_pace_enabled();
    int64_t wait_start = (timing_enabled || frame_pace) ?
        nv2a_recomp_vk_now_us() : 0;
    VK_CHECK(vkQueueWaitIdle(r->queue));
    if (timing_enabled || frame_pace) {
        uint64_t elapsed = (uint64_t)(nv2a_recomp_vk_now_us() - wait_start);
        if (frame_pace)
            nv2a_recomp_frame_pace_wait(true, reason, elapsed);
        if (timing_enabled) {
            wait_count++;
            wait_us += elapsed;
            wait_max_us = MAX(wait_max_us, elapsed);
            if (wait_count == 450) {
                fprintf(stderr, "[VK-WAIT-AUX] frame=%d calls=%llu total_ms=%.3f "
                        "avg_us=%.1f max_us=%llu\n", pg->frame_time,
                        (unsigned long long)wait_count, wait_us / 1000.0,
                        (double)wait_us / wait_count,
                        (unsigned long long)wait_max_us);
                wait_count = wait_us = wait_max_us = 0;
            }
        }
    }

    r->in_aux_command_buffer = false;
}

void pgraph_vk_init_command_buffers(PGRAPHState *pg)
{
    create_command_pool(pg);
    create_command_buffers(pg);
}

void pgraph_vk_finalize_command_buffers(PGRAPHState *pg)
{
    destroy_command_buffers(pg);
    destroy_command_pool(pg);
}
