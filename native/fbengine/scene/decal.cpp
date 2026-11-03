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
#include <new>
#include <numbers>
#include <utility>
#include <vector>

namespace fbe::scene {

namespace {

/// How far before its limit a decal starts to fade, in radians.
constexpr float fade_before_limit = 0.26F;
/// The most of the way to the eye a decal's drawn nearer.
constexpr float most_eye_pull = 0.05F;

/// A triangle's corner, in the world, with its normal.
struct corner_t {
    vec3_t place;
    vec3_t normal;
};

/// A polygon's corner as it's being clipped, in the box's space.
struct clipping_t {
    vec3_t in_box;
    vec3_t normal;
};

/// What's being made: into the box, its matrix's inverse, and from it; which way it faces; how far
/// off what it's on; the least a face can face it, and where it starts to fade; and what it's
/// making.
struct decal_t {
    mat4_t from_box;
    mat4_t into_box;
    vec3_t facing;
    float lift = 0.0F;
    float least_facing = 0.0F;
    float whole_facing = 1.0F;
    std::vector<fbe_vertex_t> vertices;
    std::vector<std::uint32_t> indices;
    /// What a polygon's clipped through, kept for the next.
    std::vector<clipping_t> polygon;
    std::vector<clipping_t> clipped;
    std::vector<std::uint32_t> stack;
    /// How much nearer it wants to be drawn, for what it's on.
    float eye_pull = 0.0F;
};

float along(vec3_t p, int axis) {
    if (axis == 0) {
        return p.x;
    }
    return axis == 1 ? p.y : p.z;
}

/// A triangle in the world, if it faces the box's way, near enough, clipped by the box's six sides,
/// as a fan. A two-sided one's back counts as a front.
void add_triangle(decal_t& decal, corner_t a, corner_t b, corner_t c, bool two_sided) {
    vec3_t raw = cross(b.place - a.place, c.place - a.place);
    const float size = length(raw);
    if (size <= 0.0F) {
        return;
    }
    if (two_sided && dot(raw, decal.facing) < 0.0F) {
        std::swap(b, c);
        raw = -raw;
        a.normal = -a.normal;
        b.normal = -b.normal;
        c.normal = -c.normal;
    }
    const vec3_t face = raw * (1.0F / size);
    if (dot(face, decal.facing) <= decal.least_facing) {
        return;
    }
    decal.polygon = {
        { .in_box = transform_point(decal.into_box, a.place), .normal = a.normal },
        { .in_box = transform_point(decal.into_box, b.place), .normal = b.normal },
        { .in_box = transform_point(decal.into_box, c.place), .normal = c.normal },
    };
    for (int axis = 0; axis < 3 && !decal.polygon.empty(); ++axis) {
        for (const float sign : { -1.0F, 1.0F }) {
            decal.clipped.clear();
            const std::size_t count = decal.polygon.size();
            for (std::size_t i = 0; i < count; ++i) {
                const clipping_t& from = decal.polygon[i];
                const clipping_t& to = decal.polygon[(i + 1) % count];
                const float from_in = 1.0F - (along(from.in_box, axis) * sign);
                const float to_in = 1.0F - (along(to.in_box, axis) * sign);
                if (from_in >= 0.0F) {
                    decal.clipped.push_back(from);
                }
                if ((from_in >= 0.0F) != (to_in >= 0.0F)) {
                    const float part = from_in / (from_in - to_in);
                    const vec3_t place_across = to.in_box - from.in_box;
                    const vec3_t normal_across = to.normal - from.normal;
                    decal.clipped.push_back({
                        .in_box = from.in_box + place_across * part,
                        .normal = from.normal + normal_across * part,
                    });
                }
            }
            decal.polygon.swap(decal.clipped);
        }
    }
    if (decal.polygon.size() < 3) {
        return;
    }
    const auto first = static_cast<std::uint32_t>(decal.vertices.size());
    for (const clipping_t& corner : decal.polygon) {
        const float long_as = length(corner.normal);
        // What's smooth goes by its vertices' normals, so neighbors meet; what has none, its face.
        const vec3_t normal = long_as > 0.0F ? corner.normal * (1.0F / long_as) : face;
        const vec3_t place = transform_point(decal.from_box, corner.in_box) + normal * decal.lift;
        const float u = (corner.in_box.x + 1.0F) * 0.5F;
        const float v = (1.0F - corner.in_box.y) * 0.5F;
        const float fading = (dot(normal, decal.facing) - decal.least_facing) /
                             (decal.whole_facing - decal.least_facing);
        decal.vertices.push_back({
            .position = { .x = place.x, .y = place.y, .z = place.z },
            .normal = { .x = normal.x, .y = normal.y, .z = normal.z },
            .uv0 = { .x = u, .y = v },
            .uv1 = { .x = u, .y = v },
            .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = std::clamp(fading, 0.0F, 1.0F) },
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

bool two_sided_material(std::uint32_t material) {
    return material != none && scene().materials[material].two_sided;
}

void add_mesh(decal_t& decal, const mesh_t& mesh) {
    const scene_t& s = scene();
    update_world(mesh.entity);
    const mat4_t& to_world = s.entities.world_matrix[mesh.entity];
    mat4_t to_mesh;
    if (!inverse_affine(to_world, to_mesh)) {
        return;
    }
    vec3_t low;
    vec3_t high;
    box_in(to_mesh * decal.from_box, low, high);
    // Its tree's work to make, so only for what the box can reach.
    vec3_t bounds_low;
    vec3_t bounds_high;
    geometry_bounds(mesh.geometry, bounds_low, bounds_high);
    if (low.x > bounds_high.x || low.y > bounds_high.y || low.z > bounds_high.z ||
        high.x < bounds_low.x || high.y < bounds_low.y || high.z < bounds_low.z) {
        return;
    }
    const bvh_t& tree = geometry_tree(mesh.geometry);
    const std::vector<surface_t>& surfaces = s.geometries[mesh.geometry].surfaces;
    const bool flipped = mirrors(to_world);
    tree.overlapping(low, high, decal.stack, [&](std::uint32_t triangle) {
        const triangle_origin_t origin = tree.origin(triangle);
        const surface_t& surface = surfaces[origin.surface];
        const std::uint32_t material = mesh.material != none ? mesh.material : surface.material;
        if (drawn_blend(material, mesh.tint[3]) != FBE_BLEND_OPAQUE) {
            return;
        }
        const auto corner_of = [&](std::size_t at) {
            const std::uint32_t index =
                surface.indices[(static_cast<std::size_t>(origin.triangle) * 3) + at];
            const fbe_vertex_t& vertex = surface.vertices[index];
            const vec3_t normal(vertex.normal.x, vertex.normal.y, vertex.normal.z);
            return corner_t{
                .place = transform_point(
                    to_world, vec3_t(vertex.position.x, vertex.position.y, vertex.position.z)),
                .normal = transform_normal(to_world, normal),
            };
        };
        const corner_t a = corner_of(0);
        const corner_t b = corner_of(1);
        const corner_t c = corner_of(2);
        add_triangle(decal, a, flipped ? c : b, flipped ? b : c, two_sided_material(material));
    });
}

void add_plane_under(decal_t& decal, const plane_t& plane) {
    const scene_t& s = scene();
    update_world(plane.entity);
    const mat4_t& to_world = s.entities.world_matrix[plane.entity];
    mat4_t to_plane;
    if (!inverse_affine(to_world, to_plane)) {
        return;
    }
    // A square of its y of 0 as big as the box over it, its own up as a terrain's.
    vec3_t low;
    vec3_t high;
    box_in(to_plane * decal.from_box, low, high);
    const bool flipped = mirrors(to_world);
    const vec3_t up = transform_normal(to_world, vec3_t(0.0F, 1.0F, 0.0F));
    const auto corner_at = [&](float x, float z) {
        return corner_t{ .place = transform_point(to_world, vec3_t(x, 0.0F, z)), .normal = up };
    };
    const corner_t a = corner_at(low.x, low.z);
    const corner_t b = corner_at(high.x, low.z);
    const corner_t c = corner_at(low.x, high.z);
    const corner_t d = corner_at(high.x, high.z);
    const bool both_sides = two_sided_material(plane.material);
    add_triangle(decal, a, flipped ? d : c, flipped ? c : d, both_sides);
    add_triangle(decal, a, flipped ? b : d, flipped ? d : b, both_sides);
}

void add_terrain(decal_t& decal, const terrain_t& terrain,
                 std::vector<std::array<vec3_t, 3>>& under) {
    const scene_t& s = scene();
    update_world(terrain.entity);
    const mat4_t& to_world = s.entities.world_matrix[terrain.entity];
    mat4_t to_terrain;
    if (!inverse_affine(to_world, to_terrain)) {
        return;
    }
    vec3_t low;
    vec3_t high;
    box_in(to_terrain * decal.from_box, low, high);
    under.clear();
    terrain_under(terrain, low, high, under);
    // Far off, a coarser piece can come up through it. Lifted that much it'd float when it's near;
    // drawn that much nearer the eye, which scales with how far, it stays on the same pixels.
    if (!under.empty()) {
        const float across = std::fmax(length(to_world.axis(0)), length(to_world.axis(2)));
        const float rise = terrain_coarse_rise(terrain, low, high, across);
        decal.eye_pull = std::fmax(decal.eye_pull, rise * length(to_world.axis(1)));
    }
    const bool flipped = mirrors(to_world);
    const bool both_sides = two_sided_material(terrain.material);
    for (const std::array<vec3_t, 3>& corners : under) {
        const auto corner_of = [&](std::size_t at) {
            const vec3_t local = corners[at];
            return corner_t{
                .place = transform_point(to_world, local),
                .normal = transform_normal(
                    to_world, terrain_corner_normal(terrain, static_cast<std::int32_t>(local.x),
                                                    static_cast<std::int32_t>(local.z))),
            };
        };
        const corner_t a = corner_of(0);
        const corner_t b = corner_of(1);
        const corner_t c = corner_of(2);
        add_triangle(decal, a, flipped ? c : b, flipped ? b : c, both_sides);
    }
}

} // namespace

} // namespace fbe::scene

extern "C" {

FBE_API uint64_t fbe_geometry_decal(uint64_t world, const fbe_mat4_t* box, float lift,
                                    float limit) try {
    using namespace fbe::scene;
    const std::uint32_t slot = world_slot(world);
    if (slot == none) {
        return 0;
    }
    if (!std::isfinite(lift) || lift < 0.0F) {
        fbe::fail(std::format("a decal's lifted 0 or more off what it's on, not {}", lift));
        return 0;
    }
    if (std::isnan(limit) || limit <= 0.0F || limit > std::numbers::pi_v<float> / 2.0F) {
        fbe::fail(std::format("a decal's limit is above 0 and up to Pi / 2, not {}", limit));
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
    decal.least_facing = std::cos(limit);
    decal.whole_facing = std::cos(std::fmax(limit - fade_before_limit, 0.0F));
    const scene_t& s = scene();
    for (const mesh_t& mesh : s.meshes) {
        if (mesh.geometry != none && s.entities.world[mesh.entity] == slot &&
            showing(mesh.entity)) {
            add_mesh(decal, mesh);
        }
    }
    for (const plane_t& plane : s.planes) {
        if (s.entities.world[plane.entity] == slot && showing(plane.entity) &&
            drawn_blend(plane.material, plane.tint[3]) == FBE_BLEND_OPAQUE) {
            add_plane_under(decal, plane);
        }
    }
    std::vector<std::array<vec3_t, 3>> under;
    for (const terrain_t& terrain : s.terrains) {
        if (s.entities.world[terrain.entity] == slot && showing(terrain.entity) &&
            drawn_blend(terrain.material, terrain.tint[3]) == FBE_BLEND_OPAQUE) {
            add_terrain(decal, terrain, under);
        }
    }
    const std::uint64_t made = fbe_geometry_create();
    if (made == 0) {
        return 0;
    }
    const std::int32_t surface = fbe_geometry_add_surface(made);
    if (surface < 0 ||
        (!decal.vertices.empty() &&
         (fbe_surface_add_vertices(made, surface, decal.vertices.data(),
                                   static_cast<std::int32_t>(decal.vertices.size())) < 0 ||
          fbe_surface_add_triangles(made, surface, decal.indices.data(),
                                    static_cast<std::int32_t>(decal.indices.size())) < 0))) {
        fbe_geometry_release(made);
        return 0;
    }
    scene().geometries[geometry_slot(made)].eye_pull = std::fmin(decal.eye_pull, most_eye_pull);
    return made;
} catch (const std::bad_alloc&) {
    fbe::fail("out of memory");
    return 0;
}

} // extern "C"
