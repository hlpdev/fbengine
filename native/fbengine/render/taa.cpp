// A camera's temporal antialiasing: its projection a part of a pixel off each frame, by 8 of
// Halton's sequence, and what it drew resolved over what it had, kept in one of two textures of its
// own, which the rest of its post-processing reads in place of what it drew. What moved goes by
// what its entities were last frame, kept here for each camera: their matrices and skins' palettes.

#include "engine_shaders.hpp"
#include "fbengine.h"
#include "gpu/graph.hpp"
#include "gpu/pipeline.hpp"
#include "render/drawing.hpp"
#include "render/render.hpp"
#include "scene/camera.hpp"
#include "scene/scene.hpp"
#include "skin/skin.hpp"
#include "texture/texture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace fbe::render {

namespace {

using scene::mat4_t;
using scene::vec3_t;

/// How many frames can go undrawn with what's been still good to go by, as a camera drawn every
/// other frame has.
constexpr std::int64_t most_frames_undrawn = 2;
/// How much of what's now goes in, once there's enough of what's been to go by.
constexpr float settled_weight = 0.1F;
constexpr std::uint32_t jitter_count = 8;

/// What shaders/taa.slang's Temporal is.
struct temporal_data_t {
    std::array<float, 2> size{};
    std::uint32_t image = 0;
    std::uint32_t prepass = 0;
    std::uint32_t history = 0;
    std::uint32_t sampler = 0;
    float weight = 1.0F;
    std::uint32_t perspective = 0;
    std::array<float, 4> viewport{};
    std::array<std::int32_t, 2> least{};
    std::array<std::int32_t, 2> most{};
    std::array<std::array<float, 4>, 3> turn{};
    std::array<float, 2> scale{};
};

std::unordered_map<std::uint64_t, history_t>& histories() {
    static std::unordered_map<std::uint64_t, history_t> made;
    return made;
}

bool registered = false;

/// As the GPU stops, its textures go with it.
void forget_histories() {
    histories().clear();
}

void release_textures(history_t& history) {
    for (std::uint64_t& texture : history.textures) {
        if (texture != 0) {
            fbe_texture_release(texture);
            texture = 0;
        }
    }
}

/// A place along Halton's sequence, of a base, from 0 to 1.
float halton(std::uint32_t index, std::uint32_t base) {
    float found = 0.0F;
    float part = 1.0F;
    for (std::uint32_t left = index; left > 0; left /= base) {
        part /= static_cast<float>(base);
        found += part * static_cast<float>(left % base);
    }
    return found;
}

} // namespace

history_t* history_of(const scene::camera_t& camera, std::uint64_t target, VkExtent2D size,
                      const std::array<float, 4>& viewport) {
    if (!registered) {
        gpu::at_stop(forget_histories);
        registered = true;
    }
    const scene::scene_t& s = scene::scene();
    history_t& history = histories()[s.entities.slots.handle(camera.entity)];
    const bool same_size = history.size.width == size.width && history.size.height == size.height;
    if (!same_size) {
        release_textures(history);
    }
    if (history.textures[0] == 0 || history.textures[1] == 0) {
        release_textures(history);
        for (std::uint64_t& texture : history.textures) {
            texture = texture::create_engine_target(size.width, size.height, FBE_TEXTURE_RGBA16F,
                                                    VK_SAMPLE_COUNT_1_BIT, false);
            if (texture == 0) {
                release_textures(history);
                return nullptr;
            }
        }
        history.frames = 0;
    }
    const std::int64_t frame = fbe_frame_count();
    if (history.viewport != viewport || history.resets != camera.history_resets ||
        history.target != target || frame - history.last_frame > most_frames_undrawn) {
        history.frames = 0;
    }
    history.target = target;
    history.last_frame = frame;
    history.size = size;
    history.viewport = viewport;
    history.resets = camera.history_resets;
    if (history.frames == 0) {
        history.motion = {};
    }
    const std::uint32_t place = (history.jittered % jitter_count) + 1;
    ++history.jittered;
    history.jitter = {
        ((halton(place, 2) * 2.0F) - 1.0F) / viewport[2],
        ((halton(place, 3) * 2.0F) - 1.0F) / viewport[3],
    };
    return &history;
}

std::uint32_t previous_of(motion_t& motion, const item_t& item, const scene::tweened_t& tweened,
                          bool& failed) {
    const auto [found, fresh] = motion.indices.try_emplace(
        item.draw.entity, static_cast<std::uint32_t>(motion.records.size()));
    if (!fresh) {
        return found->second;
    }
    mat4_t world;
    std::ranges::copy(item.draw.matrix, world.m.begin());
    motion.matrices_now.try_emplace(item.draw.entity, world);
    const auto then = motion.matrices.find(item.draw.entity);
    const mat4_t to_clip = motion.to_clip * (then != motion.matrices.end() ? then->second : world);
    previous_data_t record{};
    record.to_clip = to_clip.m;
    if (item.draw.kind == FBE_ENTITY_MESH) {
        const std::span<const mat4_t> poses = anim::skin_palette(item.entity, tweened);
        if (!poses.empty()) {
            motion.palettes_now[item.draw.entity].assign(poses.begin(), poses.end());
            const auto palette = motion.palettes.find(item.draw.entity);
            if (palette != motion.palettes.end() && palette->second.size() == poses.size()) {
                record.palette = in_ring<mat4_t>(palette->second, failed).address;
                record.has_palette = 1U;
            }
        }
    }
    motion.records.push_back(record);
    return found->second;
}

void finish_motion(motion_t& motion, const mat4_t& to_clip_now) {
    motion.to_clip = to_clip_now;
    motion.matrices.swap(motion.matrices_now);
    motion.matrices_now.clear();
    motion.palettes.swap(motion.palettes_now);
    motion.palettes_now.clear();
    motion.records.clear();
    motion.indices.clear();
}

gpu::resource_t add_resolve(history_t& history, const scene::camera_t& camera,
                            const scene::view_t& view, gpu::resource_t image,
                            gpu::resource_t prepass, const recorded_pass_t& scene_pass) {
    VkPipeline pipeline = gpu::pipeline({
        .shader = &shaders::taa,
        .color = VK_FORMAT_R16G16B16A16_SFLOAT,
    });
    if (pipeline == VK_NULL_HANDLE) {
        return gpu::no_resource;
    }
    const std::size_t next = history.newest ^ 1U;
    const gpu::resource_t before = gpu::texture_resource(history.textures[history.newest]);
    const gpu::resource_t into = gpu::texture_resource(history.textures[next]);
    if (before == gpu::no_resource || into == gpu::no_resource) {
        return gpu::no_resource;
    }
    const mat4_t projection = scene::draw_projection(camera, view);
    const VkRect2D& scissor = scene_pass.scissor;
    temporal_data_t data{
        .size = { static_cast<float>(history.size.width), static_cast<float>(history.size.height) },
        .sampler = gpu::sampler_2d_index(FBE_TEXTURE_LINEAR, false),
        .weight = std::max(settled_weight, 1.0F / static_cast<float>(history.frames + 1)),
        .perspective = camera.projection == FBE_PROJECTION_PERSPECTIVE ? 1U : 0U,
        .viewport = history.viewport,
        .least = { scissor.offset.x, scissor.offset.y },
        .most = { scissor.offset.x + static_cast<std::int32_t>(scissor.extent.width) - 1,
                  scissor.offset.y + static_cast<std::int32_t>(scissor.extent.height) - 1, },
        .scale = { projection.at(0, 0), projection.at(1, 1) },
    };
    // From this view's directions to the world's, then to the last view's.
    const scene::quat_t turned = history.from_world * view.rotation;
    const vec3_t x = turned * vec3_t(1.0F, 0.0F, 0.0F);
    const vec3_t y = turned * vec3_t(0.0F, 1.0F, 0.0F);
    const vec3_t z = turned * vec3_t(0.0F, 0.0F, 1.0F);
    data.turn[0] = { x.x, y.x, z.x, 0.0F };
    data.turn[1] = { x.y, y.y, z.y, 0.0F };
    data.turn[2] = { x.z, y.z, z.z, 0.0F };
    const bool added = gpu::add_pass({
        .name = "temporal resolve",
        .colors = { {
            .target = into,
        }, },
        .uses = { { image, gpu::use_t::SAMPLED }, { prepass, gpu::use_t::SAMPLED },
                  { before, gpu::use_t::SAMPLED }, },
        .record =
            [data, image, prepass, before, pipeline](VkCommandBuffer commands) {
                temporal_data_t push = data;
                push.image = gpu::resource_index(image);
                push.prepass = gpu::resource_index(prepass);
                push.history = gpu::resource_index(before);
                record_full_screen(commands, pipeline, push);
            },
    });
    if (!added) {
        return gpu::no_resource;
    }
    history.from_world = scene::inverse(view.rotation);
    history.newest = next;
    ++history.frames;
    return into;
}

void forget_unused_histories() {
    const scene::scene_t& s = scene::scene();
    for (auto it = histories().begin(); it != histories().end();) {
        const auto slot = s.entities.slots.find(it->first);
        const bool used = slot && s.entities.kind[*slot] == FBE_ENTITY_CAMERA &&
                          s.cameras[s.entities.component[*slot]].temporal_aa &&
                          s.cameras[s.entities.component[*slot]].hdr;
        if (used) {
            ++it;
        } else {
            release_textures(it->second);
            it = histories().erase(it);
        }
    }
}

std::size_t history_count() {
    return histories().size();
}

std::uint32_t history_width(std::uint64_t camera) {
    const auto found = histories().find(camera);
    return found != histories().end()
               ? static_cast<std::uint32_t>(fbe_texture_width(found->second.textures[0]))
               : 0;
}

} // namespace fbe::render
