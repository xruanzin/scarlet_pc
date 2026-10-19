#pragma once
// Version constant for older SDK headers; this does not override driver capabilities.
#ifndef VK_API_VERSION_1_4
#define VK_API_VERSION_1_4 VK_MAKE_API_VERSION(0, 1, 4, 0)
#endif
// Promoted Vulkan 1.3 commands: core on native 1.3, KHR/EXT on negotiated 1.2.
#define VK_COMPAT_COMMANDS(X) \
    X(vkCmdBeginRendering, "KHR") \
    X(vkCmdEndRendering, "KHR") \
    X(vkCmdPipelineBarrier2, "KHR") \
    X(vkCmdWriteTimestamp2, "KHR") \
    X(vkQueueSubmit2, "KHR") \
    X(vkCmdSetCullMode, "EXT") \
    X(vkCmdSetDepthCompareOp, "EXT") \
    X(vkCmdSetDepthTestEnable, "EXT") \
    X(vkCmdSetDepthWriteEnable, "EXT") \
    X(vkCmdSetPrimitiveTopology, "EXT") \
    X(vkCmdSetStencilOp, "EXT") \
    X(vkCmdSetStencilTestEnable, "EXT") \
    X(vkCmdSetDepthBiasEnable, "EXT") \
    X(vkCmdSetPrimitiveRestartEnable, "EXT")

#define VK_COMPAT_DECLARE(name, suffix) extern PFN_##name lvk_##name;
VK_COMPAT_COMMANDS(VK_COMPAT_DECLARE)
#undef VK_COMPAT_DECLARE

#define vkCmdBeginRendering lvk_vkCmdBeginRendering
#define vkCmdEndRendering lvk_vkCmdEndRendering
#define vkCmdPipelineBarrier2 lvk_vkCmdPipelineBarrier2
#define vkCmdWriteTimestamp2 lvk_vkCmdWriteTimestamp2
#define vkQueueSubmit2 lvk_vkQueueSubmit2
#define vkCmdSetCullMode lvk_vkCmdSetCullMode
#define vkCmdSetDepthCompareOp lvk_vkCmdSetDepthCompareOp
#define vkCmdSetDepthTestEnable lvk_vkCmdSetDepthTestEnable
#define vkCmdSetDepthWriteEnable lvk_vkCmdSetDepthWriteEnable
#define vkCmdSetPrimitiveTopology lvk_vkCmdSetPrimitiveTopology
#define vkCmdSetStencilOp lvk_vkCmdSetStencilOp
#define vkCmdSetStencilTestEnable lvk_vkCmdSetStencilTestEnable
#define vkCmdSetDepthBiasEnable lvk_vkCmdSetDepthBiasEnable
#define vkCmdSetPrimitiveRestartEnable lvk_vkCmdSetPrimitiveRestartEnable
