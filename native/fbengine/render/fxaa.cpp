// A camera's fast approximate antialiasing: a pass of the frame's graph, after its tonemap, of one
// triangle over its target, in shaders/fxaa.slang: no prepass, no history, so what a low GPU can
// spend. It softens what's sharp in the picture, whatever made it.

#include "engine_shaders.hpp"
#include "fbengine.h"
#include "gpu/graph.hpp"
#include "gpu/pipeline.hpp"
#include "render/drawing.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>

namespace fbe::render {

namespace {

/// What shaders/fxaa.slang's Fxaa is.
struct fxaa_data_t {
    std::array<float, 2> size{};
    std::uint32_t image = 0;
    std::uint32_t sampler = 0;
};

} // namespace

bool add_fxaa(const scene::camera_t& camera, gpu::resource_t image, gpu::resource_t into,
              VkExtent2D size, VkFormat format, VkSampleCountFlagBits samples, VkRect2D scissor) {
    VkPipeline pipeline = gpu::pipeline({
        .shader = &shaders::fxaa,
        .color = format,
        .samples = samples,
        .blend = camera.clears_color ? gpu::blend_t::NONE : gpu::blend_t::PREMULTIPLIED,
    });
    if (pipeline == VK_NULL_HANDLE) {
        return false;
    }
    const fxaa_data_t data{
        .size = { static_cast<float>(size.width), static_cast<float>(size.height) },
        .sampler = gpu::sampler_2d_index(FBE_TEXTURE_LINEAR, false),
    };
    return gpu::add_pass({
        .name = "fxaa",
        .colors = { {
            .target = into,
        }, },
        .uses = { { image, gpu::use_t::SAMPLED } },
        .record =
            [data, image, pipeline, scissor](VkCommandBuffer commands) {
                fxaa_data_t push = data;
                push.image = gpu::resource_index(image);
                vkCmdSetScissor(commands, 0, 1, &scissor);
                record_full_screen(commands, pipeline, push);
            },
    });
}

} // namespace fbe::render
