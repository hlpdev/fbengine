// Morph targets' C API: what a surface's vertices move by, each mesh's weights, and the vertices
// they make, kept on the scene's surfaces and meshes so copies and clones have them as they should.

#include "morph/morph.hpp"

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace fbe::anim {

namespace {

bool all_numbers(std::span<const float> values) {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}

/// `count` vectors of three floats each.
std::vector<scene::vec3_t> vectors_of(const float* floats, std::size_t count) {
    const std::span<const float> given(floats, count * 3);
    std::vector<scene::vec3_t> vectors;
    vectors.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        vectors.emplace_back(given[i * 3], given[(i * 3) + 1], given[(i * 3) + 2]);
    }
    return vectors;
}

/// The live mesh a handle is for, with its entity's slot, or null, with fbe_error saying so.
scene::mesh_t* live_mesh(std::uint64_t handle, std::uint32_t& slot) {
    slot = scene::entity_slot(handle);
    if (slot == scene::none) {
        return nullptr;
    }
    scene::scene_t& all = scene::scene();
    if (all.entities.kind[slot] != FBE_ENTITY_MESH) {
        fail("the entity isn't a mesh");
        return nullptr;
    }
    return &all.meshes[all.entities.component[slot]];
}

/// A geometry's surface, or null, with fbe_error saying so.
scene::surface_t* live_surface(std::uint32_t geometry, std::int32_t surface) {
    std::vector<scene::surface_t>& surfaces = scene::scene().geometries[geometry].surfaces;
    if (surface < 0 || std::cmp_greater_equal(surface, surfaces.size())) {
        fail(std::format("the geometry has {} surfaces; there's no surface {}", surfaces.size(),
                         surface));
        return nullptr;
    }
    return &surfaces[static_cast<std::size_t>(surface)];
}

} // namespace

std::span<const float> morph_weights(std::uint32_t mesh, float tween) {
    const scene::scene_t& all = scene::scene();
    const scene::mesh_t& found = all.meshes[all.entities.component[mesh]];
    if (tween >= 1.0F || !all.entities.captured[mesh]) {
        return found.weights;
    }
    thread_local std::vector<float> between;
    between.assign(std::max(found.weights.size(), found.captured_weights.size()), 0.0F);
    for (std::size_t i = 0; i < between.size(); ++i) {
        const float was = i < found.captured_weights.size() ? found.captured_weights[i] : 0.0F;
        const float now = i < found.weights.size() ? found.weights[i] : 0.0F;
        between[i] = scene::mix(was, now, tween);
    }
    return between;
}

std::size_t strongest_targets(std::span<const float> weights, std::size_t targets,
                              std::array<drawn_target_t, drawn_targets>& drawn) {
    std::size_t count = 0;
    const std::size_t last = std::min(weights.size(), targets);
    for (std::size_t t = 0; t < last; ++t) {
        const float strength = std::abs(weights[t]);
        if (strength <= 0.0F) {
            continue;
        }
        // Kept strongest first, each after any as strong, so the earlier stays.
        std::size_t at = count;
        while (at > 0 && std::abs(drawn[at - 1].weight) < strength) {
            --at;
        }
        if (at == drawn.size()) {
            continue;
        }
        count = std::min(count + 1, drawn.size());
        for (std::size_t i = count - 1; i > at; --i) {
            drawn[i] = drawn[i - 1];
        }
        drawn[at] = { .target = static_cast<std::uint32_t>(t), .weight = weights[t] };
    }
    return count;
}

void grow_by_targets(std::uint32_t geometry, std::span<const float> weights, scene::vec3_t& min,
                     scene::vec3_t& max) {
    const std::vector<scene::surface_t>& surfaces = scene::scene().geometries[geometry].surfaces;
    for (std::size_t t = 0; t < weights.size(); ++t) {
        scene::vec3_t low;
        scene::vec3_t high;
        // What it moves a vertex of any surface by, the other way for a weight below 0.
        for (const scene::surface_t& surface : surfaces) {
            if (t < surface.targets.size()) {
                const scene::morph_target_t& target = surface.targets[t];
                const float weight = std::abs(weights[t]);
                const scene::vec3_t from = target.low * weight;
                const scene::vec3_t to = target.high * weight;
                const bool forward = weights[t] >= 0.0F;
                low = { std::fmin(low.x, forward ? from.x : -to.x),
                        std::fmin(low.y, forward ? from.y : -to.y),
                        std::fmin(low.z, forward ? from.z : -to.z) };
                high = { std::fmax(high.x, forward ? to.x : -from.x),
                         std::fmax(high.y, forward ? to.y : -from.y),
                         std::fmax(high.z, forward ? to.z : -from.z) };
            }
        }
        min = min + low;
        max = max + high;
    }
}

} // namespace fbe::anim

using fbe::fail;
using fbe::scene::none;
using fbe::scene::vec3_t;

/// How short a morphed normal is before it's taken to be no normal at all.
constexpr float smallest_normal = 1e-6F;

extern "C" {

FBE_API int32_t fbe_morph_add_target(uint64_t geometry, int32_t surface, const float* positions,
                                     const float* normals, int32_t vertices) {
    const std::uint32_t slot = fbe::scene::geometry_slot(geometry);
    if (slot == none) {
        return -1;
    }
    fbe::scene::surface_t* found = fbe::anim::live_surface(slot, surface);
    if (found == nullptr) {
        return -1;
    }
    if (std::cmp_not_equal(vertices, found->vertices.size())) {
        fail(std::format("a target moves each of the surface's {} vertices, not {}",
                         found->vertices.size(), vertices));
        return -1;
    }
    const auto count = static_cast<std::size_t>(vertices);
    if ((positions != nullptr && !fbe::anim::all_numbers({ positions, count * 3 })) ||
        (normals != nullptr && !fbe::anim::all_numbers({ normals, count * 3 }))) {
        fail("a target's vectors are numbers");
        return -1;
    }
    try {
        fbe::scene::morph_target_t made;
        if (positions != nullptr && count > 0) {
            made.positions = fbe::anim::vectors_of(positions, count);
        }
        if (normals != nullptr && count > 0) {
            made.normals = fbe::anim::vectors_of(normals, count);
        }
        fbe::scene::fit_target_box(made);
        found->targets.push_back(std::move(made));
        fbe::scene::touch_geometry(slot, false);
        return static_cast<int32_t>(found->targets.size()) - 1;
    } catch (const std::bad_alloc&) {
        fail("out of memory");
        return -1;
    }
}

FBE_API int32_t fbe_morph_target_count(uint64_t geometry, int32_t surface) {
    const std::uint32_t slot = fbe::scene::geometry_slot(geometry);
    if (slot == none) {
        return -1;
    }
    const fbe::scene::surface_t* found = fbe::anim::live_surface(slot, surface);
    return found != nullptr ? static_cast<int32_t>(found->targets.size()) : -1;
}

FBE_API bool fbe_morph_set_weights(uint64_t mesh, const float* weights, int32_t count) {
    std::uint32_t slot = none;
    fbe::scene::mesh_t* found = fbe::anim::live_mesh(mesh, slot);
    if (found == nullptr) {
        return false;
    }
    const std::span<const float> given(
        weights, weights != nullptr && count > 0 ? static_cast<std::size_t>(count) : 0);
    if (!fbe::anim::all_numbers(given)) {
        return fail("a mesh's weights are numbers");
    }
    try {
        found->weights.assign(given.begin(), given.end());
        return true;
    } catch (const std::bad_alloc&) {
        return fail("out of memory");
    }
}

FBE_API int32_t fbe_morph_weights(uint64_t mesh, float tween, float* weights, int32_t capacity) {
    std::uint32_t slot = none;
    if (fbe::anim::live_mesh(mesh, slot) == nullptr) {
        return -1;
    }
    if (std::isnan(tween) || tween < 0.0F || tween > 1.0F) {
        fail(std::format("a tween is from 0 to 1, not {}", tween));
        return -1;
    }
    try {
        const std::span<const float> found = fbe::anim::morph_weights(slot, tween);
        const std::size_t room =
            weights != nullptr && capacity > 0 ? static_cast<std::size_t>(capacity) : 0;
        std::copy_n(found.begin(), std::min(room, found.size()),
                    std::span<float>(weights, room).begin());
        return static_cast<int32_t>(found.size());
    } catch (const std::bad_alloc&) {
        fail("out of memory");
        return -1;
    }
}

FBE_API int32_t fbe_morph_vertices(uint64_t mesh, int32_t surface, float* positions, float* normals,
                                   int32_t capacity) {
    std::uint32_t slot = none;
    const fbe::scene::mesh_t* found = fbe::anim::live_mesh(mesh, slot);
    if (found == nullptr) {
        return -1;
    }
    if (found->geometry == none) {
        fail("the mesh draws nothing");
        return -1;
    }
    const fbe::scene::surface_t* drawn = fbe::anim::live_surface(found->geometry, surface);
    if (drawn == nullptr) {
        return -1;
    }
    const std::size_t count = drawn->vertices.size();
    if (std::cmp_less(capacity, count)) {
        fail(std::format("the surface has {} vertices", count));
        return -1;
    }
    const std::span<float> out_positions(positions, positions != nullptr ? count * 3 : 0);
    const std::span<float> out_normals(normals, normals != nullptr ? count * 3 : 0);
    const std::size_t targets = std::min(found->weights.size(), drawn->targets.size());
    for (std::size_t i = 0; i < count; ++i) {
        const fbe_vertex_t& vertex = drawn->vertices[i];
        vec3_t position{ vertex.position.x, vertex.position.y, vertex.position.z };
        vec3_t normal{ vertex.normal.x, vertex.normal.y, vertex.normal.z };
        for (std::size_t t = 0; t < targets; ++t) {
            const fbe::scene::morph_target_t& target = drawn->targets[t];
            const float weight = found->weights[t];
            if (i < target.positions.size()) {
                const vec3_t moved = target.positions[i] * weight;
                position = position + moved;
            }
            if (i < target.normals.size()) {
                const vec3_t turned = target.normals[i] * weight;
                normal = normal + turned;
            }
        }
        // A target can cancel a normal, which leaves no way to face but the vertex's own.
        const float size = fbe::scene::length(normal);
        if (size > smallest_normal && std::isfinite(size)) {
            normal = normal * (1.0F / size);
        } else {
            normal = { vertex.normal.x, vertex.normal.y, vertex.normal.z };
        }
        if (!out_positions.empty()) {
            out_positions[i * 3] = position.x;
            out_positions[(i * 3) + 1] = position.y;
            out_positions[(i * 3) + 2] = position.z;
        }
        if (!out_normals.empty()) {
            out_normals[i * 3] = normal.x;
            out_normals[(i * 3) + 1] = normal.y;
            out_normals[(i * 3) + 2] = normal.z;
        }
    }
    return static_cast<int32_t>(count);
}

} // extern "C"
