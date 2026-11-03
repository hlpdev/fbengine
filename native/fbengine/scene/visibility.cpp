// Visibility: entities' boxes in the world, and line of sight past what blocks it.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cmath>
#include <cstdint>

namespace fbe::scene {

namespace {

fbe_vec3_t c_vec(vec3_t v) {
    return { .x = v.x, .y = v.y, .z = v.z };
}

/// Whether anything that blocks sight in a world, but the two entities, crosses the segment.
bool blocked(std::uint32_t world, vec3_t a, vec3_t b, std::uint32_t first, std::uint32_t second) {
    const scene_t& s = scene();
    for (const mesh_t& mesh : s.meshes) {
        const std::uint32_t entity = mesh.entity;
        if (mesh.geometry == none || entity == first || entity == second ||
            s.entities.world[entity] != world || !s.entities.blocks_sight[entity] ||
            !showing(entity)) {
            continue;
        }
        update_world(entity);
        mat4_t into;
        if (!inverse_affine(s.entities.world_matrix[entity], into)) {
            continue;
        }
        const bvh_t& tree = geometry_tree(mesh.geometry);
        if (tree.blocks(transform_point(into, a), transform_point(into, b))) {
            return true;
        }
    }
    for (const terrain_t& terrain : s.terrains) {
        const std::uint32_t entity = terrain.entity;
        if (entity == first || entity == second || s.entities.world[entity] != world ||
            !s.entities.blocks_sight[entity] || !showing(entity)) {
            continue;
        }
        update_world(entity);
        mat4_t into;
        if (inverse_affine(s.entities.world_matrix[entity], into) &&
            terrain_blocks(terrain, transform_point(into, a), transform_point(into, b))) {
            return true;
        }
    }
    return false;
}

vec3_t world_position(std::uint32_t entity) {
    update_world(entity);
    return scene().entities.world_matrix[entity].axis(3);
}

} // namespace

void world_box(std::uint32_t entity, vec3_t local_min, vec3_t local_max, vec3_t& world_min,
               vec3_t& world_max) {
    update_world(entity);
    const mat4_t& m = scene().entities.world_matrix[entity];
    for (unsigned corner = 0; corner < 8; ++corner) {
        const vec3_t local((corner & 1U) != 0 ? local_max.x : local_min.x,
                           (corner & 2U) != 0 ? local_max.y : local_min.y,
                           (corner & 4U) != 0 ? local_max.z : local_min.z);
        const vec3_t p = transform_point(m, local);
        if (corner == 0) {
            world_min = p;
            world_max = p;
        } else {
            world_min = { std::fmin(world_min.x, p.x), std::fmin(world_min.y, p.y),
                          std::fmin(world_min.z, p.z) };
            world_max = { std::fmax(world_max.x, p.x), std::fmax(world_max.y, p.y),
                          std::fmax(world_max.z, p.z) };
        }
    }
}

} // namespace fbe::scene

using fbe::scene::entity_slot;
using fbe::scene::none;
using fbe::scene::scene;
using fbe::scene::vec3_t;

extern "C" {

FBE_API bool fbe_entity_bounds(uint64_t entity, fbe_aabb_t* bounds) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    vec3_t min;
    vec3_t max;
    const fbe::scene::scene_t& s = scene();
    if (s.entities.kind[slot] == FBE_ENTITY_PLANE) {
        // It goes on forever, as collisions and picks find it.
        min = vec3_t(-INFINITY, -INFINITY, -INFINITY);
        max = vec3_t(INFINITY, INFINITY, INFINITY);
    } else if (s.entities.kind[slot] == FBE_ENTITY_MESH &&
               s.meshes[s.entities.component[slot]].geometry != none) {
        vec3_t local_min;
        vec3_t local_max;
        fbe::scene::geometry_bounds(s.meshes[s.entities.component[slot]].geometry, local_min,
                                    local_max);
        vec3_t world_min;
        vec3_t world_max;
        fbe::scene::world_box(slot, local_min, local_max, world_min, world_max);
        min = world_min;
        max = world_max;
    } else if (s.entities.kind[slot] == FBE_ENTITY_SPRITE) {
        fbe::scene::sprite_bounds(slot, min, max);
    } else if (s.entities.kind[slot] == FBE_ENTITY_TERRAIN) {
        vec3_t local_min;
        vec3_t local_max;
        fbe::scene::terrain_bounds(s.terrains[s.entities.component[slot]], local_min, local_max);
        fbe::scene::world_box(slot, local_min, local_max, min, max);
    } else {
        min = fbe::scene::world_position(slot);
        max = min;
    }
    *bounds = { .min = fbe::scene::c_vec(min), .max = fbe::scene::c_vec(max) };
    return true;
}

FBE_API bool fbe_entity_set_casts_shadows(uint64_t entity, bool casts) {
    const std::uint32_t slot = fbe::scene::entity_slot(entity);
    if (slot == none) {
        return false;
    }
    scene().entities.casts_shadows[slot] = casts;
    return true;
}

FBE_API bool fbe_entity_casts_shadows(uint64_t entity) {
    const std::uint32_t slot = fbe::scene::entity_slot(entity);
    return slot != none && scene().entities.casts_shadows[slot];
}

FBE_API bool fbe_entity_set_blocks_sight(uint64_t entity, bool blocks) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    scene().entities.blocks_sight[slot] = blocks;
    return true;
}

FBE_API bool fbe_entity_blocks_sight(uint64_t entity) {
    const std::uint32_t slot = entity_slot(entity);
    return slot != none && scene().entities.blocks_sight[slot];
}

FBE_API int32_t fbe_entity_can_see_point(uint64_t entity, float x, float y, float z) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return -1;
    }
    const vec3_t from = fbe::scene::world_position(slot);
    return fbe::scene::blocked(scene().entities.world[slot], from, { x, y, z }, slot, none) ? 0 : 1;
}

FBE_API int32_t fbe_entity_can_see(uint64_t entity, uint64_t target) {
    const std::uint32_t slot = entity_slot(entity);
    const std::uint32_t other = slot != none ? entity_slot(target) : none;
    if (other == none) {
        return -1;
    }
    // An entity sees nothing in another World.
    if (scene().entities.world[slot] != scene().entities.world[other]) {
        return 0;
    }
    const vec3_t from = fbe::scene::world_position(slot);
    const vec3_t to = fbe::scene::world_position(other);
    return fbe::scene::blocked(scene().entities.world[slot], from, to, slot, other) ? 0 : 1;
}

} // extern "C"
