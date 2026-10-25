// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: stereo 3D (GOW3_STEREO): the scene depth of each display buffer, copied when the scene
// is drawn into it, and the buffer's picture before the HUD is drawn over it. The game clears
// its depth for the next frame before the flip reaches the presenter, so the presenter reads
// these copies (host_passes/stereo_pass.h); pixels the HUD changed are not moved.

#pragma once

#include <unordered_map>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;

class StereoDepth {
public:
    StereoDepth(const Instance& instance, Scheduler& scheduler, Runtime& runtime);
    ~StereoDepth();

    /// Copies `depth` (a depth target) for the display buffer at `display`.
    void Capture(VAddr display, VideoCore::Image& depth);
    /// Copies the picture of `display` (`color`, the scene alone: the HUD is drawn next).
    void CaptureScene(VAddr display, VideoCore::Image& color);
    /// Forgets the depth and scene of `display` (its next picture has no scene).
    void Drop(VAddr display) {
        if (auto it = copies.find(display); it != copies.end()) {
            it->second.valid = it->second.scene_valid = false;
        }
    }
    /// A new picture in `display`: its scene copy is taken again before the HUD.
    void NewPicture(VAddr display) {
        if (auto it = copies.find(display); it != copies.end()) it->second.scene_valid = false;
    }

    struct View {
        vk::ImageView view{}; ///< R32F, ShaderReadOnlyOptimal; null when there is none
        vk::Extent2D size{};
        vk::Image scene{};    ///< the picture before the HUD, ShaderReadOnlyOptimal, or null
    };
    [[nodiscard]] View Find(VAddr display) const;

private:
    struct Copy {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::Extent2D size{};
        bool valid = false;
        VideoCore::UniqueImage scene;
        vk::Extent2D scene_size{};
        vk::Format scene_format{};
        bool scene_valid = false;
    };

    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    vk::UniqueDescriptorSetLayout descriptor_set_layout;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;
    vk::UniquePipeline pipeline_ms;
    std::unordered_map<VAddr, Copy> copies;
};

} // namespace Vulkan
