#pragma once

// A camera's view in a target, which its projections, the in-view tests, and drawing share.

#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstddef>

namespace fbe::scene {

/// A camera's viewport in its target's units, and its view, as the projections work them out.
/// A viewport with no size, as a window dragged to nothing high has, has the shape of a square,
/// and nothing projects into it.
struct view_t {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    bool sized = true;
    vec3_t position;
    quat_t rotation;
};

/// The viewport in a target of this size, and where the camera is.
bool view_of(const camera_t& camera, float target_width, float target_height, view_t& view);

/// Half the view's height and width at a depth of 1, for a perspective camera, or in world units,
/// for an orthographic one.
void half_extents(const camera_t& camera, const view_t& view, float& half_width,
                  float& half_height);

/// A view's six sides in the world, near, far, left, right, bottom, top, for many tests of what's
/// in it: how far in front of a side a point is, from the eye, is its normal's dot with the point's
/// offset, plus its distance.
struct frustum_t {
    vec3_t eye;
    std::array<vec3_t, 6> normals;
    std::array<float, 6> distances{};
};

frustum_t frustum_of(const camera_t& camera, const view_t& view);

/// Whether a frustum might have any of some points: false only when every one is behind one side.
bool in_frustum(const frustum_t& frustum, const vec3_t* points, std::size_t count);

/// Whether a frustum might have any of a box, along the world's axes: as in_frustum of its corners.
bool box_in_frustum(const frustum_t& frustum, vec3_t min, vec3_t max);

/// Whether a camera might see any of some points, by the six planes of its frustum: false only
/// when every one is behind one plane.
bool in_view(const camera_t& camera, const view_t& view, const vec3_t* points, std::size_t count);

/// Whether a camera might see any of a plane through `place` facing `normal`, from above: false
/// only when its whole view is under it.
bool plane_in_view(const camera_t& camera, const view_t& view, vec3_t place, vec3_t normal);

/// The camera's projection as the engine draws with it: Vulkan's clip space, y down, and depth
/// reversed, 1 at its near plane and 0 at its far one.
mat4_t draw_projection(const camera_t& camera, const view_t& view);

} // namespace fbe::scene
