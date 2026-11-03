#pragma once

// What the morph area gives drawing: a mesh's weights as drawn.

#include <cstdint>
#include <span>

namespace fbe::anim {

/// The weights the mesh in slot `mesh` is drawn with, `tween` of the way from its World's last
/// capture to now, one a target from the first: what's past them is 0. Valid until the next call.
std::span<const float> morph_weights(std::uint32_t mesh, float tween);

} // namespace fbe::anim
