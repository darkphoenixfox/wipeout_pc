// SPDX-License-Identifier: GPL-2.0-or-later
// The Win32 handle structures of Vulkan (VK_KHR_external_memory_win32), as vk_platform.cpp.
#ifdef _WIN64
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "video_core/renderer_vulkan/vk_leiasr.h"

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../../../leiasr_bridge/gow3_leiasr_bridge.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace Vulkan {

struct LeiaSR::Api {
    Gow3LeiaAbiFn abi;
    Gow3LeiaCreateFn create;
    Gow3LeiaResizeFn resize;
    Gow3LeiaWeaveFn weave;
    Gow3LeiaLensFn lens;
    Gow3LeiaDestroyFn destroy;
};

namespace {

void BridgeLog(int warning, const char* message) {
    std::printf("LeiaSR: %s%s\n", warning ? "warning: " : "", message);
}

constexpr vk::Format SharedFormat = vk::Format::eB8G8R8A8Unorm;

} // namespace

LeiaSR::LeiaSR(const Instance& instance_, void* window_handle_)
    : instance{instance_}, window_handle{window_handle_} {}

LeiaSR::~LeiaSR() {
    Stop();
#ifdef _WIN32
    if (library) FreeLibrary(static_cast<HMODULE>(library));
#endif
    delete api;
}

bool LeiaSR::Load() {
#ifdef _WIN32
    if (api) return true;
    if (load_failed) return false;
    load_failed = true;
    // Next to the executable (gow3-probe.exe); the SR Platform's DLLs come from its install.
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dll(path, length);
    dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"gow3_leiasr.dll";
    HMODULE module = LoadLibraryW(dll.c_str());
    if (!module) {
        std::printf("LeiaSR: gow3_leiasr.dll not loaded (error %lu): 3D stays side by side\n",
                    GetLastError());
        return false;
    }
    auto* functions = new Api{
        reinterpret_cast<Gow3LeiaAbiFn>(GetProcAddress(module, "gow3_leiasr_abi")),
        reinterpret_cast<Gow3LeiaCreateFn>(GetProcAddress(module, "gow3_leiasr_create")),
        reinterpret_cast<Gow3LeiaResizeFn>(GetProcAddress(module, "gow3_leiasr_resize")),
        reinterpret_cast<Gow3LeiaWeaveFn>(GetProcAddress(module, "gow3_leiasr_weave")),
        reinterpret_cast<Gow3LeiaLensFn>(GetProcAddress(module, "gow3_leiasr_lens")),
        reinterpret_cast<Gow3LeiaDestroyFn>(GetProcAddress(module, "gow3_leiasr_destroy")),
    };
    if (!functions->abi || functions->abi() != GOW3_LEIASR_BRIDGE_ABI || !functions->create ||
        !functions->resize || !functions->weave || !functions->lens || !functions->destroy) {
        std::printf("LeiaSR: gow3_leiasr.dll is from another version\n");
        delete functions;
        FreeLibrary(module);
        return false;
    }
    library = module;
    api = functions;
    load_failed = false;
    return true;
#else
    return false;
#endif
}

bool LeiaSR::Update(bool want) {
    if (!want) {
        if (running) {
            Stop();
            std::printf("LeiaSR: off\n");
        }
        return false;
    }
    if (running) return true;
    // A failed start is tried again after a while (the display may be switched on later).
    if (retry_frames) {
        --retry_frames;
        return false;
    }
    if (!instance.IsExternalMemoryWin32Supported()) {
        std::printf("LeiaSR: the Vulkan device cannot share images with Direct3D\n");
        retry_frames = ~0u;
        return false;
    }
    if (!Load()) {
        retry_frames = ~0u;
        return false;
    }
    const auto properties = instance.GetPhysicalDevice()
                                .getProperties2<vk::PhysicalDeviceProperties2,
                                                vk::PhysicalDeviceIDProperties>();
    const auto& id = properties.get<vk::PhysicalDeviceIDProperties>();
    if (!id.deviceLUIDValid) {
        std::printf("LeiaSR: the Vulkan device has no LUID\n");
        retry_frames = ~0u;
        return false;
    }
    Gow3LeiaDisplay display{};
    if (api->create(id.deviceLUID.data(), window_handle, 3000, BridgeLog, &display) != 0) {
        retry_frames = 600; // about 10 seconds at 60 FPS
        return false;
    }
    std::printf("LeiaSR: weaving; display %d,%d-%d,%d, recommended view %ux%u\n", display.left,
                display.top, display.right, display.bottom, display.view_width,
                display.view_height);
    running = true;
    return true;
}

void LeiaSR::Stop() {
    const auto device = instance.GetDevice();
    if (running || views_image.image || output.image) {
        (void)device.waitIdle();
    }
    Release(views_image);
    Release(output);
    views = {};
    views_size = output_size = {};
    if (api && running) {
        api->destroy();
    }
    running = false;
}

void LeiaSR::Release(Shared& shared) {
    const auto device = instance.GetDevice();
    if (shared.view) device.destroyImageView(shared.view);
    if (shared.image) device.destroyImage(shared.image);
    if (shared.memory) device.freeMemory(shared.memory);
    shared = {};
}

bool LeiaSR::Import(Shared& shared, void* handle, u32 width, u32 height) {
#ifdef _WIN32
    const auto device = instance.GetDevice();
    const vk::ExternalMemoryImageCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eD3D11Texture,
    };
    const auto [image_result, image] = device.createImage({
        .pNext = &external,
        .imageType = vk::ImageType::e2D,
        .format = SharedFormat,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    if (image_result != vk::Result::eSuccess) return false;
    shared.image = image;
    const auto requirements = device.getImageMemoryRequirements(image);
    const auto memory_properties = instance.GetPhysicalDevice().getMemoryProperties();
    u32 type = ~0u;
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags &
             vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            type = i;
            break;
        }
    }
    if (type == ~0u) return false;
    const vk::MemoryDedicatedAllocateInfo dedicated{.image = image};
    const vk::ImportMemoryWin32HandleInfoKHR import{
        .pNext = &dedicated,
        .handleType = vk::ExternalMemoryHandleTypeFlagBits::eD3D11Texture,
        .handle = static_cast<HANDLE>(handle),
    };
    const auto [memory_result, memory] = device.allocateMemory({
        .pNext = &import,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type,
    });
    if (memory_result != vk::Result::eSuccess) return false;
    shared.memory = memory;
    if (device.bindImageMemory(image, memory, 0) != vk::Result::eSuccess) return false;
    const auto [view_result, view] = device.createImageView({
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = SharedFormat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    });
    if (view_result != vk::Result::eSuccess) return false;
    shared.view = view;
    return true;
#else
    return false;
#endif
}

bool LeiaSR::Prepare(vk::CommandBuffer cmdbuf, u32 views_width, u32 views_height,
                     u32 output_width, u32 output_height) {
    if (!running) return false;
    const vk::Extent2D want_views{views_width, views_height};
    const vk::Extent2D want_output{output_width, output_height};
    if (want_views == views_size && want_output == output_size && views_image.image) {
        return true;
    }
    (void)instance.GetDevice().waitIdle();
    Release(views_image);
    Release(output);
    views_size = output_size = {};
    Gow3LeiaTextures textures{};
    if (api->resize(views_width, views_height, output_width, output_height, &textures) != 0 ||
        !Import(views_image, textures.views_handle, views_width, views_height) ||
        !Import(output, textures.output_handle, output_width, output_height)) {
        std::printf("LeiaSR: the shared images could not be made: 3D stays side by side\n");
        Release(views_image);
        Release(output);
        Stop();
        retry_frames = ~0u;
        return false;
    }
    views_size = want_views;
    output_size = want_output;
    views = Frame{
        .width = views_width,
        .height = views_height,
        .image = views_image.image,
        .image_view = views_image.view,
        .format = SharedFormat,
    };
    // The output keeps General: Direct3D writes it between frames.
    const vk::ImageMemoryBarrier2 to_general{
        .srcStageMask = vk::PipelineStageFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = output.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general});
    std::printf("LeiaSR: views %ux%u, output %ux%u\n", views_width, views_height, output_width,
                output_height);
    return true;
}

bool LeiaSR::Weave() {
    if (!running || !views_image.image) return false;
    return api->weave() == 0;
}

} // namespace Vulkan
