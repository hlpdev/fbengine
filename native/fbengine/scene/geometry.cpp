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

/// Transforms a surface's vertices and normals, turning its triangles when the matrix flips.
void transform_surface(surface_t& surface, const mat4_t& m) {
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

void update_normals(surface_t& surface) {
    std::unordered_map<place_t, std::size_t, place_hash_t> places;
    std::vector<std::size_t> place_of(surface.vertices.size());
    std::vector<vec3_t> sums;
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        const fbe_vec3_t& p = surface.vertices[i].position;
        const place_t place{ .x = bits(p.x), .y = bits(p.y), .z = bits(p.z) };
        const auto [at, added] = places.try_emplace(place, sums.size());
        if (added) {
            sums.emplace_back();
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
        const vec3_t face = normalize(cross(at[1] - at[0], at[2] - at[0]));
        // Each corner takes the face's normal by its angle there, so how a face is cut into
        // triangles doesn't change its weight.
        for (std::size_t k = 0; k < 3; ++k) {
            const vec3_t to_next = normalize(at[(k + 1) % 3] - at[k]);
            const vec3_t to_last = normalize(at[(k + 2) % 3] - at[k]);
            const float angle = std::acos(std::fmax(-1.0F, std::fmin(1.0F, dot(to_next, to_last))));
            const std::size_t place = place_of[corners[k]];
            sums[place] = sums[place] + face * angle;
        }
    }
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        surface.vertices[i].normal = c_vec(normalize(sums[place_of[i]]));
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
    return slot;
}

std::uint32_t geometry_slot(std::uint64_t handle) {
    const std::optional<std::uint32_t> slot = scene().geometry_slots.find(handle);
    if (!slot) {
        fail(handle == 0 ? "no geometry was given" : "the geometry was freed");
        return none;
    }
    return *slot;
}

void retain_geometry(std::uint32_t geometry) {
    ++scene().geometries[geometry].references;
}

void release_geometry(std::uint32_t geometry) {
    geometry_t& found = scene().geometries[geometry];
    if (--found.references == 0) {
        found.surfaces.clear();
        found.surfaces.shrink_to_fit();
        scene().geometry_slots.free(geometry);
    }
}

void touch_geometry(std::uint32_t geometry) {
    geometry_t& found = scene().geometries[geometry];
    ++found.version;
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
    if (index != s.meshes.size() - 1) {
        s.meshes[index] = s.meshes.back();
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
    const std::uint32_t copy = fbe::scene::make_geometry();
    scene().geometries[copy].surfaces = std::move(surfaces);
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
    fbe::scene::touch_geometry(slot);
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
    const fbe::scene::mat4_t m = fbe::scene::matrix(*transform);
    for (surface_t& surface : added) {
        fbe::scene::transform_surface(surface, m);
    }
    std::vector<surface_t>& surfaces = scene().geometries[slot].surfaces;
    surfaces.insert(surfaces.end(), std::make_move_iterator(added.begin()),
                    std::make_move_iterator(added.end()));
    fbe::scene::touch_geometry(slot);
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
    fbe::scene::touch_geometry(slot);
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
        for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
            std::swap(surface.indices[i + 1], surface.indices[i + 2]);
        }
    }
    fbe::scene::touch_geometry(slot);
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
    fbe::scene::touch_geometry(slot);
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
        return fbe::fail("a box to fit into has its least corner at or below its greatest");
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
    fbe::scene::touch_geometry(slot);
    return true;
}

FBE_API int32_t fbe_geometry_positions(uint64_t geometry, fbe_vec3_t* positions, int32_t capacity) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return -1;
    }
    std::size_t count = 0;
    const auto room = static_cast<std::size_t>(std::max(capacity, 0));
    for (const surface_t& surface : scene().geometries[slot].surfaces) {
        for (const fbe_vertex_t& vertex : surface.vertices) {
            if (count < room) {
                positions[count] = vertex.position;
            }
            ++count;
        }
    }
    return static_cast<std::int32_t>(count);
}

FBE_API int32_t fbe_geometry_indices(uint64_t geometry, uint32_t* indices, int32_t capacity) {
    const std::uint32_t slot = geometry_slot(geometry);
    if (slot == none) {
        return -1;
    }
    std::size_t count = 0;
    std::uint32_t first = 0;
    const auto room = static_cast<std::size_t>(std::max(capacity, 0));
    for (const surface_t& surface : scene().geometries[slot].surfaces) {
        for (const std::uint32_t index : surface.indices) {
            if (count < room) {
                indices[count] = first + index;
            }
            ++count;
        }
        first += static_cast<std::uint32_t>(surface.vertices.size());
    }
    return static_cast<std::int32_t>(count);
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
    fbe::scene::touch_geometry(geometry_slot(geometry));
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
    fbe::scene::touch_geometry(geometry_slot(geometry));
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
        return fbe::fail(std::format("the surface's vertices are 0 to {}, not {} to {}",
                                     static_cast<std::int64_t>(found->vertices.size()) - 1, first,
                                     static_cast<std::int64_t>(first) + count - 1));
    }
    std::copy_n(found->vertices.begin() + first, count, vertices);
    return true;
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
        return fbe::fail(std::format("the surface's vertices are 0 to {}, not {} to {}",
                                     static_cast<std::int64_t>(found->vertices.size()) - 1, first,
                                     static_cast<std::int64_t>(first) + count - 1));
    }
    std::copy_n(vertices, count, found->vertices.begin() + first);
    fbe::scene::touch_geometry(geometry_slot(geometry));
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
        return fbe::fail(std::format("the surface's triangles are 0 to {}, not {} to {}",
                                     static_cast<std::int64_t>(triangles) - 1, first,
                                     static_cast<std::int64_t>(first) + count - 1));
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
    }
    fbe::scene::touch_geometry(geometry_slot(geometry));
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
    return true;
}

FBE_API uint64_t fbe_mesh_geometry(uint64_t mesh) {
    const fbe::scene::mesh_t* found = fbe::scene::mesh_of(mesh);
    if (found == nullptr || found->geometry == none) {
        return 0;
    }
    return scene().geometry_slots.handle(found->geometry);
}

} // extern "C"
