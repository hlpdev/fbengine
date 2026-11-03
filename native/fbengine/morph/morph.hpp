#pragma once

// What the morph area gives drawing: a mesh's weights as drawn, the targets a surface draws, and
// how far they can take it from its bounds.

#include "scene/math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fbe::anim {

/// How many targets a surface is drawn with at once, those weighted most.
constexpr std::size_t drawn_targets = 8;

/// A target a surface is drawn with, as the scene shader reads it, and how much of it.
struct drawn_target_t {
    std::uint32_t target = 0;
    float weight = 0.0F;
};

/// The weights the mesh in slot `mesh` is drawn with, `tween` of the way from its World's last
/// capture to now, one a target from the first: what's past them is 0. Valid until the next call.
std::span<const float> morph_weights(std::uint32_t mesh, float tween);

/// Of `weights`, for a surface of `targets` targets, the ones it's drawn with, into `drawn`: those
/// furthest from 0, the earlier of two as far, and none at 0. Gives how many.
std::size_t strongest_targets(std::span<const float> weights, std::size_t targets,
                              std::array<drawn_target_t, drawn_targets>& drawn);

/// Grows a geometry's box by how far `weights` can move its vertices.
void grow_by_targets(std::uint32_t geometry, std::span<const float> weights, scene::vec3_t& min,
                     scene::vec3_t& max);

} // namespace fbe::anim
