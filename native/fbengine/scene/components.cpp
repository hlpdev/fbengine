// What each kind of entity keeps beside its place in the tree and its transform, in a pool of its
// kind's, which the entity's component indexes.

#include "fbengine.h"
#include "scene/scene.hpp"
#include "skin/skin.hpp"

#include <cstdint>

namespace fbe::scene {

void add_component(std::uint32_t entity) {
    entities_t& entities = scene().entities;
    entities.component[entity] = none;
    if (entities.kind[entity] == FBE_ENTITY_CAMERA) {
        add_camera(entity);
    } else if (entities.kind[entity] == FBE_ENTITY_LIGHT) {
        add_light(entity);
    } else if (entities.kind[entity] == FBE_ENTITY_MESH) {
        add_mesh(entity);
    } else if (entities.kind[entity] == FBE_ENTITY_SPRITE) {
        add_sprite(entity);
    } else if (entities.kind[entity] == FBE_ENTITY_PARTICLES) {
        add_particles(entity);
    } else if (entities.kind[entity] == FBE_ENTITY_PLANE) {
        add_plane(entity);
    } else if (entities.kind[entity] == FBE_ENTITY_TERRAIN) {
        add_terrain(entity);
    }
}

void remove_component(std::uint32_t entity) {
    if (scene().entities.kind[entity] == FBE_ENTITY_CAMERA) {
        remove_camera(entity);
    } else if (scene().entities.kind[entity] == FBE_ENTITY_LIGHT) {
        remove_light(entity);
    } else if (scene().entities.kind[entity] == FBE_ENTITY_MESH) {
        fbe::anim::forget_skin(entity);
        remove_mesh(entity);
    } else if (scene().entities.kind[entity] == FBE_ENTITY_SPRITE) {
        remove_sprite(entity);
    } else if (scene().entities.kind[entity] == FBE_ENTITY_PARTICLES) {
        remove_particles(entity);
    } else if (scene().entities.kind[entity] == FBE_ENTITY_PLANE) {
        remove_plane(entity);
    } else if (scene().entities.kind[entity] == FBE_ENTITY_TERRAIN) {
        remove_terrain(entity);
    }
    scene().entities.component[entity] = none;
}

void copy_component(std::uint32_t from, std::uint32_t to) {
    entities_t& entities = scene().entities;
    if (entities.kind[from] == FBE_ENTITY_CAMERA) {
        camera_t& copy = scene().cameras[entities.component[to]];
        const std::uint64_t made = copy.made;
        copy = scene().cameras[entities.component[from]];
        copy.entity = to;
        copy.made = made;
        if (copy.target != 0) {
            fbe_texture_retain(copy.target);
        }
    } else if (entities.kind[from] == FBE_ENTITY_LIGHT) {
        light_t& copy = scene().lights[entities.component[to]];
        copy = scene().lights[entities.component[from]];
        copy.entity = to;
    } else if (entities.kind[from] == FBE_ENTITY_MESH) {
        // Copies share their original's geometry and material, and have its tint.
        mesh_t& copy = scene().meshes[entities.component[to]];
        copy = scene().meshes[entities.component[from]];
        copy.entity = to;
        if (copy.geometry != none) {
            retain_geometry(copy.geometry);
        }
        retain_material(copy.material);
    } else if (entities.kind[from] == FBE_ENTITY_SPRITE) {
        // Copies share their original's material, and have its settings.
        sprite_t& copy = scene().sprites[entities.component[to]];
        copy = scene().sprites[entities.component[from]];
        copy.entity = to;
        retain_material(copy.material);
    } else if (entities.kind[from] == FBE_ENTITY_PARTICLES) {
        copy_particles(from, to);
    } else if (entities.kind[from] == FBE_ENTITY_PLANE) {
        plane_t& copy = scene().planes[entities.component[to]];
        copy = scene().planes[entities.component[from]];
        copy.entity = to;
        retain_material(copy.material);
    } else if (entities.kind[from] == FBE_ENTITY_TERRAIN) {
        // A copy has its own heights, to change apart from its original's.
        terrain_t& copy = scene().terrains[entities.component[to]];
        copy = scene().terrains[entities.component[from]];
        copy.entity = to;
        retain_material(copy.material);
    }
}

} // namespace fbe::scene
