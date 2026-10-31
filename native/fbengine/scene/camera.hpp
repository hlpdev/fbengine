#pragma once

// A camera's view in a target, which its projections, the in-view tests, and drawing share.

#include "scene/math.hpp"
#include "scene/scene.hpp"

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

/// Whether a camera might see any of some points, by the six planes of its frustum: false only
/// when every one is behind one plane.
bool in_view(const camera_t& camera, const view_t& view, const vec3_t* points, std::size_t count);

/// The camera's projection as the engine draws with it: Vulkan's clip space, y down, and depth
/// reversed, 1 at its near plane and 0 at its far one.
mat4_t draw_projection(const camera_t& camera, const view_t& view);

} // namespace fbe::scene
