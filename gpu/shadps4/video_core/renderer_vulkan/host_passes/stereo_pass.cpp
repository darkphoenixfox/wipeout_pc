// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/host_passes/stereo_pass.h"

#include <array>
#include <cstdio>
#include <cstdlib>

#include "video_core/host_shaders/stereo_comp.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

namespace {

struct PushConstants {
    std::array<float, 2> out_size;
    std::array<float, 2> depth_size;
    float separation;
    float convergence;
    float max_popout;
    u32 flags;
};

float EnvFloat(const char* name, float fallback) {
    const char* value = std::getenv(name);
    return value && value[0] ? std::strtof(value, nullptr) : fallback;
}

bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] == '1';
}

} // namespace

StereoPass::Settings StereoPass::FromEnvironment() {
    Settings settings{};
    settings.enable = EnvFlag("GOW3_STEREO");
    settings.separation = EnvFloat("GOW3_STEREO_SEPARATION", settings.separation);
    settings.convergence = EnvFloat("GOW3_STEREO_CONVERGENCE", settings.convergence);
    settings.max_popout = EnvFloat("GOW3_STEREO_POPOUT", settings.max_popout);
    settings.reversed = EnvFlag("GOW3_STEREO_REVERSED");
    settings.show_depth = EnvFlag("GOW3_STEREO_SHOW_DEPTH");
    if (settings.enable) {
        std::printf("Stereo: side by side, separation %.4f, convergence %.4f, pop-out %.4f%s%s\n",
                    settings.separation, settings.convergence, settings.max_popout,
                    settings.reversed ? ", reversed depth" : "",
                    settings.show_depth ? ", showing depth" : "");
    }
    return settings;
}

void StereoPass::Create(vk::Device device_, VmaAllocator allocator, u32 num_images) {
    device = device_;
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings = {{
        {.binding = 0,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 2,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
        {.binding = 3,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .descriptorCount = 1,
         .stageFlags = vk::ShaderStageFlagBits::eCompute},
    }};
    descriptor_set_layout = Check<"create stereo descriptor set layout">(
        device.createDescriptorSetLayoutUnique({
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        }));
    const vk::PushConstantRange range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                      .offset = 0,
                                      .size = sizeof(PushConstants)};
    pipeline_layout = Check<"create stereo pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1,
        .pSetLayouts = &*descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    }));
    const auto module = CompileSPV(STEREO_COMP, device);
    SetObjectName(device, module, "stereo.comp");
    pipeline = Check<"create stereo pipeline">(device.createComputePipelineUnique(
        {}, vk::ComputePipelineCreateInfo{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main"},
                .layout = *pipeline_layout,
            }));
    device.destroyShaderModule(module);
    sampler = Check<"create stereo sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    }));
    images.resize(num_images);
    for (auto& img : images) {
        img.image = VideoCore::UniqueImage(device, allocator);
    }
}

vk::ImageView StereoPass::Render(vk::CommandBuffer cmdbuf, vk::ImageView input,
                                 vk::Extent2D input_size, const Depth& depth,
                                 const Settings& settings) {
    if (input_size != cur_size) {
        cur_size = input_size;
        for (auto& img : images) {
            img.dirty = true;
        }
    }
    auto& img = images[cur_image];
    cur_image = (cur_image + 1) % images.size();
    if (img.dirty) {
        CreateImage(img);
    }

    constexpr vk::ImageSubresourceRange color_range = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const vk::ImageMemoryBarrier2 enter{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = img.image,
        .subresourceRange = color_range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &enter});

    const bool has_depth = bool(depth.view);
    u32 flags = has_depth ? 2u : 0u;
    flags |= settings.reversed ? 1u : 0u;
    flags |= settings.show_depth ? 4u : 0u;
    flags |= depth.scene ? 8u : 0u;
    const PushConstants push{
        .out_size = {float(input_size.width), float(input_size.height)},
        .depth_size = {float(std::min(input_size.width, depth.size.width ? depth.size.width : 1u)),
                       float(std::min(input_size.height, depth.size.height ? depth.size.height : 1u))},
        .separation = settings.separation,
        .convergence = settings.convergence,
        .max_popout = settings.max_popout,
        .flags = flags,
    };
    // Without depth the picture itself fills the depth binding; the shader does not read it.
    const vk::DescriptorImageInfo color_info{.sampler = *sampler,
                                             .imageView = input,
                                             .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorImageInfo depth_info{.imageView = has_depth ? depth.view : input,
                                             .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorImageInfo out_info{.imageView = *img.view,
                                           .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo scene_info{.sampler = *sampler,
                                             .imageView = depth.scene ? depth.scene : input,
                                             .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    const std::array<vk::WriteDescriptorSet, 4> writes = {{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .pImageInfo = &color_info},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eSampledImage,
         .pImageInfo = &depth_info},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage,
         .pImageInfo = &out_info},
        {.dstBinding = 3,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eCombinedImageSampler,
         .pImageInfo = &scene_info},
    }};
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                         &push);
    cmdbuf.dispatch((input_size.width + 7) / 8, (input_size.height + 7) / 8, 1);

    const vk::ImageMemoryBarrier2 leave{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = img.image,
        .subresourceRange = color_range,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &leave});
    return *img.view;
}

void StereoPass::CreateImage(Img& img) const {
    img.dirty = false;
    img.view.reset();
    img.image.Destroy();
    img.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .extent = {cur_size.width, cur_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                 vk::ImageUsageFlagBits::eTransferSrc,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    SetObjectName(device, static_cast<vk::Image>(img.image), "Stereo output");
    img.view = Check<"create stereo output view">(device.createImageViewUnique({
        .image = img.image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR16G16B16A16Sfloat,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    }));
}

} // namespace Vulkan::HostPasses
