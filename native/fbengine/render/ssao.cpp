// A camera's ambient occlusion: passes of the frame's graph, from what its prepass left to what its
// scene pass reads, of its target's size, in shaders/ssao.slang.

#include "engine_shaders.hpp"
#include "fbengine.h"
#include "gpu/graph.hpp"
#include "gpu/pipeline.hpp"
#include "render/drawing.hpp"
#include "scene/camera.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace fbe::render {

namespace {

/// What shaders/ssao.slang's Occlusion is.
struct occlusion_data_t {
    std::array<float, 4> viewport{};
    std::array<std::int32_t, 2> least{};
    std::array<std::int32_t, 2> most{};
    std::array<float, 2> scale{};
    std::array<float, 2> jitter{};
    float radius = 0.0F;
    std::uint32_t perspective = 0;
    std::uint32_t frame = 0;
    std::uint32_t depth = 0;
    std::uint32_t source = 0;
    std::uint32_t spare = 0;
    std::array<std::int32_t, 2> way{};
};

/// A pass of it into a new target: the target, or no_resource, with fbe_error's why.
gpu::resource_t add_occlusion_pass(std::string_view entry, const occlusion_data_t& data,
                                   gpu::resource_t prepass, gpu::resource_t source,
                                   VkExtent2D size) {
    VkPipeline pipeline = gpu::pipeline({
        .shader = &shaders::ssao,
        .fragment = entry,
        .color = VK_FORMAT_R8_UNORM,
    });
    const gpu::transient_t level{
        .width = size.width,
        .height = size.height,
        .format = FBE_TEXTURE_R8,
    };
    if (pipeline == VK_NULL_HANDLE || !gpu::reserve_transient(level)) {
        return gpu::no_resource;
    }
    const gpu::resource_t target = gpu::transient_resource(level);
    if (target == gpu::no_resource) {
        return gpu::no_resource;
    }
    std::vector<std::pair<gpu::resource_t, gpu::use_t>> uses{ { prepass, gpu::use_t::SAMPLED } };
    if (source != gpu::no_resource) {
        uses.emplace_back(source, gpu::use_t::SAMPLED);
    }
    const bool added = gpu::add_pass({
        .name = "ambient occlusion",
        .colors = { {
            .target = target,
            .clear = true,
            .rgba = { 1.0F, 1.0F, 1.0F, 1.0F },
        }, },
        .uses = std::move(uses),
        .record =
            [data, prepass, source, pipeline](VkCommandBuffer commands) {
                occlusion_data_t push = data;
                push.depth = gpu::resource_index(prepass);
                push.source = source != gpu::no_resource ? gpu::resource_index(source) : 0;
                record_full_screen(commands, pipeline, push);
            },
    });
    return added ? target : gpu::no_resource;
}

} // namespace

gpu::resource_t add_ambient_occlusion(const scene::camera_t& camera, const scene::view_t& view,
                                      gpu::resource_t prepass, const recorded_pass_t& scene_pass,
                                      const std::array<float, 2>& jitter, std::uint32_t frame,
                                      VkExtent2D size) {
    const scene::mat4_t projection = scene::draw_projection(camera, view);
    const VkRect2D& scissor = scene_pass.scissor;
    const occlusion_data_t data{
        .viewport = { view.x, view.y, view.width, view.height },
        .least = { scissor.offset.x, scissor.offset.y },
        .most = { scissor.offset.x + static_cast<std::int32_t>(scissor.extent.width) - 1,
                  scissor.offset.y + static_cast<std::int32_t>(scissor.extent.height) - 1, },
        .scale = { projection.at(0, 0), projection.at(1, 1) },
        .jitter = jitter,
        .radius = camera.ambient_occlusion_radius,
        .perspective = camera.projection == FBE_PROJECTION_PERSPECTIVE ? 1U : 0U,
        .frame = frame,
    };
    const gpu::resource_t raw =
        add_occlusion_pass("occlusionMain", data, prepass, gpu::no_resource, size);
    if (raw == gpu::no_resource) {
        return gpu::no_resource;
    }
    occlusion_data_t across = data;
    across.way = { 1, 0 };
    const gpu::resource_t once = add_occlusion_pass("blurMain", across, prepass, raw, size);
    if (once == gpu::no_resource) {
        return gpu::no_resource;
    }
    occlusion_data_t down = data;
    down.way = { 0, 1 };
    return add_occlusion_pass("blurMain", down, prepass, once, size);
}

} // namespace fbe::render
