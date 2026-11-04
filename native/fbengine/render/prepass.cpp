// What a camera with ambient occlusion or temporal antialiasing draws before its scene: its opaque
// draws again, into a target of half floats, one sample, with depth of its own. Red is how far each
// pixel is along the view, 0 for nothing, to a part in 2048 of itself, which ambient occlusion's
// bias keeps it above; blue and alpha how far it's moved on its view since the last frame, as parts
// of it. One target, as a pipeline has, and depth's all the normals these want. Particles and what
// blends aren't in it, so they have no motion.

#include "fbengine.h"
#include "gpu/graph.hpp"
#include "render/drawing.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <memory>
#include <utility>

namespace fbe::render {

gpu::resource_t add_prepass(const view_batches_t& view, const recorded_pass_t& scene_pass,
                            VkExtent2D size) {
    const gpu::transient_t target{
        .width = size.width,
        .height = size.height,
        .format = FBE_TEXTURE_RGBA16F,
        .depth = true,
    };
    if (!gpu::reserve_transient(target)) {
        return gpu::no_resource;
    }
    const gpu::resource_t made = gpu::transient_resource(target);
    if (made == gpu::no_resource) {
        return gpu::no_resource;
    }
    auto pass = std::make_shared<recorded_pass_t>();
    pass->viewport = scene_pass.viewport;
    pass->scissor = scene_pass.scissor;
    pass->format = VK_FORMAT_R16G16B16A16_SFLOAT;
    pass->prepass = true;
    view_batches_t only{};
    only.camera = view.camera;
    only.flipped = view.flipped;
    for (const batch_t& batch : view.batches) {
        const auto depth_mode = static_cast<std::int32_t>((batch.key >> 10U) & 3U);
        const bool particles = ((batch.key >> 50U) & 1U) != 0;
        if (batch.sky == 0 && depth_mode == FBE_DEPTH_TEST_AND_WRITE && !particles) {
            only.batches.push_back(batch);
        }
    }
    pass->views.push_back(std::move(only));
    const bool added = gpu::add_pass({
        .name = "prepass",
        .colors = { {
            .target = made,
            .clear = true,
        }, },
        .depth = made,
        .clear_depth = true,
        .record = [pass](VkCommandBuffer commands) { record_pass(commands, *pass); },
    });
    return added ? made : gpu::no_resource;
}

} // namespace fbe::render
