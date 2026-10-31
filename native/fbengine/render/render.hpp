#pragma once

// What the render area's files share: the camera a call draws for, and its view as drawing sees
// from.

#include "scene/camera.hpp"
#include "scene/scene.hpp"

#include <cstdint>

namespace fbe::render {

/// The camera a handle is for, its slot, and its view in a target of this size `tween` of the way
/// from its World's last capture, as drawing sees from: false, with fbe_error saying why, for a
/// freed camera, another entity, or a tween outside 0 to 1. `shows` is false for a hidden camera or
/// one whose viewport has no size, which draws nothing.
bool drawn_view(std::uint64_t camera, float target_width, float target_height, float tween,
                std::uint32_t& slot, scene::view_t& view, bool& shows);

} // namespace fbe::render
