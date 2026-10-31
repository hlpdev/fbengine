// Extracting what a camera draws from the scene into a list, in the order it draws it, so drawing
// reads the list and never the live scene.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

namespace fbe::render {

namespace {

using scene::camera_t;
using scene::mat4_t;
using scene::none;
using scene::vec3_t;

/// A draw, and what it's sorted by.
struct item_t {
    fbe_draw_t draw{};
    std::int32_t order = 0;
    bool blended = false;
    std::uint32_t material = none;
    std::uint32_t entity = none;
    std::uint64_t made = 0;
};

void put(fbe_draw_t& draw, const mat4_t& matrix, const std::array<float, 4>& tint, float fade) {
    std::ranges::copy(matrix.m, draw.matrix);
    draw.color[0] = tint[0];
    draw.color[1] = tint[1];
    draw.color[2] = tint[2];
    draw.color[3] = tint[3] * fade;
}

/// The item of something drawn in `material` with this alpha, at `matrix`, seen from a view.
item_t item_of(std::uint32_t entity, std::uint32_t material, const mat4_t& matrix,
               const std::array<float, 4>& tint, float fade, const scene::view_t& view) {
    const scene::scene_t& s = scene::scene();
    item_t item;
    item.entity = entity;
    item.made = s.entities.made[entity];
    item.material = material;
    item.order = material != none ? s.materials[material].order : 0;
    item.draw.entity = s.entities.slots.handle(entity);
    item.draw.kind = s.entities.kind[entity];
    item.draw.surface = -1;
    item.draw.material = material != none ? s.material_slots.handle(material) : 0;
    item.draw.blend = scene::drawn_blend(material, tint[3] * fade);
    item.blended = item.draw.blend != FBE_BLEND_OPAQUE;
    put(item.draw, matrix, tint, fade);
    const vec3_t forward = view.rotation * vec3_t(0.0F, 0.0F, -1.0F);
    item.draw.depth = scene::dot(matrix.axis(3) - view.position, forward);
    return item;
}

/// The eight corners of a box through a matrix.
std::array<vec3_t, 8> corners_of(const mat4_t& matrix, vec3_t min, vec3_t max) {
    std::array<vec3_t, 8> corners;
    for (unsigned corner = 0; corner < 8; ++corner) {
        corners[corner] = scene::transform_point(
            matrix, vec3_t((corner & 1U) != 0 ? max.x : min.x, (corner & 2U) != 0 ? max.y : min.y,
                           (corner & 4U) != 0 ? max.z : min.z));
    }
    return corners;
}

/// What a camera draws, in its order.
std::vector<item_t> extract(const camera_t& camera, const scene::view_t& view,
                            const scene::tweened_t& tweened) {
    const scene::scene_t& s = scene::scene();
    const std::uint32_t world = s.entities.world[camera.entity];
    std::vector<item_t> items;
    const auto seen = [&](std::uint32_t entity, float& fade) {
        if (s.entities.world[entity] != world || !scene::showing(entity)) {
            return false;
        }
        fade = scene::entity_fade(entity, camera.entity);
        return fade > 0.0F;
    };
    for (const scene::mesh_t& mesh : s.meshes) {
        float fade = 1.0F;
        if (mesh.geometry == none || !seen(mesh.entity, fade) || mesh.tint[3] * fade <= 0.0F) {
            continue;
        }
        const mat4_t matrix = tweened.pose(mesh.entity).matrix;
        // Its cull box, if its geometry has one, else its bounds.
        const scene::geometry_t& geometry = s.geometries[mesh.geometry];
        vec3_t min = geometry.cull_min;
        vec3_t max = geometry.cull_max;
        if (!geometry.has_cull_box) {
            scene::geometry_bounds(mesh.geometry, min, max);
        }
        const std::array<vec3_t, 8> corners = corners_of(matrix, min, max);
        if (!scene::in_view(camera, view, corners.data(), corners.size())) {
            continue;
        }
        const std::vector<scene::surface_t>& surfaces = s.geometries[mesh.geometry].surfaces;
        for (std::size_t surface = 0; surface < surfaces.size(); ++surface) {
            if (surfaces[surface].indices.empty()) {
                continue;
            }
            const std::uint32_t material =
                mesh.material != none ? mesh.material : surfaces[surface].material;
            item_t item = item_of(mesh.entity, material, matrix, mesh.tint, fade, view);
            item.draw.geometry = s.geometry_slots.handle(mesh.geometry);
            item.draw.surface = static_cast<std::int32_t>(surface);
            items.push_back(item);
        }
    }
    for (const scene::sprite_t& sprite : s.sprites) {
        float fade = 1.0F;
        if (!seen(sprite.entity, fade) || sprite.tint[3] * fade <= 0.0F) {
            continue;
        }
        // Where it is between the steps, turned to the camera as it is then.
        const mat4_t own = tweened.pose(sprite.entity).matrix;
        vec3_t min;
        vec3_t max;
        scene::sprite_bounds(sprite.entity, own, min, max);
        const std::array<vec3_t, 8> corners = corners_of(mat4_t(), min, max);
        if (!scene::in_view(camera, view, corners.data(), corners.size())) {
            continue;
        }
        const mat4_t matrix = scene::sprite_matrix(sprite.entity, own, view.rotation);
        items.push_back(item_of(sprite.entity, sprite.material, matrix, sprite.tint, fade, view));
    }
    for (const scene::plane_t& plane : s.planes) {
        float fade = 1.0F;
        if (!seen(plane.entity, fade) || plane.tint[3] * fade <= 0.0F) {
            continue;
        }
        // It goes on to the horizon, so it's in view unless the whole view is under it.
        const mat4_t matrix = tweened.pose(plane.entity).matrix;
        const vec3_t up = scene::transform_normal(matrix, vec3_t(0.0F, 1.0F, 0.0F));
        if (!scene::plane_in_view(camera, view, matrix.axis(3), up)) {
            continue;
        }
        items.push_back(item_of(plane.entity, plane.material, matrix, plane.tint, fade, view));
    }
    for (const scene::terrain_t& terrain : s.terrains) {
        float fade = 1.0F;
        if (!seen(terrain.entity, fade) || terrain.tint[3] * fade <= 0.0F) {
            continue;
        }
        // Its grid's box; drawing makes its triangles from its heights.
        const mat4_t matrix = tweened.pose(terrain.entity).matrix;
        vec3_t min;
        vec3_t max;
        scene::terrain_bounds(terrain, min, max);
        const std::array<vec3_t, 8> corners = corners_of(matrix, min, max);
        if (!scene::in_view(camera, view, corners.data(), corners.size())) {
            continue;
        }
        item_t item = item_of(terrain.entity, terrain.material, matrix, terrain.tint, fade, view);
        // How far its grid's middle is, since its place is a corner of it.
        const vec3_t across = max - min;
        const vec3_t middle = scene::transform_point(matrix, min + across * 0.5F);
        const vec3_t forward = view.rotation * vec3_t(0.0F, 0.0F, -1.0F);
        item.draw.depth = scene::dot(middle - view.position, forward);
        items.push_back(item);
    }
    // By order; in one, opaque by material and nearest first, then blended furthest first; ties as
    // the entities were made, and a mesh's surfaces in order, so a frame draws as the last did
    // whatever's freed between.
    std::ranges::sort(items, [](const item_t& a, const item_t& b) {
        if (a.order != b.order) {
            return a.order < b.order;
        }
        if (a.blended != b.blended) {
            return !a.blended;
        }
        if (!a.blended && a.material != b.material) {
            return a.material < b.material;
        }
        if (a.draw.depth != b.draw.depth) {
            return a.blended ? a.draw.depth > b.draw.depth : a.draw.depth < b.draw.depth;
        }
        if (a.made != b.made) {
            return a.made < b.made;
        }
        return a.draw.surface < b.draw.surface;
    });
    return items;
}

} // namespace

} // namespace fbe::render

extern "C" {

FBE_API int32_t fbe_render_draws(uint64_t camera, float target_width, float target_height,
                                 float tween, fbe_draw_t* draws, int32_t capacity) {
    const std::uint32_t slot = fbe::scene::entity_slot(camera);
    if (slot == fbe::scene::none) {
        return -1;
    }
    const fbe::scene::scene_t& s = fbe::scene::scene();
    if (s.entities.kind[slot] != FBE_ENTITY_CAMERA) {
        fbe::fail("the entity isn't a camera");
        return -1;
    }
    if (std::isnan(tween) || tween < 0.0F || tween > 1.0F) {
        fbe::fail(std::format("a tween is 0 to 1, not {}", tween));
        return -1;
    }
    const fbe::scene::camera_t& found = s.cameras[s.entities.component[slot]];
    fbe::scene::view_t view;
    if (!fbe::scene::showing(slot) ||
        !fbe::scene::view_of(found, target_width, target_height, view) || !view.sized) {
        return 0;
    }
    // Seen from where the camera is between the steps, as everything else is drawn.
    const fbe::scene::tweened_t tweened(tween);
    const fbe::scene::pose_t eye = tweened.pose(slot);
    view.position = eye.matrix.axis(3);
    view.rotation = eye.rotation;
    const std::vector<fbe::render::item_t> items = fbe::render::extract(found, view, tweened);
    const auto count = std::min(items.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    for (std::size_t i = 0; i < count; ++i) {
        draws[i] = items[i].draw;
    }
    return static_cast<std::int32_t>(items.size());
}

} // extern "C"
