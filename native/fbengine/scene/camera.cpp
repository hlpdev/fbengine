// Cameras: how each projects, the part of its target it draws to, what it clears, its order among
// the others, and projecting points between the world and its view.

#include "scene/camera.hpp"

#include "core/engine.hpp"
#include "fbengine.h"
#include "morph/morph.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"
#include "shader/shader.hpp"

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

frustum_t frustum_of(const camera_t& camera, const view_t& view) {
    float half_width = 0.0F;
    float half_height = 0.0F;
    half_extents(camera, view, half_width, half_height);
    const vec3_t right = view.rotation * vec3_t(1.0F, 0.0F, 0.0F);
    const vec3_t up = view.rotation * vec3_t(0.0F, 1.0F, 0.0F);
    const vec3_t forward = view.rotation * vec3_t(0.0F, 0.0F, -1.0F);
    frustum_t frustum;
    frustum.eye = view.position;
    frustum.normals[0] = forward;
    frustum.distances[0] = -camera.nearest;
    frustum.normals[1] = -forward;
    frustum.distances[1] = camera.farthest;
    if (camera.projection == FBE_PROJECTION_PERSPECTIVE) {
        frustum.normals[2] = right + forward * half_width;
        frustum.normals[3] = forward * half_width - right;
        frustum.normals[4] = up + forward * half_height;
        frustum.normals[5] = forward * half_height - up;
    } else {
        frustum.normals[2] = right;
        frustum.distances[2] = half_width;
        frustum.normals[3] = -right;
        frustum.distances[3] = half_width;
        frustum.normals[4] = up;
        frustum.distances[4] = half_height;
        frustum.normals[5] = -up;
        frustum.distances[5] = half_height;
    }
    return frustum;
}

bool in_frustum(const frustum_t& frustum, const vec3_t* points, std::size_t count) {
    for (std::size_t side = 0; side < frustum.normals.size(); ++side) {
        bool all_behind = true;
        for (std::size_t i = 0; i < count && all_behind; ++i) {
            const vec3_t offset = points[i] - frustum.eye;
            all_behind = dot(offset, frustum.normals[side]) + frustum.distances[side] < 0.0F;
        }
        if (all_behind) {
            return false;
        }
    }
    return true;
}

bool box_in_frustum(const frustum_t& frustum, vec3_t min, vec3_t max) {
    for (std::size_t side = 0; side < frustum.normals.size(); ++side) {
        // Its corner furthest in front of the side: if that's behind, they all are.
        const vec3_t normal = frustum.normals[side];
        const vec3_t corner(normal.x >= 0.0F ? max.x : min.x, normal.y >= 0.0F ? max.y : min.y,
                            normal.z >= 0.0F ? max.z : min.z);
        const vec3_t offset = corner - frustum.eye;
        if (dot(offset, normal) + frustum.distances[side] < 0.0F) {
            return false;
        }
    }
    return true;
}

/// Whether some of the points might be in view: not all of them behind one of the view
/// planes, near and far included.
bool in_view(const camera_t& camera, const view_t& view, const vec3_t* points, std::size_t count) {
    return in_frustum(frustum_of(camera, view), points, count);
}

std::uint32_t lod_for(std::uint32_t geometry, vec3_t center, float radius, const camera_t& camera,
                      vec3_t eye, float half_height) {
    return lod_at(geometry, length(center - eye), radius, camera, half_height);
}

std::uint32_t lod_at(std::uint32_t geometry, float distance, float radius, const camera_t& camera,
                     float half_height) {
    const std::vector<lod_t>& lods = scene().geometries[geometry].lods;
    if (lods.empty()) {
        return geometry;
    }
    float share = radius / half_height;
    if (camera.projection == FBE_PROJECTION_PERSPECTIVE) {
        share /= std::fmax(distance, camera.nearest);
    }
    std::uint32_t drawn = geometry;
    for (const lod_t& lod : lods) {
        if (share >= lod.share) {
            break;
        }
        drawn = lod.geometry;
    }
    return drawn;
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
    release_texture(s.cameras[index].target);
    for (const std::uint64_t pass : s.cameras[index].passes) {
        fbe_material_release(pass);
    }
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
        // Divided first, so a far plane near float's top doesn't overflow.
        m.at(3, 2) = nearest * (farthest / span);
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

FBE_API bool fbe_camera_set_occlusion(uint64_t camera, bool occlusion) {
    camera_t* found = fbe::scene::camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    found->occlusion = occlusion;
    return true;
}

FBE_API bool fbe_camera_occlusion(uint64_t camera) {
    const camera_t* found = fbe::scene::camera_of(camera);
    return found != nullptr && found->occlusion;
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

FBE_API bool fbe_camera_set_target(uint64_t camera, uint64_t texture) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (texture != 0) {
        if (!fbe_texture_is_target(texture)) {
            return fbe::fail("a camera draws into a target, not another texture");
        }
        if (!fbe_texture_has_depth(texture)) {
            return fbe::fail("a camera draws into a target with depth");
        }
        fbe_texture_retain(texture);
    }
    fbe::scene::release_texture(found->target);
    found->target = texture;
    return true;
}

FBE_API uint64_t fbe_camera_target(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? found->target : 0;
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
    return fbe_camera_tweened_view_matrix(camera, 1.0F, matrix);
}

FBE_API bool fbe_camera_tweened_view_matrix(uint64_t camera, float tween, fbe_mat4_t* matrix) {
    const camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (std::isnan(tween) || tween < 0.0F || tween > 1.0F) {
        return fbe::fail(std::format("a tween is 0 to 1, not {}", tween));
    }
    // The camera's place and turn, without its scale, taken back to the origin.
    const fbe::scene::pose_t pose = fbe::scene::tweened_t(tween).pose(found->entity);
    const fbe::scene::quat_t back = fbe::scene::inverse(pose.rotation);
    const fbe::scene::vec3_t at = pose.matrix.axis(3);
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

FBE_API bool fbe_camera_add_pass(uint64_t camera, uint64_t material) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    if (std::ranges::find(found->passes, material) != found->passes.end()) {
        return fbe::fail("a camera's pass is added once");
    }
    if (fbe::shaders_own::pass_code_of(fbe_material_shader(material)) == 0) {
        return fbe::fail("a camera's pass is a material with a pass's shader, one that says "
                         "`#define FBE_PASS`");
    }
    found->passes.reserve(found->passes.size() + 1);
    if (!fbe_material_retain(material)) {
        return false;
    }
    found->passes.push_back(material);
    return true;
}

FBE_API bool fbe_camera_remove_pass(uint64_t camera, uint64_t material) {
    camera_t* found = camera_of(camera);
    if (found == nullptr) {
        return false;
    }
    const auto at = std::ranges::find(found->passes, material);
    if (at == found->passes.end()) {
        return fbe::fail("that isn't one of the camera's passes");
    }
    found->passes.erase(at);
    fbe_material_release(material);
    return true;
}

FBE_API int32_t fbe_camera_pass_count(uint64_t camera) {
    const camera_t* found = camera_of(camera);
    return found != nullptr ? static_cast<std::int32_t>(found->passes.size()) : -1;
}

FBE_API uint64_t fbe_camera_pass(uint64_t camera, int32_t index) {
    const camera_t* found = camera_of(camera);
    return found != nullptr && index >= 0 && std::cmp_less(index, found->passes.size())
               ? found->passes[static_cast<std::size_t>(index)]
               : 0;
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
    if (s.entities.kind[slot] == FBE_ENTITY_INSTANCES ||
        s.entities.kind[slot] == FBE_ENTITY_PARTICLES) {
        fbe_aabb_t box{};
        if (!fbe_entity_bounds(entity, &box)) {
            return -1;
        }
        return fbe_camera_box_in_view(camera, target_width, target_height, &box);
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
    const std::uint32_t geometry = s.meshes[s.entities.component[slot]].geometry;
    fbe::scene::geometry_bounds(geometry, min, max);
    fbe::anim::grow_by_targets(geometry, fbe::anim::morph_weights(slot, 1.0F), min, max);
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
