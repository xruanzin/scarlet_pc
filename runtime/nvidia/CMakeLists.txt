cmake_minimum_required(VERSION 3.20)
project(ScarletDlssBridge LANGUAGES CXX)

set(DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK root")
set(VULKAN_INCLUDE "" CACHE PATH "Vulkan-Headers include directory")

if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Scarlet DLSS bridge is Windows x64 only")
endif()
if (NOT EXISTS "${DLSS_SDK_ROOT}/include/nvsdk_ngx_vk.h")
    message(FATAL_ERROR "Set DLSS_SDK_ROOT to a NVIDIA/DLSS checkout")
endif()
if (NOT EXISTS "${VULKAN_INCLUDE}/vulkan/vulkan.h")
    message(FATAL_ERROR "Set VULKAN_INCLUDE to a Vulkan-Headers include directory")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(ScarletDlssBridge SHARED scarlet_dlss_bridge.cpp scarlet_dlss_bridge.h)
target_include_directories(ScarletDlssBridge PRIVATE
    "${DLSS_SDK_ROOT}/include"
    "${VULKAN_INCLUDE}"
)
target_link_libraries(ScarletDlssBridge PRIVATE
    "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"
)
target_compile_definitions(ScarletDlssBridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(ScarletDlssBridge PROPERTIES OUTPUT_NAME "ScarletDlssBridge")
