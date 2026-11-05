// Sprites: entities that draw a quad facing the camera, or turned as they are, and how a camera
// sees one: the quad's matrix in the world.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <cmath>
#include <cstdint>
#include <format>

namespace fbe::scene {

namespace {

/// A vector made length 1, or `otherwise` for one too short to have a direction.
vec3_t direction(vec3_t v, vec3_t otherwise) {
    const float size = length(v);
    return size > 1e-6F ? v * (1.0F / size) : otherwise;
}

} // namespace

sprite_t* sprite_of(std::uint64_t handle) {
    const std::uint32_t entity = entity_slot(handle);
    if (entity == none) {
        return nullptr;
    }
    if (scene().entities.kind[entity] != FBE_ENTITY_SPRITE) {
        fail("the entity isn't a sprite");
        return nullptr;
    }
    return &scene().sprites[scene().entities.component[entity]];
}

void add_sprite(std::uint32_t entity) {
    scene_t& s = scene();
    s.entities.component[entity] = static_cast<std::uint32_t>(s.sprites.size());
    s.sprites.push_back({ .entity = entity });
}

void remove_sprite(std::uint32_t entity) {
    scene_t& s = scene();
    const std::uint32_t index = s.entities.component[entity];
    release_material(s.sprites[index].material);
    if (index != s.sprites.size() - 1) {
        s.sprites[index] = s.sprites.back();
        s.entities.component[s.sprites[index].entity] = index;
    }
    s.sprites.pop_back();
}

mat4_t sprite_matrix(std::uint32_t entity, std::uint32_t camera) {
    update_world(entity);
    update_world(camera);
    const scene_t& s = scene();
    return sprite_matrix(entity, s.entities.world_matrix[entity],
                         s.entities.world_rotation[camera]);
}

mat4_t sprite_matrix(std::uint32_t entity, const mat4_t& own, quat_t eye) {
    return sprite_matrix(entity, own, eye * vec3_t(1.0F, 0.0F, 0.0F),
                         eye * vec3_t(0.0F, 1.0F, 0.0F), eye * vec3_t(0.0F, 0.0F, 1.0F));
}

mat4_t sprite_matrix(std::uint32_t entity, const mat4_t& own, vec3_t eye_x, vec3_t eye_y,
                     vec3_t eye_z) {
    const scene_t& s = scene();
    const sprite_t& sprite = s.sprites[s.entities.component[entity]];
    const vec3_t place = own.axis(3);
    // The quad's axes in the world, as its view mode turns it, its front facing +Z.
    vec3_t x;
    vec3_t y;
    vec3_t z;
    if (sprite.view_mode == FBE_SPRITE_FIXED) {
        x = own.axis(0);
        y = own.axis(1);
        z = own.axis(2);
    } else if (sprite.view_mode == FBE_SPRITE_UPRIGHT) {
        // Its own up, and toward the camera as far as that lets it.
        y = direction(own.axis(1), vec3_t(0.0F, 1.0F, 0.0F));
        z = direction(eye_z - y * dot(eye_z, y), direction(own.axis(2), vec3_t(0.0F, 0.0F, 1.0F)));
        x = cross(y, z);
    } else if (sprite.view_mode == FBE_SPRITE_UPRIGHT_YAW) {
        // The camera's yaw, from where it looks across the ground, or, looking straight up or down,
        // from its side; then its own turn and scale.
        vec3_t facing = vec3_t(eye_z.x, 0.0F, eye_z.z);
        if (dot(facing, facing) < 0.000001F) {
            facing = vec3_t(-eye_x.z, 0.0F, eye_x.x);
        }
        facing = direction(facing, vec3_t(0.0F, 0.0F, 1.0F));
        const auto yawed = [&](vec3_t axis) {
            return vec3_t((facing.z * axis.x) + (facing.x * axis.z), axis.y,
                          (facing.z * axis.z) - (facing.x * axis.x));
        };
        x = yawed(own.axis(0));
        y = yawed(own.axis(1));
        z = yawed(own.axis(2));
    } else {
        // Parallel to the camera's view, whatever the entity's own turn and scale: the camera's
        // turn alone, as its view has it, so a mirror above the camera doesn't mirror it.
        x = eye_x;
        y = eye_y;
        z = eye_z;
    }
    // Rolled in its plane, counterclockwise as seen, sized, and moved by its handle.
    const float c = sprite.roll != 0.0F ? std::cos(sprite.roll) : 1.0F;
    const float r = sprite.roll != 0.0F ? std::sin(sprite.roll) : 0.0F;
    const vec3_t rolled_x = x * c + y * r;
    const vec3_t rolled_y = y * c - x * r;
    const vec3_t across = rolled_x * sprite.size_x;
    const vec3_t up = rolled_y * sprite.size_y;
    return { across, up, z, place - across * sprite.handle_x - up * sprite.handle_y };
}

void sprite_bounds(std::uint32_t entity, vec3_t& min, vec3_t& max) {
    update_world(entity);
    sprite_bounds(entity, scene().entities.world_matrix[entity], min, max);
}

void sprite_bounds(std::uint32_t entity, const mat4_t& own, vec3_t& min, vec3_t& max) {
    const scene_t& s = scene();
    const sprite_t& sprite = s.sprites[s.entities.component[entity]];
    if (sprite.view_mode == FBE_SPRITE_FIXED) {
        // It doesn't turn toward a camera: the box of its four corners, sheared or not.
        const mat4_t quad = sprite_matrix(entity, own, quat_t());
        for (unsigned corner = 0; corner < 4; ++corner) {
            const vec3_t p = transform_point(quad, vec3_t((corner & 1U) != 0 ? 1.0F : -1.0F,
                                                          (corner & 2U) != 0 ? 1.0F : -1.0F, 0.0F));
            min = corner == 0
                      ? p
                      : vec3_t(std::fmin(min.x, p.x), std::fmin(min.y, p.y), std::fmin(min.z, p.z));
            max = corner == 0
                      ? p
                      : vec3_t(std::fmax(max.x, p.x), std::fmax(max.y, p.y), std::fmax(max.z, p.z));
        }
        return;
    }
    // However it turns, its corners are no further than this from its place.
    const bool scaled = sprite.view_mode == FBE_SPRITE_UPRIGHT_YAW;
    const float wide =
        (std::fabs(sprite.handle_x) + 1.0F) * sprite.size_x * (scaled ? length(own.axis(0)) : 1.0F);
    const float high =
        (std::fabs(sprite.handle_y) + 1.0F) * sprite.size_y * (scaled ? length(own.axis(1)) : 1.0F);
    const float reach = std::sqrt((wide * wide) + (high * high));
    const vec3_t place = own.axis(3);
    min = place - vec3_t(reach, reach, reach);
    max = place + vec3_t(reach, reach, reach);
}

} // namespace fbe::scene

using fbe::scene::entity_slot;
using fbe::scene::none;
using fbe::scene::scene;
using fbe::scene::sprite_t;

extern "C" {

FBE_API bool fbe_sprite_set_view_mode(uint64_t sprite, int32_t mode) {
    sprite_t* found = fbe::scene::sprite_of(sprite);
    if (found == nullptr) {
        return false;
    }
    if (mode < FBE_SPRITE_BILLBOARD || mode > FBE_SPRITE_UPRIGHT_YAW) {
        return fbe::fail(std::format(
            "a view mode is FBE_SPRITE_BILLBOARD to FBE_SPRITE_UPRIGHT_YAW, not {}", mode));
    }
    found->view_mode = mode;
    return true;
}

FBE_API int32_t fbe_sprite_view_mode(uint64_t sprite) {
    const sprite_t* found = fbe::scene::sprite_of(sprite);
    return found != nullptr ? found->view_mode : -1;
}

FBE_API bool fbe_sprite_set_size(uint64_t sprite, float half_width, float half_height) {
    sprite_t* found = fbe::scene::sprite_of(sprite);
    if (found == nullptr) {
        return false;
    }
    if (!(half_width >= 0.0F && half_height >= 0.0F && std::isfinite(half_width) &&
          std::isfinite(half_height))) {
        return fbe::fail(std::format("a sprite's size is 0 or more, finite, not {} by {}",
                                     half_width, half_height));
    }
    found->size_x = half_width;
    found->size_y = half_height;
    return true;
}

FBE_API bool fbe_sprite_size(uint64_t sprite, fbe_vec2_t* size) {
    const sprite_t* found = fbe::scene::sprite_of(sprite);
    if (found == nullptr) {
        return false;
    }
    *size = { .x = found->size_x, .y = found->size_y };
    return true;
}

FBE_API bool fbe_sprite_set_handle(uint64_t sprite, float x, float y) {
    sprite_t* found = fbe::scene::sprite_of(sprite);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return fbe::fail(std::format("a sprite's handle is finite, not ({}, {})", x, y));
    }
    found->handle_x = x;
    found->handle_y = y;
    return true;
}

FBE_API bool fbe_sprite_handle(uint64_t sprite, fbe_vec2_t* handle) {
    const sprite_t* found = fbe::scene::sprite_of(sprite);
    if (found == nullptr) {
        return false;
    }
    *handle = { .x = found->handle_x, .y = found->handle_y };
    return true;
}

FBE_API bool fbe_sprite_set_roll(uint64_t sprite, float radians) {
    sprite_t* found = fbe::scene::sprite_of(sprite);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(radians)) {
        return fbe::fail(std::format("a sprite's roll is finite, not {}", radians));
    }
    found->roll = radians;
    return true;
}

FBE_API float fbe_sprite_roll(uint64_t sprite) {
    const sprite_t* found = fbe::scene::sprite_of(sprite);
    return found != nullptr ? found->roll : 0.0F;
}

FBE_API bool fbe_sprite_matrix(uint64_t sprite, uint64_t camera, fbe_mat4_t* matrix) {
    const std::uint32_t slot = entity_slot(sprite);
    if (slot == none) {
        return false;
    }
    if (scene().entities.kind[slot] != FBE_ENTITY_SPRITE) {
        return fbe::fail("the entity isn't a sprite");
    }
    const std::uint32_t eye = entity_slot(camera);
    if (eye == none) {
        return false;
    }
    const fbe::scene::mat4_t found = fbe::scene::sprite_matrix(slot, eye);
    for (std::size_t i = 0; i < 16; ++i) {
        matrix->m[i] = found.m[i];
    }
    return true;
}

} // extern "C"
