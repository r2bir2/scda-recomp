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

#include "qemu/osdep.h"
#include "qemu/fast-hash.h"
#include "renderer.h"

void pgraph_vk_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;

    NV2A_VK_DPRINTF("NV097_SET_BEGIN_END: 0x%x", d->pgraph.primitive_mode);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    pgraph_vk_surface_update(d, true, true, depth_test || stencil_test);

    if (is_nop_draw) {
        NV2A_VK_DPRINTF("nop!");
        return;
    }
}

static VkPrimitiveTopology get_primitive_topology(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    int polygon_mode = r->shader_binding->state.polygon_front_mode;
    int primitive_mode = r->shader_binding->state.primitive_mode;

    if (polygon_mode == POLY_MODE_POINT) {
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }

    // FIXME: Replace with LUT
    switch (primitive_mode) {
    case PRIM_TYPE_POINTS:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PRIM_TYPE_LINES:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PRIM_TYPE_LINE_LOOP:
        // FIXME: line strips, except that the first and last vertices are also used as a line
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_LINE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PRIM_TYPE_TRIANGLE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS:
        /* Mali mode (docs Update 675): split into a triangle list */
        if (!r->geometry_shader_supported)
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP:
        if (!r->geometry_shader_supported)
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; // FIXME
        } else if (polygon_mode == POLY_MODE_FILL) {
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        }
        assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static void pipeline_cache_entry_init(Lru *lru, LruNode *node, void *state)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    snode->layout = VK_NULL_HANDLE;
    snode->pipeline = VK_NULL_HANDLE;
    snode->draw_time = 0;
}

static void pipeline_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    PGRAPHVkState *r = container_of(lru, PGRAPHVkState, pipeline_cache);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    assert((!r->in_command_buffer ||
            snode->draw_time < r->command_buffer_start_time) &&
           "Pipeline evicted while in use!");

    vkDestroyPipeline(r->device, snode->pipeline, NULL);
    snode->pipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(r->device, snode->layout, NULL);
    snode->layout = VK_NULL_HANDLE;
}

static bool pipeline_cache_entry_compare(Lru *lru, LruNode *node, void *key)
{
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    return memcmp(&snode->key, key, sizeof(PipelineKey));
}

static void init_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VkPipelineCacheCreateInfo cache_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .flags = 0,
        .initialDataSize = 0,
        .pInitialData = NULL,
        .pNext = NULL,
    };
    VK_CHECK(vkCreatePipelineCache(r->device, &cache_info, NULL,
                                   &r->vk_pipeline_cache));

    const size_t pipeline_cache_size = 2048;
    lru_init(&r->pipeline_cache);
    r->pipeline_cache_entries =
        g_malloc_n(pipeline_cache_size, sizeof(PipelineBinding));
    assert(r->pipeline_cache_entries != NULL);
    for (int i = 0; i < pipeline_cache_size; i++) {
        lru_add_free(&r->pipeline_cache, &r->pipeline_cache_entries[i].node);
    }

    r->pipeline_cache.init_node = pipeline_cache_entry_init;
    r->pipeline_cache.compare_nodes = pipeline_cache_entry_compare;
    r->pipeline_cache.post_node_evict = pipeline_cache_entry_post_evict;
}

static void finalize_pipeline_cache(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    lru_flush(&r->pipeline_cache);
    g_free(r->pipeline_cache_entries);
    r->pipeline_cache_entries = NULL;

    vkDestroyPipelineCache(r->device, r->vk_pipeline_cache, NULL);
}

static char const *const quad_glsl =
    "#version 450\n"
    "void main()\n"
    "{\n"
    "    float x = -1.0 + float((gl_VertexIndex & 1) << 2);\n"
    "    float y = -1.0 + float((gl_VertexIndex & 2) << 1);\n"
    "    gl_Position = vec4(x, y, 0, 1);\n"
    "}\n";

static char const *const solid_frag_glsl =
    "#version 450\n"
    "layout(location = 0) out vec4 fragColor;\n"
    "void main()\n"
    "{\n"
    "    fragColor = vec4(1.0);"
    "}\n";

static void init_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    r->quad_vert_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_VERTEX_BIT, quad_glsl);
    r->solid_frag_module = pgraph_vk_create_shader_module_from_glsl(
        r, VK_SHADER_STAGE_FRAGMENT_BIT, solid_frag_glsl);
}

static void finalize_clear_shaders(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    pgraph_vk_destroy_shader_module(r, r->quad_vert_module);
    pgraph_vk_destroy_shader_module(r, r->solid_frag_module);
}

static void init_render_passes(PGRAPHVkState *r)
{
    r->render_passes = g_array_new(false, false, sizeof(RenderPass));
}

static void finalize_render_passes(PGRAPHVkState *r)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        vkDestroyRenderPass(r->device, p->render_pass, NULL);
    }
    g_array_free(r->render_passes, true);
    r->render_passes = NULL;
}

void pgraph_vk_init_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    init_pipeline_cache(pg);
    init_clear_shaders(pg);
    init_render_passes(r);

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };
    VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                               &r->command_buffer_semaphore));

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    VK_CHECK(
        vkCreateFence(r->device, &fence_info, NULL, &r->command_buffer_fence));

    /* Chunk ring (docs Update 645); fences start signalled so the first
     * use of a slot does not wait. */
    VkFenceCreateInfo chunk_fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    for (int i = 0; i < RECOMP_VK_CHUNKS; i++) {
        VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                                   &r->chunk_sem_aux[i]));
        VK_CHECK(vkCreateSemaphore(r->device, &semaphore_info, NULL,
                                   &r->chunk_sem_main[i]));
        VK_CHECK(vkCreateFence(r->device, &chunk_fence_info, NULL,
                               &r->chunk_fence[i]));
    }

    const char *probe = getenv("RECOMP_VK_PASS_GPU_PROBE");
    r->pass_probe_enabled = probe && probe[0] == '1';
    r->pass_probe_pool = VK_NULL_HANDLE;
    r->pass_probe_active = -1;
    if (r->pass_probe_enabled) {
        VkQueryPoolCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = RECOMP_VK_PASS_PROBE_MAX * 2,
        };
        VK_CHECK(vkCreateQueryPool(r->device, &info, NULL,
                                   &r->pass_probe_pool));
    }
}

void pgraph_vk_finalize_pipelines(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->pass_probe_pool != VK_NULL_HANDLE) {
        vkDestroyQueryPool(r->device, r->pass_probe_pool, NULL);
        r->pass_probe_pool = VK_NULL_HANDLE;
    }

    finalize_clear_shaders(pg);
    finalize_pipeline_cache(pg);
    finalize_render_passes(r);

    vkDestroyFence(r->device, r->command_buffer_fence, NULL);
    vkDestroySemaphore(r->device, r->command_buffer_semaphore, NULL);
    for (int i = 0; i < RECOMP_VK_CHUNKS; i++) {
        vkDestroyFence(r->device, r->chunk_fence[i], NULL);
        vkDestroySemaphore(r->device, r->chunk_sem_aux[i], NULL);
        vkDestroySemaphore(r->device, r->chunk_sem_main[i], NULL);
    }
}

static void init_render_pass_state(PGRAPHState *pg, RenderPassState *state)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    state->color_format = r->color_binding ?
                              r->color_binding->host_fmt.vk_format :
                              VK_FORMAT_UNDEFINED;
    state->zeta_format = r->zeta_binding ? r->zeta_binding->host_fmt.vk_format :
                                           VK_FORMAT_UNDEFINED;
}

static VkRenderPass create_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    NV2A_VK_DPRINTF("Creating render pass");

    VkAttachmentDescription attachments[2];
    int num_attachments = 0;

    bool color = state->color_format != VK_FORMAT_UNDEFINED;
    bool zeta = state->zeta_format != VK_FORMAT_UNDEFINED;

    VkAttachmentReference color_reference;
    if (color) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->color_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        color_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };
        num_attachments++;
    }

    VkAttachmentReference depth_reference;
    if (zeta) {
        attachments[num_attachments] = (VkAttachmentDescription){
            .format = state->zeta_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        depth_reference = (VkAttachmentReference){
            num_attachments, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        num_attachments++;
    }

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
    };

    if (color) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask |= VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }

    if (zeta) {
        dependency.srcStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |=
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |=
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = color ? 1 : 0,
        .pColorAttachments = color ? &color_reference : NULL,
        .pDepthStencilAttachment = zeta ? &depth_reference : NULL,
    };

    VkRenderPassCreateInfo renderpass_create_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = num_attachments,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    VkRenderPass render_pass;
    VK_CHECK(vkCreateRenderPass(r->device, &renderpass_create_info, NULL,
                                &render_pass));
    return render_pass;
}

static VkRenderPass add_new_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    RenderPass new_pass;
    memcpy(&new_pass.state, state, sizeof(*state));
    new_pass.render_pass = create_render_pass(r, state);
    g_array_append_vals(r->render_passes, &new_pass, 1);
    return new_pass.render_pass;
}

static VkRenderPass get_render_pass(PGRAPHVkState *r, RenderPassState *state)
{
    for (int i = 0; i < r->render_passes->len; i++) {
        RenderPass *p = &g_array_index(r->render_passes, RenderPass, i);
        if (!memcmp(&p->state, state, sizeof(*state))) {
            return p->render_pass;
        }
    }
    return add_new_render_pass(r, state);
}

static void create_frame_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DPRINTF("Creating framebuffer");

    assert(r->color_binding || r->zeta_binding);

    if (r->framebuffer_index >= ARRAY_SIZE(r->framebuffers)) {
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
    }

    VkImageView attachments[2];
    int attachment_count = 0;

    if (r->color_binding) {
        attachments[attachment_count++] = r->color_binding->image_view;
    }
    if (r->zeta_binding) {
        attachments[attachment_count++] = r->zeta_binding->image_view;
    }

    /* xboxrecomp: no GNU ?: in MSVC */
    SurfaceBinding *binding = r->color_binding ? r->color_binding
                                               : r->zeta_binding;

    VkFramebufferCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = r->render_pass,
        .attachmentCount = attachment_count,
        .pAttachments = attachments,
        .width = binding->width,
        .height = binding->height,
        .layers = 1,
    };
    pgraph_apply_scaling_factor(pg, &create_info.width, &create_info.height);
    VK_CHECK(vkCreateFramebuffer(r->device, &create_info, NULL,
                                 &r->framebuffers[r->framebuffer_index++]));
}

static void destroy_framebuffers(PGRAPHState *pg)
{
    NV2A_VK_DPRINTF("Destroying framebuffer");
    PGRAPHVkState *r = pg->vk_renderer_state;

    for (int i = 0; i < r->framebuffer_index; i++) {
        vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
        r->framebuffers[i] = VK_NULL_HANDLE;
    }
    r->framebuffer_index = 0;
}

static void create_clear_pipeline(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    NV2A_VK_DGROUP_BEGIN("Creating clear pipeline");

    PipelineKey key;
    memset(&key, 0, sizeof(key));
    key.clear = true;
    init_render_pass_state(pg, &key.render_pass_state);

    key.regs[0] = r->clear_parameter;

    uint64_t hash = fast_hash((void *)&key, sizeof(key));
    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    PipelineBinding *snode = container_of(node, PipelineBinding, node);

    if (snode->pipeline != VK_NULL_HANDLE) {
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);
    memcpy(&snode->key, &key, sizeof(key));

    bool clear_any_color_channels =
        r->clear_parameter & NV097_CLEAR_SURFACE_COLOR;
    bool clear_all_color_channels =
        (r->clear_parameter & NV097_CLEAR_SURFACE_COLOR) ==
        (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G | NV097_CLEAR_SURFACE_B |
         NV097_CLEAR_SURFACE_A);
    bool partial_color_clear =
        clear_any_color_channels && !clear_all_color_channels;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[2];
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->quad_vert_module->module,
            .pName = "main",
        };
    if (partial_color_clear) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = r->solid_frag_module->module,
                .pName = "main",
            };
     }

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable =
            (r->clear_parameter & NV097_CLEAR_SURFACE_Z) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
    };

    if (r->clear_parameter & NV097_CLEAR_SURFACE_STENCIL) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        depth_stencil.front.failOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.passOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
        depth_stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
        depth_stencil.front.compareMask = 0xff;
        depth_stencil.front.writeMask = 0xff;
        depth_stencil.front.reference = 0xff;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_R)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_G)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_B)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (r->clear_parameter & NV097_CLEAR_SURFACE_A)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
        .blendEnable = VK_TRUE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
    };

    VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = partial_color_clear ? 3 : 2,
        .pDynamicStates = dynamic_states,
    };

    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_info, NULL, &pipeline));

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    NV2A_VK_DGROUP_END();
}

static bool check_render_pass_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->pipeline_binding);

    RenderPassState state;
    init_render_pass_state(pg, &state);

    return memcmp(&state, &r->pipeline_binding->key.render_pass_state,
                  sizeof(state)) != 0;
}

// Quickly check for any state changes that would require more analysis
static bool check_pipeline_dirty(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->pipeline_binding || r->shader_bindings_changed ||
        r->texture_bindings_changed || check_render_pass_dirty(pg)) {
        return true;
    }

    const unsigned int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
        NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };

    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        if (pgraph_is_reg_dirty(pg, regs[i])) {
            return true;
        }
    }

    // FIXME: Use dirty bits instead
    if (memcmp(r->vertex_attribute_descriptions,
               r->pipeline_binding->key.attribute_descriptions,
               r->num_active_vertex_attribute_descriptions *
                   sizeof(r->vertex_attribute_descriptions[0])) ||
        memcmp(r->vertex_binding_descriptions,
               r->pipeline_binding->key.binding_descriptions,
               r->num_active_vertex_binding_descriptions *
                   sizeof(r->vertex_binding_descriptions[0]))) {
        return true;
    }

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_NOTDIRTY);

    return false;
}

static void init_pipeline_key(PGRAPHState *pg, PipelineKey *key)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    memset(key, 0, sizeof(*key));
    init_render_pass_state(pg, &key->render_pass_state);
    memcpy(&key->shader_state, &r->shader_binding->state, sizeof(ShaderState));
    memcpy(key->binding_descriptions, r->vertex_binding_descriptions,
           sizeof(key->binding_descriptions[0]) *
               r->num_active_vertex_binding_descriptions);
    memcpy(key->attribute_descriptions, r->vertex_attribute_descriptions,
           sizeof(key->attribute_descriptions[0]) *
               r->num_active_vertex_attribute_descriptions);

    // FIXME: Register masking
    // FIXME: Use more dynamic state updates
    const int regs[] = {
        NV_PGRAPH_BLEND,       NV_PGRAPH_BLENDCOLOR,  NV_PGRAPH_CONTROL_0,
        NV_PGRAPH_CONTROL_1,   NV_PGRAPH_CONTROL_2,   NV_PGRAPH_CONTROL_3,
        NV_PGRAPH_SETUPRASTER, NV_PGRAPH_ZOFFSETBIAS, NV_PGRAPH_ZOFFSETFACTOR,
    };
    assert(ARRAY_SIZE(regs) == ARRAY_SIZE(key->regs));
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        key->regs[i] = pgraph_reg_r(pg, regs[i]);
    }
}

static void create_pipeline(PGRAPHState *pg)
{
    NV2A_VK_DGROUP_BEGIN("Creating pipeline");

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    static int timing_enabled = -1, sampled_frame = -1;
    static uint64_t phase_us[5], calls, unchanged, cache_hits;
    int64_t phase_start = 0;
    if (timing_enabled < 0) {
        const char *setting = getenv("RECOMP_VK_DRAW_CPU_SAMPLE");
        timing_enabled = setting && setting[0] == '1';
    }
    if (timing_enabled && pg->frame_time >= 120 &&
        pg->frame_time % 30 == 0) {
        if (sampled_frame != pg->frame_time) {
            if (sampled_frame >= 0) {
                fprintf(stderr,
                        "[VK-PIPELINE-PHASE] frame=%d calls=%llu "
                        "unchanged=%llu cache_hits=%llu textures=%llu_us "
                        "shaders=%llu_us dirty=%llu_us key_hash=%llu_us "
                        "lookup=%llu_us\n",
                        sampled_frame, (unsigned long long)calls,
                        (unsigned long long)unchanged,
                        (unsigned long long)cache_hits,
                        (unsigned long long)phase_us[0],
                        (unsigned long long)phase_us[1],
                        (unsigned long long)phase_us[2],
                        (unsigned long long)phase_us[3],
                        (unsigned long long)phase_us[4]);
            }
            memset(phase_us, 0, sizeof phase_us);
            calls = unchanged = cache_hits = 0;
            sampled_frame = pg->frame_time;
        }
        calls++;
        phase_start = nv2a_recomp_vk_now_us();
    }

    pgraph_vk_bind_textures(d);
    if (phase_start) {
        int64_t now = nv2a_recomp_vk_now_us();
        phase_us[0] += (uint64_t)(now - phase_start);
        phase_start = now;
    }
    pgraph_vk_bind_shaders(pg);
    if (phase_start) {
        int64_t now = nv2a_recomp_vk_now_us();
        phase_us[1] += (uint64_t)(now - phase_start);
        phase_start = now;
    }

    // FIXME: If nothing was dirty, don't even try creating the key or hashing.
    //        Just use the same pipeline.
    bool pipeline_dirty = check_pipeline_dirty(pg);

    pgraph_clear_dirty_reg_map(pg);
    // FIXME: We could clear less
    if (phase_start) {
        int64_t now = nv2a_recomp_vk_now_us();
        phase_us[2] += (uint64_t)(now - phase_start);
        phase_start = now;
    }

    if (r->pipeline_binding && !pipeline_dirty) {
        if (phase_start) unchanged++;
        NV2A_VK_DPRINTF("Cache hit");
        NV2A_VK_DGROUP_END();
        return;
    }

    PipelineKey key;
    init_pipeline_key(pg, &key);
    uint64_t hash = fast_hash((void *)&key, sizeof(key));
    if (phase_start) {
        int64_t now = nv2a_recomp_vk_now_us();
        phase_us[3] += (uint64_t)(now - phase_start);
        phase_start = now;
    }

    LruNode *node = lru_lookup(&r->pipeline_cache, hash, &key);
    if (phase_start) {
        phase_us[4] += (uint64_t)(nv2a_recomp_vk_now_us() - phase_start);
    }
    PipelineBinding *snode = container_of(node, PipelineBinding, node);
    if (snode->pipeline != VK_NULL_HANDLE) {
        if (phase_start) cache_hits++;
        NV2A_VK_DPRINTF("Cache hit");
        r->pipeline_binding_changed = r->pipeline_binding != snode;
        r->pipeline_binding = snode;
        NV2A_VK_DGROUP_END();
        return;
    }

    NV2A_VK_DPRINTF("Cache miss");
    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_GEN);

    memcpy(&snode->key, &key, sizeof(key));

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool depth_write = !!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE);
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;

    int num_active_shader_stages = 0;
    VkPipelineShaderStageCreateInfo shader_stages[3];

    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = r->shader_binding->vertex->module,
            .pName = "main",
        };
    if (r->shader_binding->geometry) {
        shader_stages[num_active_shader_stages++] =
            (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_GEOMETRY_BIT,
                .module = r->shader_binding->geometry->module,
                .pName = "main",
            };
    }
    shader_stages[num_active_shader_stages++] =
        (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = r->shader_binding->fragment->module,
            .pName = "main",
        };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount =
            r->num_active_vertex_binding_descriptions,
        .pVertexBindingDescriptions = r->vertex_binding_descriptions,
        .vertexAttributeDescriptionCount =
            r->num_active_vertex_attribute_descriptions,
        .pVertexAttributeDescriptions = r->vertex_attribute_descriptions,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = get_primitive_topology(pg),
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    void *rasterizer_next_struct = NULL;

    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_state;

    if (r->provoking_vertex_extension_enabled) {
        VkProvokingVertexModeEXT provoking_mode =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                     NV_PGRAPH_CONTROL_3_SHADEMODE) ==
                    NV_PGRAPH_CONTROL_3_SHADEMODE_FLAT ?
                VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT :
                VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;

        provoking_state =
            (VkPipelineRasterizationProvokingVertexStateCreateInfoEXT){
                .sType =
                    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
                .provokingVertexMode = provoking_mode,
            };
        rasterizer_next_struct = &provoking_state;
    } else {
        // FIXME: Handle in shader?
    }

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_TRUE,
        .rasterizerDiscardEnable = VK_FALSE,
        /* without fillModeNonSolid (Mali) wireframe/point fill falls back
         * to solid (docs Update 675) */
        .polygonMode = r->fill_mode_non_solid_enabled ?
                           pgraph_polygon_mode_vk_map[r->shader_binding->state
                                                          .polygon_front_mode] :
                           VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .frontFace = (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                      NV_PGRAPH_SETUPRASTER_FRONTFACE) ?
                         VK_FRONT_FACE_COUNTER_CLOCKWISE :
                         VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .pNext = rasterizer_next_struct,
    };

    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                      NV_PGRAPH_SETUPRASTER_CULLCTRL);
        assert(cull_face < ARRAY_SIZE(pgraph_cull_face_vk_map));
        rasterizer.cullMode = pgraph_cull_face_vk_map[cull_face];
    } else {
        rasterizer.cullMode = VK_CULL_MODE_NONE;
    }

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE,
    };

    if (depth_test) {
        depth_stencil.depthTestEnable = VK_TRUE;
        uint32_t depth_func =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0), NV_PGRAPH_CONTROL_0_ZFUNC);
        assert(depth_func < ARRAY_SIZE(pgraph_depth_func_vk_map));
        depth_stencil.depthCompareOp = pgraph_depth_func_vk_map[depth_func];
    }

    if (stencil_test) {
        depth_stencil.stencilTestEnable = VK_TRUE;
        uint32_t stencil_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                         NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
        uint32_t stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                        NV_PGRAPH_CONTROL_1_STENCIL_REF);
        uint32_t mask_read = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                      NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        uint32_t mask_write = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                       NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE);
        uint32_t op_fail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                    NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                     NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

        assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_vk_map));
        assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_vk_map));
        assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_vk_map));

        depth_stencil.front.failOp = pgraph_stencil_op_vk_map[op_fail];
        depth_stencil.front.passOp = pgraph_stencil_op_vk_map[op_zpass];
        depth_stencil.front.depthFailOp = pgraph_stencil_op_vk_map[op_zfail];
        depth_stencil.front.compareOp =
            pgraph_stencil_func_vk_map[stencil_func];
        depth_stencil.front.compareMask = mask_read;
        depth_stencil.front.writeMask = mask_write;
        depth_stencil.front.reference = stencil_ref;
        depth_stencil.back = depth_stencil.front;
    }

    VkColorComponentFlags write_mask = 0;
    if (control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if (control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE)
        write_mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = write_mask,
    };

    float blend_constant[4] = { 0, 0, 0, 0 };

    if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) {
        color_blend_attachment.blendEnable = VK_TRUE;

        uint32_t sfactor =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_DFACTOR);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_vk_map));
        color_blend_attachment.srcColorBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstColorBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];
        color_blend_attachment.srcAlphaBlendFactor =
            pgraph_blend_factor_vk_map[sfactor];
        color_blend_attachment.dstAlphaBlendFactor =
            pgraph_blend_factor_vk_map[dfactor];

        uint32_t equation =
            GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND), NV_PGRAPH_BLEND_EQN);
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_vk_map));

        color_blend_attachment.colorBlendOp =
            pgraph_blend_equation_vk_map[equation];
        color_blend_attachment.alphaBlendOp =
            pgraph_blend_equation_vk_map[equation];

        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        pgraph_argb_pack32_to_rgba_float(blend_color, blend_constant);
    }

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = r->color_binding ? 1 : 0,
        .pAttachments = r->color_binding ? &color_blend_attachment : NULL,
        .blendConstants[0] = blend_constant[0],
        .blendConstants[1] = blend_constant[1],
        .blendConstants[2] = blend_constant[2],
        .blendConstants[3] = blend_constant[3],
    };

    VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT,
                                         VK_DYNAMIC_STATE_SCISSOR };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = ARRAY_SIZE(dynamic_states),
        .pDynamicStates = dynamic_states,
    };

    // /* Polygon offset */
    // /* FIXME: GL implementation-specific, maybe do this in VS? */
    // if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //         NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE)
    // if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //         NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE)
    // if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //         NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE)
    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
        (NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE |
         NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE |
         NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE)) {
        uint32_t zfactor_u32 = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETFACTOR);
        float zfactor = *(float *)&zfactor_u32;
        uint32_t zbias_u32 = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETBIAS);
        float zbias = *(float *)&zbias_u32;
        rasterizer.depthBiasEnable = VK_TRUE;
        /* xboxrecomp (docs Update 636): the slope term multiplies the depth
         * slope per *host* pixel, which is 1/scale of the slope per guest
         * pixel the NV2A uses -- at render scale 3 shadow maps got a third
         * of their slope bias (shadow acne rings on distant floors).
         * RECOMP_VK_SCALE_SLOPE_BIAS=0 restores xemu's behaviour. */
        static int scale_slope = -1;
        if (scale_slope < 0) {
            const char *e = getenv("RECOMP_VK_SCALE_SLOPE_BIAS");
            scale_slope = !(e && e[0] == '0');
        }
        if (scale_slope) {
            zfactor *= (float)pg->surface_scale_factor;
        }
        /* RECOMP_SHADOW_BIAS_SCALE=<f> (docs Update 659, diagnosis):
         * multiply both bias terms, to test whether edge "teeth" in shadows
         * are self-shadowing that a larger offset removes. */
        static float bias_scale = -1.0f;
        if (bias_scale < 0.0f) {
            const char *e = getenv("RECOMP_SHADOW_BIAS_SCALE");
            bias_scale = e ? (float)atof(e) : 1.0f;
        }
        rasterizer.depthBiasSlopeFactor = zfactor * bias_scale;
        rasterizer.depthBiasConstantFactor = zbias * bias_scale;
    }

    // FIXME: Dither
    // if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
    //         NV_PGRAPH_CONTROL_0_DITHERENABLE))
    // FIXME: point size
    // FIXME: Edge Antialiasing
    // bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING),
    // NV_PGRAPH_ANTIALIASING_ENABLE);
    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE) {
    // FIXME: VK_EXT_line_rasterization
    // }

    // if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
    //                           NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE) {
    // FIXME: No direct analog. Just do it with MSAA.
    // }


    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &r->descriptor_set_layout,
    };

    VkPushConstantRange push_constant_range;
    if (r->shader_binding->state.use_push_constants_for_uniform_attrs) {
        int num_uniform_attributes =
            ctpop32(r->shader_binding->state.uniform_attrs); /* xboxrecomp: MSVC */
        if (num_uniform_attributes) {
            push_constant_range = (VkPushConstantRange){
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
                .offset = 0,
                // FIXME: Minimize push constants
                .size = num_uniform_attributes * 4 * sizeof(float),
            };
            pipeline_layout_info.pushConstantRangeCount = 1;
            pipeline_layout_info.pPushConstantRanges = &push_constant_range;
        }
    }

    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(r->device, &pipeline_layout_info, NULL,
                                    &layout));

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = num_active_shader_stages,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = r->zeta_binding ? &depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = layout,
        .renderPass = get_render_pass(r, &key.render_pass_state),
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };
    VkPipeline pipeline;
    static int pipe_timing = -1;
    if (pipe_timing < 0) {
        const char *e = getenv("RECOMP_VK_SHADER_TIMING");
        pipe_timing = e && e[0] == '1';
    }
    int64_t pipe_t0 = pipe_timing ? nv2a_recomp_vk_now_us() : 0;
    VK_CHECK(vkCreateGraphicsPipelines(r->device, r->vk_pipeline_cache, 1,
                                       &pipeline_create_info, NULL, &pipeline));
    if (pipe_timing) {
        fprintf(stderr, "[VK-PIPE-TIME] frame=%d create_us=%lld\n",
                pg->frame_time, (long long)(nv2a_recomp_vk_now_us() - pipe_t0));
    }

    snode->pipeline = pipeline;
    snode->layout = layout;
    snode->render_pass = pipeline_create_info.renderPass;
    snode->draw_time = pg->draw_time;

    r->pipeline_binding = snode;
    r->pipeline_binding_changed = true;

    NV2A_VK_DGROUP_END();
}

static void push_vertex_attr_values(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->shader_binding->state.use_push_constants_for_uniform_attrs) {
        return;
    }

    // FIXME: Partial updates

    float values[NV2A_VERTEXSHADER_ATTRIBUTES][4];
    int num_uniform_attrs = 0;

    pgraph_get_inline_values(pg, r->shader_binding->state.uniform_attrs, values,
                             &num_uniform_attrs);

    if (num_uniform_attrs > 0) {
        vkCmdPushConstants(r->command_buffer, r->pipeline_binding->layout,
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           num_uniform_attrs * 4 * sizeof(float),
                           &values);
    }
}

static void bind_descriptor_sets(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(r->descriptor_set_index >= 1);

    vkCmdBindDescriptorSets(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            r->pipeline_binding->layout, 0, 1,
                            &r->descriptor_sets[r->descriptor_set_index - 1], 0,
                            NULL);
}

static void begin_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(!r->query_in_flight);

    // FIXME: We should handle this. Make the query buffer bigger, but at least
    // flush current queries.
    assert(r->num_queries_in_flight < r->max_queries_in_flight);

    nv2a_profile_inc_counter(NV2A_PROF_QUERY);
    vkCmdResetQueryPool(r->command_buffer, r->query_pool,
                        r->num_queries_in_flight, 1);
    vkCmdBeginQuery(r->command_buffer, r->query_pool, r->num_queries_in_flight,
                    VK_QUERY_CONTROL_PRECISE_BIT);

    r->query_in_flight = true;
    r->new_query_needed = false;
    r->num_queries_in_flight++;
}

static void end_query(PGRAPHVkState *r)
{
    assert(r->in_command_buffer);
    assert(!r->in_render_pass);
    assert(r->query_in_flight);

    vkCmdEndQuery(r->command_buffer, r->query_pool,
                  r->num_queries_in_flight - 1);
    r->query_in_flight = false;
}

/* Copies the staging range appended since the last sync (docs Update 645:
 * a partial submit syncs what its chunk needs and keeps the offsets, so
 * later chunks append behind data the GPU may still be reading); `reset`
 * (full finish, GPU idle afterwards) rewinds the staging buffer. */
static void sync_staging_buffer(PGRAPHState *pg, VkCommandBuffer cmd,
                                int index_src, int index_dst, bool reset)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *b_src = &r->storage_buffers[index_src];
    StorageBuffer *b_dst = &r->storage_buffers[index_dst];
    VkDeviceSize from = b_src->synced_offset;

    if (b_src->buffer_offset <= from) {
        if (reset) {
            b_src->buffer_offset = 0;
            b_src->synced_offset = 0;
        }
        return;
    }

    VkBufferCopy copy_region = {
        .srcOffset = from,
        .dstOffset = from,
        .size = b_src->buffer_offset - from,
    };
    vkCmdCopyBuffer(cmd, b_src->buffer, b_dst->buffer, 1, &copy_region);

    VkAccessFlags dst_access_mask;
    VkPipelineStageFlags dst_stage_mask;

    switch (index_dst) {
    case BUFFER_INDEX:
        dst_access_mask = VK_ACCESS_INDEX_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_VERTEX_INLINE:
        dst_access_mask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        break;
    case BUFFER_UNIFORM:
        dst_access_mask = VK_ACCESS_UNIFORM_READ_BIT;
        dst_stage_mask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        break;
    default:
        assert(0);
        break;
    }

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = dst_access_mask,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = b_dst->buffer,
        .offset = from,
        .size = b_src->buffer_offset - from,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage_mask, 0,
                         0, NULL, 1, &barrier, 0, NULL);

    if (reset) {
        b_src->buffer_offset = 0;
        b_src->synced_offset = 0;
    } else {
        b_src->synced_offset = b_src->buffer_offset;
    }
}

static void flush_memory_buffer(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VK_CHECK(vmaFlushAllocation(
        r->allocator, r->storage_buffers[BUFFER_VERTEX_RAM].allocation, 0,
        VK_WHOLE_SIZE));

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = r->storage_buffers[BUFFER_VERTEX_RAM].buffer,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
                         &barrier, 0, NULL);
}

static void begin_render_pass(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(!r->in_render_pass);

    nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_RENDERPASSES);

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

    assert(r->framebuffer_index > 0);

    if (r->pass_probe_collect &&
        r->pass_probe_count < RECOMP_VK_PASS_PROBE_MAX) {
        uint32_t index = r->pass_probe_count++;
        r->pass_probe[index].first_query = r->pass_probe_query_count;
        r->pass_probe[index].frame = pg->frame_time;
        r->pass_probe[index].color_addr = r->color_binding ?
                                          r->color_binding->vram_addr : 0;
        r->pass_probe[index].zeta_addr = r->zeta_binding ?
                                         r->zeta_binding->vram_addr : 0;
        r->pass_probe[index].width = vp_width;
        r->pass_probe[index].height = vp_height;
        r->pass_probe[index].end_line = 0;
        r->pass_probe[index].draws = 0;
        r->pass_probe_active = (int)index;
        vkCmdWriteTimestamp(r->command_buffer,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            r->pass_probe_pool, r->pass_probe_query_count++);
    }

    VkRenderPassBeginInfo render_pass_begin_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = r->render_pass,
        .framebuffer = r->framebuffers[r->framebuffer_index - 1],
        .renderArea.extent.width = vp_width,
        .renderArea.extent.height = vp_height,
        .clearValueCount = 0,
        .pClearValues = NULL,
    };
    vkCmdBeginRenderPass(r->command_buffer, &render_pass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);
    r->in_render_pass = true;

}

/* Clears inside the running render pass (ARM port, docs Update 678): a
 * clear used to end the pass, clear in a pass of its own and end that too --
 * on a tiling GPU (Mali) two extra full load/store rounds of the target per
 * clear. vkCmdClearAttachments is valid inside the pass; the framebuffer and
 * render-pass checks in begin_pre_draw still end it when the clear targets
 * something else. RECOMP_VK_CLEAR_IN_PASS=0 restores the old behaviour. */
static bool clear_in_pass(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_VK_CLEAR_IN_PASS");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* the probe records where each pass ended (RECOMP_VK_PASS_GPU_PROBE) */
#define end_render_pass(r) end_render_pass_at((r), __LINE__)
static void end_render_pass_at(PGRAPHVkState *r, int line)
{
    if (r->in_render_pass) {
        vkCmdEndRenderPass(r->command_buffer);
        if (r->pass_probe_active >= 0) {
            r->pass_probe[r->pass_probe_active].end_line = line;
            vkCmdWriteTimestamp(r->command_buffer,
                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                r->pass_probe_pool, r->pass_probe_query_count++);
            r->pass_probe_active = -1;
        }
        r->in_render_pass = false;
    }
}

const enum NV2A_PROF_COUNTERS_ENUM finish_reason_to_counter_enum[] = {
    [VK_FINISH_REASON_VERTEX_BUFFER_DIRTY] = NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY,
    [VK_FINISH_REASON_SURFACE_CREATE] = NV2A_PROF_FINISH_SURFACE_CREATE,
    [VK_FINISH_REASON_SURFACE_DOWN] = NV2A_PROF_FINISH_SURFACE_DOWN,
    [VK_FINISH_REASON_NEED_BUFFER_SPACE] = NV2A_PROF_FINISH_NEED_BUFFER_SPACE,
    [VK_FINISH_REASON_FRAMEBUFFER_DIRTY] = NV2A_PROF_FINISH_FRAMEBUFFER_DIRTY,
    [VK_FINISH_REASON_PRESENTING] = NV2A_PROF_FINISH_PRESENTING,
    [VK_FINISH_REASON_FLIP_STALL] = NV2A_PROF_FINISH_FLIP_STALL,
    [VK_FINISH_REASON_FLUSH] = NV2A_PROF_FINISH_FLUSH,
    [VK_FINISH_REASON_STALLED] = NV2A_PROF_FINISH_STALLED,
};

/* Ends the current main command buffer and submits it with its staging
 * sync (docs Update 645). Each submit waits for the previous partial
 * chunk's main-done semaphore, so the GPU executes the chunks strictly in
 * recording order, as one command buffer before. `final` (pgraph_vk_finish)
 * rewinds the staging buffers and clears the upload bitmap -- only valid
 * because the caller then waits for the returned fence. */
static VkFence submit_chunk(PGRAPHState *pg, bool final)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    int s = r->chunk_slot;
    VkCommandBuffer aux = r->chunk_aux[s];

    VK_CHECK(vkEndCommandBuffer(r->command_buffer));

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(aux, &begin_info));
    sync_staging_buffer(pg, aux, BUFFER_INDEX_STAGING, BUFFER_INDEX, final);
    sync_staging_buffer(pg, aux, BUFFER_VERTEX_INLINE_STAGING,
                        BUFFER_VERTEX_INLINE, final);
    sync_staging_buffer(pg, aux, BUFFER_UNIFORM_STAGING, BUFFER_UNIFORM,
                        final);
    if (final) {
        bitmap_clear(r->uploaded_bitmap, 0, r->bitmap_size);
    }
    flush_memory_buffer(pg, aux);
    VK_CHECK(vkEndCommandBuffer(aux));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSemaphore prev = r->chunk_wait_sem;
    VkSubmitInfo submit_infos[] = {
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = prev != VK_NULL_HANDLE ? 1 : 0,
            .pWaitSemaphores = &prev,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &aux,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &r->chunk_sem_aux[s],
        },
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &r->chunk_sem_aux[s],
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &r->command_buffer,
            .signalSemaphoreCount = final ? 0 : 1,
            .pSignalSemaphores = &r->chunk_sem_main[s],
        }
    };
    nv2a_profile_inc_counter(NV2A_PROF_QUEUE_SUBMIT);
    VK_CHECK(vkResetFences(r->device, 1, &r->chunk_fence[s]));
    VK_CHECK(vkQueueSubmit(r->queue, ARRAY_SIZE(submit_infos), submit_infos,
                           r->chunk_fence[s]));
    r->chunk_wait_sem = final ? VK_NULL_HANDLE : r->chunk_sem_main[s];
    r->chunk_slot = (s + 1) % RECOMP_VK_CHUNKS;
    return r->chunk_fence[s];
}

void pgraph_vk_finish(PGRAPHState *pg, FinishReason finish_reason)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    static int timing_enabled = -1;
    static struct { uint64_t count, wait_us, max_us; }
        wait_by_reason[VK_FINISH_REASON_STALLED + 1];
    static uint64_t total_waits;
    if (timing_enabled < 0) {
        const char *e = getenv("RECOMP_VK_STAGE_TIMING");
        timing_enabled = e && e[0] == '1';
    }

    assert(!r->in_draw);
    assert(r->debug_depth == 0);

    if (r->in_command_buffer) {
        nv2a_profile_inc_counter(finish_reason_to_counter_enum[finish_reason]);

        if (r->in_render_pass) {
            end_render_pass(r);
        }
        if (r->query_in_flight) {
            end_query(r);
        }
        VkFence chunk_fence = submit_chunk(pg, true);
        r->submit_count += 1;

        bool check_budget = false;

        // Periodically check memory budget
        const int max_num_submits_before_budget_update = 5;
        if (finish_reason == VK_FINISH_REASON_FLIP_STALL ||
            (r->submit_count - r->allocator_last_submit_index) >
                max_num_submits_before_budget_update) {

            // VMA queries budget via vmaSetCurrentFrameIndex
            vmaSetCurrentFrameIndex(r->allocator, r->submit_count);
            r->allocator_last_submit_index = r->submit_count;
            check_budget = true;
        }

        bool frame_pace = nv2a_recomp_frame_pace_enabled();
        int64_t wait_start = (timing_enabled || frame_pace) ?
            nv2a_recomp_vk_now_us() : 0;
        /* The last chunk's fence: the chunks are chained, so all earlier
         * partial submits have completed too. */
        VK_CHECK(vkWaitForFences(r->device, 1, &chunk_fence,
                                 VK_TRUE, UINT64_MAX));
        r->chunks_partial = 0;
        if (frame_pace)
            nv2a_recomp_frame_pace_wait(false, finish_reason,
                (uint64_t)(nv2a_recomp_vk_now_us() - wait_start));
        /* RECOMP_VK_FINISH_TIMELINE=1 (docs Update 641): every 60th frame,
         * each full GPU wait with its reason, its time since the previous
         * FLIP_STALL and the draws recorded since then. */
        {
            static int timeline = -1;
            static int64_t last_flip_us;
            static uint64_t last_flip_draw;
            if (timeline < 0) {
                const char *e = getenv("RECOMP_VK_FINISH_TIMELINE");
                timeline = e && e[0] == '1';
            }
            if (timeline) {
                int64_t now = nv2a_recomp_vk_now_us();
                if (pg->frame_time % 60 == 0) {
                    fprintf(stderr, "[VK-FINISH] frame=%d reason=%d since_flip_us=%lld "
                            "wait_us=%lld draws=%llu\n", pg->frame_time,
                            (int)finish_reason, (long long)(now - last_flip_us),
                            (long long)(now - wait_start),
                            (unsigned long long)(pg->draw_time - last_flip_draw));
                }
                if (finish_reason == VK_FINISH_REASON_FLIP_STALL) {
                    last_flip_us = now;
                    last_flip_draw = pg->draw_time;
                }
            }
        }
        if (r->pass_probe_collect && r->pass_probe_query_count) {
            uint64_t stamps[RECOMP_VK_PASS_PROBE_MAX * 2];
            VkResult result = vkGetQueryPoolResults(
                r->device, r->pass_probe_pool, 0, r->pass_probe_query_count,
                sizeof stamps, stamps, sizeof stamps[0],
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (result == VK_SUCCESS) {
                /* panvk reports timestampPeriod 0 although the counter runs
                 * (Mali: the 24 MHz system timer); RECOMP_VK_TS_PERIOD_NS
                 * overrides, default 41.667 ns then (ARM port, docs
                 * Update 678) */
                double period = r->device_props.limits.timestampPeriod;
                if (period <= 0.0) {
                    const char *e = getenv("RECOMP_VK_TS_PERIOD_NS");
                    period = e ? atof(e) : 1000.0 / 24.0;
                }
                for (uint32_t i = 0; i < r->pass_probe_count; i++) {
                    uint32_t first = r->pass_probe[i].first_query;
                    double gpu_us = (double)(stamps[first + 1] -
                                             stamps[first]) *
                                    period / 1000.0;
                    fprintf(stderr,
                            "[VK-PASS-GPU] frame=%u color=%08llX "
                            "zeta=%08llX size=%ux%u gpu_us=%.1f "
                            "draws=%u end=%d\n",
                            r->pass_probe[i].frame,
                            (unsigned long long)r->pass_probe[i].color_addr,
                            (unsigned long long)r->pass_probe[i].zeta_addr,
                            r->pass_probe[i].width,
                            r->pass_probe[i].height, gpu_us,
                            r->pass_probe[i].draws,
                            r->pass_probe[i].end_line);
                }
            } else {
                fprintf(stderr, "[VK-PASS-GPU] query_error=%d\n", result);
            }
        }
        if (timing_enabled) {
            uint64_t elapsed = (uint64_t)(nv2a_recomp_vk_now_us() - wait_start);
            wait_by_reason[finish_reason].count++;
            wait_by_reason[finish_reason].wait_us += elapsed;
            wait_by_reason[finish_reason].max_us =
                MAX(wait_by_reason[finish_reason].max_us, elapsed);
            if (++total_waits == 300) {
                uint64_t sum_us = 0;
                for (int i = 0; i <= VK_FINISH_REASON_STALLED; i++) {
                    if (!wait_by_reason[i].count) continue;
                    sum_us += wait_by_reason[i].wait_us;
                    fprintf(stderr, "[VK-WAIT-MAIN] frame=%d reason=%d "
                            "calls=%llu total_ms=%.3f max_us=%llu\n",
                            pg->frame_time, i,
                            (unsigned long long)wait_by_reason[i].count,
                            wait_by_reason[i].wait_us / 1000.0,
                            (unsigned long long)wait_by_reason[i].max_us);
                }
                fprintf(stderr, "[VK-WAIT-MAIN] frame=%d all_calls=300 "
                        "total_ms=%.3f\n", pg->frame_time,
                        sum_us / 1000.0);
                memset(wait_by_reason, 0, sizeof wait_by_reason);
                total_waits = 0;
            }
        }

        r->descriptor_set_index = 0;
        r->in_command_buffer = false;
        destroy_framebuffers(pg);
        pgraph_vk_retire_deferred_invalid_surfaces(pg);

        if (check_budget) {
            pgraph_vk_check_memory_budget(pg);
        }
    }

    NV2AState *d = container_of(pg, NV2AState, pgraph);
    pgraph_vk_process_pending_reports_internal(d);

    pgraph_vk_compute_finish_complete(r);
}

/* Opens the ring slot's main command buffer; the slot's previous use (a
 * chunk RECOMP_VK_CHUNKS submits ago) must have completed. */
static void begin_chunk(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    int s = r->chunk_slot;

    VK_CHECK(vkWaitForFences(r->device, 1, &r->chunk_fence[s], VK_TRUE,
                             UINT64_MAX));
    r->command_buffer = r->chunk_cmd[s];

    VkCommandBufferBeginInfo command_buffer_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(r->command_buffer,
                                  &command_buffer_begin_info));
    r->chunk_draw_start = pg->draw_time;
}

/* RECOMP_VK_PARTIAL_SUBMIT=<draws> (default 150, 0 = off; docs Update 645):
 * after that many draws since the current chunk began, hand the recorded
 * part to the GPU without waiting, so the GPU works while the rest of the
 * frame is still being recorded and a later wait (surface readback, flip)
 * only covers what is left. Not while a visibility query or the pass probe
 * is open -- both are collected at the next full finish. */
static void maybe_partial_submit(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    static int threshold = -1;

    if (threshold < 0) {
        const char *e = getenv("RECOMP_VK_PARTIAL_SUBMIT");
        threshold = e ? atoi(e) : 150;
        if (threshold < 0)
            threshold = 0;
    }
    if (!threshold || !r->in_command_buffer || r->in_draw ||
        r->in_aux_command_buffer || r->query_in_flight ||
        r->pass_probe_collect || r->debug_depth != 0 ||
        pg->draw_time - r->chunk_draw_start < (unsigned)threshold) {
        return;
    }

    end_render_pass(r);
    submit_chunk(pg, false);
    r->chunks_partial++;
    begin_chunk(pg);
}

void pgraph_vk_begin_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(!r->in_command_buffer);

    begin_chunk(pg);
    r->pass_probe_collect = r->pass_probe_enabled &&
                            pg->frame_time >= 120 &&
                            pg->frame_time % 30 == 0;
    r->pass_probe_query_count = 0;
    r->pass_probe_count = 0;
    r->pass_probe_active = -1;
    if (r->pass_probe_collect) {
        vkCmdResetQueryPool(r->command_buffer, r->pass_probe_pool, 0,
                            RECOMP_VK_PASS_PROBE_MAX * 2);
    }
    r->command_buffer_start_time = pg->draw_time;
    r->in_command_buffer = true;
}

// FIXME: Refactor below

void pgraph_vk_ensure_command_buffer(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (!r->in_command_buffer) {
        pgraph_vk_begin_command_buffer(pg);
    }
}

void pgraph_vk_ensure_not_in_render_pass_at(PGRAPHState *pg, int line)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    end_render_pass_at(r, line);
    if (r->query_in_flight) {
        end_query(r);
    }
}

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    pgraph_vk_ensure_command_buffer(pg);
    pgraph_vk_ensure_not_in_render_pass(pg);
    return r->command_buffer;
}

void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    assert(cmd == r->command_buffer);
}

// FIXME: Add more metrics for determining command buffer 'fullness' and
// conservatively flush. Unfortunately there doesn't appear to be a good
// way to determine what the actual maximum capacity of a command buffer
// is, but we are obviously not supposed to endlessly append to one command
// buffer. For other reasons though (like descriptor set amount, surface
// changes, etc) we do flush often.

static void begin_pre_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    static int timing_enabled = -1;
    static int sampled_frame = -1;
    static uint64_t phase_us[4], calls;
    int64_t phase_start = 0;
    if (timing_enabled < 0) {
        const char *setting = getenv("RECOMP_VK_DRAW_CPU_SAMPLE");
        timing_enabled = setting && setting[0] == '1';
    }
    if (timing_enabled && pg->frame_time >= 120 &&
        pg->frame_time % 30 == 0) {
        if (sampled_frame != pg->frame_time) {
            if (sampled_frame >= 0) {
                fprintf(stderr,
                        "[VK-PREDRAW-PHASE] frame=%d calls=%llu "
                        "pipeline=%llu_us framebuffer=%llu_us "
                        "descriptors=%llu_us command=%llu_us\n",
                        sampled_frame, (unsigned long long)calls,
                        (unsigned long long)phase_us[0],
                        (unsigned long long)phase_us[1],
                        (unsigned long long)phase_us[2],
                        (unsigned long long)phase_us[3]);
            }
            memset(phase_us, 0, sizeof phase_us);
            calls = 0;
            sampled_frame = pg->frame_time;
        }
        calls++;
        phase_start = nv2a_recomp_vk_now_us();
    }

    assert(r->color_binding || r->zeta_binding);
    assert(!r->color_binding || r->color_binding->initialized);
    assert(!r->zeta_binding || r->zeta_binding->initialized);

    if (pg->clearing) {
        create_clear_pipeline(pg);
    } else {
        create_pipeline(pg);
    }
    if (phase_start) {
        int64_t now = nv2a_recomp_vk_now_us();
        phase_us[0] += (uint64_t)(now - phase_start);
        phase_start = now;
    }

    bool render_pass_dirty = r->pipeline_binding->render_pass != r->render_pass;

    if (r->framebuffer_dirty || render_pass_dirty) {
        pgraph_vk_ensure_not_in_render_pass(pg);
    }
    if (render_pass_dirty) {
        r->render_pass = r->pipeline_binding->render_pass;
    }
    if (r->framebuffer_dirty) {
        create_frame_buffer(pg);
        r->framebuffer_dirty = false;
    }
    if (phase_start) {
        int64_t now = nv2a_recomp_vk_now_us();
        phase_us[1] += (uint64_t)(now - phase_start);
        phase_start = now;
    }
    if (!pg->clearing) {
        pgraph_vk_update_descriptor_sets(pg);
    }
    if (phase_start) {
        int64_t now = nv2a_recomp_vk_now_us();
        phase_us[2] += (uint64_t)(now - phase_start);
        phase_start = now;
    }
    if (r->framebuffer_index == 0) {
        create_frame_buffer(pg);
    }

    pgraph_vk_ensure_command_buffer(pg);
    if (phase_start) {
        phase_us[3] += (uint64_t)(nv2a_recomp_vk_now_us() - phase_start);
    }
}

static void begin_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);

    // Visibility testing
    if (!pg->clearing && pg->zpass_pixel_count_enable) {
        if (r->new_query_needed && r->query_in_flight) {
            end_render_pass(r);
            end_query(r);
        }
        if (!r->query_in_flight) {
            end_render_pass(r);
            begin_query(r);
        }
    } else if (r->query_in_flight) {
        end_render_pass(r);
        end_query(r);
    }

    if (pg->clearing && !clear_in_pass()) {
        end_render_pass(r);
    }

    bool must_bind_pipeline = r->pipeline_binding_changed;

    if (!r->in_render_pass) {
        begin_render_pass(pg);
        must_bind_pipeline = true;
    }
    if (r->pass_probe_active >= 0)
        r->pass_probe[r->pass_probe_active].draws++;

    if (must_bind_pipeline) {
        nv2a_profile_inc_counter(NV2A_PROF_PIPELINE_BIND);
        vkCmdBindPipeline(r->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          r->pipeline_binding->pipeline);
        r->pipeline_binding->draw_time = pg->draw_time;

        unsigned int vp_width = pg->surface_binding_dim.width,
                     vp_height = pg->surface_binding_dim.height;
        pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);

        /* xboxrecomp: pixel-offset correction for render scale > 1 from
         * the xemu #949 patch v3 (r3538987, 2026-09-21; docs Update 486):
         * removes the glow/green border at the screen edges in SCDA. */
        /* RECOMP_VK_SCALE_FIX_VP="a,b,stretch" (docs Update 652): offset
         * a*(scale-1)+b, and whether the far edge stays put (stretch=1) or
         * moves along. Default 0.5,0.25,1 = the #949 patch: switching it off
         * (Update 653) brought green veils back at the left/top screen edges
         * and flicker at the left edge of each co-op view (user, Update 654). */
        static float fix_a = 0.5f, fix_b = 0.25f;
        static int fix_stretch = 1, fix_parsed;
        if (!fix_parsed) {
            const char *e = getenv("RECOMP_VK_SCALE_FIX_VP");
            fix_parsed = 1;
            if (e && sscanf(e, "%f,%f,%d", &fix_a, &fix_b, &fix_stretch) == 3)
                fprintf(stderr, "[VK] scale-fix viewport a=%.3f b=%.3f "
                        "stretch=%d\n", fix_a, fix_b, fix_stretch);
        }
        float vp_offset = pgraph_vk_scale_fix_enabled(pg)
                              ? fix_a * (pg->surface_scale_factor - 1) + fix_b
                              : 0.0f;
        /* Not into the small swizzled targets that texture.c samples at
         * native size (bloom chain): there the offset is ~0.4 texel per pass
         * and the glow drifted up-left over five passes (docs Update 655).
         * RECOMP_VK_SCALE_FIX_VP_SMALL=1 applies it there too. */
        {
            static int small_too = -1;
            if (small_too < 0) {
                const char *e = getenv("RECOMP_VK_SCALE_FIX_VP_SMALL");
                small_too = e && e[0] == '1';
            }
            SurfaceBinding *t = r->color_binding;
            if (!small_too && t && t->swizzle && t->width <= 256 &&
                t->height <= 256) {
                vp_offset = 0.0f;
            }
        }
        VkViewport viewport = {
            .x = -vp_offset,
            .y = -vp_offset,
            .width = vp_width + (fix_stretch ? vp_offset : 0.0f),
            .height = vp_height + (fix_stretch ? vp_offset : 0.0f),
            .minDepth = 0.0,
            .maxDepth = 1.0,
        };
        vkCmdSetViewport(r->command_buffer, 0, 1, &viewport);

        /* Surface clip */
        /* FIXME: Consider moving to PSH w/ window clip */
        unsigned int xmin = pg->surface_shape.clip_x,
                     ymin = pg->surface_shape.clip_y;

        unsigned int scissor_width = pg->surface_shape.clip_width,
                     scissor_height = pg->surface_shape.clip_height;

        pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
        pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

        pgraph_apply_scaling_factor(pg, &xmin, &ymin);
        pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

        VkRect2D scissor = {
            .offset.x = xmin,
            .offset.y = ymin,
            .extent.width = scissor_width,
            .extent.height = scissor_height,
        };
        vkCmdSetScissor(r->command_buffer, 0, 1, &scissor);
    }

    if (!pg->clearing) {
        bind_descriptor_sets(pg);
        push_vertex_attr_values(pg);
    }

    r->in_draw = true;
}

static void end_draw(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    assert(r->in_command_buffer);
    assert(r->in_render_pass);

    if (pg->clearing && !clear_in_pass()) {
        end_render_pass(r);
    }

    r->in_draw = false;
}

void pgraph_vk_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    if (is_nop_draw) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        NV2A_VK_DPRINTF("nop draw!\n");
        return;
    }

    pgraph_vk_flush_draw(d);

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_vk_set_surface_dirty(pg, color_write, depth_test || stencil_test);
    maybe_partial_submit(pg);
}

static int compare_memory_sync_requirement_by_addr(const void *p1,
                                                   const void *p2)
{
    const MemorySyncRequirement *l = p1, *r = p2;
    if (l->addr < r->addr)
        return -1;
    if (l->addr > r->addr)
        return 1;
    return 0;
}

/* RECOMP_VK_DRAW_CPU_SAMPLE counters (docs Update 651): vertex-RAM range
 * tests, re-uploads and their bytes, attribute remaps and copied vertices. */
static uint64_t s_vram_sync_tests, s_vram_sync_uploads, s_vram_sync_bytes;
static uint64_t s_vram_sync_test_bytes, s_vram_sync_max_range;
static uint64_t s_remap_draws, s_remap_vertices;

static void sync_vertex_ram_buffer(PGRAPHState *pg)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_vertex_ram_buffer_syncs == 0) {
        return;
    }

    // Align sync requirements to page boundaries
    NV2A_VK_DGROUP_BEGIN("Sync vertex RAM buffer");

    for (int i = 0; i < r->num_vertex_ram_buffer_syncs; i++) {
        NV2A_VK_DPRINTF("Need to sync vertex memory @%" HWADDR_PRIx
                        ", %" HWADDR_PRIx " bytes",
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size);

        hwaddr start_addr =
            r->vertex_ram_buffer_syncs[i].addr & TARGET_PAGE_MASK;
        hwaddr end_addr = r->vertex_ram_buffer_syncs[i].addr +
                          r->vertex_ram_buffer_syncs[i].size;
        end_addr = ROUND_UP(end_addr, TARGET_PAGE_SIZE);

        NV2A_VK_DPRINTF("- %d: %08" HWADDR_PRIx " %zd bytes"
                          " -> %08" HWADDR_PRIx " %zd bytes", i,
                        r->vertex_ram_buffer_syncs[i].addr,
                        r->vertex_ram_buffer_syncs[i].size, start_addr,
                        end_addr - start_addr);

        r->vertex_ram_buffer_syncs[i].addr = start_addr;
        r->vertex_ram_buffer_syncs[i].size = end_addr - start_addr;
    }

    // Sort the requirements in increasing order of addresses
    qsort(r->vertex_ram_buffer_syncs, r->num_vertex_ram_buffer_syncs,
          sizeof(MemorySyncRequirement),
          compare_memory_sync_requirement_by_addr);

    // Merge overlapping/adjacent requests to minimize number of tests
    MemorySyncRequirement merged[16];
    int num_syncs = 1;

    merged[0] = r->vertex_ram_buffer_syncs[0];

    for (int i = 1; i < r->num_vertex_ram_buffer_syncs; i++) {
        MemorySyncRequirement *p = &merged[num_syncs - 1];
        MemorySyncRequirement *t = &r->vertex_ram_buffer_syncs[i];

        if (t->addr <= (p->addr + p->size)) {
            // Merge with previous
            hwaddr p_end_addr = p->addr + p->size;
            hwaddr t_end_addr = t->addr + t->size;
            hwaddr new_end_addr = MAX(p_end_addr, t_end_addr);
            p->size = new_end_addr - p->addr;
        } else {
            merged[num_syncs++] = *t;
        }
    }

    if (num_syncs < r->num_vertex_ram_buffer_syncs) {
        NV2A_VK_DPRINTF("Reduced to %d sync checks", num_syncs);
    }

    for (int i = 0; i < num_syncs; i++) {
        hwaddr addr = merged[i].addr;
        VkDeviceSize size = merged[i].size;

        NV2A_VK_DPRINTF("- %d: %08"HWADDR_PRIx" %zd bytes", i, addr, size);

        s_vram_sync_tests++;
        s_vram_sync_test_bytes += size;
        s_vram_sync_max_range = MAX(s_vram_sync_max_range, size);
        if (memory_region_test_and_clear_dirty(d->vram, addr, size,
                                               DIRTY_MEMORY_NV2A)) {
            NV2A_VK_DPRINTF("Memory dirty. Synchronizing...");
            s_vram_sync_uploads++;
            s_vram_sync_bytes += size;
            pgraph_vk_update_vertex_ram_buffer(pg, addr, d->vram_ptr + addr,
                                               size);
        }
    }

    r->num_vertex_ram_buffer_syncs = 0;

    NV2A_VK_DGROUP_END();
}

void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;

    nv2a_profile_inc_counter(NV2A_PROF_CLEAR);

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    pg->clearing = true;
    r->clear_parameter = parameter;

    static int z16_reshape_trace = -1;
    if (z16_reshape_trace < 0) {
        const char *e = getenv("RECOMP_VK_Z16_RESHAPE_TRACE");
        z16_reshape_trace = e && e[0] == '1';
    }
    if (z16_reshape_trace && write_zeta) {
        uint32_t rect_x = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
        uint32_t rect_y = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);
        fprintf(stderr,
                "[VK-Z16-CLEAR-INPUT] frame=%d param=%08X "
                "rect=%u..%u,%u..%u prior_zeta=%ux%u "
                "clip=%ux%u+%u+%u\n",
                pg->frame_time, parameter,
                GET_MASK(rect_x, NV_PGRAPH_CLEARRECTX_XMIN),
                GET_MASK(rect_x, NV_PGRAPH_CLEARRECTX_XMAX),
                GET_MASK(rect_y, NV_PGRAPH_CLEARRECTY_YMIN),
                GET_MASK(rect_y, NV_PGRAPH_CLEARRECTY_YMAX),
                r->zeta_binding ? r->zeta_binding->width : 0,
                r->zeta_binding ? r->zeta_binding->height : 0,
                pg->surface_shape.clip_width,
                pg->surface_shape.clip_height,
                pg->surface_shape.clip_x,
                pg->surface_shape.clip_y);
    }

    // FIXME: If doing a full surface clear, mark the surface for full clear
    // and we can just do the clear as part of the surface load.
    pgraph_vk_surface_update(d, true, write_color, write_zeta);

    /* xboxrecomp: no GNU ?: in MSVC */
    SurfaceBinding *binding = r->color_binding ? r->color_binding
                                               : r->zeta_binding;
    if (!binding) {
        /* Nothing bound to clear */
        pg->clearing = false;
        return;
    }

    uint32_t clearrectx = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX);
    uint32_t clearrecty = pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY);

    unsigned int xmin = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax = GET_MASK(clearrectx, NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax = GET_MASK(clearrecty, NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_VK_DGROUP_BEGIN("CLEAR min=(%d,%d) max=(%d,%d)%s%s", xmin, ymin, xmax,
                         ymax, write_color ? " color" : "",
                         write_zeta ? " zeta" : "");

    begin_pre_draw(pg);
    pgraph_vk_begin_debug_marker(r, r->command_buffer,
        RGBA_BLUE, "Clear %08" HWADDR_PRIx,
        binding->vram_addr);
    begin_draw(pg);

    // FIXME: What does hardware do when min >= max?
    // FIXME: What does hardware do when min >= surface size?
    xmin = MIN(xmin, binding->width - 1);
    ymin = MIN(ymin, binding->height - 1);
    xmax = MAX(xmin, MIN(xmax, binding->width - 1));
    ymax = MAX(ymin, MIN(ymax, binding->height - 1));

    unsigned int scissor_width = MAX(0, xmax - xmin + 1);
    unsigned int scissor_height = MAX(0, ymax - ymin + 1);

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    VkClearRect clear_rect = {
        .rect = {
            .offset = { .x = xmin, .y = ymin },
            .extent = { .width = scissor_width, .height = scissor_height },
        },
        .baseArrayLayer = 0,
        .layerCount = 1,
    };

    int num_attachments = 0;
    VkClearAttachment attachments[2];

    if (write_color && r->color_binding) {
        const bool clear_all_color_channels =
            (parameter & NV097_CLEAR_SURFACE_COLOR) ==
            (NV097_CLEAR_SURFACE_R | NV097_CLEAR_SURFACE_G |
             NV097_CLEAR_SURFACE_B | NV097_CLEAR_SURFACE_A);

        if (clear_all_color_channels) {
            attachments[num_attachments] = (VkClearAttachment){
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .colorAttachment = 0,
            };
            pgraph_get_clear_color(
                pg, attachments[num_attachments].clearValue.color.float32);
            num_attachments++;
        } else {
            float blend_constants[4];
            pgraph_get_clear_color(pg, blend_constants);
            vkCmdSetScissor(r->command_buffer, 0, 1, &clear_rect.rect);
            vkCmdSetBlendConstants(r->command_buffer, blend_constants);
            vkCmdDraw(r->command_buffer, 3, 1, 0, 0);
        }
    }

    if (write_zeta && r->zeta_binding) {
        int stencil_value = 0;
        float depth_value = 1.0;
        pgraph_get_clear_depth_stencil_value(pg, &depth_value, &stencil_value);

        VkImageAspectFlags aspect = 0;
        if (parameter & NV097_CLEAR_SURFACE_Z) {
            aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((parameter & NV097_CLEAR_SURFACE_STENCIL) &&
            (r->zeta_binding->host_fmt.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        attachments[num_attachments++] = (VkClearAttachment){
            .aspectMask = aspect,
            .clearValue.depthStencil.depth = depth_value,
            .clearValue.depthStencil.stencil = stencil_value,
        };
    }

    if (num_attachments) {
        vkCmdClearAttachments(r->command_buffer, num_attachments, attachments,
                              1, &clear_rect);
    }
    end_draw(pg);
    pgraph_vk_end_debug_marker(r, r->command_buffer);

    pg->clearing = false;

    pgraph_vk_set_surface_dirty(pg, write_color, write_zeta);

    NV2A_VK_DGROUP_END();
}

#if 0
static void pgraph_vk_debug_attrs(NV2AState *d)
{
    for (int vertex_idx = 0; vertex_idx < pg->draw_arrays_count[i]; vertex_idx++) {
        NV2A_VK_DGROUP_BEGIN("Vertex %d+%d", pg->draw_arrays_start[i], vertex_idx);
        for (int attr_idx = 0; attr_idx < NV2A_VERTEXSHADER_ATTRIBUTES; attr_idx++) {
            VertexAttribute *attr = &pg->vertex_attributes[attr_idx];
            if (attr->count) {
                char *p = (char *)d->vram_ptr + r->attribute_offsets[attr_idx] + (pg->draw_arrays_start[i] + vertex_idx) * attr->stride;
                NV2A_VK_DGROUP_BEGIN("Attribute %d data at %tx", attr_idx, (ptrdiff_t)(p - (char*)d->vram_ptr));
                for (int count_idx = 0; count_idx < attr->count; count_idx++) {
                    switch (attr->format) {
                    case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
                        NV2A_VK_DPRINTF("[%d] %f", count_idx, *(float*)p);
                        p += sizeof(float);
                        break;
                    default:
                        assert(0);
                        break;
                    }
                }
                NV2A_VK_DGROUP_END();
            }
        }
        NV2A_VK_DGROUP_END();
    }
}
#endif

static void bind_vertex_buffer(PGRAPHState *pg, uint16_t inline_map,
                               VkDeviceSize offset)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    if (r->num_active_vertex_binding_descriptions == 0) {
        return;
    }

    VkBuffer buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    for (int i = 0; i < r->num_active_vertex_binding_descriptions; i++) {
        int attr_idx = r->vertex_attribute_descriptions[i].location;
        int buffer_idx = (inline_map & (1 << attr_idx)) ? BUFFER_VERTEX_INLINE :
                                                          BUFFER_VERTEX_RAM;
        buffers[i] = r->storage_buffers[buffer_idx].buffer;
        offsets[i] = offset + r->vertex_attribute_offsets[attr_idx];
    }

    vkCmdBindVertexBuffers(r->command_buffer, 0,
                           r->num_active_vertex_binding_descriptions, buffers,
                           offsets);
}

static void bind_inline_vertex_buffer(PGRAPHState *pg, VkDeviceSize offset)
{
    bind_vertex_buffer(pg, 0xffff, offset);
}

void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta)
{
    NV2A_DPRINTF("pgraph_set_surface_dirty(%d, %d) -- %d %d\n", color, zeta,
                 pgraph_color_write_enabled(pg), pgraph_zeta_write_enabled(pg));

    PGRAPHVkState *r = pg->vk_renderer_state;

    /* FIXME: Does this apply to CLEARs too? */
    color = color && pgraph_color_write_enabled(pg);
    zeta = zeta && pgraph_zeta_write_enabled(pg);
    pg->surface_color.draw_dirty |= color;
    pg->surface_zeta.draw_dirty |= zeta;

    if (r->color_binding) {
        r->color_binding->draw_dirty |= color;
        if (color) /* xboxrecomp: GPU copy now newer (docs Update 492) */
            nv2a_recomp_access_watch(r->color_binding->access_cb, 2);
        r->color_binding->frame_time = pg->frame_time;
        r->color_binding->cleared = false;
    }

    if (r->zeta_binding) {
        r->zeta_binding->draw_dirty |= zeta;
        if (zeta) /* xboxrecomp: GPU copy now newer (docs Update 492) */
            nv2a_recomp_access_watch(r->zeta_binding->access_cb, 2);
        r->zeta_binding->frame_time = pg->frame_time;
        r->zeta_binding->cleared = false;
    }
}

/* RECOMP_VK_VERTEX_INSTREAM (default on, =0 off; docs Update 651): vertex
 * RAM that changes again while draws recorded since the last full finish
 * may still read the old bytes used to cost a full GPU wait
 * (VK_FINISH_REASON_VERTEX_BUFFER_DIRTY, 4-5 per co-op frame for the last
 * ~100 draws). Instead the new bytes go to the inline staging buffer (only
 * rewound at a full finish, so they stay valid until the GPU ran this
 * command buffer) and a transfer recorded at this point of the command
 * stream copies them into BUFFER_VERTEX_RAM: earlier draws read the old
 * bytes, later draws the new ones. Needs host-coherent vertex RAM, since
 * flush_memory_buffer would otherwise write cached host lines back over the
 * GPU's copy. Returns false when the caller must finish instead. */
bool pgraph_vk_stage_vertex_ram_update(PGRAPHState *pg, hwaddr offset,
                                       const void *data, VkDeviceSize size)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *staging = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];
    VkBuffer vram = r->storage_buffers[BUFFER_VERTEX_RAM].buffer;
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("RECOMP_VK_VERTEX_INSTREAM");
        VkMemoryPropertyFlags flags = 0;
        vmaGetAllocationMemoryProperties(
            r->allocator, r->storage_buffers[BUFFER_VERTEX_RAM].allocation,
            &flags);
        enabled = !(e && e[0] == '0') &&
                  (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        fprintf(stderr, "[VK] in-stream vertex RAM updates %s%s\n",
                enabled ? "on" : "off",
                (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "" :
                " (vertex RAM not host-coherent)");
    }
    if (!enabled || !size || !r->in_command_buffer || r->in_draw ||
        r->query_in_flight ||
        !pgraph_vk_buffer_has_space_for(pg, BUFFER_VERTEX_INLINE_STAGING,
                                        size, 16)) {
        return false;
    }

    VkDeviceSize src = ROUND_UP(staging->buffer_offset, 16);
    memcpy(staging->mapped + src, data, size);
    staging->buffer_offset = src + size;

    end_render_pass(r);

    VkBufferMemoryBarrier before = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = vram,
        .offset = offset,
        .size = size,
    };
    vkCmdPipelineBarrier(r->command_buffer, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1,
                         &before, 0, NULL);
    VkBufferCopy region = { .srcOffset = src, .dstOffset = offset,
                            .size = size };
    vkCmdCopyBuffer(r->command_buffer, staging->buffer, vram, 1, &region);
    VkBufferMemoryBarrier after = before;
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(r->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1,
                         &after, 0, NULL);
    return true;
}

static bool ensure_buffer_space(PGRAPHState *pg, int index, VkDeviceSize size)
{
    if (!pgraph_vk_buffer_has_space_for(pg, index, size, 1)) {
        /* RECOMP_VK_FINISH_TIMELINE=1: which buffer ran full (Update 651) */
        static int trace = -1;
        static unsigned traced;
        if (trace < 0) {
            const char *e = getenv("RECOMP_VK_FINISH_TIMELINE");
            trace = e && e[0] == '1';
        }
        if (trace && traced++ < 40) {
            PGRAPHVkState *r = pg->vk_renderer_state;
            fprintf(stderr, "[VK-BUFFER-FULL] frame=%d buffer=%d offset=%llu "
                    "need=%llu size=%llu\n", pg->frame_time, index,
                    (unsigned long long)r->storage_buffers[index].buffer_offset,
                    (unsigned long long)size,
                    (unsigned long long)r->storage_buffers[index].buffer_size);
        }
        pgraph_vk_finish(pg, VK_FINISH_REASON_NEED_BUFFER_SPACE);
        return true;
    }

    return false;
}

static void get_size_and_count_for_format(VkFormat fmt, size_t *size, size_t *count)
{
    static const struct {
        size_t size;
        size_t count;
    } table[] = {
        [VK_FORMAT_R8_UNORM] =              { 1, 1 },
        [VK_FORMAT_R8G8_UNORM] =            { 1, 2 },
        [VK_FORMAT_R8G8B8_UNORM] =          { 1, 3 },
        [VK_FORMAT_R8G8B8A8_UNORM] =        { 1, 4 },
        [VK_FORMAT_R16_SNORM] =             { 2, 1 },
        [VK_FORMAT_R16G16_SNORM] =          { 2, 2 },
        [VK_FORMAT_R16G16B16_SNORM] =       { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SNORM] =    { 2, 4 },
        [VK_FORMAT_R16_SSCALED] =           { 2, 1 },
        [VK_FORMAT_R16G16_SSCALED] =        { 2, 2 },
        [VK_FORMAT_R16G16B16_SSCALED] =     { 2, 3 },
        [VK_FORMAT_R16G16B16A16_SSCALED] =  { 2, 4 },
        [VK_FORMAT_R32_SFLOAT] =            { 4, 1 },
        [VK_FORMAT_R32G32_SFLOAT] =         { 4, 2 },
        [VK_FORMAT_R32G32B32_SFLOAT] =      { 4, 3 },
        [VK_FORMAT_R32G32B32A32_SFLOAT] =   { 4, 4 },
        [VK_FORMAT_R32_SINT] =              { 4, 1 },
    };

    assert(fmt < ARRAY_SIZE(table));
    assert(table[fmt].size);

    *size = table[fmt].size;
    *count = table[fmt].count;
}

typedef struct VertexBufferRemap {
    uint16_t attributes;
    size_t buffer_space_required;
    struct {
        VkDeviceAddress offset;
        VkDeviceSize old_stride;
        VkDeviceSize new_stride;
    } map[NV2A_VERTEXSHADER_ATTRIBUTES];
} VertexBufferRemap;

static VertexBufferRemap remap_unaligned_attributes(PGRAPHState *pg,
                                                    uint32_t num_vertices)
{
    PGRAPHVkState *r = pg->vk_renderer_state;

    VertexBufferRemap remap = {0};

    VkDeviceAddress output_offset = 0;

    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        int desc_loc = r->vertex_attribute_to_description_location[attr_id];
        if (desc_loc < 0) {
            continue;
        }

        VkVertexInputBindingDescription *desc =
            &r->vertex_binding_descriptions[desc_loc];
        VkVertexInputAttributeDescription *attr =
            &r->vertex_attribute_descriptions[desc_loc];

        size_t element_size, element_count;
        get_size_and_count_for_format(attr->format, &element_size, &element_count);

        bool offset_valid =
            (r->vertex_attribute_offsets[attr_id] % element_size == 0);
        bool stride_valid = (desc->stride % element_size == 0);

        if (offset_valid && stride_valid) {
            continue;
        }

        remap.attributes |= 1 << attr_id;
        remap.map[attr_id].offset = ROUND_UP(output_offset, element_size);
        remap.map[attr_id].old_stride = desc->stride;
        remap.map[attr_id].new_stride = element_size * element_count;

        // fprintf(stderr,
        //         "attr %02d remapped: "
        //         "%08" HWADDR_PRIx "->%08" HWADDR_PRIx " "
        //         "stride=%d->%zd\n",
        //         attr_id, r->vertex_attribute_offsets[attr_id],
        //         remap.map[attr_id].offset,
        //         remap.map[attr_id].old_stride,
        //         remap.map[attr_id].new_stride);

        output_offset =
            remap.map[attr_id].offset + remap.map[attr_id].new_stride * num_vertices;
        desc->stride = remap.map[attr_id].new_stride;
    }

    remap.buffer_space_required = output_offset;

    // reserve space
    if (remap.attributes) {
        StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];
        VkDeviceSize starting_offset = ROUND_UP(buffer->buffer_offset, 16);
        size_t total_space_required =
            (starting_offset - buffer->buffer_offset) + remap.buffer_space_required;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, total_space_required);
        buffer->buffer_offset = ROUND_UP(buffer->buffer_offset, 16);
    }

    return remap;
}

static void copy_remapped_attributes_to_inline_buffer(PGRAPHState *pg,
                                                      VertexBufferRemap remap,
                                                      uint32_t start_vertex,
                                                      uint32_t num_vertices)
{
    NV2AState *d = container_of(pg, NV2AState, pgraph);
    PGRAPHVkState *r = pg->vk_renderer_state;
    StorageBuffer *buffer = &r->storage_buffers[BUFFER_VERTEX_INLINE_STAGING];

    if (!remap.attributes) {
        return;
    }
    s_remap_draws++;
    s_remap_vertices += num_vertices;

    assert(pgraph_vk_buffer_has_space_for(pg, BUFFER_VERTEX_INLINE_STAGING,
                                          remap.buffer_space_required, 256));

    // FIXME: SIMD memcpy
    // FIXME: Caching
    // FIXME: Account for only what is drawn
    assert(start_vertex == 0);
    assert(buffer->mapped);

    // Copy vertex data
    for (int attr_id = 0; attr_id < NV2A_VERTEXSHADER_ATTRIBUTES; attr_id++) {
        if (!(remap.attributes & (1 << attr_id))) {
            continue;
        }

        VkDeviceSize attr_buffer_offset =
            buffer->buffer_offset + remap.map[attr_id].offset;

        uint8_t *out_ptr = buffer->mapped + attr_buffer_offset;
        uint8_t *in_ptr = d->vram_ptr + r->vertex_attribute_offsets[attr_id];

        for (int vertex_id = 0; vertex_id < num_vertices; vertex_id++) {
            memcpy(out_ptr, in_ptr, remap.map[attr_id].new_stride);
            out_ptr += remap.map[attr_id].new_stride;
            in_ptr += remap.map[attr_id].old_stride;
        }

        r->vertex_attribute_offsets[attr_id] = attr_buffer_offset;
    }


    buffer->buffer_offset += remap.buffer_space_required;
}

/* Mali mode (docs Update 675): without geometry shaders, quads and quad
 * strips are drawn as an indexed triangle list built here. Smooth quads keep
 * the geometry shader's 0-2 diagonal; flat ones split along 1-3 so every
 * triangle ends on the quad's last vertex, which the provoking-vertex
 * extension (last) then uses for the flat colour, as the shader did. */
/* Read from the PGRAPH state the shader state is built from (shaders.c):
 * the bound shader binding may still be the previous draw's here. */
static bool needs_quad_split(PGRAPHState *pg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (r->geometry_shader_supported)
        return false;
    int pm = pg->primitive_mode;
    int poly = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                        NV_PGRAPH_SETUPRASTER_FRONTFACEMODE);
    return (pm == PRIM_TYPE_QUADS || pm == PRIM_TYPE_QUAD_STRIP) &&
           poly != POLY_MODE_POINT;
}

static uint32_t *s_quad_idx;
static size_t s_quad_cap;

/* Appends the triangles of `count` vertices -- src[i], or first + i when src
 * is NULL -- after the n indices already built; returns the new total. */
static size_t append_quad_split(PGRAPHState *pg, size_t n,
                                const uint32_t *src, uint32_t first,
                                uint32_t count)
{
    bool strip = pg->primitive_mode == PRIM_TYPE_QUAD_STRIP;
    bool flat = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                         NV_PGRAPH_CONTROL_3_SHADEMODE) !=
                NV_PGRAPH_CONTROL_3_SHADEMODE_SMOOTH;
    size_t quads = strip ? (count >= 4 ? (count - 2) / 2 : 0) : count / 4;
    size_t need = n + quads * 6;

    if (need > s_quad_cap) {
        s_quad_cap = need * 2 + 1024;
        s_quad_idx = (uint32_t *)realloc(s_quad_idx, s_quad_cap * sizeof(uint32_t));
    }
    uint32_t *o = s_quad_idx + n;
    for (size_t q = 0; q < quads; q++) {
        size_t b = strip ? q * 2 : q * 4;
        uint32_t v[4];
        for (int k = 0; k < 4; k++)
            v[k] = src ? src[b + k] : first + (uint32_t)(b + k);
        /* quad corner order: 0,1,2,3 for quads, 0,1,3,2 for strips */
        static const uint8_t quad_smooth[6] = { 0, 1, 2, 0, 2, 3 };
        static const uint8_t quad_flat[6]   = { 0, 1, 3, 1, 2, 3 };
        static const uint8_t strip_smooth[6] = { 0, 1, 2, 1, 3, 2 };
        static const uint8_t strip_flat[6]   = { 0, 1, 3, 2, 0, 3 };
        const uint8_t *t = strip ? (flat ? strip_flat : strip_smooth) :
                                   (flat ? quad_flat : quad_smooth);
        for (int k = 0; k < 6; k++)
            *o++ = v[t[k]];
    }
    return need;
}

/* Stages the split indices for the index buffer; returns their offset there.
 * Called after begin_pre_draw and before begin_draw, the way the
 * inline-elements path stages the game's own indices. */
static VkDeviceSize upload_quad_split(PGRAPHState *pg, size_t n)
{
    return pgraph_vk_update_index_buffer(pg, s_quad_idx,
                                         n * sizeof(s_quad_idx[0]));
}

void pgraph_vk_flush_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHVkState *r = pg->vk_renderer_state;
    static int cpu_sample_enabled = -1;
    static int sampled_frame = -1;
    static uint64_t sampled_us[3], sampled_count[3];
    static uint64_t sampled_inline_phase_us[6];
    static uint64_t sampled_arrays_phase_us[6];
    static struct {
        uint64_t color_addr, zeta_addr;
        unsigned int color_width, color_height, zeta_width, zeta_height;
        uint64_t inline_count, inline_us;
    } sampled_targets[32];
    static int sampled_target_count;
    static struct {
        ShaderBinding *binding;
        uint64_t hash, count, cpu_us;
        bool lighting, vertex_program, fixed_function;
    } sampled_main_shaders[128];
    static int sampled_main_shader_count;
    int sample_kind = 0;
    int sample_target = -1;
    int sample_main_shader = -1;
    int64_t sample_start = 0;

    if (cpu_sample_enabled < 0) {
        const char *setting = getenv("RECOMP_VK_DRAW_CPU_SAMPLE");
        cpu_sample_enabled = setting && setting[0] == '1';
    }

    if (!(r->color_binding || r->zeta_binding)) {
        NV2A_VK_DPRINTF("No binding present!!!\n");
        return;
    }

    /* Measure one complete draw-setup frame in thirty, without adding a
     * timer call to every draw of normal gameplay.  This distinguishes
     * CPU-side inline-geometry work from the separately measured Vulkan
     * fence/queue waits.  Opt-in diagnostic only; no rendering change. */
    if (cpu_sample_enabled && pg->frame_time >= 120 &&
        pg->frame_time % 30 == 0) {
        if (sampled_frame != pg->frame_time) {
            if (sampled_frame >= 0) {
                fprintf(stderr,
                        "[VK-DRAW-CPU] frame=%d arrays=%llu/%llu_us "
                        "inline=%llu/%llu_us other=%llu/%llu_us\n",
                        sampled_frame,
                        (unsigned long long)sampled_count[0],
                        (unsigned long long)sampled_us[0],
                        (unsigned long long)sampled_count[1],
                        (unsigned long long)sampled_us[1],
                        (unsigned long long)sampled_count[2],
                        (unsigned long long)sampled_us[2]);
                fprintf(stderr,
                        "[VK-INLINE-PHASE] frame=%d scan=%llu_us "
                        "attributes=%llu_us sync_remap=%llu_us "
                        "pre_draw=%llu_us buffer_copy=%llu_us "
                        "draw_commands=%llu_us\n",
                        sampled_frame,
                        (unsigned long long)sampled_inline_phase_us[0],
                        (unsigned long long)sampled_inline_phase_us[1],
                        (unsigned long long)sampled_inline_phase_us[2],
                        (unsigned long long)sampled_inline_phase_us[3],
                        (unsigned long long)sampled_inline_phase_us[4],
                        (unsigned long long)sampled_inline_phase_us[5]);
                fprintf(stderr,
                        "[VK-ARRAYS-PHASE] frame=%d attributes=%llu_us "
                        "sync=%llu_us remap=%llu_us pre_draw=%llu_us "
                        "copy=%llu_us draw_commands=%llu_us\n",
                        sampled_frame,
                        (unsigned long long)sampled_arrays_phase_us[0],
                        (unsigned long long)sampled_arrays_phase_us[1],
                        (unsigned long long)sampled_arrays_phase_us[2],
                        (unsigned long long)sampled_arrays_phase_us[3],
                        (unsigned long long)sampled_arrays_phase_us[4],
                        (unsigned long long)sampled_arrays_phase_us[5]);
                fprintf(stderr,
                        "[VK-VERTEX-SYNC] frame=%d tests=%llu uploads=%llu "
                        "bytes=%llu tested=%llu max_range=%llu "
                        "remap_draws=%llu remap_vertices=%llu (30 frames)\n",
                        sampled_frame,
                        (unsigned long long)s_vram_sync_tests,
                        (unsigned long long)s_vram_sync_uploads,
                        (unsigned long long)s_vram_sync_bytes,
                        (unsigned long long)s_vram_sync_test_bytes,
                        (unsigned long long)s_vram_sync_max_range,
                        (unsigned long long)s_remap_draws,
                        (unsigned long long)s_remap_vertices);
                for (int i = 0; i < sampled_target_count; i++) {
                    fprintf(stderr,
                            "[VK-DRAW-TARGET] frame=%d color=%08llX/%ux%u "
                            "zeta=%08llX/%ux%u inline=%llu cpu_us=%llu\n",
                            sampled_frame,
                            (unsigned long long)sampled_targets[i].color_addr,
                            sampled_targets[i].color_width,
                            sampled_targets[i].color_height,
                            (unsigned long long)sampled_targets[i].zeta_addr,
                            sampled_targets[i].zeta_width,
                            sampled_targets[i].zeta_height,
                            (unsigned long long)sampled_targets[i].inline_count,
                            (unsigned long long)sampled_targets[i].inline_us);
                }
                for (int rank = 0; rank < 8; rank++) {
                    int best = -1;
                    for (int i = 0; i < sampled_main_shader_count; i++) {
                        if (!sampled_main_shaders[i].count) continue;
                        if (best < 0 || sampled_main_shaders[i].count >
                                        sampled_main_shaders[best].count)
                            best = i;
                    }
                    if (best < 0) break;
                    fprintf(stderr,
                            "[VK-MAIN-SHADER] frame=%d rank=%d hash=%016llX "
                            "draws=%llu cpu_us=%llu lighting=%d vprog=%d fixed=%d\n",
                            sampled_frame, rank + 1,
                            (unsigned long long)sampled_main_shaders[best].hash,
                            (unsigned long long)sampled_main_shaders[best].count,
                            (unsigned long long)sampled_main_shaders[best].cpu_us,
                            sampled_main_shaders[best].lighting,
                            sampled_main_shaders[best].vertex_program,
                            sampled_main_shaders[best].fixed_function);
                    sampled_main_shaders[best].count = 0;
                }
            }
            memset(sampled_us, 0, sizeof sampled_us);
            memset(sampled_count, 0, sizeof sampled_count);
            memset(sampled_inline_phase_us, 0, sizeof sampled_inline_phase_us);
            memset(sampled_arrays_phase_us, 0, sizeof sampled_arrays_phase_us);
            sampled_target_count = 0;
            sampled_main_shader_count = 0;
            sampled_frame = pg->frame_time;
            s_vram_sync_tests = s_vram_sync_uploads = s_vram_sync_bytes = 0;
            s_vram_sync_test_bytes = s_vram_sync_max_range = 0;
            s_remap_draws = s_remap_vertices = 0;
        }
        sample_kind = pg->draw_arrays_length ? 0 :
                      pg->inline_elements_length ? 1 : 2;
        if (sample_kind == 1) {
            uint64_t color_addr = r->color_binding ?
                r->color_binding->vram_addr : 0;
            uint64_t zeta_addr = r->zeta_binding ?
                r->zeta_binding->vram_addr : 0;
            for (int i = 0; i < sampled_target_count; i++) {
                if (sampled_targets[i].color_addr == color_addr &&
                    sampled_targets[i].zeta_addr == zeta_addr) {
                    sample_target = i;
                    break;
                }
            }
            if (sample_target < 0 && sampled_target_count <
                ARRAY_SIZE(sampled_targets)) {
                sample_target = sampled_target_count++;
                sampled_targets[sample_target].color_addr = color_addr;
                sampled_targets[sample_target].zeta_addr = zeta_addr;
                sampled_targets[sample_target].color_width = r->color_binding ?
                    r->color_binding->width : 0;
                sampled_targets[sample_target].color_height = r->color_binding ?
                    r->color_binding->height : 0;
                sampled_targets[sample_target].zeta_width = r->zeta_binding ?
                    r->zeta_binding->width : 0;
                sampled_targets[sample_target].zeta_height = r->zeta_binding ?
                    r->zeta_binding->height : 0;
                sampled_targets[sample_target].inline_count = 0;
                sampled_targets[sample_target].inline_us = 0;
            }
            if (color_addr == 0x048C0000 && zeta_addr == 0x04B18000 &&
                r->shader_binding) {
                for (int i = 0; i < sampled_main_shader_count; i++) {
                    if (sampled_main_shaders[i].binding == r->shader_binding) {
                        sample_main_shader = i;
                        break;
                    }
                }
                if (sample_main_shader < 0 && sampled_main_shader_count <
                    ARRAY_SIZE(sampled_main_shaders)) {
                    sample_main_shader = sampled_main_shader_count++;
                    sampled_main_shaders[sample_main_shader].binding =
                        r->shader_binding;
                    sampled_main_shaders[sample_main_shader].hash =
                        fast_hash((const uint8_t *)&r->shader_binding->state,
                                  sizeof(ShaderState));
                    sampled_main_shaders[sample_main_shader].count = 0;
                    sampled_main_shaders[sample_main_shader].cpu_us = 0;
                    sampled_main_shaders[sample_main_shader].lighting =
                        r->shader_binding->state.lighting;
                    sampled_main_shaders[sample_main_shader].vertex_program =
                        r->shader_binding->state.vertex_program;
                    sampled_main_shaders[sample_main_shader].fixed_function =
                        r->shader_binding->state.fixed_function;
                }
            }
        }
        sample_start = nv2a_recomp_vk_now_us();
    }

    r->num_vertex_ram_buffer_syncs = 0;

    if (pg->draw_arrays_length) {
        NV2A_VK_DGROUP_BEGIN("Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);

        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        bool quad_split = needs_quad_split(pg);
        size_t quad_split_n = 0;
        if (quad_split) {
            for (int i = 0; i < pg->draw_arrays_length; i++)
                quad_split_n = append_quad_split(pg, quad_split_n, NULL,
                                                 pg->draw_arrays_start[i],
                                                 pg->draw_arrays_count[i]);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                                quad_split_n * sizeof(uint32_t));
        }

        int64_t phase_start = sample_start;
#define ARRAYS_PHASE(i) do { if (phase_start) { \
            int64_t now_ = nv2a_recomp_vk_now_us(); \
            sampled_arrays_phase_us[i] += (uint64_t)(now_ - phase_start); \
            phase_start = now_; } } while (0)
        pgraph_vk_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                         pg->draw_arrays_max_count - 1, false,
                                         0, pg->draw_arrays_max_count - 1);
        uint32_t min_element = INT_MAX;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            min_element = MIN(pg->draw_arrays_start[i], min_element);
            max_element = MAX(max_element, pg->draw_arrays_start[i] + pg->draw_arrays_count[i]);
        }
        ARRAYS_PHASE(0);
        sync_vertex_ram_buffer(pg);
        ARRAYS_PHASE(1);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element);
        ARRAYS_PHASE(2);

        begin_pre_draw(pg);
        ARRAYS_PHASE(3);
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element);
        VkDeviceSize quad_split_offset =
            quad_split ? upload_quad_split(pg, quad_split_n) : 0;
        ARRAYS_PHASE(4);
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Draw Arrays");
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);
        if (quad_split) {
            vkCmdBindIndexBuffer(r->command_buffer,
                                 r->storage_buffers[BUFFER_INDEX].buffer,
                                 quad_split_offset, VK_INDEX_TYPE_UINT32);
            if (quad_split_n)
                vkCmdDrawIndexed(r->command_buffer, (uint32_t)quad_split_n,
                                 1, 0, 0, 0);
        } else
        for (int i = 0; i < pg->draw_arrays_length; i++) {
            uint32_t start = pg->draw_arrays_start[i],
                     count = pg->draw_arrays_count[i];
            NV2A_VK_DPRINTF("- [%d] Start:%d Count:%d", i, start, count);
            vkCmdDraw(r->command_buffer, count, 1, start, 0);
        }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        ARRAYS_PHASE(5);
#undef ARRAYS_PHASE

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_elements_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Elements");
        int64_t phase_start = sample_start;
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);

        size_t index_data_size =
            pg->inline_elements_length * sizeof(pg->inline_elements[0]);
        /* Mali mode: the game's indices, split into triangles */
        bool quad_split = needs_quad_split(pg);
        size_t quad_split_n = 0;
        if (quad_split) {
            quad_split_n = append_quad_split(pg, 0, pg->inline_elements, 0,
                                             pg->inline_elements_length);
            index_data_size = quad_split_n * sizeof(uint32_t);
        }

        ensure_buffer_space(pg, BUFFER_INDEX_STAGING, index_data_size);

        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i = 0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }
        if (phase_start) {
            int64_t now = nv2a_recomp_vk_now_us();
            sampled_inline_phase_us[0] += (uint64_t)(now - phase_start);
            phase_start = now;
        }
        pgraph_vk_bind_vertex_attributes(
            d, min_element, max_element, false, 0,
            pg->inline_elements[pg->inline_elements_length - 1]);
        if (phase_start) {
            int64_t now = nv2a_recomp_vk_now_us();
            sampled_inline_phase_us[1] += (uint64_t)(now - phase_start);
            phase_start = now;
        }
        sync_vertex_ram_buffer(pg);
        VertexBufferRemap remap = remap_unaligned_attributes(pg, max_element + 1);
        if (phase_start) {
            int64_t now = nv2a_recomp_vk_now_us();
            sampled_inline_phase_us[2] += (uint64_t)(now - phase_start);
            phase_start = now;
        }

        begin_pre_draw(pg);
        if (phase_start) {
            int64_t now = nv2a_recomp_vk_now_us();
            sampled_inline_phase_us[3] += (uint64_t)(now - phase_start);
            phase_start = now;
        }
        copy_remapped_attributes_to_inline_buffer(pg, remap, 0, max_element + 1);
        VkDeviceSize buffer_offset =
            quad_split ? upload_quad_split(pg, quad_split_n) :
                         pgraph_vk_update_index_buffer(
                             pg, pg->inline_elements, index_data_size);
        if (phase_start) {
            int64_t now = nv2a_recomp_vk_now_us();
            sampled_inline_phase_us[4] += (uint64_t)(now - phase_start);
            phase_start = now;
        }
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Elements");
        begin_draw(pg);
        bind_vertex_buffer(pg, remap.attributes, 0);
        vkCmdBindIndexBuffer(r->command_buffer,
                             r->storage_buffers[BUFFER_INDEX].buffer,
                             buffer_offset, VK_INDEX_TYPE_UINT32);
        uint32_t index_count = quad_split ? (uint32_t)quad_split_n :
                                            pg->inline_elements_length;
        if (index_count)
            vkCmdDrawIndexed(r->command_buffer, index_count, 1, 0, 0, 0);
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        if (phase_start) {
            sampled_inline_phase_us[5] +=
                (uint64_t)(nv2a_recomp_vk_now_us() - phase_start);
        }

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_buffer_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        size_t vertex_data_size = pg->inline_buffer_length * sizeof(float) * 4;
        void *data[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t sizes[NV2A_VERTEXSHADER_ATTRIBUTES];
        size_t offset = 0;

        pgraph_vk_bind_vertex_attributes_inline(d);
        for (int i = 0; i < r->num_active_vertex_attribute_descriptions; i++) {
            int attr_index = r->vertex_attribute_descriptions[i].location;

            VertexAttribute *attr = &pg->vertex_attributes[attr_index];
            r->vertex_attribute_offsets[attr_index] = offset;

            data[i] = attr->inline_buffer;
            sizes[i] = vertex_data_size;

            attr->inline_buffer_populated = false;
            offset += vertex_data_size;
        }
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING, offset);
        bool quad_split = needs_quad_split(pg);
        size_t quad_split_n = 0;
        if (quad_split) {
            quad_split_n = append_quad_split(pg, 0, NULL, 0,
                                             pg->inline_buffer_length);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                                quad_split_n * sizeof(uint32_t));
        }

        begin_pre_draw(pg);
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, data, sizes, r->num_active_vertex_attribute_descriptions);
        VkDeviceSize quad_split_offset =
            quad_split ? upload_quad_split(pg, quad_split_n) : 0;
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Buffer");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);
        if (quad_split) {
            vkCmdBindIndexBuffer(r->command_buffer,
                                 r->storage_buffers[BUFFER_INDEX].buffer,
                                 quad_split_offset, VK_INDEX_TYPE_UINT32);
            if (quad_split_n)
                vkCmdDrawIndexed(r->command_buffer, (uint32_t)quad_split_n,
                                 1, 0, 0, 0);
        } else {
            vkCmdDraw(r->command_buffer, pg->inline_buffer_length, 1, 0, 0);
        }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);

        NV2A_VK_DGROUP_END();
    } else if (pg->inline_array_length) {
        NV2A_VK_DGROUP_BEGIN("Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        VkDeviceSize inline_array_data_size = pg->inline_array_length * 4;
        ensure_buffer_space(pg, BUFFER_VERTEX_INLINE_STAGING,
                               inline_array_data_size);

        unsigned int offset = 0;
        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->count == 0) {
                continue;
            }

            /* FIXME: Double check */
            offset = ROUND_UP(offset, attr->size);
            attr->inline_array_offset = offset;
            NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n", i,
                         attr->size, attr->count);
            offset += attr->size * attr->count;
            offset = ROUND_UP(offset, attr->size);
        }

        unsigned int vertex_size = offset;
        unsigned int index_count = pg->inline_array_length * 4 / vertex_size;

        NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);
        pgraph_vk_bind_vertex_attributes(d, 0, index_count - 1, true,
                                         vertex_size, index_count - 1);
        bool quad_split = needs_quad_split(pg);
        size_t quad_split_n = 0;
        if (quad_split) {
            quad_split_n = append_quad_split(pg, 0, NULL, 0, index_count);
            ensure_buffer_space(pg, BUFFER_INDEX_STAGING,
                                quad_split_n * sizeof(uint32_t));
        }

        begin_pre_draw(pg);
        void *inline_array_data = pg->inline_array;
        VkDeviceSize buffer_offset = pgraph_vk_update_vertex_inline_buffer(
            pg, &inline_array_data, &inline_array_data_size, 1);
        VkDeviceSize quad_split_offset =
            quad_split ? upload_quad_split(pg, quad_split_n) : 0;
        pgraph_vk_begin_debug_marker(r, r->command_buffer, RGBA_BLUE,
                                     "Inline Array");
        begin_draw(pg);
        bind_inline_vertex_buffer(pg, buffer_offset);
        if (quad_split) {
            vkCmdBindIndexBuffer(r->command_buffer,
                                 r->storage_buffers[BUFFER_INDEX].buffer,
                                 quad_split_offset, VK_INDEX_TYPE_UINT32);
            if (quad_split_n)
                vkCmdDrawIndexed(r->command_buffer, (uint32_t)quad_split_n,
                                 1, 0, 0, 0);
        } else {
            vkCmdDraw(r->command_buffer, index_count, 1, 0, 0);
        }
        end_draw(pg);
        pgraph_vk_end_debug_marker(r, r->command_buffer);
        NV2A_VK_DGROUP_END();
    } else {
        NV2A_VK_DPRINTF("EMPTY NV097_SET_BEGIN_END");
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
    }
    if (sample_start) {
        uint64_t elapsed = (uint64_t)(nv2a_recomp_vk_now_us() - sample_start);
        sampled_us[sample_kind] += elapsed;
        sampled_count[sample_kind]++;
        if (sample_target >= 0) {
            sampled_targets[sample_target].inline_count++;
            sampled_targets[sample_target].inline_us += elapsed;
        }
        if (sample_main_shader >= 0) {
            sampled_main_shaders[sample_main_shader].count++;
            sampled_main_shaders[sample_main_shader].cpu_us += elapsed;
        }
    }
}
