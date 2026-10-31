// Cameras: how each projects, the part of its target it draws to, what it clears, its order among
// the others, and projecting points between the world and its view.

#include "scene/camera.hpp"

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numbers>
#include <vector>

namespace fbe::scene {

namespace {

/// How far in front a point a camera sees can be, its nearest less what rounding moves a point on
/// the near plane by, so Unproject's points at the near plane project again.
float nearest_seen(const camera_t& camera) {
    return camera.nearest * (1.0F - 1e-4F);
}

bool valid_range(float nearest, float farthest) {
    return nearest > 0.0F && nearest < farthest && std::isfinite(farthest);
}

fbe_vec3_t from_vec(vec3_t v) {
    return { .x = v.x, .y = v.y, .z = v.z };
}

/// The view a handle's camera has in a target, or false with fbe_error saying why.
bool camera_view(std::uint64_t handle, float target_width, float target_height,
                 const camera_t*& camera, view_t& view) {
    camera = camera_of(handle);
    return camera != nullptr && view_of(*camera, target_width, target_height, view);
}

} // namespace

/// The viewport in a target of this size, and where the camera is.
bool view_of(const camera_t& camera, float target_width, float target_height, view_t& view) {
    if (camera.whole_target) {
        view.width = target_width;
        view.height = target_height;
    } else {
        view.x = camera.viewport_x;
        view.y = camera.viewport_y;
        view.width = camera.viewport_width;
        view.height = camera.viewport_height;
    }
    view.sized = view.width > 0.0F && view.height > 0.0F && std::isfinite(view.width) &&
                 std::isfinite(view.height);
    update_world(camera.entity);
    view.position = scene().entities.world_matrix[camera.entity].axis(3);
    view.rotation = scene().entities.world_rotation[camera.entity];
    return true;
}

/// Half the view's height and width at a depth of 1, for a perspective camera, or in world units,
/// for an orthographic one.
void half_extents(const camera_t& camera, const view_t& view, float& half_width,
                  float& half_height) {
    half_height = camera.projection == FBE_PROJECTION_PERSPECTIVE
                      ? std::tan(camera.field_of_view / 2.0F)
                      : camera.orthographic_height / 2.0F;
    half_width = view.sized ? half_height * (view.width / view.height) : half_height;
}

/// Whether some of the points might be in view: not all of them behind one of the view's six
/// planes, near and far included.
bool in_view(const camera_t& camera, const view_t& view, const vec3_t* points, std::size_t count) {
    float half_width = 0.0F;
    float half_height = 0.0F;
    half_extents(camera, view, half_width, half_height);
    const bool perspective = camera.projection == FBE_PROJECTION_PERSPECTIVE;
    const quat_t back = inverse(view.rotation);
    // How far in front of each plane each point is, by plane: near, far, left, right, bottom, top.
    std::array<bool, 6> all_behind{ true, true, true, true, true, true };
    for (std::size_t i = 0; i < count; ++i) {
        const vec3_t offset = points[i] - view.position;
        const vec3_t seen = back * offset;
        const float depth = -seen.z;
        const float across = perspective ? half_width * depth : half_width;
        const float up = perspective ? half_height * depth : half_height;
        const std::array<float, 6> in_front{
            depth - camera.nearest,
            camera.farthest - depth,
            seen.x + across,
            across - seen.x,
            seen.y + up,
            up - seen.y,
        };
        for (std::size_t plane = 0; plane < 6; ++plane) {
            all_behind[plane] = all_behind[plane] && in_front[plane] < 0.0F;
        }
    }
    return std::ranges::none_of(all_behind, [](bool behind) { return behind; });
}

void add_camera(std::uint32_t entity) {
    scene_t& s = scene();
    camera_t camera;
    camera.entity = entity;
    camera.made = ++s.cameras_made;
    s.entities.component[entity] = static_cast<std::uint32_t>(s.cameras.size());
    s.cameras.push_back(camera);
}

void remove_camera(std::uint32_t entity) {
    scene_t& s = scene();
    const std::uint32_t index = s.entities.component[entity];
    if (index != s.cameras.size() - 1) {
        s.cameras[index] = s.cameras.back();
        s.entities.component[s.cameras[index].entity] = index;
    }
    s.cameras.pop_back();
}

void renumber_cameras(const std::vector<std::pair<std::uint32_t, std::size_t>>& originals,
                      const std::vector<std::uint32_t>& copies) {
    scene_t& s = scene();
    std::vector<std::pair<std::uint64_t, std::uint32_t>> made;
    for (std::size_t i = 0; i < copies.size(); ++i) {
        const std::uint32_t original = originals[i].first;
        if (s.entities.kind[original] == FBE_ENTITY_CAMERA) {
            made.emplace_back(s.cameras[s.entities.component[original]].made, copies[i]);
        }
    }
    std::ranges::sort(made);
    for (const auto& [when, copy] : made) {
        s.cameras[s.entities.component[copy]].made = ++s.cameras_made;
    }
}

camera_t* camera_of(std::uint64_t handle) {
    const std::uint32_t entity = entity_slot(handle);
    if (entity == none) {
        return nullptr;
    }
    if (scene().entities.kind[entity] != FBE_ENTITY_CAMERA) {
        fail("the entity isn't a camera");
        return nullptr;
    }
    return &scene().cameras[scene().entities.component[entity]];
}

bool plane_in_view(const camera_t& camera, const view_t& view, vec3_t place, vec3_t normal) {
    float half_width = 0.0F;
    float half_height = 0.0F;
    half_extents(camera, view, half_width, half_height);
    const bool perspective = camera.projection == FBE_PROJECTION_PERSPECTIVE;
    // The view's eight corners, near and far, in the world.
    for (const float depth : { camera.nearest, camera.farthest }) {
        const float across = perspective ? half_width * depth : half_width;
        const float up = perspective ? half_height * depth : half_height;
        for (unsigned corner = 0; corner < 4; ++corner) {
            const vec3_t seen((corner & 1U) != 0 ? across : -across, (corner & 2U) != 0 ? up : -up,
                              -depth);
            const vec3_t at = view.position + view.rotation * seen;
            if (dot(at - place, normal) >= 0.0F) {
                return true;
            }
        }
    }
    return false;
}

mat4_t draw_projection(const camera_t& camera, const view_t& view) {
    float half_width = 0.0F;
    float half_height = 0.0F;
    half_extents(camera, view, half_width, half_height);
    const float nearest = camera.nearest;
    const float farthest = camera.farthest;
    const float span = farthest - nearest;
    // Vulkan's y points down, and depth is reversed: 1 at the near plane, 0 at the far one.
    mat4_t m;
    m.at(0, 0) = 1.0F / half_width;
    m.at(1, 1) = -1.0F / half_height;
    if (camera.projection == FBE_PROJECTION_PERSPECTIVE) {
        m.at(2, 2) = nearest / span;
        m.at(3, 2) = nearest * farthest / span;
        m.at(2, 3) = -1.0F;
        m.at(3, 3) = 0.0F;
    } else {
        m.at(2, 2) = 1.0F / span;
        m.at(3, 2) = farthest / span;
    }
    return m;
}

} // namespace fbe::scene

using fbe::scene::camera_of;
using fbe::scene::camera_t;

extern "C" {

FBE_API bool fbe_camera_set_projection(uint64_t camera, int32_t projection) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (projection != FBE_PROJECTION_PERSPECTIVE && projection != FBE_PROJECTION_ORTHOGRAPHIC) {
        return fbe::fail(std::format("{} isn't a Projection", projection));
    }
    found->projection = projection;
    return true;
}

FBE_API int32_t fbe_camera_projection(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->projection : -1;
}

FBE_API bool fbe_camera_set_field_of_view(uint64_t camera, float angle) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    // One so small that the projection's 1 / tan(angle / 2) isn't finite has no view.
    if (angle <= 0.0F || angle >= std::numbers::pi_v<float> || std::isnan(angle) ||
        !std::isfinite(1.0F / std::tan(angle / 2.0F))) {
        return fbe::fail(std::format("a field of view is between 0 and Pi, not {}", angle));
    }
    found->field_of_view = angle;
    return true;
}

FBE_API float fbe_camera_field_of_view(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->field_of_view : 0.0F;
}

FBE_API bool fbe_camera_set_orthographic_height(uint64_t camera, float height) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (height <= 0.0F || !std::isfinite(height) || !std::isfinite(2.0F / height)) {
        return fbe::fail(std::format("an orthographic height is above 0, not {}", height));
    }
    found->orthographic_height = height;
    return true;
}

FBE_API float fbe_camera_orthographic_height(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->orthographic_height : 0.0F;
}

FBE_API bool fbe_camera_set_range(uint64_t camera, float nearest, float farthest) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (!fbe::scene::valid_range(nearest, farthest)) {
        return fbe::fail(std::format("a camera's range is 0 < near < far, finite, not {} to {}",
                                     nearest, farthest));
    }
    found->nearest = nearest;
    found->farthest = farthest;
    return true;
}

FBE_API float fbe_camera_nearest(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->nearest : 0.0F;
}

FBE_API float fbe_camera_farthest(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->farthest : 0.0F;
}

FBE_API bool fbe_camera_set_viewport(uint64_t camera, float x, float y, float width, float height) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (!(width > 0.0F && height > 0.0F && std::isfinite(x) && std::isfinite(y) &&
          std::isfinite(width) && std::isfinite(height))) {
        return fbe::fail(std::format("a viewport has a finite place and a size above 0, not {}, "
                                     "{} at {}x{}",
                                     x, y, width, height));
    }
    found->whole_target = false;
    found->viewport_x = x;
    found->viewport_y = y;
    found->viewport_width = width;
    found->viewport_height = height;
    return true;
}

FBE_API bool fbe_camera_reset_viewport(uint64_t camera) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    found->whole_target = true;
    return true;
}

FBE_API bool fbe_camera_viewport(uint64_t camera, float target_width, float target_height,
                                 fbe_viewport_t* viewport) {
    const camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (found->whole_target) {
        *viewport = { .x = 0.0F, .y = 0.0F, .width = target_width, .height = target_height };
    } else {
        *viewport = {
            .x = found->viewport_x,
            .y = found->viewport_y,
            .width = found->viewport_width,
            .height = found->viewport_height,
        };
    }
    return true;
}

FBE_API bool fbe_camera_whole_target(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr && found->whole_target;
}

FBE_API bool fbe_camera_set_clear(uint64_t camera, bool color, bool depth) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    found->clears_color = color;
    found->clears_depth = depth;
    return true;
}

FBE_API bool fbe_camera_clears_color(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr && found->clears_color;
}

FBE_API bool fbe_camera_clears_depth(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr && found->clears_depth;
}

FBE_API bool fbe_camera_set_clear_color(uint64_t camera, float r, float g, float b, float a) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    found->clear_color = { r, g, b, a };
    return true;
}

FBE_API bool fbe_camera_clear_color(uint64_t camera, fbe_color_t* color) {
    const camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    *color = {
        .r = found->clear_color[0],
        .g = found->clear_color[1],
        .b = found->clear_color[2],
        .a = found->clear_color[3],
    };
    return true;
}

FBE_API bool fbe_camera_set_order(uint64_t camera, int32_t order) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    found->order = order;
    return true;
}

FBE_API int32_t fbe_camera_order(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->order : 0;
}

FBE_API int32_t fbe_world_cameras(uint64_t world, uint64_t* cameras, int32_t capacity) {
    const std::uint32_t slot = fbe::scene::world_slot(world);
    if (slot == fbe::scene::none) {
        return -1;
    }
    const fbe::scene::scene_t& s = fbe::scene::scene();
    std::vector<const camera_t*> in;
    for (const camera_t& camera : s.cameras) {
        if (s.entities.world[camera.entity] == slot) {
            in.push_back(&camera);
        }
    }
    std::ranges::sort(in, [](const camera_t* a, const camera_t* b) {
        return a->order != b->order ? a->order < b->order : a->made < b->made;
    });
    const auto count = std::min(in.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    for (std::size_t i = 0; i < count; ++i) {
        cameras[i] = s.entities.slots.handle(in[i]->entity);
    }
    return static_cast<std::int32_t>(in.size());
}

FBE_API bool fbe_camera_view_matrix(uint64_t camera, fbe_mat4_t* matrix) {
    const camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    fbe::scene::update_world(found->entity);
    const fbe::scene::scene_t& s = fbe::scene::scene();
    // The camera's place and turn, without its scale, taken back to the origin.
    const fbe::scene::quat_t back = fbe::scene::inverse(s.entities.world_rotation[found->entity]);
    const fbe::scene::vec3_t at = s.entities.world_matrix[found->entity].axis(3);
    const fbe::scene::mat4_t view = fbe::scene::compose(back * -at, back, { 1.0F, 1.0F, 1.0F });
    std::ranges::copy(view.m, matrix->m);
    return true;
}

FBE_API bool fbe_camera_draw_projection(uint64_t camera, float target_width, float target_height,
                                        fbe_mat4_t* matrix) {
    const camera_t* found = camera_of(camera);
    fbe::scene::view_t view;
    if (found == nullptr || !fbe::scene::view_of(*found, target_width, target_height, view)) {
        return false;
    }
    const fbe::scene::mat4_t m = fbe::scene::draw_projection(*found, view);
    std::ranges::copy(m.m, matrix->m);
    return true;
}

FBE_API bool fbe_camera_projection_matrix(uint64_t camera, float target_width, float target_height,
                                          fbe_mat4_t* matrix) {
    const camera_t* found = camera_of(camera);
    fbe::scene::view_t view;
    if (found == nullptr || !fbe::scene::view_of(*found, target_width, target_height, view)) {
        return false;
    }
    float half_width = 0.0F;
    float half_height = 0.0F;
    fbe::scene::half_extents(*found, view, half_width, half_height);
    // std.math's Mat4.Perspective and Orthographic: right-handed, depth from 0 at near to 1 at far.
    fbe::scene::mat4_t m;
    const float nearest = found->nearest;
    const float farthest = found->farthest;
    if (found->projection == FBE_PROJECTION_PERSPECTIVE) {
        const float depth = farthest / (nearest - farthest);
        m.at(0, 0) = 1.0F / half_width;
        m.at(1, 1) = 1.0F / half_height;
        m.at(2, 2) = depth;
        m.at(2, 3) = -1.0F;
        m.at(3, 2) = nearest * depth;
        m.at(3, 3) = 0.0F;
    } else {
        m.at(0, 0) = 1.0F / half_width;
        m.at(1, 1) = 1.0F / half_height;
        m.at(2, 2) = -1.0F / (farthest - nearest);
        m.at(3, 2) = -nearest / (farthest - nearest);
    }
    std::ranges::copy(m.m, matrix->m);
    return true;
}

FBE_API int32_t fbe_camera_project(uint64_t camera, float target_width, float target_height,
                                   float x, float y, float z, fbe_vec3_t* projected) {
    const camera_t* found = camera_of(camera);
    fbe::scene::view_t view;
    if (found == nullptr || !fbe::scene::view_of(*found, target_width, target_height, view)) {
        return -1;
    }
    const fbe::scene::vec3_t offset = fbe::scene::vec3_t(x, y, z) - view.position;
    const fbe::scene::vec3_t seen = fbe::scene::inverse(view.rotation) * offset;
    const float depth = -seen.z;
    if (!view.sized || !(depth >= fbe::scene::nearest_seen(*found))) {
        return 0;
    }
    float half_width = 0.0F;
    float half_height = 0.0F;
    fbe::scene::half_extents(*found, view, half_width, half_height);
    const float scale = found->projection == FBE_PROJECTION_PERSPECTIVE ? 1.0F / depth : 1.0F;
    const float across = seen.x * scale / half_width;
    const float up = seen.y * scale / half_height;
    const fbe::scene::vec3_t at(view.x + ((across + 1.0F) * 0.5F * view.width),
                                view.y + ((1.0F - up) * 0.5F * view.height), depth);
    *projected = fbe::scene::from_vec(at);
    return 1;
}

FBE_API int32_t fbe_camera_point_in_view(uint64_t camera, float target_width, float target_height,
                                         float x, float y, float z) {
    const camera_t* found = nullptr;
    fbe::scene::view_t view;
    if (!fbe::scene::camera_view(camera, target_width, target_height, found, view)) {
        return -1;
    }
    const fbe::scene::vec3_t point(x, y, z);
    return fbe::scene::in_view(*found, view, &point, 1) ? 1 : 0;
}

FBE_API int32_t fbe_camera_box_in_view(uint64_t camera, float target_width, float target_height,
                                       const fbe_aabb_t* box) {
    const camera_t* found = nullptr;
    fbe::scene::view_t view;
    if (!fbe::scene::camera_view(camera, target_width, target_height, found, view)) {
        return -1;
    }
    std::array<fbe::scene::vec3_t, 8> corners;
    for (std::size_t corner = 0; corner < 8; ++corner) {
        corners[corner] = { (corner & 1U) != 0 ? box->max.x : box->min.x,
                            (corner & 2U) != 0 ? box->max.y : box->min.y,
                            (corner & 4U) != 0 ? box->max.z : box->min.z };
    }
    return fbe::scene::in_view(*found, view, corners.data(), corners.size()) ? 1 : 0;
}

FBE_API int32_t fbe_camera_entity_in_view(uint64_t camera, float target_width, float target_height,
                                          uint64_t entity) {
    const camera_t* found = nullptr;
    fbe::scene::view_t view;
    if (!fbe::scene::camera_view(camera, target_width, target_height, found, view)) {
        return -1;
    }
    const std::uint32_t slot = fbe::scene::entity_slot(entity);
    if (slot == fbe::scene::none) {
        return -1;
    }
    const fbe::scene::scene_t& s = fbe::scene::scene();
    // A camera sees only its own World.
    if (s.entities.world[slot] != s.entities.world[found->entity]) {
        return 0;
    }
    fbe::scene::update_world(slot);
    const fbe::scene::mat4_t& m = s.entities.world_matrix[slot];
    if (s.entities.kind[slot] == FBE_ENTITY_PLANE) {
        const fbe::scene::vec3_t up =
            fbe::scene::transform_normal(m, fbe::scene::vec3_t(0.0F, 1.0F, 0.0F));
        return fbe::scene::plane_in_view(*found, view, m.axis(3), up) ? 1 : 0;
    }
    if (s.entities.kind[slot] == FBE_ENTITY_SPRITE || s.entities.kind[slot] == FBE_ENTITY_TERRAIN) {
        // A sprite's quad stays in its box however it turns; a terrain's grid is in its own.
        fbe::scene::vec3_t min;
        fbe::scene::vec3_t max;
        if (s.entities.kind[slot] == FBE_ENTITY_SPRITE) {
            fbe::scene::sprite_bounds(slot, min, max);
        } else {
            fbe::scene::vec3_t local_min;
            fbe::scene::vec3_t local_max;
            fbe::scene::terrain_bounds(s.terrains[s.entities.component[slot]], local_min,
                                       local_max);
            fbe::scene::world_box(slot, local_min, local_max, min, max);
        }
        std::array<fbe::scene::vec3_t, 8> corners;
        for (std::size_t corner = 0; corner < 8; ++corner) {
            corners[corner] = fbe::scene::vec3_t((corner & 1U) != 0 ? max.x : min.x,
                                                 (corner & 2U) != 0 ? max.y : min.y,
                                                 (corner & 4U) != 0 ? max.z : min.z);
        }
        return fbe::scene::in_view(*found, view, corners.data(), corners.size()) ? 1 : 0;
    }
    if (s.entities.kind[slot] != FBE_ENTITY_MESH ||
        s.meshes[s.entities.component[slot]].geometry == fbe::scene::none) {
        const fbe::scene::vec3_t at = m.axis(3);
        return fbe::scene::in_view(*found, view, &at, 1) ? 1 : 0;
    }
    // The box's corners turned and scaled with the mesh, not the box around them.
    fbe::scene::vec3_t min;
    fbe::scene::vec3_t max;
    fbe::scene::geometry_bounds(s.meshes[s.entities.component[slot]].geometry, min, max);
    std::array<fbe::scene::vec3_t, 8> corners;
    for (std::size_t corner = 0; corner < 8; ++corner) {
        const fbe::scene::vec3_t local((corner & 1U) != 0 ? max.x : min.x,
                                       (corner & 2U) != 0 ? max.y : min.y,
                                       (corner & 4U) != 0 ? max.z : min.z);
        corners[corner] = fbe::scene::transform_point(m, local);
    }
    return fbe::scene::in_view(*found, view, corners.data(), corners.size()) ? 1 : 0;
}

FBE_API int32_t fbe_camera_unproject(uint64_t camera, float target_width, float target_height,
                                     float x, float y, float depth, fbe_vec3_t* point) {
    const camera_t* found = camera_of(camera);
    fbe::scene::view_t view;
    if (found == nullptr || !fbe::scene::view_of(*found, target_width, target_height, view)) {
        return -1;
    }
    if (!view.sized) {
        return 0;
    }
    float half_width = 0.0F;
    float half_height = 0.0F;
    fbe::scene::half_extents(*found, view, half_width, half_height);
    const float across = (((x - view.x) / view.width) * 2.0F) - 1.0F;
    const float up = 1.0F - (((y - view.y) / view.height) * 2.0F);
    const float scale = found->projection == FBE_PROJECTION_PERSPECTIVE ? depth : 1.0F;
    const fbe::scene::vec3_t seen(across * half_width * scale, up * half_height * scale, -depth);
    *point = fbe::scene::from_vec(view.rotation * seen + view.position);
    return 1;
}

} // extern "C"
