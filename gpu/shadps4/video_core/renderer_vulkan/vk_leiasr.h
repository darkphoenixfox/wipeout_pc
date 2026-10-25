// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: 3D on LeiaSR displays (Samsung Odyssey 3D, Acer SpatialLabs). gow3_leiasr.dll
// (gpu/leiasr_bridge) weaves the stereo pair with the LeiaSR SDK's Direct3D 11 weaver; this
// loads it, imports its two shared textures and hands it each frame's pair. The port runs
// without it: 3D then stays side by side.

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

namespace Vulkan {

class Instance;

class LeiaSR {
public:
    LeiaSR(const Instance& instance, void* window_handle);
    ~LeiaSR();

    /// Starts the weaver when `want` (and keeps it), stops it otherwise. True when it weaves.
    bool Update(bool want);

    /// The shared images at these sizes (both views side by side; the window's client area).
    /// False when they cannot be made.
    bool Prepare(vk::CommandBuffer cmdbuf, u32 views_width, u32 views_height, u32 output_width,
                 u32 output_height);

    /// The views image as a frame for the post-process pass (B8G8R8A8_UNORM).
    Frame& Views() noexcept {
        return views;
    }
    /// Weaves the views (their Vulkan work must be finished) into the output image.
    bool Weave();
    /// The woven picture, General layout, the size given to Prepare.
    vk::Image Output() const noexcept {
        return output.image;
    }

private:
    struct Shared {
        vk::Image image{};
        vk::DeviceMemory memory{};
        vk::ImageView view{};
    };
    bool Load();
    bool Import(Shared& shared, void* handle, u32 width, u32 height);
    void Release(Shared& shared);
    void Stop();

    const Instance& instance;
    void* window_handle;
    void* library = nullptr;
    bool load_failed = false;
    bool running = false;
    u32 retry_frames = 0;
    struct Api;
    Api* api = nullptr;
    Shared views_image, output;
    Frame views{};
    vk::Extent2D views_size{}, output_size{};
};

} // namespace Vulkan
