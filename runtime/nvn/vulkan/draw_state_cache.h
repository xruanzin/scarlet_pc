#pragma once

// Include after the Vulkan declarations. The owner must invalidate on a new
// recording (even if the VkCommandBuffer handle is reused), before foreign
// graphics commands, and before commands that directly change this state.
#include <stdbool.h>
#include <string.h>

typedef struct {
    VkStencilOp fail, pass, depth_fail;
    VkCompareOp compare;
    uint32_t compare_mask, write_mask, reference;
} ScarletStencilState;

typedef struct {
    VkBool32 depth_test, depth_write, stencil_test;
    VkCompareOp depth_compare;
    ScarletStencilState stencil[2];
    float blend[4];
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPrimitiveTopology topology;
    VkBool32 restart, discard, bias_enable;
    float bias_constant, bias_clamp, bias_slope;
} ScarletDrawState;

typedef struct {
    VkCommandBuffer command;
    VkPipeline pipeline;
    bool valid;
    ScarletDrawState state;
    bool viewport_valid, scissor_valid;
    VkViewport viewport;
    VkRect2D scissor;
} ScarletDrawStateCache;

static inline void scarlet_draw_state_invalidate(ScarletDrawStateCache *cache) {
    cache->command = VK_NULL_HANDLE;
    cache->pipeline = VK_NULL_HANDLE;
    cache->valid = false;
    cache->viewport_valid = cache->scissor_valid = false;
}

static inline void scarlet_apply_viewport(ScarletDrawStateCache *cache,
                                        VkCommandBuffer cb, const VkViewport *viewport) {
    if (cache->command != cb) {
        scarlet_draw_state_invalidate(cache);
        cache->command = cb;
    }
    if (cache->viewport_valid && !memcmp(&cache->viewport, viewport, sizeof *viewport)) return;
    vkCmdSetViewport(cb, 0, 1, viewport);
    cache->viewport = *viewport;
    cache->viewport_valid = true;
}

static inline void scarlet_apply_scissor(ScarletDrawStateCache *cache,
                                       VkCommandBuffer cb, const VkRect2D *scissor) {
    if (cache->command != cb) {
        scarlet_draw_state_invalidate(cache);
        cache->command = cb;
    }
    if (cache->scissor_valid && !memcmp(&cache->scissor, scissor, sizeof *scissor)) return;
    vkCmdSetScissor(cb, 0, 1, scissor);
    cache->scissor = *scissor;
    cache->scissor_valid = true;
}

static inline void scarlet_apply_draw_state(ScarletDrawStateCache *cache,
                                            VkCommandBuffer cb, VkPipeline pipeline,
                                            const ScarletDrawState *state) {
    if (cache->command != cb || cache->pipeline != pipeline) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cache->command = cb;
        cache->pipeline = pipeline;
        // Conservatively emit all values after a pipeline change, including
        // values that would survive between two wholly dynamic pipelines.
        cache->valid = false;
        cache->viewport_valid = cache->scissor_valid = false;
    }
    if (cache->valid && !memcmp(&cache->state, state, sizeof *state)) return;

    vkCmdSetDepthTestEnable(cb, state->depth_test);
    vkCmdSetDepthWriteEnable(cb, state->depth_write);
    vkCmdSetDepthCompareOp(cb, state->depth_compare);
    vkCmdSetStencilTestEnable(cb, state->stencil_test);
    if (state->stencil_test) {
        for (unsigned i = 0; i < 2; ++i) {
            const ScarletStencilState *s = &state->stencil[i];
            VkStencilFaceFlags face = i ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilOp(cb, face, s->fail, s->pass, s->depth_fail, s->compare);
            vkCmdSetStencilCompareMask(cb, face, s->compare_mask);
            vkCmdSetStencilWriteMask(cb, face, s->write_mask);
            vkCmdSetStencilReference(cb, face, s->reference);
        }
    }
    vkCmdSetBlendConstants(cb, state->blend);
    vkCmdSetCullMode(cb, state->cull);
    vkCmdSetFrontFace(cb, state->front);
    vkCmdSetPrimitiveTopology(cb, state->topology);
    vkCmdSetPrimitiveRestartEnable(cb, state->restart);
    vkCmdSetRasterizerDiscardEnable(cb, state->discard);
    vkCmdSetDepthBiasEnable(cb, state->bias_enable);
    vkCmdSetDepthBias(cb, state->bias_constant, state->bias_clamp, state->bias_slope);
    cache->state = *state;
    cache->valid = true;
}
