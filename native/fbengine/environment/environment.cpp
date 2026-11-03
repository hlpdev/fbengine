// Environments: passes of the frame's graph drawing a full-screen triangle of
// shaders/environment.slang into a target, a level of what's baked at a time, copied into its
// texture; and the split sum's table, baked once for all of them each time the GPU starts.

#include "environment/environment.hpp"

#include "../../common/handles.hpp"
#include "core/engine.hpp"
#include "engine_shaders.hpp"
#include "environment/fbe_environment.h"
#include "fbengine.h"
#include "gpu/graph.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/vulkan.hpp"
#include "texture/texture.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using fbe::gpu::flag;

/// The sizes environments can be, the size of their diffuse light, and of the table.
constexpr std::int32_t smallest = 16;
constexpr std::int32_t largest = 2048;
constexpr std::int32_t most_samples = 4096;
constexpr std::uint32_t irradiance_size = 32;
constexpr std::uint32_t table_size = 128;
constexpr std::uint32_t table_samples = 1024;
/// The smallest level of specular light that's read, in texels across.
constexpr std::uint32_t smallest_level = 4;

/// What environment.slang's Bake is.
struct bake_t {
    std::uint32_t source = 0;
    std::uint32_t sampler = 0;
    float roughness = 0.0F;
    std::uint32_t samples = 0;
    float size = 0.0F;
};

struct environment_t {
    std::uint64_t irradiance = 0;
    std::uint64_t specular = 0;
    std::uint32_t levels = 0;
};

/// What's baked, by its handle's slot.
struct baked_t {
    fbe::handle_slots_t slots{ fbe::ENVIRONMENT_KIND };
    std::vector<environment_t> environments;
    std::int32_t count = 0;
    std::uint64_t table = 0;
};

baked_t& baked() {
    static baked_t state;
    return state;
}

/// What a level of a bake is: where its pixels go, and how they're drawn.
struct level_t {
    std::string_view entry;
    fbe::gpu::resource_t source = fbe::gpu::no_resource;
    std::uint64_t into = 0;
    std::uint32_t level = 0;
    std::uint32_t size = 0;
    float roughness = 0.0F;
    std::uint32_t samples = 0;
};

/// Adds the passes that draw `what` into a target, and copy it into its texture's level; false,
/// with fbe_error's why, if they can't be.
bool bake_level(const level_t& what) {
    const fbe::gpu::resource_t into = fbe::gpu::texture_resource(what.into);
    const fbe::gpu::resource_t target = fbe::gpu::transient_resource({
        .width = what.size,
        .height = what.size,
        .format = FBE_TEXTURE_RGBA16F,
    });
    if (into == fbe::gpu::no_resource || target == fbe::gpu::no_resource) {
        return false;
    }
    VkPipeline pipeline = fbe::gpu::pipeline({
        .shader = &fbe::shaders::environment,
        .fragment = what.entry,
        .color = VK_FORMAT_R16G16B16A16_SFLOAT,
    });
    if (pipeline == VK_NULL_HANDLE) {
        return false;
    }
    std::vector<std::pair<fbe::gpu::resource_t, fbe::gpu::use_t>> uses;
    if (what.source != fbe::gpu::no_resource) {
        uses.emplace_back(what.source, fbe::gpu::use_t::SAMPLED);
    }
    const bool drawn = fbe::gpu::add_pass({
        .name = "environment",
        .colors = { { .target = target, .clear = true } },
        .uses = std::move(uses),
        .record =
            [what, pipeline](VkCommandBuffer commands) {
                VkDescriptorSet set = fbe::gpu::bindless_set();
                vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        fbe::gpu::pipeline_layout(), 0, 1, &set, 0, nullptr);
                const bake_t push{
                    .source = what.source != fbe::gpu::no_resource
                                  ? fbe::gpu::resource_index(what.source)
                                  : 0,
                    .sampler = fbe::gpu::sampler_2d_index(FBE_TEXTURE_LINEAR, true),
                    .roughness = what.roughness,
                    .samples = what.samples,
                    .size = static_cast<float>(what.size),
                };
                vkCmdPushConstants(commands, fbe::gpu::pipeline_layout(),
                                   flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0, sizeof(push),
                                   &push);
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdDraw(commands, 3, 1, 0, 0);
            },
    });
    if (!drawn) {
        return false;
    }
    return fbe::gpu::add_pass({
        .name = "environment copy",
        .uses = { { target, fbe::gpu::use_t::COPY_FROM }, { into, fbe::gpu::use_t::COPY_TO } },
        .record =
            [what, target, into](VkCommandBuffer commands) {
                const VkImageCopy region{
                    .srcSubresource = { .aspectMask =
                                            flag<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
                                        .layerCount = 1, },
                    .dstSubresource = { .aspectMask =
                                            flag<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
                                        .mipLevel = what.level,
                                        .layerCount = 1, },
                    .extent = { .width = what.size, .height = what.size, .depth = 1 },
                };
                vkCmdCopyImage(commands, fbe::gpu::resource_image(target),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fbe::gpu::resource_image(into),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            },
    });
}

/// Lets go of a texture, unless a stopped GPU freed it.
void release(std::uint64_t texture) {
    if (texture != 0 && fbe::texture::find(texture) != nullptr) {
        fbe_texture_release(texture);
    }
}

/// The split sum's table, baked if it isn't yet since the GPU started; false, with fbe_error's why,
/// if it can't be.
bool ready_table() {
    if (baked().table != 0 && fbe::texture::find(baked().table) != nullptr) {
        return true;
    }
    const std::uint64_t made =
        fbe::texture::create_engine_texture(table_size, table_size, FBE_TEXTURE_RGBA16F);
    if (made == 0) {
        return false;
    }
    if (!bake_level(
            { .entry = "tableMain", .into = made, .size = table_size, .samples = table_samples })) {
        release(made);
        return false;
    }
    fbe::texture::changed_on_gpu(made);
    baked().table = made;
    return true;
}

/// The environment a handle is for, with its textures live; nullptr, with fbe_error's why, if it
/// isn't one's or the GPU's stopped since it was baked.
const environment_t* live_of(std::uint64_t handle) {
    const std::optional<std::uint32_t> slot = baked().slots.find(handle);
    if (!slot) {
        fbe::fail("that isn't an environment");
        return nullptr;
    }
    const environment_t& found = baked().environments[*slot];
    if (fbe::texture::find(found.irradiance) == nullptr ||
        fbe::texture::find(found.specular) == nullptr ||
        fbe::texture::find(baked().table) == nullptr) {
        fbe::fail("the GPU stopped since this environment was baked");
        return nullptr;
    }
    return &found;
}

} // namespace

namespace fbe::environment {

bool shader_data(std::uint64_t environment, shader_data_t& into) {
    const environment_t* found = live_of(environment);
    if (found == nullptr) {
        return false;
    }
    into = {
        .irradiance = fbe::texture::index_of(found->irradiance),
        .specular = fbe::texture::index_of(found->specular),
        .table = fbe::texture::index_of(baked().table),
        .sampler = fbe::gpu::sampler_2d_index(FBE_TEXTURE_LINEAR, false),
        .levels = static_cast<float>(found->levels),
    };
    return true;
}

} // namespace fbe::environment

extern "C" {

FBE_API uint64_t fbe_environment_bake(uint64_t source, int32_t size, int32_t samples) {
    if (size < smallest || size > largest ||
        !std::has_single_bit(static_cast<std::uint32_t>(size))) {
        fbe::fail(std::format("an environment's size is a power of 2 from {} to {}, not {}",
                              smallest, largest, size));
        return 0;
    }
    if (samples < 1 || samples > most_samples) {
        fbe::fail(
            std::format("an environment takes 1 to {} samples, not {}", most_samples, samples));
        return 0;
    }
    if (!fbe_gpu_start()) {
        return 0;
    }
    if (fbe::gpu::lost()) {
        fbe::gpu::fail_lost();
        return 0;
    }
    if (fbe::texture::find(source) == nullptr) {
        return 0;
    }
    // Pixels written on the CPU reach the GPU first.
    fbe::texture::send_pixels(source);
    if (!ready_table()) {
        return 0;
    }
    const auto width = static_cast<std::uint32_t>(size);
    const auto taken = static_cast<std::uint32_t>(samples);
    const std::uint64_t irradiance =
        fbe::texture::create_engine_texture(irradiance_size, irradiance_size, FBE_TEXTURE_RGBA16F);
    const std::uint64_t specular =
        irradiance != 0
            ? fbe::texture::create_engine_texture(width, width, FBE_TEXTURE_RGBA16F, true)
            : 0;
    const fbe::gpu::resource_t resource = fbe::gpu::texture_resource(source);
    const auto failed = [&] {
        release(irradiance);
        release(specular);
        return std::uint64_t{ 0 };
    };
    if (specular == 0 || resource == fbe::gpu::no_resource) {
        return failed();
    }
    if (!bake_level({
            .entry = "irradianceMain",
            .source = resource,
            .into = irradiance,
            .size = irradiance_size,
            .samples = taken,
        })) {
        return failed();
    }
    // Level by level as roughness goes up to the smallest that's read, those past it the same.
    const auto chain = static_cast<std::uint32_t>(std::bit_width(width));
    const std::uint32_t levels =
        chain - 1 - static_cast<std::uint32_t>(std::bit_width(smallest_level) - 1);
    for (std::uint32_t level = 0; level < chain; ++level) {
        const float roughness =
            std::min(static_cast<float>(level) / static_cast<float>(levels), 1.0F);
        if (!bake_level({
                .entry = "specularMain",
                .source = resource,
                .into = specular,
                .level = level,
                .size = width >> level,
                .roughness = roughness,
                .samples = taken,
            })) {
            return failed();
        }
    }
    fbe::texture::changed_on_gpu(irradiance);
    fbe::texture::changed_on_gpu(specular);
    baked_t& state = baked();
    const std::uint32_t slot = state.slots.make();
    if (slot >= state.environments.size()) {
        state.environments.resize(slot + 1);
    }
    state.environments[slot] = { .irradiance = irradiance, .specular = specular, .levels = levels };
    ++state.count;
    return state.slots.handle(slot);
}

FBE_API bool fbe_environment_free(uint64_t environment) {
    baked_t& state = baked();
    const std::optional<std::uint32_t> slot = state.slots.find(environment);
    if (!slot) {
        return fbe::fail("that isn't an environment");
    }
    release(state.environments[*slot].irradiance);
    release(state.environments[*slot].specular);
    state.environments[*slot] = {};
    state.slots.free(*slot);
    --state.count;
    return true;
}

FBE_API uint64_t fbe_environment_irradiance(uint64_t environment) {
    const environment_t* found = live_of(environment);
    return found != nullptr ? found->irradiance : 0;
}

FBE_API uint64_t fbe_environment_specular(uint64_t environment) {
    const environment_t* found = live_of(environment);
    return found != nullptr ? found->specular : 0;
}

FBE_API uint64_t fbe_environment_table(uint64_t environment) {
    return live_of(environment) != nullptr ? baked().table : 0;
}

FBE_API int32_t fbe_environment_levels(uint64_t environment) {
    const std::optional<std::uint32_t> slot = baked().slots.find(environment);
    if (!slot) {
        fbe::fail("that isn't an environment");
        return -1;
    }
    return static_cast<std::int32_t>(baked().environments[*slot].levels);
}

FBE_API int32_t fbe_environment_count(void) {
    return baked().count;
}

} // extern "C"
