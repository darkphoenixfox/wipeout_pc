// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: stereo 3D from the picture and the scene depth (GOW3_STEREO, host_shaders/stereo.comp):
// the presented picture becomes a half-width side-by-side pair, left eye on the left.

#pragma once

#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::HostPasses {

class StereoPass {
public:
    struct Settings {
        bool enable = false;
        float separation = 0.03f;  ///< parallax at infinity, fraction of one eye's width
        float convergence = 0.02f; ///< nearness (1 - depth) of the screen plane
        float max_popout = 0.02f;  ///< largest parallax in front of the screen
        bool swap = false;         ///< right eye on the left
        bool reversed = false;     ///< depth 1 is near (GOW3_STEREO_REVERSED)
        bool show_depth = false;   ///< debug: nearness as grey (GOW3_STEREO_SHOW_DEPTH)
        bool leiasr = false;       ///< woven for a LeiaSR display (Gow3Settings::stereo_output)
        bool full_width = false;   ///< each eye at the picture's width (output twice as wide)
    };
    /// This frame's settings: the in-game menu's (Gow3Settings), the debug ones from the
    /// environment.
    static Settings Current();

    struct Depth {
        vk::ImageView view{}; ///< R32F scene depth, ShaderReadOnlyOptimal; null: none (eyes alike)
        vk::Extent2D size{};
        /// The picture before the HUD (the input's format), ShaderReadOnlyOptimal; null: no HUD
        vk::ImageView scene{};
    };

    void Create(vk::Device device, VmaAllocator allocator, u32 num_images);

    /// The side-by-side picture of `input` (ShaderReadOnlyOptimal), ShaderReadOnlyOptimal: the
    /// input's size, or twice its width with Settings::full_width.
    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                         const Depth& depth, const Settings& settings);

private:
    struct Img {
        bool dirty = true;
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
    };

    void CreateImage(Img& img) const;

    vk::Device device{};
    vk::UniqueDescriptorSetLayout descriptor_set_layout{};
    vk::UniqueSampler sampler{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipeline pipeline{};
    vk::Extent2D cur_size{};
    u32 cur_image = 0;
    std::vector<Img> images;
};

} // namespace Vulkan::HostPasses
