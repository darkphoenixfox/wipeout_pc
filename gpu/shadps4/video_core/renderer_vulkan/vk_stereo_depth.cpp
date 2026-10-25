// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_stereo_depth.h"

#include <array>

#include "video_core/host_shaders/stereo_depth_copy_comp.h"
#include "video_core/host_shaders/stereo_depth_copy_ms_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

StereoDepth::StereoDepth(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    descriptor_set_layout = Check(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    pipeline_layout = Check(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*descriptor_set_layout,
    }));
    const auto make_pipeline = [&](const auto& code) {
        const auto module = CompileSPV(code, device);
        auto result = Check(device.createComputePipelineUnique(
            {}, vk::ComputePipelineCreateInfo{
                    .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                              .module = module,
                              .pName = "main"},
                    .layout = *pipeline_layout,
                }));
        device.destroyShaderModule(module);
        return result;
    };
    pipeline = make_pipeline(STEREO_DEPTH_COPY_COMP);
    pipeline_ms = make_pipeline(STEREO_DEPTH_COPY_MS_COMP);
}

StereoDepth::~StereoDepth() = default;

void StereoDepth::Capture(VAddr display, VideoCore::Image& depth) {
    const auto device = instance.GetDevice();
    const vk::Extent2D size{depth.info.size.width, depth.info.size.height};
    auto& copy = copies[display];
    if (copy.size != size || !copy.view) {
        copy.view.reset();
        copy.image = VideoCore::UniqueImage(device, instance.GetAllocator());
        copy.image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR32Sfloat,
            .extent = {size.width, size.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        SetObjectName(device, static_cast<vk::Image>(copy.image), "Stereo scene depth");
        copy.view = Check(device.createImageViewUnique({
            .image = copy.image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR32Sfloat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        copy.size = size;
    }

    const auto depth_view = Check(device.createImageView({
        .image = depth.GetImage(),
        .viewType = vk::ImageViewType::e2D,
        .format = depth.info.pixel_format,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    }));
    scheduler.DeferOperation([device, depth_view] { device.destroyImageView(depth_view); });

    scheduler.EndRendering();
    runtime.Transit(&depth, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderRead);
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::ImageSubresourceRange color_range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier2 enter{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = copy.image,
        .subresourceRange = color_range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &enter});
    const vk::DescriptorImageInfo depth_info{.imageView = depth_view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo out_info{.imageView = *copy.view,
                                           .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 2> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &out_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute,
                        depth.info.num_samples > 1 ? *pipeline_ms : *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
    const vk::ImageMemoryBarrier2 leave{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = copy.image,
        .subresourceRange = color_range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &leave});
    copy.valid = true;
}

void StereoDepth::CaptureScene(VAddr display, VideoCore::Image& color) {
    const auto device = instance.GetDevice();
    const vk::Extent2D size{color.info.size.width, color.info.size.height};
    auto& copy = copies[display];
    if (copy.scene_size != size || copy.scene_format != color.info.pixel_format) {
        copy.scene = VideoCore::UniqueImage(device, instance.GetAllocator());
        // Mutable: the presenter views it in the display's view format (sRGB or not).
        copy.scene.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = color.info.pixel_format,
            .extent = {size.width, size.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        SetObjectName(device, static_cast<vk::Image>(copy.scene), "Stereo scene picture");
        copy.scene_size = size;
        copy.scene_format = color.info.pixel_format;
    }
    scheduler.EndRendering();
    runtime.Transit(&color, vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier2 enter{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = copy.scene,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &enter});
    const vk::ImageSubresourceLayers layers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    cmdbuf.copyImage(color.GetImage(), vk::ImageLayout::eTransferSrcOptimal, copy.scene,
                     vk::ImageLayout::eTransferDstOptimal,
                     vk::ImageCopy{.srcSubresource = layers,
                                   .dstSubresource = layers,
                                   .extent = {size.width, size.height, 1}});
    const vk::ImageMemoryBarrier2 leave{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = copy.scene,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &leave});
    copy.scene_valid = true;
}

StereoDepth::View StereoDepth::Find(VAddr display) const {
    const auto it = copies.find(display);
    if (it == copies.end() || !it->second.valid) {
        return {};
    }
    const auto& copy = it->second;
    return {*copy.view, copy.size, copy.scene_valid ? vk::Image(copy.scene) : vk::Image{}};
}

} // namespace Vulkan
