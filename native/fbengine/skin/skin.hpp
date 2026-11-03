#pragma once

// What the skin area gives the scene: a copied tree's meshes skinned as their originals, a freed
// mesh's skin let go of, and the palette a skinned mesh is drawn with.

#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace fbe::anim {

/// Skins each copy of a skinned mesh in a copied tree as its original is, by the same skin, its
/// joints found under its original's root, or that root's copy when the tree copied it too.
/// `originals` is the tree's entities as scene's list_tree gives them, and `copies` each one's
/// copy.
void copy_skins(const std::vector<std::pair<std::uint32_t, std::size_t>>& originals,
                const std::vector<std::uint32_t>& copies);

/// Lets go of the skin of a mesh, by its slot, as it's freed.
void forget_skin(std::uint32_t mesh);

/// The palette the mesh in slot `mesh` is drawn with, `tweened` as drawing's poses are: a matrix a
/// joint, the joint's matrix to the world times its inverse bind, in the mesh's space. Empty for a
/// mesh skinned by nothing; valid until the next call for that mesh, or its skin changes.
std::span<const scene::mat4_t> skin_palette(std::uint32_t mesh, const scene::tweened_t& tweened);

} // namespace fbe::anim
