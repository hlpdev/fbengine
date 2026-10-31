// Picks: lines, and spheres swept along them, against the entities with a pick mode, as their
// ellipsoids, boxes, or meshes' triangles, the way collisions touch them; the nearest hit wins.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/bvh.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"
#include "scene/sweep.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <vector>

namespace fbe::scene {

namespace {

fbe_vec3_t c_vec(vec3_t v) {
    return { .x = v.x, .y = v.y, .z = v.z };
}

vec3_t vec(const fbe_vec3_t& v) {
    return { v.x, v.y, v.z };
}

/// The most a matrix stretches a length: never more than the root of its axes' squared lengths.
float stretch(const mat4_t& m) {
    return std::sqrt(dot(m.axis(0), m.axis(0)) + dot(m.axis(1), m.axis(1)) +
                     dot(m.axis(2), m.axis(2)));
}

/// Sweeps the pick against an entity as its pick mode says, keeping the hit if it's the nearest
/// yet; its box in the world is already known to be in the pick's way.
void pick_entity(std::uint32_t entity, vec3_t from, vec3_t along, float radius, hit_t& hit,
                 bool& found) {
    const scene_t& s = scene();
    const collider_t& collider = s.entities.colliders[entity];
    update_world(entity);
    const mat4_t& matrix = s.entities.world_matrix[entity];
    if (collider.pick == FBE_PICK_ELLIPSOID) {
        const ellipsoid_t ellipsoid{
            .center = matrix.axis(3),
            .radii = vec3_t(collider.horizontal + radius, collider.vertical + radius,
                            collider.horizontal + radius),
            .target = entity,
        };
        sweep_ellipsoid(from, along, radius, ellipsoid, hit, found);
        return;
    }
    const bool mirrored = mirrors(matrix);
    if (collider.pick == FBE_PICK_BOX) {
        for (const auto& corners : box_triangles) {
            triangle_t triangle;
            if (make_triangle(
                    {
                        transform_point(matrix,
                                        box_corner(collider.box_min, collider.box_max, corners[0])),
                        transform_point(matrix,
                                        box_corner(collider.box_min, collider.box_max, corners[1])),
                        transform_point(matrix,
                                        box_corner(collider.box_min, collider.box_max, corners[2])),
                    },
                    mirrored, triangle)) {
                triangle.target = entity;
                sweep_triangle(from, along, radius, triangle, hit, found);
            }
        }
        return;
    }
    if (collider.pick == FBE_PICK_MESH && s.entities.kind[entity] == FBE_ENTITY_TERRAIN) {
        mat4_t into;
        if (!inverse_affine(matrix, into)) {
            return;
        }
        // In pieces of a few squares each, so a long pick over a big terrain tries only the
        // squares near its line, nearest first, and stops at the first piece with a hit.
        const terrain_t& terrain = s.terrains[s.entities.component[entity]];
        const vec3_t a = transform_point(into, from);
        const vec3_t b = transform_point(into, from + along);
        const float grow = radius * stretch(into);
        const float span = std::fmax(std::fabs(b.x - a.x), std::fabs(b.z - a.z));
        const int pieces = std::clamp(static_cast<int>(std::ceil(span / 4.0F)), 1, 4096);
        std::vector<std::array<vec3_t, 3>> under;
        for (int piece = 0; piece < pieces && !found; ++piece) {
            const float t0 = static_cast<float>(piece) / static_cast<float>(pieces);
            const float t1 = static_cast<float>(piece + 1) / static_cast<float>(pieces);
            const vec3_t across = b - a;
            const vec3_t p0 = a + across * t0;
            const vec3_t p1 = a + across * t1;
            const vec3_t margin(grow, grow, grow);
            under.clear();
            terrain_under(
                terrain,
                vec3_t(std::fmin(p0.x, p1.x), std::fmin(p0.y, p1.y), std::fmin(p0.z, p1.z)) -
                    margin,
                vec3_t(std::fmax(p0.x, p1.x), std::fmax(p0.y, p1.y), std::fmax(p0.z, p1.z)) +
                    margin,
                under);
            for (const auto& corners : under) {
                triangle_t triangle;
                if (make_triangle(
                        {
                            transform_point(matrix, corners[0]),
                            transform_point(matrix, corners[1]),
                            transform_point(matrix, corners[2]),
                        },
                        mirrored, triangle)) {
                    triangle.target = entity;
                    sweep_triangle(from, along, radius, triangle, hit, found);
                }
            }
        }
        return;
    }
    if (collider.pick == FBE_PICK_MESH && s.entities.kind[entity] == FBE_ENTITY_PLANE) {
        // The part of the plane under the pick's box.
        const vec3_t to = from + along;
        const vec3_t grow(radius, radius, radius);
        const vec3_t min(std::fmin(from.x, to.x), std::fmin(from.y, to.y), std::fmin(from.z, to.z));
        const vec3_t max(std::fmax(from.x, to.x), std::fmax(from.y, to.y), std::fmax(from.z, to.z));
        std::array<std::array<vec3_t, 3>, 2> under;
        if (plane_under(matrix, min - grow, max + grow, under)) {
            for (const auto& corners : under) {
                triangle_t triangle;
                if (make_triangle(corners, mirrored, triangle)) {
                    triangle.target = entity;
                    sweep_triangle(from, along, radius, triangle, hit, found);
                }
            }
        }
        return;
    }
    if (collider.pick != FBE_PICK_MESH || s.entities.kind[entity] != FBE_ENTITY_MESH) {
        return;
    }
    const std::uint32_t geometry = s.meshes[s.entities.component[entity]].geometry;
    mat4_t into;
    if (geometry == none || !inverse_affine(matrix, into)) {
        return;
    }
    // The tree is in the mesh's own space, so the pick goes there to search it, its radius
    // stretched as far as that space can, and each triangle it might hit comes back to the world.
    const bvh_t& tree = geometry_tree(geometry);
    const vec3_t a = transform_point(into, from);
    const vec3_t b = transform_point(into, from + along);
    tree.along(
        a, b, radius * stretch(into), [&] { return found ? hit.time : 1.0F; },
        [&](std::uint32_t index) {
            const vec3_t* corners = tree.corners(index);
            triangle_t triangle;
            if (!make_triangle(
                    {
                        transform_point(matrix, corners[0]),
                        transform_point(matrix, corners[1]),
                        transform_point(matrix, corners[2]),
                    },
                    mirrored, triangle)) {
                return;
            }
            triangle.target = entity;
            triangle.origin = tree.origin(index);
            triangle.from_mesh = true;
            sweep_triangle(from, along, radius, triangle, hit, found);
        });
}

/// Works out a pickables entry's box again if it's stale, or its mesh's geometry changed since;
/// false for a mesh with no geometry, which nothing hits.
bool refresh(pickable_t& entry) {
    const scene_t& s = scene();
    const std::uint32_t entity = entry.entity;
    if (entry.mode == FBE_PICK_MESH && s.entities.kind[entity] == FBE_ENTITY_TERRAIN) {
        const terrain_t& terrain = s.terrains[s.entities.component[entity]];
        if (entry.stale || entry.version != terrain.version) {
            vec3_t min;
            vec3_t max;
            terrain_bounds(terrain, min, max);
            world_box(entity, min, max, entry.min, entry.max);
            entry.version = terrain.version;
            entry.stale = false;
        }
        return true;
    }
    if (entry.mode == FBE_PICK_MESH && s.entities.kind[entity] == FBE_ENTITY_PLANE) {
        // Everywhere: the plane's own test says.
        entry.min = vec3_t(-INFINITY, -INFINITY, -INFINITY);
        entry.max = vec3_t(INFINITY, INFINITY, INFINITY);
        entry.stale = false;
        return true;
    }
    if (entry.mode == FBE_PICK_MESH) {
        const std::uint32_t geometry = s.entities.kind[entity] == FBE_ENTITY_MESH
                                           ? s.meshes[s.entities.component[entity]].geometry
                                           : none;
        if (geometry == none) {
            entry.geometry = none;
            return false;
        }
        const std::uint64_t version = s.geometries[geometry].version;
        if (entry.stale || entry.geometry != geometry || entry.version != version) {
            vec3_t min;
            vec3_t max;
            geometry_bounds(geometry, min, max);
            world_box(entity, min, max, entry.min, entry.max);
            entry.geometry = geometry;
            entry.version = version;
            entry.stale = false;
        }
        return true;
    }
    if (entry.stale) {
        const collider_t& collider = s.entities.colliders[entity];
        if (entry.mode == FBE_PICK_ELLIPSOID) {
            update_world(entity);
            const vec3_t center = s.entities.world_matrix[entity].axis(3);
            const vec3_t radii(collider.horizontal, collider.vertical, collider.horizontal);
            entry.min = center - radii;
            entry.max = center + radii;
        } else {
            world_box(entity, collider.box_min, collider.box_max, entry.min, entry.max);
        }
        entry.stale = false;
    }
    return true;
}

/// Drops the pickables entries no longer used once there are as many as used ones.
void compact_pickables() {
    scene_t& s = scene();
    if (s.unused_pickables < 64 || s.unused_pickables * 2 < s.pickables.size()) {
        return;
    }
    std::erase_if(s.pickables, [](const pickable_t& entry) { return entry.entity == none; });
    for (std::uint32_t i = 0; i < s.pickables.size(); ++i) {
        s.entities.pickable[s.pickables[i].entity] = i;
    }
    s.unused_pickables = 0;
}

} // namespace

void pickable_changed(std::uint32_t entity) {
    scene_t& s = scene();
    std::uint32_t& index = s.entities.pickable[entity];
    const std::int32_t mode = s.entities.colliders[entity].pick;
    if (mode == FBE_PICK_NONE) {
        if (index != none) {
            s.pickables[index].entity = none;
            ++s.unused_pickables;
            index = none;
        }
        return;
    }
    if (index == none) {
        index = static_cast<std::uint32_t>(s.pickables.size());
        s.pickables.push_back({ .entity = entity });
    }
    pickable_t& entry = s.pickables[index];
    entry.world = s.entities.world[entity];
    entry.mode = mode;
    entry.stale = true;
}

void pickable_moved(std::uint32_t entity) {
    scene_t& s = scene();
    const std::uint32_t index = s.entities.pickable[entity];
    if (index != none) {
        s.pickables[index].stale = true;
    }
}

} // namespace fbe::scene

using fbe::scene::entity_slot;
using fbe::scene::none;
using fbe::scene::scene;
using fbe::scene::vec3_t;

extern "C" {

FBE_API bool fbe_entity_set_pick_mode(uint64_t entity, int32_t mode) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    if (mode < FBE_PICK_NONE || mode > FBE_PICK_MESH) {
        return fbe::fail(
            std::format("a pick mode is FBE_PICK_NONE to FBE_PICK_MESH, not {}", mode));
    }
    scene().entities.colliders[slot].pick = mode;
    fbe::scene::pickable_changed(slot);
    return true;
}

FBE_API int32_t fbe_entity_pick_mode(uint64_t entity) {
    const std::uint32_t slot = entity_slot(entity);
    return slot != none ? scene().entities.colliders[slot].pick : FBE_PICK_NONE;
}

FBE_API int32_t fbe_world_pick(uint64_t world, const fbe_vec3_t* from, const fbe_vec3_t* to,
                               float radius, uint64_t ignore, fbe_pick_t* pick) {
    const std::uint32_t world_slot = fbe::scene::world_slot(world);
    if (world_slot == none) {
        return -1;
    }
    std::uint32_t ignored = none;
    if (ignore != 0) {
        ignored = entity_slot(ignore);
        if (ignored == none) {
            return -1;
        }
    }
    if (!std::isfinite(radius) || radius < 0.0F) {
        fbe::fail(std::format("a pick's radius is 0 or more, not {}", radius));
        return -1;
    }
    const vec3_t start = fbe::scene::vec(*from);
    const vec3_t end = fbe::scene::vec(*to);
    if (!std::isfinite(start.x) || !std::isfinite(start.y) || !std::isfinite(start.z) ||
        !std::isfinite(end.x) || !std::isfinite(end.y) || !std::isfinite(end.z)) {
        fbe::fail("a pick goes between finite points");
        return -1;
    }
    fbe::scene::compact_pickables();
    fbe::scene::scene_t& s = scene();
    const vec3_t along = end - start;
    const fbe::scene::segment_t segment(start, along);
    const vec3_t grow(radius, radius, radius);
    fbe::scene::hit_t hit;
    bool found = false;
    // Each entry's box first, from one array, and only what the pick might reach closer.
    for (fbe::scene::pickable_t& entry : s.pickables) {
        if (entry.entity == none || entry.world != world_slot || entry.entity == ignored ||
            !fbe::scene::refresh(entry) ||
            !fbe::scene::crosses_box(segment, entry.min - grow, entry.max + grow,
                                     found ? hit.time : 1.0F) ||
            !fbe::scene::showing(entry.entity)) {
            continue;
        }
        fbe::scene::pick_entity(entry.entity, start, along, radius, hit, found);
    }
    if (!found) {
        return 0;
    }
    *pick = {
        .entity = s.entities.slots.handle(hit.target),
        .point = fbe::scene::c_vec(hit.point),
        .normal = fbe::scene::c_vec(hit.normal),
        .time = hit.time,
        .geometry = 0,
        .surface = -1,
        .triangle = -1,
    };
    if (hit.from_mesh) {
        const std::uint32_t geometry = s.meshes[s.entities.component[hit.target]].geometry;
        pick->geometry = s.geometry_slots.handle(geometry);
        pick->surface = static_cast<std::int32_t>(hit.origin.surface);
        pick->triangle = static_cast<std::int32_t>(hit.origin.triangle);
    }
    return 1;
}

} // extern "C"
