#include "video_core/renderer_vulkan/scarlet_keyboard_overlay_pass.h"

#include <array>
#include <tuple>
#include <vector>

#include "video_core/host_shaders/vulkan_present_frag_spv.h"
#include "video_core/host_shaders/vulkan_present_vert_spv.h"
#include "video_core/renderer_vulkan/present/present_push_constants.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/scarlet_keyboard_overlay.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

ScarletKeyboardOverlayPass::ScarletKeyboardOverlayPass(
    const Device& device, MemoryAllocator& allocator_, Scheduler& scheduler_,
    VkFormat frame_format)
    : allocator{allocator_}, scheduler{scheduler_} {
    descriptor_set_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    descriptor_pool =
        CreateWrappedDescriptorPool(device, 1, 1, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});

    const std::vector<VkDescriptorSetLayout> layouts{*descriptor_set_layout};
    descriptor_sets = CreateWrappedDescriptorSets(descriptor_pool, layouts);

    const VkPushConstantRange range{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(PresentPushConstants),
    };
    pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    });

    sampler = CreateWrappedSampler(device, VK_FILTER_LINEAR);
    vertex_shader = BuildShader(device, VULKAN_PRESENT_VERT_SPV);
    fragment_shader = BuildShader(device, VULKAN_PRESENT_FRAG_SPV);

    // LOAD preserves the game/FSR result already rendered into the frame.
    render_pass = CreateWrappedRenderPass(device, frame_format, VK_IMAGE_LAYOUT_GENERAL);
    pipeline = CreateWrappedPipeline(device, render_pass, pipeline_layout,
                                     std::tie(vertex_shader, fragment_shader));
}

ScarletKeyboardOverlayPass::~ScarletKeyboardOverlayPass() = default;

bool ScarletKeyboardOverlayPass::RefreshImage(const Device& device, std::uint32_t width,
                                               std::uint32_t height) {
    if (!ScarletPC::KeyboardOverlay::Visible()) {
        return false;
    }

    const bool extent_changed =
        uploaded_extent.width != width || uploaded_extent.height != height;
    std::uint64_t next_version{};
    if (!ScarletPC::KeyboardOverlay::BuildBgraFrame(
            width, height, extent_changed ? 0 : uploaded_version, next_version, pixels)) {
        return *image_view != VK_NULL_HANDLE;
    }

    // Uploads only happen when the keyboard state changes (selection/text/etc.) or
    // when the output extent changes, never once per game frame.
    scheduler.Finish();

    image = CreateWrappedImage(allocator, VkExtent2D{width, height},
                               VK_FORMAT_B8G8R8A8_UNORM);
    UploadImage(device, allocator, scheduler, image, VkExtent2D{width, height},
                VK_FORMAT_B8G8R8A8_UNORM, pixels);
    image_view = CreateWrappedImageView(device, image, VK_FORMAT_B8G8R8A8_UNORM);

    uploaded_extent = VkExtent2D{width, height};
    uploaded_version = next_version;
    UpdateDescriptor(device);
    return true;
}

void ScarletKeyboardOverlayPass::UpdateDescriptor(const Device& device) {
    if (*image_view == VK_NULL_HANDLE) {
        return;
    }
    const VkDescriptorImageInfo info{
        .sampler = *sampler,
        .imageView = *image_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_sets[0],
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    device.GetLogical().UpdateDescriptorSets(std::array{write}, {});
}

void ScarletKeyboardOverlayPass::Draw(const Device& device, Frame* frame) {
    if (!frame || !ScarletPC::KeyboardOverlay::Visible()) {
        return;
    }
    if (!RefreshImage(device, frame->width, frame->height) || *image_view == VK_NULL_HANDLE) {
        return;
    }

    PresentPushConstants push{};
    push.modelview_matrix =
        MakeOrthographicMatrix(static_cast<float>(frame->width),
                               static_cast<float>(frame->height));
    push.vertices[0] = ScreenRectVertex(0.0f, 0.0f, 0.0f, 0.0f);
    push.vertices[1] =
        ScreenRectVertex(static_cast<float>(frame->width), 0.0f, 1.0f, 0.0f);
    push.vertices[2] =
        ScreenRectVertex(0.0f, static_cast<float>(frame->height), 0.0f, 1.0f);
    push.vertices[3] = ScreenRectVertex(static_cast<float>(frame->width),
                                        static_cast<float>(frame->height), 1.0f, 1.0f);

    const VkRenderPass rp = *render_pass;
    const VkPipelineLayout layout = *pipeline_layout;
    const VkPipeline graphics_pipeline = *pipeline;
    const VkDescriptorSet descriptor = descriptor_sets[0];
    const VkFramebuffer framebuffer = *frame->framebuffer;
    const VkExtent2D extent{frame->width, frame->height};

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        BeginRenderPass(cmdbuf, rp, framebuffer, extent);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
        cmdbuf.PushConstants(layout, VK_SHADER_STAGE_VERTEX_BIT, push);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptor, {});
        cmdbuf.Draw(4, 1, 0, 0);
        cmdbuf.EndRenderPass();
    });
}

} // namespace Vulkan
