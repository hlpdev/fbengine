// Geometry: surfaces of vertices and triangles, built in code, transformed, merged, copied,
// flipped, and measured; and the meshes that draw it.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbe::scene {

namespace {

vec3_t vec(const fbe_vec3_t& v) {
    return { v.x, v.y, v.z };
}

fbe_vec3_t c_vec(vec3_t v) {
    return { .x = v.x, .y = v.y, .z = v.z };
}

mat4_t matrix(const fbe_mat4_t& m) {
    mat4_t found;
    std::ranges::copy(m.m, found.m.begin());
    return found;
}

/// The surface at `index` of a live geometry, or null, with fbe_error saying why.
surface_t* surface_of(std::uint64_t geometry, std::int32_t index) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return nullptr;
    }
    std::vector<surface_t>& surfaces = scene().geometries[slot].surfaces;
    if (index < 0 || static_cast<std::size_t>(index) >= surfaces.size()) {
        fail(std::format("the geometry's surfaces are 0 to {}, not {}",
                         static_cast<std::int64_t>(surfaces.size()) - 1, index));
        return nullptr;
    }
    return &surfaces[static_cast<std::size_t>(index)];
}

/// What a read or write of `count` of a surface's `what` from `first` says when they aren't all
/// there: the one it asked for, or the run.
std::string out_of_range(const char* what, std::size_t size, std::int32_t first,
                         std::int32_t count) {
    const auto last = static_cast<std::int64_t>(size) - 1;
    if (count == 1) {
        return std::format("the surface's {} are 0 to {}, not {}", what, last, first);
    }
    return std::format("the surface's {} are 0 to {}, not {} to {}", what, last, first,
                       static_cast<std::int64_t>(first) + count - 1);
}

/// Transforms a surface's vertices and normals, turning its triangles when the matrix flips.
void transform_surface(surface_t& surface, const mat4_t& m) {
    // A target's position moves as a direction does, and its normal so that, at any weight, the
    // vertex's is what the transform makes of it; both while the vertices are as they were.
    for (morph_target_t& target : surface.targets) {
        for (vec3_t& moved : target.positions) {
            moved = transform_vector(m, moved);
        }
        fit_target_box(target);
        for (std::size_t i = 0; i < target.normals.size() && i < surface.vertices.size(); ++i) {
            target.normals[i] =
                transform_normal_move(m, vec(surface.vertices[i].normal), target.normals[i]);
        }
    }
    for (fbe_vertex_t& vertex : surface.vertices) {
        vertex.position = c_vec(transform_point(m, vec(vertex.position)));
        vertex.normal = c_vec(transform_normal(m, vec(vertex.normal)));
    }
    if (dot(cross(m.axis(0), m.axis(1)), m.axis(2)) < 0.0F) {
        for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
            std::swap(surface.indices[i + 1], surface.indices[i + 2]);
        }
    }
}

/// A position's bits, so vertices at exactly one place smooth together.
struct place_t {
    std::uint32_t x;
    std::uint32_t y;
    std::uint32_t z;

    bool operator==(const place_t&) const = default;
};

struct place_hash_t {
    std::size_t operator()(const place_t& place) const {
        return (static_cast<std::size_t>(place.x) * 73856093U) ^
               (static_cast<std::size_t>(place.y) * 19349663U) ^
               (static_cast<std::size_t>(place.z) * 83492791U);
    }
};

/// The bits of a coordinate, with -0 as 0.
std::uint32_t bits(float value) {
    return std::bit_cast<std::uint32_t>(value + 0.0F);
}

/// A position's place, bit for bit, with -0 as 0.
place_t place_of_point(const fbe_vec3_t& p) {
    return { .x = bits(p.x), .y = bits(p.y), .z = bits(p.z) };
}

/// A triangle's unit normal, or zero for one too thin to have a direction its rounding doesn't
/// make up.
vec3_t face_normal(vec3_t a, vec3_t b, vec3_t c) {
    const vec3_t first = b - a;
    const vec3_t second = c - a;
    const vec3_t across = cross(first, second);
    const float size = length(across);
    if (size <= 1e-6F * length(first) * length(second)) {
        return {};
    }
    return across * (1.0F / size);
}

void update_normals(surface_t& surface) {
    std::unordered_map<place_t, std::size_t, place_hash_t> places;
    std::vector<std::size_t> place_of(surface.vertices.size());
    // Each place's normals weighted by angle, and how much angle that is, then each vertex's own.
    std::vector<vec3_t> sums;
    std::vector<float> weights;
    std::vector<vec3_t> own(surface.vertices.size());
    std::vector<float> own_weights(surface.vertices.size(), 0.0F);
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        const auto [at, added] =
            places.try_emplace(place_of_point(surface.vertices[i].position), sums.size());
        if (added) {
            sums.emplace_back();
            weights.push_back(0.0F);
        }
        place_of[i] = at->second;
    }
    for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
        const std::array<std::uint32_t, 3> corners{
            surface.indices[i],
            surface.indices[i + 1],
            surface.indices[i + 2],
        };
        const std::array<vec3_t, 3> at{
            vec(surface.vertices[corners[0]].position),
            vec(surface.vertices[corners[1]].position),
            vec(surface.vertices[corners[2]].position),
        };
        const vec3_t face = face_normal(at[0], at[1], at[2]);
        if (dot(face, face) == 0.0F) {
            continue;
        }
        // Each corner takes the face's normal by its angle there, so how a face is cut into
        // triangles doesn't change its weight.
        for (std::size_t k = 0; k < 3; ++k) {
            const vec3_t to_next = normalize(at[(k + 1) % 3] - at[k]);
            const vec3_t to_last = normalize(at[(k + 2) % 3] - at[k]);
            const float angle = std::acos(std::fmax(-1.0F, std::fmin(1.0F, dot(to_next, to_last))));
            const std::size_t place = place_of[corners[k]];
            sums[place] = sums[place] + face * angle;
            weights[place] += angle;
            own[corners[k]] = own[corners[k]] + face * angle;
            own_weights[corners[k]] += angle;
        }
    }
    // Where a place's faces cancel, as a sheet's two sides do, its vertex takes its own triangles'
    // normal, and one in no triangle keeps the normal it had.
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        const std::size_t place = place_of[i];
        if (length(sums[place]) > 1e-4F * weights[place]) {
            surface.vertices[i].normal = c_vec(normalize(sums[place]));
        } else if (length(own[i]) > 1e-4F * own_weights[i]) {
            surface.vertices[i].normal = c_vec(normalize(own[i]));
        }
    }
}

/// A geometry's positions welded, each place once in the order it's first used, surface after
/// surface, and its triangles' indices into them, in their order.
void weld(const geometry_t& geometry, std::vector<fbe_vec3_t>& positions,
          std::vector<std::uint32_t>& indices) {
    std::unordered_map<place_t, std::uint32_t, place_hash_t> places;
    for (const surface_t& surface : geometry.surfaces) {
        std::vector<std::uint32_t> welded(surface.vertices.size());
        for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
            const fbe_vec3_t& p = surface.vertices[i].position;
            const auto [at, added] =
                places.try_emplace(place_of_point(p), static_cast<std::uint32_t>(positions.size()));
            if (added) {
                positions.push_back({ .x = p.x + 0.0F, .y = p.y + 0.0F, .z = p.z + 0.0F });
            }
            welded[i] = at->second;
        }
        for (const std::uint32_t index : surface.indices) {
            indices.push_back(welded[index]);
        }
    }
}

/// The scale and offset that take `from` into `to` on one axis; a flat `from` is moved, not
/// scaled.
float axis_scale(float from_min, float from_max, float to_min, float to_max) {
    const float size = from_max - from_min;
    return size > 0.0F ? (to_max - to_min) / size : 1.0F;
}

} // namespace

std::uint32_t make_geometry() {
    scene_t& s = scene();
    const std::uint32_t slot = s.geometry_slots.make();
    if (slot == s.geometries.size()) {
        s.geometries.emplace_back();
    } else {
        s.geometries[slot] = {};
    }
    s.geometries[slot].references = 1;
    s.geometries[slot].version = ++s.geometry_versions;
    return slot;
}

std::uint32_t geometry_slot(std::uint64_t handle) {
    return live_slot(scene().geometry_slots, handle, "geometry");
}

void retain_geometry(std::uint32_t geometry) {
    ++scene().geometries[geometry].references;
}

void release_geometry(std::uint32_t geometry) {
    geometry_t& found = scene().geometries[geometry];
    if (--found.references == 0) {
        release_materials(found.surfaces);
        found = {};
        scene().geometry_slots.free(geometry);
    }
}

void touch_geometry(std::uint32_t geometry, bool reshaped) {
    geometry_t& found = scene().geometries[geometry];
    found.version = ++scene().geometry_versions;
    if (reshaped) {
        ++found.shape_version;
    }
    found.bounds_stale = true;
}

void geometry_bounds(std::uint32_t geometry, vec3_t& min, vec3_t& max) {
    geometry_t& found = scene().geometries[geometry];
    if (found.bounds_stale) {
        bool any = false;
        vec3_t low;
        vec3_t high;
        for (const surface_t& surface : found.surfaces) {
            for (const fbe_vertex_t& vertex : surface.vertices) {
                const vec3_t p = vec(vertex.position);
                if (!any) {
                    low = p;
                    high = p;
                    any = true;
                }
                low = { std::fmin(low.x, p.x), std::fmin(low.y, p.y), std::fmin(low.z, p.z) };
                high = { std::fmax(high.x, p.x), std::fmax(high.y, p.y), std::fmax(high.z, p.z) };
            }
        }
        found.bounds_min = low;
        found.bounds_max = high;
        found.bounds_stale = false;
    }
    min = found.bounds_min;
    max = found.bounds_max;
}

void fit_target_box(morph_target_t& target) {
    target.low = {};
    target.high = {};
    for (const vec3_t& moved : target.positions) {
        target.low = { std::fmin(target.low.x, moved.x), std::fmin(target.low.y, moved.y),
                       std::fmin(target.low.z, moved.z) };
        target.high = { std::fmax(target.high.x, moved.x), std::fmax(target.high.y, moved.y),
                        std::fmax(target.high.z, moved.z) };
    }
}

const bvh_t& geometry_tree(std::uint32_t geometry) {
    geometry_t& found = scene().geometries[geometry];
    if (found.tree_version != found.version) {
        // Only moved: the same triangles in the same leaves, their boxes fitted again.
        if (found.tree_shape_version == found.shape_version) {
            found.tree.refit(found.surfaces);
        } else {
            found.tree.build(found.surfaces);
        }
        found.tree_version = found.version;
        found.tree_shape_version = found.shape_version;
    }
    return found.tree;
}

void add_mesh(std::uint32_t entity) {
    scene_t& s = scene();
    s.entities.component[entity] = static_cast<std::uint32_t>(s.meshes.size());
    s.meshes.push_back({ .entity = entity, .geometry = none });
}

void remove_mesh(std::uint32_t entity) {
    scene_t& s = scene();
    const std::uint32_t index = s.entities.component[entity];
    if (s.meshes[index].geometry != none) {
        release_geometry(s.meshes[index].geometry);
    }
    release_material(s.meshes[index].material);
    if (index != s.meshes.size() - 1) {
        s.meshes[index] = std::move(s.meshes.back());
        s.entities.component[s.meshes[index].entity] = index;
    }
    s.meshes.pop_back();
}

mesh_t* mesh_of(std::uint64_t handle) {
    const std::uint32_t entity = entity_slot(handle);
    if (entity == none) {
        return nullptr;
    }
    if (scene().entities.kind[entity] != FBE_ENTITY_MESH) {
        fail("the entity isn't a mesh");
        return nullptr;
    }
    return &scene().meshes[scene().entities.component[entity]];
}

} // namespace fbe::scene

using fbe::scene::geometry_slot;
using fbe::scene::none;
using fbe::scene::scene;
using fbe::scene::surface_t;

extern "C" {

FBE_API uint64_t fbe_geometry_create(void) {
    return scene().geometry_slots.handle(fbe::scene::make_geometry());
}

FBE_API bool fbe_geometry_retain(uint64_t geometry) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return false;
    }
    fbe::scene::retain_geometry(slot);
    return true;
}

FBE_API bool fbe_geometry_release(uint64_t geometry) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return false;
    }
    fbe::scene::release_geometry(slot);
    return true;
}

FBE_API bool fbe_geometry_alive(uint64_t geometry) {
    return scene().geometry_slots.find(geometry).has_value();
}

FBE_API uint64_t fbe_geometry_clone(uint64_t geometry) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return 0;
    }
    std::vector<surface_t> surfaces = scene().geometries[slot].surfaces;
    // Its cull box too, read before making the copy moves the geometries.
    const bool has_cull_box = scene().geometries[slot].has_cull_box;
    const fbe::scene::vec3_t cull_min = scene().geometries[slot].cull_min;
    const fbe::scene::vec3_t cull_max = scene().geometries[slot].cull_max;
    fbe::scene::retain_materials(surfaces);
    const std::uint32_t copy = fbe::scene::make_geometry();
    fbe::scene::geometry_t& made = scene().geometries[copy];
    made.surfaces = std::move(surfaces);
    made.has_cull_box = has_cull_box;
    made.cull_min = cull_min;
    made.cull_max = cull_max;
    return scene().geometry_slots.handle(copy);
}

FBE_API uint64_t fbe_geometry_version(uint64_t geometry) {
    const std::uint32_t slot = geometry_slot(geometry);
    return slot != none ? scene().geometries[slot].version : 0;
}

FBE_API int32_t fbe_geometry_surface_count(uint64_t geometry) {
    const std::uint32_t slot = geometry_slot(geometry);
    return slot != none ? static_cast<std::int32_t>(scene().geometries[slot].surfaces.size()) : -1;
}

FBE_API int32_t fbe_geometry_add_surface(uint64_t geometry) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return -1;
    }
    std::vector<surface_t>& surfaces = scene().geometries[slot].surfaces;
    surfaces.emplace_back();
    fbe::scene::touch_geometry(slot, true);
    return static_cast<std::int32_t>(surfaces.size() - 1);
}

FBE_API bool fbe_geometry_add(uint64_t geometry, uint64_t other, const fbe_mat4_t* transform) {
    const std::uint32_t slot = geometry_slot(geometry);
    const std::uint32_t from = slot != none ? geometry_slot(other) : none;
    if (from == none) {
        return false;
    }
    // A copy first, so a geometry added to itself adds what it had.
    std::vector<surface_t> added = scene().geometries[from].surfaces;
    fbe::scene::retain_materials(added);
    const fbe::scene::mat4_t m = fbe::scene::matrix(*transform);
    for (surface_t& surface : added) {
        fbe::scene::transform_surface(surface, m);
    }
    std::vector<surface_t>& surfaces = scene().geometries[slot].surfaces;
    surfaces.insert(surfaces.end(), std::make_move_iterator(added.begin()),
                    std::make_move_iterator(added.end()));
    fbe::scene::touch_geometry(slot, true);
    return true;
}

FBE_API bool fbe_geometry_transform(uint64_t geometry, const fbe_mat4_t* transform) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return false;
    }
    const fbe::scene::mat4_t m = fbe::scene::matrix(*transform);
    for (surface_t& surface : scene().geometries[slot].surfaces) {
        fbe::scene::transform_surface(surface, m);
    }
    fbe::scene::touch_geometry(slot, false);
    return true;
}

FBE_API bool fbe_geometry_flip(uint64_t geometry) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return false;
    }
    for (surface_t& surface : scene().geometries[slot].surfaces) {
        for (fbe_vertex_t& vertex : surface.vertices) {
            vertex.normal = { .x = -vertex.normal.x, .y = -vertex.normal.y, .z = -vertex.normal.z };
        }
        for (fbe::scene::morph_target_t& target : surface.targets) {
            for (fbe::scene::vec3_t& normal : target.normals) {
                normal = -normal;
            }
        }
        for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
            std::swap(surface.indices[i + 1], surface.indices[i + 2]);
        }
    }
    fbe::scene::touch_geometry(slot, false);
    return true;
}

FBE_API bool fbe_geometry_update_normals(uint64_t geometry) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return false;
    }
    for (surface_t& surface : scene().geometries[slot].surfaces) {
        fbe::scene::update_normals(surface);
    }
    fbe::scene::touch_geometry(slot, false);
    return true;
}

FBE_API bool fbe_geometry_bounds(uint64_t geometry, fbe_aabb_t* bounds) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return false;
    }
    fbe::scene::vec3_t min;
    fbe::scene::vec3_t max;
    fbe::scene::geometry_bounds(slot, min, max);
    *bounds = { .min = fbe::scene::c_vec(min), .max = fbe::scene::c_vec(max) };
    return true;
}

FBE_API bool fbe_geometry_fit(uint64_t geometry, const fbe_aabb_t* box, bool uniform) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return false;
    }
    const bool ordered =
        box->min.x <= box->max.x && box->min.y <= box->max.y && box->min.z <= box->max.z;
    if (!ordered) {
        return fbe::fail(std::format("a box to fit into has its least corner at or below its "
                                     "greatest, not ({}, {}, {}) to ({}, {}, {})",
                                     box->min.x, box->min.y, box->min.z, box->max.x, box->max.y,
                                     box->max.z));
    }
    fbe::scene::vec3_t min;
    fbe::scene::vec3_t max;
    fbe::scene::geometry_bounds(slot, min, max);
    fbe::scene::vec3_t scale(fbe::scene::axis_scale(min.x, max.x, box->min.x, box->max.x),
                             fbe::scene::axis_scale(min.y, max.y, box->min.y, box->max.y),
                             fbe::scene::axis_scale(min.z, max.z, box->min.z, box->max.z));
    if (uniform) {
        // The smallest scale of the axes that have a size keeps the shape and fits.
        float least = 0.0F;
        bool any = false;
        const std::array<std::pair<float, float>, 3> axes{
            std::pair{ max.x - min.x, scale.x },
            std::pair{ max.y - min.y, scale.y },
            std::pair{ max.z - min.z, scale.z },
        };
        for (const auto& [size, axis] : axes) {
            if (size > 0.0F && (!any || axis < least)) {
                least = axis;
                any = true;
            }
        }
        scale =
            any ? fbe::scene::vec3_t(least, least, least) : fbe::scene::vec3_t(1.0F, 1.0F, 1.0F);
    }
    const fbe::scene::vec3_t from_sum = min + max;
    const fbe::scene::vec3_t to_sum = fbe::scene::vec(box->min) + fbe::scene::vec(box->max);
    const fbe::scene::vec3_t from_center = from_sum * 0.5F;
    const fbe::scene::vec3_t to_center = to_sum * 0.5F;
    const fbe::scene::mat4_t m(
        fbe::scene::vec3_t(scale.x, 0.0F, 0.0F), fbe::scene::vec3_t(0.0F, scale.y, 0.0F),
        fbe::scene::vec3_t(0.0F, 0.0F, scale.z), to_center - from_center * scale);
    for (surface_t& surface : scene().geometries[slot].surfaces) {
        fbe::scene::transform_surface(surface, m);
    }
    fbe::scene::touch_geometry(slot, false);
    return true;
}

FBE_API int32_t fbe_geometry_positions(uint64_t geometry, fbe_vec3_t* positions, int32_t capacity) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return -1;
    }
    std::vector<fbe_vec3_t> welded;
    std::vector<std::uint32_t> indices;
    fbe::scene::weld(scene().geometries[slot], welded, indices);
    const std::size_t count =
        std::min(welded.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    std::copy_n(welded.begin(), count, positions);
    return static_cast<std::int32_t>(welded.size());
}

FBE_API int32_t fbe_geometry_indices(uint64_t geometry, uint32_t* indices, int32_t capacity) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return -1;
    }
    std::vector<fbe_vec3_t> positions;
    std::vector<std::uint32_t> welded;
    fbe::scene::weld(scene().geometries[slot], positions, welded);
    const std::size_t count =
        std::min(welded.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    std::copy_n(welded.begin(), count, indices);
    return static_cast<std::int32_t>(welded.size());
}

FBE_API int32_t fbe_surface_add_vertices(uint64_t geometry, int32_t surface,
                                         const fbe_vertex_t* vertices, int32_t count) {
    surface_t* found = fbe::scene::surface_of(geometry, surface);
    if (found == nullptr) {
        return -1;
    }
    if (count < 0) {
        fbe::fail(std::format("{} vertices can't be added", count));
        return -1;
    }
    const auto first = static_cast<std::int32_t>(found->vertices.size());
    found->vertices.insert(found->vertices.end(), vertices, vertices + count);
    found->joints.clear();
    fbe::scene::touch_geometry(geometry_slot(geometry), false);
    return first;
}

FBE_API int32_t fbe_surface_add_triangles(uint64_t geometry, int32_t surface,
                                          const uint32_t* indices, int32_t count) {
    surface_t* found = fbe::scene::surface_of(geometry, surface);
    if (found == nullptr) {
        return -1;
    }
    if (count < 0 || count % 3 != 0) {
        fbe::fail(std::format("triangles take indices three at a time, not {}", count));
        return -1;
    }
    const auto vertex_count = static_cast<std::uint32_t>(found->vertices.size());
    for (std::int32_t i = 0; i < count; ++i) {
        if (indices[i] >= vertex_count) {
            fbe::fail(std::format("the surface's vertices are 0 to {}, not {}",
                                  static_cast<std::int64_t>(vertex_count) - 1, indices[i]));
            return -1;
        }
    }
    const auto first = static_cast<std::int32_t>(found->indices.size() / 3);
    found->indices.insert(found->indices.end(), indices, indices + count);
    fbe::scene::touch_geometry(geometry_slot(geometry), true);
    return first;
}

FBE_API int32_t fbe_surface_vertex_count(uint64_t geometry, int32_t surface) {
    const surface_t* found = fbe::scene::surface_of(geometry, surface);
    return found != nullptr ? static_cast<std::int32_t>(found->vertices.size()) : -1;
}

FBE_API int32_t fbe_surface_triangle_count(uint64_t geometry, int32_t surface) {
    const surface_t* found = fbe::scene::surface_of(geometry, surface);
    return found != nullptr ? static_cast<std::int32_t>(found->indices.size() / 3) : -1;
}

FBE_API bool fbe_surface_vertices(uint64_t geometry, int32_t surface, int32_t first,
                                  fbe_vertex_t* vertices, int32_t count) {
    const surface_t* found = fbe::scene::surface_of(geometry, surface);
    if (found == nullptr) {
        return false;
    }
    if (first < 0 || count < 0 ||
        static_cast<std::size_t>(first) + static_cast<std::size_t>(count) >
            found->vertices.size()) {
        return fbe::fail(
            fbe::scene::out_of_range("vertices", found->vertices.size(), first, count));
    }
    std::copy_n(found->vertices.begin() + first, count, vertices);
    return true;
}

FBE_API bool fbe_surface_set_joints(uint64_t geometry, int32_t surface,
                                    const fbe_vertex_joints_t* joints, int32_t count) {
    surface_t* found = fbe::scene::surface_of(geometry, surface);
    if (found == nullptr) {
        return false;
    }
    if (count != 0 && std::cmp_not_equal(count, found->vertices.size())) {
        return fbe::fail(std::format("a surface of {} vertices has as many joints, or none, not {}",
                                     found->vertices.size(), count));
    }
    found->joints.assign(joints, joints + count);
    // One with no weight at all would take its triangles with it: it's on no bone.
    for (fbe_vertex_joints_t& vertex : found->joints) {
        if (vertex.weights[0] == 0.0F && vertex.weights[1] == 0.0F && vertex.weights[2] == 0.0F &&
            vertex.weights[3] == 0.0F) {
            vertex.joints[0] = FBE_NO_JOINT;
            vertex.weights[0] = 1.0F;
        }
    }
    fbe::scene::touch_geometry(geometry_slot(geometry), false);
    return true;
}

FBE_API int32_t fbe_surface_joints(uint64_t geometry, int32_t surface, fbe_vertex_joints_t* joints,
                                   int32_t capacity) {
    const surface_t* found = fbe::scene::surface_of(geometry, surface);
    if (found == nullptr) {
        return -1;
    }
    const auto count =
        std::min(found->joints.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    std::copy_n(found->joints.begin(), count, joints);
    return static_cast<std::int32_t>(found->joints.size());
}

FBE_API bool fbe_surface_set_vertices(uint64_t geometry, int32_t surface, int32_t first,
                                      const fbe_vertex_t* vertices, int32_t count) {
    surface_t* found = fbe::scene::surface_of(geometry, surface);
    if (found == nullptr) {
        return false;
    }
    if (first < 0 || count < 0 ||
        static_cast<std::size_t>(first) + static_cast<std::size_t>(count) >
            found->vertices.size()) {
        return fbe::fail(
            fbe::scene::out_of_range("vertices", found->vertices.size(), first, count));
    }
    std::copy_n(vertices, count, found->vertices.begin() + first);
    fbe::scene::touch_geometry(geometry_slot(geometry), false);
    return true;
}

FBE_API bool fbe_surface_triangles(uint64_t geometry, int32_t surface, int32_t first,
                                   uint32_t* indices, int32_t count) {
    const surface_t* found = fbe::scene::surface_of(geometry, surface);
    if (found == nullptr) {
        return false;
    }
    const std::size_t triangles = found->indices.size() / 3;
    if (first < 0 || count < 0 ||
        static_cast<std::size_t>(first) + static_cast<std::size_t>(count) > triangles) {
        return fbe::fail(fbe::scene::out_of_range("triangles", triangles, first, count));
    }
    std::copy_n(found->indices.begin() + (static_cast<std::ptrdiff_t>(first) * 3), count * 3,
                indices);
    return true;
}

FBE_API bool fbe_surface_clear(uint64_t geometry, int32_t surface, bool vertices) {
    surface_t* found = fbe::scene::surface_of(geometry, surface);
    if (found == nullptr) {
        return false;
    }
    found->indices.clear();
    if (vertices) {
        found->vertices.clear();
        found->targets.clear();
        found->joints.clear();
    }
    fbe::scene::touch_geometry(geometry_slot(geometry), true);
    return true;
}

FBE_API bool fbe_mesh_set_geometry(uint64_t mesh, uint64_t geometry) {
    fbe::scene::mesh_t* found = fbe::scene::mesh_of(mesh);
    if (found == nullptr) {
        return false;
    }
    std::uint32_t slot = none;
    if (geometry != 0) {
        slot = geometry_slot(geometry);
        if (slot == none) {
            return false;
        }
        fbe::scene::retain_geometry(slot);
    }
    if (found->geometry != none) {
        fbe::scene::release_geometry(found->geometry);
    }
    found->geometry = slot;
    fbe::scene::pickable_moved(found->entity);
    return true;
}

FBE_API uint64_t fbe_mesh_geometry(uint64_t mesh) {
    const fbe::scene::mesh_t* found = fbe::scene::mesh_of(mesh);
    if (found == nullptr || found->geometry == none) {
        return 0;
    }
    return scene().geometry_slots.handle(found->geometry);
}

FBE_API bool fbe_geometry_set_cull_box(uint64_t geometry, const fbe_aabb_t* box) {
    const std::uint32_t slot = fbe::scene::geometry_slot(geometry);
    if (slot == none) {
        return false;
    }
    if (std::isnan(box->min.x) || std::isnan(box->min.y) || std::isnan(box->min.z) ||
        std::isnan(box->max.x) || std::isnan(box->max.y) || std::isnan(box->max.z) ||
        box->min.x > box->max.x || box->min.y > box->max.y || box->min.z > box->max.z) {
        return fbe::fail(std::format("a cull box has its least corner at or below its greatest, "
                                     "not ({}, {}, {}) to ({}, {}, {})",
                                     box->min.x, box->min.y, box->min.z, box->max.x, box->max.y,
                                     box->max.z));
    }
    fbe::scene::geometry_t& found = scene().geometries[slot];
    found.has_cull_box = true;
    found.cull_min = { box->min.x, box->min.y, box->min.z };
    found.cull_max = { box->max.x, box->max.y, box->max.z };
    return true;
}

FBE_API bool fbe_geometry_clear_cull_box(uint64_t geometry) {
    const std::uint32_t slot = fbe::scene::geometry_slot(geometry);
    if (slot == none) {
        return false;
    }
    scene().geometries[slot].has_cull_box = false;
    return true;
}

FBE_API bool fbe_geometry_cull_box(uint64_t geometry, fbe_aabb_t* box) {
    const std::uint32_t slot = fbe::scene::geometry_slot(geometry);
    if (slot == none) {
        return false;
    }
    const fbe::scene::geometry_t& found = scene().geometries[slot];
    if (!found.has_cull_box) {
        return false;
    }
    *box = {
        .min = { .x = found.cull_min.x, .y = found.cull_min.y, .z = found.cull_min.z },
        .max = { .x = found.cull_max.x, .y = found.cull_max.y, .z = found.cull_max.z },
    };
    return true;
}

} // extern "C"
