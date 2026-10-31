// Ground planes, mirrors, how far from a camera an entity fades, and whether two meshes cross.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <cmath>
#include <cstdint>
#include <format>

namespace fbe::scene {

namespace {

constexpr std::int32_t most_segments = 16;

/// Where a mirror's plane is and which way it faces, in the world.
void mirror_plane(std::uint32_t mirror, vec3_t& place, vec3_t& normal) {
    update_world(mirror);
    const mat4_t& m = scene().entities.world_matrix[mirror];
    place = m.axis(3);
    normal = transform_normal(m, vec3_t(0.0F, 1.0F, 0.0F));
}

} // namespace

plane_t* plane_of(std::uint64_t handle) {
    const std::uint32_t entity = entity_slot(handle);
    if (entity == none) {
        return nullptr;
    }
    if (scene().entities.kind[entity] != FBE_ENTITY_PLANE) {
        fail("the entity isn't a ground plane");
        return nullptr;
    }
    return &scene().planes[scene().entities.component[entity]];
}

float entity_fade(std::uint32_t entity, std::uint32_t camera) {
    const entities_t& entities = scene().entities;
    const float start = entities.fade_near[entity];
    const float end = entities.fade_far[entity];
    if (end <= 0.0F) {
        return 1.0F;
    }
    update_world(entity);
    update_world(camera);
    const float distance =
        length(entities.world_matrix[entity].axis(3) - entities.world_matrix[camera].axis(3));
    if (distance <= start) {
        return 1.0F;
    }
    if (distance >= end) {
        return 0.0F;
    }
    return 1.0F - ((distance - start) / (end - start));
}

void add_plane(std::uint32_t entity) {
    scene_t& s = scene();
    s.entities.component[entity] = static_cast<std::uint32_t>(s.planes.size());
    s.planes.push_back({ .entity = entity });
}

void remove_plane(std::uint32_t entity) {
    scene_t& s = scene();
    const std::uint32_t index = s.entities.component[entity];
    release_material(s.planes[index].material);
    if (index != s.planes.size() - 1) {
        s.planes[index] = s.planes.back();
        s.entities.component[s.planes[index].entity] = index;
    }
    s.planes.pop_back();
}

} // namespace fbe::scene

using fbe::scene::entity_slot;
using fbe::scene::none;
using fbe::scene::scene;
using fbe::scene::vec3_t;

extern "C" {

FBE_API bool fbe_plane_set_segments(uint64_t plane, int32_t segments) {
    fbe::scene::plane_t* found = fbe::scene::plane_of(plane);
    if (found == nullptr) {
        return false;
    }
    if (segments < 1 || segments > fbe::scene::most_segments) {
        return fbe::fail(std::format("a ground plane has 1 to {} segments, not {}",
                                     fbe::scene::most_segments, segments));
    }
    found->segments = segments;
    return true;
}

FBE_API int32_t fbe_plane_segments(uint64_t plane) {
    const fbe::scene::plane_t* found = fbe::scene::plane_of(plane);
    return found != nullptr ? found->segments : -1;
}

FBE_API bool fbe_mirror_reflection(uint64_t mirror, fbe_mat4_t* matrix) {
    const std::uint32_t slot = entity_slot(mirror);
    if (slot == none) {
        return false;
    }
    if (scene().entities.kind[slot] != FBE_ENTITY_MIRROR) {
        return fbe::fail("the entity isn't a mirror");
    }
    vec3_t place;
    vec3_t n;
    fbe::scene::mirror_plane(slot, place, n);
    // x - 2 ((x - place) . n) n, as a matrix: I - 2 n n, and 2 (place . n) n.
    const float along = 2.0F * fbe::scene::dot(place, n);
    const fbe::scene::mat4_t found(
        vec3_t(1.0F - (2.0F * n.x * n.x), -2.0F * n.y * n.x, -2.0F * n.z * n.x),
        vec3_t(-2.0F * n.x * n.y, 1.0F - (2.0F * n.y * n.y), -2.0F * n.z * n.y),
        vec3_t(-2.0F * n.x * n.z, -2.0F * n.y * n.z, 1.0F - (2.0F * n.z * n.z)), n * along);
    for (std::size_t i = 0; i < found.m.size(); ++i) {
        matrix->m[i] = found.m[i];
    }
    return true;
}

FBE_API bool fbe_entity_set_auto_fade(uint64_t entity, float start, float end) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    const bool never = start == 0.0F && end == 0.0F;
    if (!never && (std::isnan(start) || start < 0.0F || !(start < end) || !std::isfinite(end))) {
        return fbe::fail(std::format("a fade is 0 <= start < end, finite, or 0 and 0 for none, not "
                                     "{} to {}",
                                     start, end));
    }
    scene().entities.fade_near[slot] = start;
    scene().entities.fade_far[slot] = end;
    return true;
}

FBE_API bool fbe_entity_auto_fade(uint64_t entity, fbe_vec2_t* fade) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    *fade = { .x = scene().entities.fade_near[slot], .y = scene().entities.fade_far[slot] };
    return true;
}

FBE_API float fbe_entity_fade(uint64_t entity, uint64_t camera) {
    const std::uint32_t slot = entity_slot(entity);
    const std::uint32_t eye = slot != none ? entity_slot(camera) : none;
    if (eye == none) {
        return -1.0F;
    }
    return fbe::scene::entity_fade(slot, eye);
}

FBE_API int32_t fbe_mesh_intersects(uint64_t mesh, uint64_t other) {
    const std::uint32_t first = entity_slot(mesh);
    const std::uint32_t second = first != none ? entity_slot(other) : none;
    if (second == none) {
        return -1;
    }
    const fbe::scene::entities_t& entities = scene().entities;
    if (entities.kind[first] != FBE_ENTITY_MESH || entities.kind[second] != FBE_ENTITY_MESH) {
        fbe::fail("both entities have to be meshes");
        return -1;
    }
    return fbe::scene::meshes_cross(first, second) ? 1 : 0;
}

} // extern "C"
