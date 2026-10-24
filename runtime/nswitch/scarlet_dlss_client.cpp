// SPDX-License-Identifier: MIT
#include "video_core/renderer_vulkan/scarlet_dlss_client.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "common/logging/log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace ScarletPC::Graphics {
namespace {

bool RequestedByEnvironment() {
    const char* value = std::getenv("SCARLET_UPSCALER");
    return value && std::strcmp(value, "dlss") == 0;
}

#ifdef _WIN32
std::filesystem::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

void BridgeLog(int warning, const char* message) {
    if (warning) {
        LOG_WARNING(Render_Vulkan, "[Scarlet.DLSS] {}", message ? message : "");
    } else {
        LOG_INFO(Render_Vulkan, "[Scarlet.DLSS] {}", message ? message : "");
    }
}
#endif

} // namespace

struct DlssClient::Impl {
#ifdef _WIN32
    HMODULE module{};
#endif
    const ScarletDlssApi* api{};
    std::vector<VkExtensionProperties> instance_extensions;
    std::vector<VkExtensionProperties> device_extensions;
    std::string problem;
    bool load_tried{};
    bool initialized{};
};

DlssClient& DlssClient::Get() {
    static DlssClient client;
    return client;
}

DlssClient::~DlssClient() {
    Shutdown();
    delete impl;
    impl = nullptr;
}

bool DlssClient::Requested() const {
    return RequestedByEnvironment();
}

bool DlssClient::Load() {
    if (!impl) {
        impl = new Impl;
    }
    if (impl->load_tried) {
        return impl->api != nullptr;
    }
    impl->load_tried = true;

    if (!Requested()) {
        impl->problem = "DLSS is not selected";
        return false;
    }

#ifdef _WIN32
    const auto exe_dir = ExecutableDirectory();
    const auto bridge_path = exe_dir / L"ScarletDlssBridge.dll";
    impl->module = LoadLibraryW(bridge_path.c_str());
    if (!impl->module) {
        impl->problem = "ScarletDlssBridge.dll was not found";
        LOG_WARNING(Render_Vulkan, "[Scarlet.DLSS] {}", impl->problem);
        return false;
    }

    const auto get_api = reinterpret_cast<ScarletDlssGetApiFn>(
        GetProcAddress(impl->module, "ScarletDlssGetApi"));
    if (!get_api) {
        impl->problem = "ScarletDlssBridge.dll has no ScarletDlssGetApi export";
        LOG_WARNING(Render_Vulkan, "[Scarlet.DLSS] {}", impl->problem);
        return false;
    }

    impl->api = get_api();
    if (!impl->api || impl->api->abi != SCARLET_DLSS_BRIDGE_ABI) {
        impl->api = nullptr;
        impl->problem = "Scarlet DLSS bridge ABI mismatch";
        LOG_WARNING(Render_Vulkan, "[Scarlet.DLSS] {}", impl->problem);
        return false;
    }

    const auto cache_dir = exe_dir / L"user" / L"cache" / L"scarlet" / L"ngx";
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (!impl->api->Configure(exe_dir.c_str(), cache_dir.c_str(), &BridgeLog)) {
        impl->api = nullptr;
        impl->problem = "Scarlet DLSS bridge configuration failed";
        LOG_WARNING(Render_Vulkan, "[Scarlet.DLSS] {}", impl->problem);
        return false;
    }

    LOG_INFO(Render_Vulkan, "[Scarlet.DLSS] NGX bridge loaded");
    return true;
#else
    impl->problem = "DLSS bridge is Windows-only";
    return false;
#endif
}

bool DlssClient::IsLoaded() const {
    return impl && impl->api;
}

bool DlssClient::IsInitialized() const {
    return impl && impl->initialized;
}

std::span<const VkExtensionProperties> DlssClient::InstanceExtensions() {
    if (!Load()) {
        return {};
    }
    std::uint32_t count{};
    const VkExtensionProperties* properties{};
    if (!impl->api->InstanceExtensions(&count, &properties) || !properties || count == 0) {
        return {};
    }
    impl->instance_extensions.assign(properties, properties + count);
    return impl->instance_extensions;
}

std::span<const VkExtensionProperties> DlssClient::DeviceExtensions(
    VkInstance instance, VkPhysicalDevice physical) {
    if (!Load()) {
        return {};
    }
    std::uint32_t count{};
    const VkExtensionProperties* properties{};
    if (!impl->api->DeviceExtensions(instance, physical, &count, &properties) ||
        !properties || count == 0) {
        return {};
    }
    impl->device_extensions.assign(properties, properties + count);
    return impl->device_extensions;
}

bool DlssClient::Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                            PFN_vkGetInstanceProcAddr get_instance_proc,
                            PFN_vkGetDeviceProcAddr get_device_proc) {
    if (!Load()) {
        return false;
    }
    if (impl->initialized) {
        return true;
    }
    if (!impl->api->Initialize(instance, physical, device, get_instance_proc,
                               get_device_proc)) {
        impl->problem = "NVIDIA NGX initialization failed or DLSS is unsupported";
        LOG_WARNING(Render_Vulkan, "[Scarlet.DLSS] {}", impl->problem);
        return false;
    }
    impl->initialized = true;
    LOG_INFO(Render_Vulkan, "[Scarlet.DLSS] NVIDIA NGX initialized");
    return true;
}

bool DlssClient::CreateFeature(VkCommandBuffer command, const ScarletDlssFeature& feature) {
    return IsInitialized() && impl->api->CreateFeature(command, &feature) != 0;
}

bool DlssClient::Evaluate(VkCommandBuffer command, const ScarletDlssEvaluate& evaluate) {
    return IsInitialized() && impl->api->Evaluate(command, &evaluate) != 0;
}

void DlssClient::ReleaseFeature() {
    if (IsInitialized() && impl->api->ReleaseFeature) {
        impl->api->ReleaseFeature();
    }
}

void DlssClient::Shutdown() {
    if (!impl) {
        return;
    }
    if (impl->api && impl->initialized) {
        impl->api->Shutdown();
        impl->initialized = false;
    }
#ifdef _WIN32
    if (impl->module) {
        FreeLibrary(impl->module);
        impl->module = nullptr;
    }
#endif
    impl->api = nullptr;
}

const char* DlssClient::Problem() const {
    return impl ? impl->problem.c_str() : "DLSS client is not initialized";
}

} // namespace ScarletPC::Graphics
