// What each kind of entity keeps beside its place in the tree and its transform, in a pool of its
// kind's, which the entity's component indexes.

#include "fbengine.h"
#include "scene/scene.hpp"

#include <cstdint>

namespace fbe::scene {

void add_component(std::uint32_t entity) {
    entities_t& entities = scene().entities;
    entities.component[entity] = none;
    if (entities.kind[entity] == FBE_ENTITY_CAMERA) {
        add_camera(entity);
    } else if (entities.kind[entity] == FBE_ENTITY_LIGHT) {
        add_light(entity);
    }
}

void remove_component(std::uint32_t entity) {
    if (scene().entities.kind[entity] == FBE_ENTITY_CAMERA) {
        remove_camera(entity);
    } else if (scene().entities.kind[entity] == FBE_ENTITY_LIGHT) {
        remove_light(entity);
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
    } else if (entities.kind[from] == FBE_ENTITY_LIGHT) {
        light_t& copy = scene().lights[entities.component[to]];
        copy = scene().lights[entities.component[from]];
        copy.entity = to;
    }
}

} // namespace fbe::scene
