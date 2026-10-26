// SPDX-License-Identifier: MIT
// Scarlet PC - AMD FidelityFX frame generation integration.
//
// Architecture follows the public FidelityFX API swapchain model used by
// Zelda-on-PC, but this implementation is Scarlet-specific and written against
// the AMD FidelityFX SDK API.

#include "video_core/renderer_vulkan/scarlet_ffx_framegen.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include "common/logging/log.h"

namespace ScarletPC::Graphics {
namespace {

using CreateFn = PfnFfxCreateContext;
using DestroyFn = PfnFfxDestroyContext;
using ConfigureFn = PfnFfxConfigure;
using QueryFn = PfnFfxQuery;
using DispatchFn = PfnFfxDispatch;

struct VulkanFns {
    HMODULE module{};
    PFN_vkGetInstanceProcAddr get_instance_proc{};
    PFN_vkGetDeviceProcAddr get_device_proc{};
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties{};
    PFN_vkCreateImage create_image{};
    PFN_vkDestroyImage destroy_image{};
    PFN_vkGetImageMemoryRequirements get_image_requirements{};
    PFN_vkAllocateMemory allocate_memory{};
    PFN_vkFreeMemory free_memory{};
    PFN_vkBindImageMemory bind_image_memory{};
    PFN_vkCmdPipelineBarrier cmd_barrier{};
    PFN_vkCmdCopyImage cmd_copy_image{};
    PFN_vkCmdClearColorImage cmd_clear_color{};

    bool Load() {
        if (module) {
            return true;
        }
        module = LoadLibraryW(L"vulkan-1.dll");
        if (!module) {
            return false;
        }
#define SCARLET_VK_LOAD(member, name)                                                     \
        member = reinterpret_cast<decltype(member)>(GetProcAddress(module, name));        \
        if (!member) return false
        SCARLET_VK_LOAD(get_instance_proc, "vkGetInstanceProcAddr");
        SCARLET_VK_LOAD(get_device_proc, "vkGetDeviceProcAddr");
        SCARLET_VK_LOAD(get_memory_properties, "vkGetPhysicalDeviceMemoryProperties");
        SCARLET_VK_LOAD(create_image, "vkCreateImage");
        SCARLET_VK_LOAD(destroy_image, "vkDestroyImage");
        SCARLET_VK_LOAD(get_image_requirements, "vkGetImageMemoryRequirements");
        SCARLET_VK_LOAD(allocate_memory, "vkAllocateMemory");
        SCARLET_VK_LOAD(free_memory, "vkFreeMemory");
        SCARLET_VK_LOAD(bind_image_memory, "vkBindImageMemory");
        SCARLET_VK_LOAD(cmd_barrier, "vkCmdPipelineBarrier");
        SCARLET_VK_LOAD(cmd_copy_image, "vkCmdCopyImage");
        SCARLET_VK_LOAD(cmd_clear_color, "vkCmdClearColorImage");
#undef SCARLET_VK_LOAD
        return true;
    }
};

VkQueueInfoFFXAPI ToFfxQueue(VkQueue queue, std::uint32_t family) {
    VkQueueInfoFFXAPI result{};
    result.queue = queue;
    result.familyIndex = family;
    result.submitFunc = nullptr;
    return result;
}

FfxApiResource ImageResource(VkImage image, VkFormat format, std::uint32_t width,
                             std::uint32_t height, std::uint32_t state, bool uav) {
    FfxApiResource result{};
    result.resource = reinterpret_cast<void*>(image);
    result.state = state;
    result.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    result.description.format = ffxApiGetSurfaceFormatVK(format);
    result.description.width = width;
    result.description.height = height;
    result.description.depth = 1;
    result.description.mipCount = 1;
    result.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    result.description.usage =
        FFX_API_RESOURCE_USAGE_READ_ONLY | (uav ? FFX_API_RESOURCE_USAGE_UAV : 0);
    return result;
}

std::filesystem::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

} // namespace

struct FidelityFxFrameGeneration::Impl {
    struct Image {
        VkImage image{};
        VkDeviceMemory memory{};
        VkFormat format{VK_FORMAT_UNDEFINED};
        std::uint32_t width{};
        std::uint32_t height{};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    };

    HMODULE module{};
    CreateFn create{};
    DestroyFn destroy{};
    ConfigureFn configure{};
    QueryFn query{};
    DispatchFn dispatch{};

    VulkanFns vk{};

    VkQueue graphics_queue{};
    VkQueue present_queue{};
    std::uint32_t graphics_family{};
    std::uint32_t present_family{};

    VkPhysicalDevice physical{};
    VkDevice device{};

    ffxContext swapchain_context{};
    VkSwapchainKHR swapchain{};
    // FidelityFX create descriptors may be retained by the runtime until the
    // corresponding context is destroyed. Keep every create chain in Impl.
    VkSwapchainKHR swapchain_storage{};
    ffxCreateContextDescFrameGenerationSwapChainVK swapchain_desc{};
    ffxCreateBackendVKDesc framegen_backend_desc{};
    ffxCreateContextDescFrameGenerationHudless framegen_hudless_desc{};
    ffxCreateContextDescFrameGeneration framegen_desc{};
    PFN_vkGetSwapchainImagesKHR get_images{};
    PFN_vkAcquireNextImageKHR acquire{};
    PFN_vkQueuePresentKHR present{};
    PFN_getLastPresentCountFFXAPI present_count{};

    ffxContext framegen_context{};
    VkExtent2D framegen_size{};
    VkFormat framegen_format{VK_FORMAT_UNDEFINED};
    std::uint64_t frame_id{};
    bool reset_pending{true};

    Image hudless{};
    Image depth{};
    Image motion{};

    std::string problem;
    bool load_tried{};
    bool loaded{};
    bool compatibility_logged{};

    static ffxReturnCode_t FrameGenerationCallback(ffxDispatchDescFrameGeneration* params,
                                                   void* user) {
        auto* self = static_cast<Impl*>(user);
        if (self->reset_pending) {
            params->reset = true;
            self->reset_pending = false;
        }
        ffxContext context = self->framegen_context;
        return self->dispatch(&context, &params->header);
    }

    void Fail(const char* reason) {
        problem = reason;
        LOG_WARNING(Render_Vulkan, "[Scarlet.FSR3] {}", reason);
    }

    std::uint32_t FindMemory(std::uint32_t bits, VkMemoryPropertyFlags wanted) {
        VkPhysicalDeviceMemoryProperties props{};
        vk.get_memory_properties(physical, &props);
        for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    void DestroyImage(Image& image) {
        if (image.image) {
            vk.destroy_image(device, image.image, nullptr);
        }
        if (image.memory) {
            vk.free_memory(device, image.memory, nullptr);
        }
        image = {};
    }

    bool CreateImage(Image& out, std::uint32_t width, std::uint32_t height, VkFormat format,
                     VkImageUsageFlags usage) {
        if (out.image && out.width == width && out.height == height && out.format == format) {
            return true;
        }
        DestroyImage(out);

        VkImageCreateInfo image_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_ci.imageType = VK_IMAGE_TYPE_2D;
        image_ci.format = format;
        image_ci.extent = {width, height, 1};
        image_ci.mipLevels = 1;
        image_ci.arrayLayers = 1;
        image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_ci.usage = usage;
        image_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk.create_image(device, &image_ci, nullptr, &out.image) != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements requirements{};
        vk.get_image_requirements(device, out.image, &requirements);
        const std::uint32_t memory_type =
            FindMemory(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memory_type == UINT32_MAX) {
            DestroyImage(out);
            return false;
        }

        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = memory_type;
        if (vk.allocate_memory(device, &alloc, nullptr, &out.memory) != VK_SUCCESS ||
            vk.bind_image_memory(device, out.image, out.memory, 0) != VK_SUCCESS) {
            DestroyImage(out);
            return false;
        }

        out.width = width;
        out.height = height;
        out.format = format;
        out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return true;
    }

    void Barrier(VkCommandBuffer command, Image& image, VkImageLayout next,
                 VkAccessFlags src_access, VkAccessFlags dst_access,
                 VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = image.layout;
        barrier.newLayout = next;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk.cmd_barrier(command, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        image.layout = next;
    }

    void BarrierRaw(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk.cmd_barrier(command, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    bool EnsureInputs(VkExtent2D size, VkFormat color_format) {
        const bool color_ok =
            CreateImage(hudless, size.width, size.height, color_format,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        const bool depth_ok =
            CreateImage(depth, size.width, size.height, VK_FORMAT_R32_SFLOAT,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT);
        const bool motion_ok =
            CreateImage(motion, size.width, size.height, VK_FORMAT_R16G16_SFLOAT,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT);
        return color_ok && depth_ok && motion_ok;
    }

    void DestroyFramegenContext() {
        if (!framegen_context) {
            return;
        }
        if (swapchain_context) {
            ffxConfigureDescFrameGeneration cfg{};
            cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            cfg.swapChain = swapchain;
            cfg.frameGenerationEnabled = false;
            ffxContext context = framegen_context;
            configure(&context, &cfg.header);
        }
        ffxContext context = framegen_context;
        destroy(&context, nullptr);
        framegen_context = nullptr;
        framegen_size = {};
        framegen_format = VK_FORMAT_UNDEFINED;
        framegen_backend_desc = {};
        framegen_hudless_desc = {};
        framegen_desc = {};
    }

    bool EnsureFramegenContext(VkExtent2D size, VkFormat color_format) {
        if (framegen_context && framegen_size.width == size.width &&
            framegen_size.height == size.height && framegen_format == color_format) {
            return true;
        }
        DestroyFramegenContext();

        framegen_backend_desc = {};
        framegen_backend_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        framegen_backend_desc.vkDevice = device;
        framegen_backend_desc.vkPhysicalDevice = physical;
        framegen_backend_desc.vkDeviceProcAddr = vk.get_device_proc;

        framegen_hudless_desc = {};
        framegen_hudless_desc.header.type =
            FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        framegen_hudless_desc.hudlessBackBufferFormat =
            ffxApiGetSurfaceFormatVK(color_format);

        framegen_desc = {};
        framegen_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        framegen_desc.header.pNext = &framegen_backend_desc.header;
        framegen_backend_desc.header.pNext = &framegen_hudless_desc.header;
        // Compatibility mode deliberately keeps all FFX work on the existing queue.
        // This avoids requesting extra Vulkan queues from the runtime before that path
        // has been validated on AMD integrated GPUs.
        framegen_desc.flags = 0;
        framegen_desc.displaySize = {size.width, size.height};
        framegen_desc.maxRenderSize = {size.width, size.height};
        framegen_desc.backBufferFormat = ffxApiGetSurfaceFormatVK(color_format);

        ffxContext context{};
        const auto rc = create(&context, &framegen_desc.header, nullptr);
        if (rc != FFX_API_RETURN_OK) {
            LOG_WARNING(Render_Vulkan,
                        "[Scarlet.FSR3] FidelityFX frame-generation context creation failed (rc={})",
                        rc);
            Fail("FidelityFX frame-generation context creation failed");
            return false;
        }
        framegen_context = context;
        framegen_size = size;
        framegen_format = color_format;
        frame_id = 0;
        reset_pending = true;
        return true;
    }

    void DestroyAll() {
        DestroyFramegenContext();
        DestroyImage(hudless);
        DestroyImage(depth);
        DestroyImage(motion);
        if (swapchain_context) {
            ffxContext context = swapchain_context;
            destroy(&context, nullptr);
            swapchain_context = nullptr;
            swapchain = VK_NULL_HANDLE;
        }
        if (module) {
            FreeLibrary(module);
            module = nullptr;
        }
        if (vk.module) {
            FreeLibrary(vk.module);
            vk.module = nullptr;
        }
        loaded = false;
    }
};

FidelityFxFrameGeneration& FidelityFxFrameGeneration::Get() {
    static FidelityFxFrameGeneration instance;
    return instance;
}

FidelityFxFrameGeneration::~FidelityFxFrameGeneration() {
    if (impl) {
        impl->DestroyAll();
        delete impl;
        impl = nullptr;
    }
}

bool FidelityFxFrameGeneration::Requested() const {
    const char* value = std::getenv("SCARLET_FRAME_GENERATION");
    return value && *value && *value != '0';
}

bool FidelityFxFrameGeneration::Load() {
    if (!impl) {
        impl = new Impl;
    }
    if (impl->load_tried) {
        return impl->loaded;
    }
    impl->load_tried = true;

    if (!Requested()) {
        impl->Fail("Frame Generation is disabled");
        return false;
    }
    if (!impl->vk.Load()) {
        impl->Fail("Vulkan loader functions are unavailable");
        return false;
    }

    const auto dll = ExecutableDirectory() / L"amd_fidelityfx_vk.dll";
    impl->module = LoadLibraryW(dll.c_str());
    if (!impl->module) {
        impl->Fail("amd_fidelityfx_vk.dll not found beside ScarletPC.exe");
        return false;
    }

#define SCARLET_FFX_LOAD(member, name)                                                    \
    impl->member = reinterpret_cast<decltype(impl->member)>(GetProcAddress(impl->module, name)); \
    if (!impl->member) { impl->Fail("FidelityFX runtime is missing required exports"); return false; }
    SCARLET_FFX_LOAD(create, "ffxCreateContext");
    SCARLET_FFX_LOAD(destroy, "ffxDestroyContext");
    SCARLET_FFX_LOAD(configure, "ffxConfigure");
    SCARLET_FFX_LOAD(query, "ffxQuery");
    SCARLET_FFX_LOAD(dispatch, "ffxDispatch");
#undef SCARLET_FFX_LOAD

    impl->loaded = true;
    LOG_INFO(Render_Vulkan, "[Scarlet.FSR3] FidelityFX Vulkan runtime loaded");
    return true;
}

bool FidelityFxFrameGeneration::IsLoaded() const {
    return impl && impl->loaded;
}

void FidelityFxFrameGeneration::SetQueues(VkQueue graphics, std::uint32_t graphics_family,
                                          VkQueue present, std::uint32_t present_family) {
    if (!impl) {
        impl = new Impl;
    }
    impl->graphics_queue = graphics;
    impl->graphics_family = graphics_family;
    impl->present_queue = present;
    impl->present_family = present_family;
}

bool FidelityFxFrameGeneration::CreateSwapchain(
    VkPhysicalDevice physical_device, VkDevice device, VkSwapchainKHR* swapchain,
    const VkSwapchainCreateInfoKHR& create_info) {
    if (!Load() || !impl->graphics_queue || !impl->present_queue || !swapchain || !*swapchain) {
        return false;
    }

    DestroySwapchain();
    impl->physical = physical_device;
    impl->device = device;

    impl->swapchain_storage = *swapchain;
    impl->swapchain_desc = {};
    impl->swapchain_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    impl->swapchain_desc.physicalDevice = physical_device;
    impl->swapchain_desc.device = device;
    impl->swapchain_desc.swapchain = &impl->swapchain_storage;
    impl->swapchain_desc.allocator = nullptr;
    impl->swapchain_desc.createInfo = create_info;
    impl->swapchain_desc.createInfo.oldSwapchain = impl->swapchain_storage;
    impl->swapchain_desc.gameQueue =
        ToFfxQueue(impl->graphics_queue, impl->graphics_family);

    // AMD documents asyncComputeQueue as optional. Reusing the engine's graphics queue
    // here defeats the purpose of the async path and can conflict with the swapchain
    // replacement. Leave it empty unless Scarlet owns a genuinely separate compute queue.
    impl->swapchain_desc.asyncComputeQueue = {};

    // This queue is reserved by the Vulkan device specifically for FidelityFX FG and is
    // not used by Scarlet's renderer/present path.
    impl->swapchain_desc.presentQueue =
        ToFfxQueue(impl->present_queue, impl->present_family);
    impl->swapchain_desc.imageAcquireQueue = impl->swapchain_desc.presentQueue;

    ffxContext context{};
    const auto rc = impl->create(&context, &impl->swapchain_desc.header, nullptr);
    if (rc != FFX_API_RETURN_OK) {
        LOG_WARNING(Render_Vulkan,
                    "[Scarlet.FSR3] FidelityFX frame-interpolation swapchain creation failed (rc={})",
                    rc);
        impl->Fail("FidelityFX frame-interpolation swapchain creation failed");
        impl->swapchain_storage = VK_NULL_HANDLE;
        return false;
    }
    *swapchain = impl->swapchain_storage;

    ffxQueryDescSwapchainReplacementFunctionsVK query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    if (impl->query(&context, &query.header) != FFX_API_RETURN_OK) {
        impl->destroy(&context, nullptr);
        *swapchain = VK_NULL_HANDLE;
        impl->Fail("FidelityFX swapchain replacement function query failed");
        return false;
    }

    impl->swapchain_context = context;
    impl->swapchain = *swapchain;
    impl->get_images = query.pOutGetSwapchainImagesKHR;
    impl->acquire = query.pOutAcquireNextImageKHR;
    impl->present = query.pOutQueuePresentKHR;
    impl->present_count = query.pOutGetLastPresentCountFFXAPI;
    impl->reset_pending = true;

    if (!impl->get_images || !impl->acquire || !impl->present) {
        DestroySwapchain();
        *swapchain = VK_NULL_HANDLE;
        impl->Fail("FidelityFX swapchain replacement is incomplete");
        return false;
    }

    LOG_INFO(Render_Vulkan, "[Scarlet.FSR3] Real FidelityFX frame-interpolation swapchain enabled");
    return true;
}

void FidelityFxFrameGeneration::DestroySwapchain() {
    if (!impl) {
        return;
    }
    impl->DestroyFramegenContext();
    impl->DestroyImage(impl->hudless);
    impl->DestroyImage(impl->depth);
    impl->DestroyImage(impl->motion);
    if (impl->swapchain_context) {
        ffxContext context = impl->swapchain_context;
        impl->destroy(&context, nullptr);
        impl->swapchain_context = nullptr;
    }
    impl->swapchain = VK_NULL_HANDLE;
    impl->swapchain_storage = VK_NULL_HANDLE;
    impl->swapchain_desc = {};
    impl->get_images = nullptr;
    impl->acquire = nullptr;
    impl->present = nullptr;
    impl->present_count = nullptr;
}

bool FidelityFxFrameGeneration::OwnsSwapchain() const {
    return impl && impl->swapchain_context != nullptr;
}

VkResult FidelityFxFrameGeneration::GetSwapchainImages(
    VkDevice device, VkSwapchainKHR swapchain, std::uint32_t* count, VkImage* images) const {
    if (!OwnsSwapchain() || !impl->get_images) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return impl->get_images(device, swapchain, count, images);
}

VkResult FidelityFxFrameGeneration::AcquireNextImage(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, std::uint32_t* image_index) const {
    if (!OwnsSwapchain() || !impl->acquire) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult result{};
    do {
        result = impl->acquire(device, swapchain, timeout, semaphore, fence, image_index);
        if (result != VK_NOT_READY) {
            break;
        }
        SwitchToThread();
    } while (true);
    return result;
}

VkResult FidelityFxFrameGeneration::QueuePresent(
    VkQueue queue, const VkPresentInfoKHR* info) const {
    if (!OwnsSwapchain() || !impl->present) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return impl->present(queue, info);
}

bool FidelityFxFrameGeneration::PrepareCompatibilityFrame(
    VkPhysicalDevice physical_device, VkDevice device, VkCommandBuffer command,
    VkImage final_color, VkFormat color_format, VkExtent2D size, float frame_time_ms) {
    if (!OwnsSwapchain() || !command || !final_color || size.width == 0 || size.height == 0) {
        return false;
    }
    impl->physical = physical_device;
    impl->device = device;

    if (!impl->EnsureInputs(size, color_format) ||
        !impl->EnsureFramegenContext(size, color_format)) {
        return false;
    }

    // Copy the completed composited frame to a shader-readable resource. This currently
    // includes HUD/UI; true HUD-less capture is the next game-specific quality step.
    impl->BarrierRaw(command, final_color, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
    impl->Barrier(command, impl->hudless, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  impl->hudless.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT,
                  impl->hudless.layout == VK_IMAGE_LAYOUT_UNDEFINED
                      ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                      : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {size.width, size.height, 1};
    impl->vk.cmd_copy_image(command, final_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            impl->hudless.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    impl->Barrier(command, impl->hudless, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    impl->BarrierRaw(command, final_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    const auto prepare_neutral = [&](Impl::Image& image, const VkClearColorValue& clear) {
        if (image.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            impl->Barrier(command, image, VK_IMAGE_LAYOUT_GENERAL, 0,
                          VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
        } else if (image.layout != VK_IMAGE_LAYOUT_GENERAL) {
            impl->Barrier(command, image, VK_IMAGE_LAYOUT_GENERAL,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        impl->vk.cmd_clear_color(command, image.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        impl->vk.cmd_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             1, &barrier, 0, nullptr, 0, nullptr);
    };

    VkClearColorValue depth_clear{};
    depth_clear.float32[0] = 1.0f;
    VkClearColorValue motion_clear{};
    prepare_neutral(impl->depth, depth_clear);
    prepare_neutral(impl->motion, motion_clear);

    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = impl->swapchain;
    config.presentCallback = nullptr;
    config.presentCallbackUserContext = nullptr;
    config.frameGenerationCallback = &Impl::FrameGenerationCallback;
    config.frameGenerationCallbackUserContext = impl;
    config.frameGenerationEnabled = true;
    config.allowAsyncWorkloads = false;
    config.HUDLessColor =
        ImageResource(impl->hudless.image, color_format, size.width, size.height,
                      FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
    config.flags = 0;
    config.onlyPresentGenerated = false;
    config.generationRect = {0, 0, static_cast<std::int32_t>(size.width),
                            static_cast<std::int32_t>(size.height)};
    config.frameID = impl->frame_id;

    ffxContext context = impl->framegen_context;
    if (impl->configure(&context, &config.header) != FFX_API_RETURN_OK) {
        impl->Fail("FidelityFX frame-generation configure failed");
        return false;
    }

    ffxDispatchDescFrameGenerationPrepareCameraInfo camera{};
    camera.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
    camera.cameraPosition[0] = camera.cameraPosition[1] = camera.cameraPosition[2] = 0.0f;
    camera.cameraUp[0] = 0.0f;
    camera.cameraUp[1] = 1.0f;
    camera.cameraUp[2] = 0.0f;
    camera.cameraRight[0] = 1.0f;
    camera.cameraRight[1] = 0.0f;
    camera.cameraRight[2] = 0.0f;
    camera.cameraForward[0] = 0.0f;
    camera.cameraForward[1] = 0.0f;
    camera.cameraForward[2] = -1.0f;

    ffxDispatchDescFrameGenerationPrepare prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.header.pNext = &camera.header;
    prepare.frameID = impl->frame_id;
    prepare.flags = 0;
    prepare.commandList = reinterpret_cast<void*>(command);
    prepare.renderSize = {size.width, size.height};
    prepare.jitterOffset = {0.0f, 0.0f};
    prepare.motionVectorScale = {static_cast<float>(size.width),
                                 static_cast<float>(size.height)};
    prepare.frameTimeDelta = std::max(frame_time_ms, 1.0f);
    prepare.cameraNear = 0.1f;
    prepare.cameraFar = 1000.0f;
    prepare.cameraFovAngleVertical = 1.0471975512f;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth =
        ImageResource(impl->depth.image, VK_FORMAT_R32_SFLOAT, size.width, size.height,
                      FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, true);
    prepare.motionVectors =
        ImageResource(impl->motion.image, VK_FORMAT_R16G16_SFLOAT, size.width, size.height,
                      FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, true);

    if (impl->dispatch(&context, &prepare.header) != FFX_API_RETURN_OK) {
        impl->Fail("FidelityFX frame-generation prepare dispatch failed");
        return false;
    }

    if (!impl->compatibility_logged) {
        impl->compatibility_logged = true;
        LOG_WARNING(Render_Vulkan,
                    "[Scarlet.FSR3] Frame Generation active in optical-flow compatibility mode; "
                    "true Scarlet depth/motion vectors are not connected yet");
    }
    ++impl->frame_id;
    return true;
}

const char* FidelityFxFrameGeneration::Problem() const {
    return impl ? impl->problem.c_str() : "FidelityFX frame generation is not initialized";
}

} // namespace ScarletPC::Graphics

#else

#include <cstdlib>

namespace ScarletPC::Graphics {

struct FidelityFxFrameGeneration::Impl {};

FidelityFxFrameGeneration& FidelityFxFrameGeneration::Get() {
    static FidelityFxFrameGeneration instance;
    return instance;
}
FidelityFxFrameGeneration::~FidelityFxFrameGeneration() {
    delete impl;
}
bool FidelityFxFrameGeneration::Requested() const {
    const char* value = std::getenv("SCARLET_FRAME_GENERATION");
    return value && *value && *value != '0';
}
bool FidelityFxFrameGeneration::Load() { return false; }
bool FidelityFxFrameGeneration::IsLoaded() const { return false; }
void FidelityFxFrameGeneration::SetQueues(VkQueue, std::uint32_t, VkQueue, std::uint32_t) {}
bool FidelityFxFrameGeneration::CreateSwapchain(VkPhysicalDevice, VkDevice, VkSwapchainKHR*,
                                                const VkSwapchainCreateInfoKHR&) { return false; }
void FidelityFxFrameGeneration::DestroySwapchain() {}
bool FidelityFxFrameGeneration::OwnsSwapchain() const { return false; }
VkResult FidelityFxFrameGeneration::GetSwapchainImages(VkDevice, VkSwapchainKHR,
                                                       std::uint32_t*, VkImage*) const {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
VkResult FidelityFxFrameGeneration::AcquireNextImage(VkDevice, VkSwapchainKHR, std::uint64_t,
                                                     VkSemaphore, VkFence, std::uint32_t*) const {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
VkResult FidelityFxFrameGeneration::QueuePresent(VkQueue, const VkPresentInfoKHR*) const {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
bool FidelityFxFrameGeneration::PrepareCompatibilityFrame(
    VkPhysicalDevice, VkDevice, VkCommandBuffer, VkImage, VkFormat, VkExtent2D, float) {
    return false;
}
const char* FidelityFxFrameGeneration::Problem() const {
    return "FidelityFX frame generation is currently Windows-only";
}

} // namespace ScarletPC::Graphics

#endif
