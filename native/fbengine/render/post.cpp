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
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace fbe::render {

namespace {

using gpu::flag;

/// The smallest bloom goes down to on a viewport's short side, so its coarsest level's 8 to 15
/// texels there, the same part of the view at any size.
constexpr std::uint32_t smallest_bloom = 8;
/// What each level's worth, from the coarsest, of what's left of bloom's light by the one before:
/// so a level is worth the same however many finer ones a bigger view has, the finest getting the
/// rest.
constexpr float bloom_fall = 0.8F;
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
    std::uint32_t spare = 0;
    std::array<float, 2> from{ 0.0F, 0.0F };
    std::array<float, 2> scale{ 1.0F, 1.0F };
    std::array<float, 2> least{};
    std::array<float, 2> most{};
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
    std::array<float, 4> bloom_rect{};
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

/// A pass of bloom's: what it runs; what it reads, of what size, and, on its way up, `below`, of
/// what size; where in what it reads, in pixels; how big a target it draws into, and how many
/// images its size needs at once; how much of what's below goes in; and whether it's the first
/// level.
struct bloom_pass_t {
    std::string_view entry;
    gpu::resource_t source = gpu::no_resource;
    texels_t source_size;
    gpu::resource_t below = gpu::no_resource;
    texels_t below_size;
    VkRect2D rect{};
    texels_t size;
    std::uint32_t images = 1;
    float spread = 0.0F;
    bool first = false;
};

/// Adds a pass of bloom's into a new target: the target, or no_resource, with fbe_error's why, if
/// it can't be added.
gpu::resource_t add_bloom_pass(const scene::camera_t& camera, const bloom_pass_t& pass) {
    VkPipeline pipeline = gpu::pipeline({
        .shader = &shaders::bloom,
        .fragment = pass.entry,
        .color = hdr_format,
    });
    const gpu::transient_t level{
        .width = pass.size.width,
        .height = pass.size.height,
        .format = FBE_TEXTURE_RGBA16F,
    };
    if (pipeline == VK_NULL_HANDLE || !gpu::reserve_transient(level, pass.images)) {
        return gpu::no_resource;
    }
    const gpu::resource_t source = pass.source;
    const gpu::resource_t below = pass.below;
    const gpu::resource_t target = gpu::transient_resource(level);
    if (target == gpu::no_resource) {
        return gpu::no_resource;
    }
    std::vector<std::pair<gpu::resource_t, gpu::use_t>> uses{ { source, gpu::use_t::SAMPLED } };
    if (below != gpu::no_resource) {
        uses.emplace_back(below, gpu::use_t::SAMPLED);
    }
    const auto source_x = static_cast<float>(pass.source_size.width);
    const auto source_y = static_cast<float>(pass.source_size.height);
    const auto from_x = static_cast<float>(pass.rect.offset.x);
    const auto from_y = static_cast<float>(pass.rect.offset.y);
    const auto much_x = static_cast<float>(pass.rect.extent.width);
    const auto much_y = static_cast<float>(pass.rect.extent.height);
    // On the way up, what's sampled around is what's below.
    const texels_t around = below != gpu::no_resource ? pass.below_size : pass.source_size;
    const bloom_data_t data{
        .texel_x = 1.0F / static_cast<float>(around.width),
        .texel_y = 1.0F / static_cast<float>(around.height),
        .threshold = camera.bloom_threshold,
        .knee = camera.bloom_threshold * bloom_knee,
        .spread = pass.spread,
        .first = pass.first ? 1U : 0U,
        .from = { from_x / source_x, from_y / source_y },
        .scale = { much_x / source_x, much_y / source_y },
        .least = { (from_x + 0.5F) / source_x, (from_y + 0.5F) / source_y },
        .most = { (from_x + much_x - 0.5F) / source_x, (from_y + much_y - 0.5F) / source_y },
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

/// All of what's of a size.
VkRect2D all_of(texels_t size) {
    return { .offset = {}, .extent = { .width = size.width, .height = size.height } };
}

/// The levels of bloom a viewport's size goes down through, each half of the last: none, under
/// twice the smallest on its short side.
std::vector<texels_t> bloom_levels(texels_t size) {
    std::vector<texels_t> levels;
    while (std::min(size.width, size.height) / 2 >= smallest_bloom) {
        size = { .width = size.width / 2, .height = size.height / 2 };
        levels.push_back(size);
    }
    return levels;
}

/// Adds the passes of a camera's bloom, of `rect` of its image of `image_size`: what's come of it
/// at half that rect's size, or no_resource, with fbe_error's why, if they can't be added.
gpu::resource_t add_bloom(const scene::camera_t& camera, gpu::resource_t image, texels_t image_size,
                          VkRect2D rect) {
    const std::vector<texels_t> levels =
        bloom_levels({ .width = rect.extent.width, .height = rect.extent.height });
    std::vector<gpu::resource_t> down;
    gpu::resource_t source = image;
    texels_t source_size = image_size;
    for (std::size_t level = 0; level < levels.size(); ++level) {
        // A level's image and the one on its way up that reads it are both needed at once.
        source = add_bloom_pass(camera, {
                                            .entry = "downMain",
                                            .source = source,
                                            .source_size = source_size,
                                            .rect = level == 0 ? rect : all_of(source_size),
                                            .size = levels[level],
                                            .images = level + 1 < levels.size() ? 2U : 1U,
                                            .first = level == 0,
                                        });
        if (source == gpu::no_resource) {
            return gpu::no_resource;
        }
        down.push_back(source);
        source_size = levels[level];
    }
    gpu::resource_t below = down.back();
    texels_t below_size = levels.back();
    for (std::size_t level = down.size() - 1; level-- > 0;) {
        // What's below is worth what the coarser levels are, and this one what's left of the fall's
        // next share, or all that's left, if it's the finest.
        const auto coarser = static_cast<float>(down.size() - 1 - level);
        const float worth_below = 1.0F - std::pow(bloom_fall, coarser);
        const float worth = level == 0 ? 1.0F : 1.0F - std::pow(bloom_fall, coarser + 1.0F);
        below = add_bloom_pass(camera, {
                                           .entry = "upMain",
                                           .source = down[level],
                                           .source_size = levels[level],
                                           .below = below,
                                           .below_size = below_size,
                                           .rect = all_of(levels[level]),
                                           .size = levels[level],
                                           .spread = worth_below / worth,
                                       });
        if (below == gpu::no_resource) {
            return gpu::no_resource;
        }
        below_size = levels[level];
    }
    return below;
}

} // namespace

bool add_post_passes(const scene::camera_t& camera, gpu::resource_t image, gpu::resource_t into,
                     VkExtent2D size, VkFormat format, VkSampleCountFlagBits samples,
                     VkRect2D scissor) {
    const bool blooms =
        camera.bloom_intensity > 0.0F &&
        !bloom_levels({ .width = scissor.extent.width, .height = scissor.extent.height }).empty();
    const gpu::resource_t bloom =
        blooms ? add_bloom(camera, image, { .width = size.width, .height = size.height }, scissor)
               : gpu::no_resource;
    if (blooms && bloom == gpu::no_resource) {
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
        .bloom_rect = { static_cast<float>(scissor.offset.x), static_cast<float>(scissor.offset.y),
                        static_cast<float>(scissor.extent.width),
                        static_cast<float>(scissor.extent.height), },
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
