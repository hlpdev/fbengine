// Decals: what's on a box's way through a World's meshes and terrains, clipped to it, as a new
// geometry, which a mesh draws like any other: every shading model has them, nothing needs the
// frame's depth to draw one, and one's only made once, when it's wanted.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/bvh.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

namespace fbe::scene {

namespace {

/// What's being made: into the box, its matrix's inverse, and from it; which way it faces; and what
/// it's making.
struct decal_t {
    mat4_t from_box;
    mat4_t into_box;
    vec3_t facing;
    float lift = 0.0F;
    std::vector<fbe_vertex_t> vertices;
    std::vector<std::uint32_t> indices;
    /// What a polygon's clipped through, kept for the next.
    std::vector<vec3_t> polygon;
    std::vector<vec3_t> clipped;
    std::vector<std::uint32_t> stack;
};

float along(vec3_t p, int axis) {
    if (axis == 0) {
        return p.x;
    }
    return axis == 1 ? p.y : p.z;
}

/// A triangle in the world, if it faces the box's way, clipped by the box's six sides, as a fan.
void add_triangle(decal_t& decal, vec3_t a, vec3_t b, vec3_t c) {
    const vec3_t raw = cross(b - a, c - a);
    const float size = length(raw);
    if (size <= 0.0F) {
        return;
    }
    const vec3_t normal = raw * (1.0F / size);
    if (dot(normal, decal.facing) <= 0.0F) {
        return;
    }
    decal.polygon = {
        transform_point(decal.into_box, a),
        transform_point(decal.into_box, b),
        transform_point(decal.into_box, c),
    };
    for (int axis = 0; axis < 3 && !decal.polygon.empty(); ++axis) {
        for (const float sign : { -1.0F, 1.0F }) {
            decal.clipped.clear();
            const std::size_t count = decal.polygon.size();
            for (std::size_t i = 0; i < count; ++i) {
                const vec3_t from = decal.polygon[i];
                const vec3_t to = decal.polygon[(i + 1) % count];
                const float from_in = 1.0F - (along(from, axis) * sign);
                const float to_in = 1.0F - (along(to, axis) * sign);
                if (from_in >= 0.0F) {
                    decal.clipped.push_back(from);
                }
                if ((from_in >= 0.0F) != (to_in >= 0.0F)) {
                    const vec3_t across = to - from;
                    const float part = from_in / (from_in - to_in);
                    decal.clipped.push_back(from + across * part);
                }
            }
            decal.polygon.swap(decal.clipped);
        }
    }
    if (decal.polygon.size() < 3) {
        return;
    }
    const auto first = static_cast<std::uint32_t>(decal.vertices.size());
    for (const vec3_t& in_box : decal.polygon) {
        const vec3_t place = transform_point(decal.from_box, in_box) + normal * decal.lift;
        const float u = (in_box.x + 1.0F) * 0.5F;
        const float v = (1.0F - in_box.y) * 0.5F;
        decal.vertices.push_back({
            .position = { .x = place.x, .y = place.y, .z = place.z },
            .normal = { .x = normal.x, .y = normal.y, .z = normal.z },
            .uv0 = { .x = u, .y = v },
            .uv1 = { .x = u, .y = v },
            .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
        });
    }
    for (std::uint32_t i = 2; i < decal.polygon.size(); ++i) {
        decal.indices.insert(decal.indices.end(), { first, first + i - 1, first + i });
    }
}

/// The box in a space, from the box's own corners through a matrix.
void box_in(const mat4_t& box_to_space, vec3_t& min, vec3_t& max) {
    for (unsigned corner = 0; corner < 8; ++corner) {
        const vec3_t in_box((corner & 1U) != 0 ? 1.0F : -1.0F, (corner & 2U) != 0 ? 1.0F : -1.0F,
                            (corner & 4U) != 0 ? 1.0F : -1.0F);
        const vec3_t p = transform_point(box_to_space, in_box);
        min = corner == 0
                  ? p
                  : vec3_t(std::fmin(min.x, p.x), std::fmin(min.y, p.y), std::fmin(min.z, p.z));
        max = corner == 0
                  ? p
                  : vec3_t(std::fmax(max.x, p.x), std::fmax(max.y, p.y), std::fmax(max.z, p.z));
    }
}

} // namespace

} // namespace fbe::scene

extern "C" {

FBE_API uint64_t fbe_geometry_decal(uint64_t world, const fbe_mat4_t* box, float lift) {
    using namespace fbe::scene;
    const std::uint32_t slot = world_slot(world);
    if (slot == none) {
        return 0;
    }
    if (!std::isfinite(lift) || lift < 0.0F) {
        fbe::fail(std::format("a decal's lifted 0 or more off what it's on, not {}", lift));
        return 0;
    }
    decal_t decal;
    std::ranges::copy(box->m, decal.from_box.m.begin());
    if (!inverse_affine(decal.from_box, decal.into_box)) {
        fbe::fail("a decal's box has an inverse");
        return 0;
    }
    decal.facing = normalize(decal.from_box.axis(2));
    decal.lift = lift;
    const scene_t& s = scene();
    for (const mesh_t& mesh : s.meshes) {
        if (mesh.geometry == none || s.entities.world[mesh.entity] != slot ||
            !showing(mesh.entity)) {
            continue;
        }
        update_world(mesh.entity);
        const mat4_t& to_world = s.entities.world_matrix[mesh.entity];
        mat4_t to_mesh;
        if (!inverse_affine(to_world, to_mesh)) {
            continue;
        }
        vec3_t low;
        vec3_t high;
        box_in(to_mesh * decal.from_box, low, high);
        const bvh_t& tree = geometry_tree(mesh.geometry);
        const std::vector<surface_t>& surfaces = s.geometries[mesh.geometry].surfaces;
        const bool flipped = mirrors(to_world);
        tree.overlapping(low, high, decal.stack, [&](std::uint32_t triangle) {
            const std::uint32_t surface = tree.origin(triangle).surface;
            const std::uint32_t material =
                mesh.material != none ? mesh.material : surfaces[surface].material;
            if (drawn_blend(material, mesh.tint[3]) != FBE_BLEND_OPAQUE) {
                return;
            }
            const vec3_t* corners = tree.corners(triangle);
            const vec3_t a = transform_point(to_world, corners[0]);
            const vec3_t b = transform_point(to_world, corners[1]);
            const vec3_t c = transform_point(to_world, corners[2]);
            add_triangle(decal, a, flipped ? c : b, flipped ? b : c);
        });
    }
    for (const plane_t& plane : s.planes) {
        if (s.entities.world[plane.entity] != slot || !showing(plane.entity) ||
            drawn_blend(plane.material, plane.tint[3]) != FBE_BLEND_OPAQUE) {
            continue;
        }
        update_world(plane.entity);
        const mat4_t& to_world = s.entities.world_matrix[plane.entity];
        mat4_t to_plane;
        if (!inverse_affine(to_world, to_plane)) {
            continue;
        }
        // A square of its y of 0 as big as the box over it, its own up as a Terrain's.
        vec3_t low;
        vec3_t high;
        box_in(to_plane * decal.from_box, low, high);
        const bool flipped = mirrors(to_world);
        const vec3_t a = transform_point(to_world, vec3_t(low.x, 0.0F, low.z));
        const vec3_t b = transform_point(to_world, vec3_t(high.x, 0.0F, low.z));
        const vec3_t c = transform_point(to_world, vec3_t(low.x, 0.0F, high.z));
        const vec3_t d = transform_point(to_world, vec3_t(high.x, 0.0F, high.z));
        add_triangle(decal, a, flipped ? d : c, flipped ? c : d);
        add_triangle(decal, a, flipped ? b : d, flipped ? d : b);
    }
    std::vector<std::array<vec3_t, 3>> under;
    for (const terrain_t& terrain : s.terrains) {
        if (s.entities.world[terrain.entity] != slot || !showing(terrain.entity) ||
            drawn_blend(terrain.material, terrain.tint[3]) != FBE_BLEND_OPAQUE) {
            continue;
        }
        update_world(terrain.entity);
        const mat4_t& to_world = s.entities.world_matrix[terrain.entity];
        mat4_t to_terrain;
        if (!inverse_affine(to_world, to_terrain)) {
            continue;
        }
        vec3_t low;
        vec3_t high;
        box_in(to_terrain * decal.from_box, low, high);
        under.clear();
        terrain_under(terrain, low, high, under);
        const bool flipped = mirrors(to_world);
        for (const std::array<vec3_t, 3>& corners : under) {
            const vec3_t a = transform_point(to_world, corners[0]);
            const vec3_t b = transform_point(to_world, corners[1]);
            const vec3_t c = transform_point(to_world, corners[2]);
            add_triangle(decal, a, flipped ? c : b, flipped ? b : c);
        }
    }
    const std::uint64_t made = fbe_geometry_create();
    if (made == 0) {
        return 0;
    }
    const std::int32_t surface = fbe_geometry_add_surface(made);
    if (!decal.vertices.empty()) {
        fbe_surface_add_vertices(made, surface, decal.vertices.data(),
                                 static_cast<std::int32_t>(decal.vertices.size()));
        fbe_surface_add_triangles(made, surface, decal.indices.data(),
                                  static_cast<std::int32_t>(decal.indices.size()));
    }
    return made;
}

} // extern "C"
