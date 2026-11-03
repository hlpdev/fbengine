// What comes of a camera's HDR image: passes of the frame's graph, each a full-screen triangle: its
// bloom, down a chain of smaller targets and back up, then its light through its exposure, tonemap,
// color grade, and gamma into its target.

#include "engine_shaders.hpp"
#include "fbengine.h"
#include "gpu/graph.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/vulkan.hpp"
#include "render/render.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace fbe::render {

namespace {

using gpu::flag;

/// The most levels bloom goes down, and the smallest it goes down to, in texels across.
constexpr std::size_t most_bloom_levels = 6;
constexpr std::uint32_t smallest_bloom = 8;
/// How much of what's below goes into a level on bloom's way up, which is how wide it spreads.
constexpr float bloom_spread = 0.7F;
/// How far below its threshold light starts to bloom, as a part of it.
constexpr float bloom_knee = 0.5F;
constexpr VkFormat hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;

/// What shaders/bloom.slang's Bloom is.
struct bloom_data_t {
    float texel_x = 0.0F;
    float texel_y = 0.0F;
    std::uint32_t source = 0;
    std::uint32_t below = 0;
    std::uint32_t sampler = 0;
    float threshold = 0.0F;
    float knee = 0.0F;
    float spread = 0.0F;
    std::uint32_t first = 0;
};

/// What shaders/tonemap.slang's Tonemap is.
struct tonemap_data_t {
    float width = 0.0F;
    float height = 0.0F;
    std::uint32_t image = 0;
    std::uint32_t sampler = 0;
    float exposure = 1.0F;
    float gamma = 1.0F;
    std::uint32_t kind = 0;
    std::uint32_t bloom = 0;
    std::uint32_t bloom_sampler = 0;
    float bloom_intensity = 0.0F;
    std::array<float, 2> spare{};
    std::array<float, 4> slope{};
    std::array<float, 4> offset{};
    std::array<float, 4> power{};
};

/// A size in texels.
struct texels_t {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// Records a full-screen triangle of a pipeline, with what its shader's given.
template <typename Push>
void record_triangle(VkCommandBuffer commands, VkPipeline pipeline, const Push& push) {
    VkDescriptorSet set = gpu::bindless_set();
    VkPipelineLayout layout = gpu::pipeline_layout();
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                            nullptr);
    vkCmdPushConstants(commands, layout, flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0,
                       sizeof(push), &push);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(commands, 3, 1, 0, 0);
    gpu::count_draws(1, 1);
}

/// Adds a pass of bloom's into a new target of `size`, reading `source` of `source_size` and, on
/// its way up, `below`: the target, or no_resource, with fbe_error's why, if it can't be added.
gpu::resource_t add_bloom_pass(std::string_view entry, const scene::camera_t& camera,
                               gpu::resource_t source, texels_t source_size, gpu::resource_t below,
                               texels_t size, bool first) {
    VkPipeline pipeline = gpu::pipeline({
        .shader = &shaders::bloom,
        .fragment = entry,
        .color = hdr_format,
    });
    const gpu::transient_t level{
        .width = size.width,
        .height = size.height,
        .format = FBE_TEXTURE_RGBA16F,
    };
    if (pipeline == VK_NULL_HANDLE || !gpu::reserve_transient(level)) {
        return gpu::no_resource;
    }
    const gpu::resource_t target = gpu::transient_resource(level);
    if (target == gpu::no_resource) {
        return gpu::no_resource;
    }
    std::vector<std::pair<gpu::resource_t, gpu::use_t>> uses{ { source, gpu::use_t::SAMPLED } };
    if (below != gpu::no_resource) {
        uses.emplace_back(below, gpu::use_t::SAMPLED);
    }
    const bloom_data_t data{
        .texel_x = 1.0F / static_cast<float>(source_size.width),
        .texel_y = 1.0F / static_cast<float>(source_size.height),
        .threshold = camera.bloom_threshold,
        .knee = camera.bloom_threshold * bloom_knee,
        .spread = bloom_spread,
        .first = first ? 1U : 0U,
    };
    const bool added = gpu::add_pass({
        .name = "bloom",
        .colors = { {
            .target = target,
            .clear = true,
        }, },
        .uses = std::move(uses),
        .record =
            [data, source, below, pipeline](VkCommandBuffer commands) {
                bloom_data_t push = data;
                push.source = gpu::resource_index(source);
                push.below = below != gpu::no_resource ? gpu::resource_index(below) : 0;
                push.sampler = gpu::sampler_2d_index(FBE_TEXTURE_LINEAR, false);
                record_triangle(commands, pipeline, push);
            },
    });
    return added ? target : gpu::no_resource;
}

/// Adds the passes of a camera's bloom: what's come of it at its target's size's half, or
/// no_resource, with fbe_error's why, if they can't be added.
gpu::resource_t add_bloom(const scene::camera_t& camera, gpu::resource_t image, texels_t size) {
    std::vector<gpu::resource_t> down;
    std::vector<texels_t> sizes;
    gpu::resource_t source = image;
    texels_t source_size = size;
    while (down.size() < most_bloom_levels &&
           std::min(source_size.width, source_size.height) / 2 >= smallest_bloom) {
        const texels_t half{ .width = source_size.width / 2, .height = source_size.height / 2 };
        source = add_bloom_pass("downMain", camera, source, source_size, gpu::no_resource, half,
                                down.empty());
        if (source == gpu::no_resource) {
            return gpu::no_resource;
        }
        down.push_back(source);
        sizes.push_back(half);
        source_size = half;
    }
    if (down.empty()) {
        return gpu::no_resource;
    }
    gpu::resource_t below = down.back();
    texels_t below_size = sizes.back();
    for (std::size_t level = down.size() - 1; level-- > 0;) {
        below =
            add_bloom_pass("upMain", camera, down[level], below_size, below, sizes[level], false);
        if (below == gpu::no_resource) {
            return gpu::no_resource;
        }
        below_size = sizes[level];
    }
    return below;
}

} // namespace

bool add_post_passes(const scene::camera_t& camera, gpu::resource_t image, gpu::resource_t into,
                     VkExtent2D size, VkFormat format, VkSampleCountFlagBits samples,
                     VkRect2D scissor) {
    const gpu::resource_t bloom =
        camera.bloom_intensity > 0.0F
            ? add_bloom(camera, image, { .width = size.width, .height = size.height })
            : gpu::no_resource;
    if (camera.bloom_intensity > 0.0F && bloom == gpu::no_resource &&
        std::min(size.width, size.height) / 2 >= smallest_bloom) {
        return false;
    }
    VkPipeline pipeline = gpu::pipeline({
        .shader = &shaders::tonemap,
        .color = format,
        .samples = samples,
        .blend = camera.clears_color ? gpu::blend_t::NONE : gpu::blend_t::PREMULTIPLIED,
    });
    if (pipeline == VK_NULL_HANDLE) {
        return false;
    }
    const tonemap_data_t data{
        .width = static_cast<float>(size.width),
        .height = static_cast<float>(size.height),
        .sampler = gpu::sampler_2d_index(FBE_TEXTURE_NEAREST, false),
        .exposure = camera.exposure,
        .gamma = camera.gamma,
        .kind = static_cast<std::uint32_t>(camera.tonemap),
        .bloom_sampler = gpu::sampler_2d_index(FBE_TEXTURE_LINEAR, false),
        .bloom_intensity = bloom != gpu::no_resource ? camera.bloom_intensity : 0.0F,
        .slope = { camera.grade_slope[0], camera.grade_slope[1], camera.grade_slope[2],
                   camera.grade_saturation, },
        .offset = { camera.grade_offset[0], camera.grade_offset[1], camera.grade_offset[2], 0.0F },
        .power = { camera.grade_power[0], camera.grade_power[1], camera.grade_power[2], 0.0F },
    };
    std::vector<std::pair<gpu::resource_t, gpu::use_t>> uses{ { image, gpu::use_t::SAMPLED } };
    if (bloom != gpu::no_resource) {
        uses.emplace_back(bloom, gpu::use_t::SAMPLED);
    }
    return gpu::add_pass({
        .name = "tonemap",
        .colors = { {
            .target = into,
        }, },
        .uses = std::move(uses),
        .record =
            [data, image, bloom, pipeline, scissor](VkCommandBuffer commands) {
                tonemap_data_t push = data;
                push.image = gpu::resource_index(image);
                push.bloom = bloom != gpu::no_resource ? gpu::resource_index(bloom) : 0;
                vkCmdSetScissor(commands, 0, 1, &scissor);
                record_triangle(commands, pipeline, push);
            },
    });
}

} // namespace fbe::render
