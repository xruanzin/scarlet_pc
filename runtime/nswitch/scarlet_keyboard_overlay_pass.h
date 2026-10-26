#pragma once

#include <cstdint>
#include <vector>

#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;
struct Frame;

class ScarletKeyboardOverlayPass final {
public:
    ScarletKeyboardOverlayPass(const Device& device, MemoryAllocator& allocator,
                               Scheduler& scheduler, VkFormat frame_format);
    ~ScarletKeyboardOverlayPass();

    void Draw(const Device& device, Frame* frame);

private:
    bool RefreshImage(const Device& device, std::uint32_t width, std::uint32_t height);
    void UpdateDescriptor(const Device& device);

    MemoryAllocator& allocator;
    Scheduler& scheduler;

    vk::DescriptorPool descriptor_pool;
    vk::DescriptorSetLayout descriptor_set_layout;
    vk::DescriptorSets descriptor_sets;
    vk::PipelineLayout pipeline_layout;
    vk::Sampler sampler;
    vk::ShaderModule vertex_shader;
    vk::ShaderModule fragment_shader;
    vk::RenderPass render_pass;
    vk::Pipeline pipeline;

    vk::Image image;
    vk::ImageView image_view;

    std::uint64_t uploaded_version{};
    VkExtent2D uploaded_extent{};
    std::vector<std::uint8_t> pixels;
};

} // namespace Vulkan
