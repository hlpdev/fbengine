// Entities' transforms: position, rotation, and scale in the parent's space or the world's, moves
// and turns, pointing, transforming between entities, and the batches that sync many at once.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <cmath>
#include <cstdint>
#include <format>
#include <ranges>
#include <vector>

namespace fbe::scene {

namespace {

/// Shorter vectors have no direction to point or align to.
constexpr float shortest = 1e-6F;

bool valid_space(std::int32_t space) {
    if (space != FBE_SPACE_LOCAL && space != FBE_SPACE_WORLD) {
        return fail(std::format("{} isn't a Space", space));
    }
    return true;
}

/// The live entity and a valid space, or none.
std::uint32_t entity_in(std::uint64_t entity, std::int32_t space) {
    return valid_space(space) ? entity_slot(entity) : none;
}

/// The world rotation of an entity's parent, or none at the top.
quat_t parent_rotation(std::uint32_t entity) {
    const std::uint32_t parent = scene().entities.parent[entity];
    if (parent == none) {
        return {};
    }
    update_world(parent);
    return scene().entities.world_rotation[parent];
}

vec3_t parent_scale(std::uint32_t entity) {
    const std::uint32_t parent = scene().entities.parent[entity];
    if (parent == none) {
        return { 1.0F, 1.0F, 1.0F };
    }
    update_world(parent);
    return scene().entities.world_scale[parent];
}

vec3_t to_vec(const fbe_vec3_t& v) {
    return { v.x, v.y, v.z };
}

fbe_vec3_t from_vec(vec3_t v) {
    return { .x = v.x, .y = v.y, .z = v.z };
}

fbe_quat_t from_quat(quat_t q) {
    return { .x = q.x, .y = q.y, .z = q.z, .w = q.w };
}

/// `a` over `b` per component, keeping `kept`'s where `b` is 0, which has no inverse.
vec3_t divide_or_keep(vec3_t a, vec3_t b, vec3_t kept) {
    return { b.x != 0.0F ? a.x / b.x : kept.x, b.y != 0.0F ? a.y / b.y : kept.y,
             b.z != 0.0F ? a.z / b.z : kept.z };
}

/// The rotation whose forward, -Z, points along `direction`, as near upright as it goes, then
/// rolled about it; straight up or down, it's unturned about Y.
quat_t facing(vec3_t direction, float roll) {
    const bool level = direction.x != 0.0F || direction.z != 0.0F;
    const float yaw = level ? std::atan2(-direction.x, -direction.z) : 0.0F;
    const float pitch = std::atan2(
        direction.y, std::sqrt((direction.x * direction.x) + (direction.z * direction.z)));
    return euler(pitch, yaw, roll);
}

/// What a C call gives for an entity's position, rotation, or scale in a space.
vec3_t position_in(std::uint32_t entity, std::int32_t space) {
    if (space == FBE_SPACE_LOCAL) {
        return scene().entities.position[entity];
    }
    update_world(entity);
    return scene().entities.world_matrix[entity].axis(3);
}

quat_t rotation_in(std::uint32_t entity, std::int32_t space) {
    if (space == FBE_SPACE_LOCAL) {
        return scene().entities.rotation[entity];
    }
    update_world(entity);
    return scene().entities.world_rotation[entity];
}

void set_position_in(std::uint32_t entity, vec3_t position, std::int32_t space) {
    if (space == FBE_SPACE_LOCAL) {
        scene().entities.position[entity] = position;
        mark_moved(entity);
    } else {
        set_world_position(entity, position);
    }
}

void set_rotation_in(std::uint32_t entity, quat_t rotation, std::int32_t space) {
    if (space == FBE_SPACE_LOCAL) {
        scene().entities.rotation[entity] = normalize(rotation);
        mark_moved(entity);
    } else {
        set_world_rotation(entity, rotation);
    }
}

/// The matrix from an entity's space to the world's, identity for none.
mat4_t to_world(std::uint32_t entity) {
    if (entity == none) {
        return {};
    }
    update_world(entity);
    return scene().entities.world_matrix[entity];
}

} // namespace

tweened_t::tweened_t(float tween) : m_tween(tween), m_stamp(++scene().posings) {}

pose_t tweened_t::pose(std::uint32_t entity) const {
    scene_t& s = scene();
    const entities_t& entities = s.entities;
    if (m_tween >= 1.0F) {
        update_world(entity);
        return {
            .matrix = entities.world_matrix[entity],
            .rotation = entities.world_rotation[entity],
            .scale = entities.world_scale[entity],
        };
    }
    if (s.poses.size() < entities.kind.size()) {
        s.poses.resize(entities.kind.size());
        s.posed.resize(entities.kind.size(), 0);
    }
    // Up to the first already worked out, then down again, each from its parent, its local
    // transform part way from its capture, as update_world does it; the chain kept for the next.
    thread_local std::vector<std::uint32_t> chain;
    chain.clear();
    for (std::uint32_t at = entity; at != none && s.posed[at] != m_stamp;
         at = entities.parent[at]) {
        chain.push_back(at);
    }
    for (const std::uint32_t at : std::views::reverse(chain)) {
        vec3_t position = entities.position[at];
        quat_t rotation = entities.rotation[at];
        vec3_t scale = entities.scale[at];
        if (entities.captured[at]) {
            const vec3_t moved = position - entities.captured_position[at];
            const vec3_t grown = scale - entities.captured_scale[at];
            position = entities.captured_position[at] + moved * m_tween;
            rotation = slerp(entities.captured_rotation[at], rotation, m_tween);
            scale = entities.captured_scale[at] + grown * m_tween;
        }
        const mat4_t local = compose(position, rotation, scale);
        const std::uint32_t parent = entities.parent[at];
        pose_t& found = s.poses[at];
        if (parent == none) {
            found = { .matrix = local, .rotation = rotation, .scale = scale };
        } else {
            const pose_t& above = s.poses[parent];
            found = {
                .matrix = above.matrix * local,
                .rotation = normalize(above.rotation * reflect(rotation, above.scale)),
                .scale = above.scale * scale,
            };
        }
        s.posed[at] = m_stamp;
    }
    return s.poses[entity];
}

mat4_t tweened_matrix(std::uint32_t entity, float tween) {
    return tweened_t(tween).pose(entity).matrix;
}

void reset_transform(std::uint32_t entity) {
    entities_t& entities = scene().entities;
    entities.position[entity] = {};
    entities.rotation[entity] = {};
    entities.scale[entity] = { 1.0F, 1.0F, 1.0F };
    entities.moved[entity] = true;
    pickable_moved(entity);
}

void mark_moved(std::uint32_t entity) {
    entities_t& entities = scene().entities;
    if (entities.moved[entity]) {
        return;
    }
    // By a stack of its own, so no tree is too deep.
    std::vector<std::uint32_t> stack{ entity };
    while (!stack.empty()) {
        const std::uint32_t at = stack.back();
        stack.pop_back();
        entities.moved[at] = true;
        pickable_moved(at);
        for (const std::uint32_t child : entities.children[at]) {
            if (!entities.moved[child]) {
                stack.push_back(child);
            }
        }
    }
}

void update_world(std::uint32_t entity) {
    entities_t& entities = scene().entities;
    if (!entities.moved[entity]) {
        return;
    }
    // Up to the topmost that moved, then down again, each from its parent.
    std::vector<std::uint32_t> chain{ entity };
    for (std::uint32_t parent = entities.parent[entity]; parent != none && entities.moved[parent];
         parent = entities.parent[parent]) {
        chain.push_back(parent);
    }
    for (const std::uint32_t moved : std::views::reverse(chain)) {
        const mat4_t local =
            compose(entities.position[moved], entities.rotation[moved], entities.scale[moved]);
        const std::uint32_t parent = entities.parent[moved];
        if (parent == none) {
            entities.world_matrix[moved] = local;
            entities.world_rotation[moved] = entities.rotation[moved];
            entities.world_scale[moved] = entities.scale[moved];
        } else {
            // Under a mirror, the child's rotation is what the mirror makes of it; the mirror
            // itself stays in the world scale's signs.
            const vec3_t parent_scale = entities.world_scale[parent];
            entities.world_matrix[moved] = entities.world_matrix[parent] * local;
            entities.world_rotation[moved] = normalize(
                entities.world_rotation[parent] * reflect(entities.rotation[moved], parent_scale));
            entities.world_scale[moved] = parent_scale * entities.scale[moved];
        }
        entities.moved[moved] = false;
    }
}

void set_world_position(std::uint32_t entity, vec3_t position) {
    entities_t& entities = scene().entities;
    const std::uint32_t parent = entities.parent[entity];
    if (parent == none) {
        entities.position[entity] = position;
    } else {
        update_world(parent);
        mat4_t into;
        if (inverse_affine(entities.world_matrix[parent], into)) {
            entities.position[entity] = transform_point(into, position);
        } else {
            // On an axis a parent scales to nothing, there's nowhere to put the child, so it
            // keeps its own place there.
            const vec3_t offset = position - entities.world_matrix[parent].axis(3);
            entities.position[entity] =
                divide_or_keep(inverse(entities.world_rotation[parent]) * offset,
                               entities.world_scale[parent], entities.position[entity]);
        }
    }
    mark_moved(entity);
}

void set_world_rotation(std::uint32_t entity, quat_t rotation) {
    scene().entities.rotation[entity] =
        normalize(reflect(inverse(parent_rotation(entity)) * rotation, parent_scale(entity)));
    mark_moved(entity);
}

void set_world_scale(std::uint32_t entity, vec3_t scale) {
    entities_t& entities = scene().entities;
    entities.scale[entity] = divide_or_keep(scale, parent_scale(entity), entities.scale[entity]);
    mark_moved(entity);
}

} // namespace fbe::scene

using fbe::scene::entity_in;
using fbe::scene::entity_slot;
using fbe::scene::none;
using fbe::scene::quat_t;
using fbe::scene::scene;
using fbe::scene::vec3_t;

extern "C" {

FBE_API bool fbe_entity_set_position(uint64_t entity, float x, float y, float z, int32_t space) {
    const std::uint32_t slot = entity_in(entity, space);
    if (slot == none) {
        return false;
    }
    fbe::scene::set_position_in(slot, { x, y, z }, space);
    return true;
}

FBE_API bool fbe_entity_position(uint64_t entity, int32_t space, fbe_vec3_t* position) {
    const std::uint32_t slot = entity_in(entity, space);
    if (slot == none) {
        return false;
    }
    *position = fbe::scene::from_vec(fbe::scene::position_in(slot, space));
    return true;
}

FBE_API bool fbe_entity_set_rotation(uint64_t entity, float x, float y, float z, float w,
                                     int32_t space) {
    const std::uint32_t slot = entity_in(entity, space);
    if (slot == none) {
        return false;
    }
    fbe::scene::set_rotation_in(slot, { x, y, z, w }, space);
    return true;
}

FBE_API bool fbe_entity_rotation(uint64_t entity, int32_t space, fbe_quat_t* rotation) {
    const std::uint32_t slot = entity_in(entity, space);
    if (slot == none) {
        return false;
    }
    *rotation = fbe::scene::from_quat(fbe::scene::rotation_in(slot, space));
    return true;
}

FBE_API bool fbe_entity_set_scale(uint64_t entity, float x, float y, float z, int32_t space) {
    const std::uint32_t slot = entity_in(entity, space);
    if (slot == none) {
        return false;
    }
    if (space == FBE_SPACE_LOCAL) {
        scene().entities.scale[slot] = { x, y, z };
        fbe::scene::mark_moved(slot);
    } else {
        fbe::scene::set_world_scale(slot, { x, y, z });
    }
    return true;
}

FBE_API bool fbe_entity_scale(uint64_t entity, int32_t space, fbe_vec3_t* scale) {
    const std::uint32_t slot = entity_in(entity, space);
    if (slot == none) {
        return false;
    }
    if (space == FBE_SPACE_LOCAL) {
        *scale = fbe::scene::from_vec(scene().entities.scale[slot]);
    } else {
        fbe::scene::update_world(slot);
        *scale = fbe::scene::from_vec(scene().entities.world_scale[slot]);
    }
    return true;
}

FBE_API bool fbe_entity_move(uint64_t entity, float x, float y, float z) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    fbe::scene::entities_t& entities = scene().entities;
    entities.position[slot] = entities.position[slot] + entities.rotation[slot] * vec3_t(x, y, z);
    fbe::scene::mark_moved(slot);
    return true;
}

FBE_API bool fbe_entity_translate(uint64_t entity, float x, float y, float z, int32_t space) {
    const std::uint32_t slot = entity_in(entity, space);
    if (slot == none) {
        return false;
    }
    if (space == FBE_SPACE_LOCAL) {
        fbe::scene::entities_t& entities = scene().entities;
        entities.position[slot] = entities.position[slot] + vec3_t{ x, y, z };
        fbe::scene::mark_moved(slot);
    } else {
        fbe::scene::update_world(slot);
        const vec3_t at = scene().entities.world_matrix[slot].axis(3);
        fbe::scene::set_world_position(slot, at + vec3_t{ x, y, z });
    }
    return true;
}

FBE_API bool fbe_entity_turn(uint64_t entity, float x, float y, float z, float w, int32_t space) {
    const std::uint32_t slot = entity_in(entity, space);
    if (slot == none) {
        return false;
    }
    const quat_t turn = fbe::scene::normalize(quat_t{ x, y, z, w });
    if (space == FBE_SPACE_LOCAL) {
        fbe::scene::set_rotation_in(slot, scene().entities.rotation[slot] * turn, space);
    } else {
        fbe::scene::set_world_rotation(slot, turn * fbe::scene::rotation_in(slot, FBE_SPACE_WORLD));
    }
    return true;
}

FBE_API bool fbe_entity_point_at(uint64_t entity, float x, float y, float z, float roll) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    const vec3_t direction = vec3_t{ x, y, z } - fbe::scene::position_in(slot, FBE_SPACE_WORLD);
    if (fbe::scene::length(direction) >= fbe::scene::shortest) {
        fbe::scene::set_world_rotation(slot, fbe::scene::facing(direction, roll));
    }
    return true;
}

FBE_API bool fbe_entity_align(uint64_t entity, float x, float y, float z, int32_t axis,
                              float rate) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    vec3_t own;
    switch (axis) {
    case FBE_AXIS_RIGHT:
        own = { 1.0F, 0.0F, 0.0F };
        break;
    case FBE_AXIS_UP:
        own = { 0.0F, 1.0F, 0.0F };
        break;
    case FBE_AXIS_FORWARD:
        own = { 0.0F, 0.0F, -1.0F };
        break;
    default:
        return fbe::fail(std::format("{} isn't an Axis", axis));
    }
    const vec3_t direction{ x, y, z };
    if (fbe::scene::length(direction) < fbe::scene::shortest) {
        return true;
    }
    const quat_t rotation = fbe::scene::rotation_in(slot, FBE_SPACE_WORLD);
    const quat_t whole = fbe::scene::from_to(rotation * own, direction);
    const quat_t part = fbe::scene::slerp(quat_t(), whole, std::fmin(std::fmax(rate, 0.0F), 1.0F));
    fbe::scene::set_world_rotation(slot, part * rotation);
    return true;
}

FBE_API bool fbe_world_capture(uint64_t world) {
    const std::uint32_t found = fbe::scene::world_slot(world);
    if (found == none) {
        return false;
    }
    fbe::scene::entities_t& entities = scene().entities;
    for (std::uint32_t entity = 0; entity < entities.slots.size(); ++entity) {
        if (entities.world[entity] != found ||
            !entities.slots.find(entities.slots.handle(entity))) {
            continue;
        }
        entities.captured_position[entity] = entities.position[entity];
        entities.captured_rotation[entity] = entities.rotation[entity];
        entities.captured_scale[entity] = entities.scale[entity];
        entities.captured[entity] = true;
    }
    return true;
}

FBE_API bool fbe_entity_tweened_matrix(uint64_t entity, float tween, fbe_mat4_t* matrix) {
    const std::uint32_t slot = entity_slot(entity);
    if (slot == none) {
        return false;
    }
    if (std::isnan(tween) || tween < 0.0F || tween > 1.0F) {
        return fbe::fail(std::format("a tween is 0 to 1, not {}", tween));
    }
    const fbe::scene::mat4_t found = fbe::scene::tweened_matrix(slot, tween);
    for (std::size_t i = 0; i < found.m.size(); ++i) {
        matrix->m[i] = found.m[i];
    }
    return true;
}

FBE_API bool fbe_entity_matrix(uint64_t entity, int32_t space, fbe_mat4_t* matrix) {
    const std::uint32_t slot = entity_in(entity, space);
    if (slot == none) {
        return false;
    }
    fbe::scene::mat4_t found;
    if (space == FBE_SPACE_LOCAL) {
        const fbe::scene::entities_t& entities = scene().entities;
        found = fbe::scene::compose(entities.position[slot], entities.rotation[slot],
                                    entities.scale[slot]);
    } else {
        fbe::scene::update_world(slot);
        found = scene().entities.world_matrix[slot];
    }
    for (std::size_t i = 0; i < found.m.size(); ++i) {
        matrix->m[i] = found.m[i];
    }
    return true;
}

FBE_API bool fbe_entity_transform(uint64_t from, uint64_t to, int32_t kind, float x, float y,
                                  float z, fbe_vec3_t* result) {
    const std::uint32_t source = from != 0 ? entity_slot(from) : none;
    const std::uint32_t target = to != 0 ? entity_slot(to) : none;
    if ((from != 0 && source == none) || (to != 0 && target == none)) {
        return false;
    }
    fbe::scene::mat4_t into = {};
    if (!fbe::scene::inverse_affine(fbe::scene::to_world(target), into)) {
        return fbe::fail("the entity to transform into is scaled to nothing on an axis");
    }
    const fbe::scene::mat4_t through = into * fbe::scene::to_world(source);
    const vec3_t v{ x, y, z };
    switch (kind) {
    case FBE_TRANSFORM_POINT:
        *result = fbe::scene::from_vec(fbe::scene::transform_point(through, v));
        return true;
    case FBE_TRANSFORM_VECTOR:
        *result = fbe::scene::from_vec(fbe::scene::transform_vector(through, v));
        return true;
    case FBE_TRANSFORM_NORMAL:
        *result = fbe::scene::from_vec(fbe::scene::transform_normal(through, v));
        return true;
    default:
        return fbe::fail(std::format("{} isn't a kind of transform", kind));
    }
}

FBE_API bool fbe_entity_set_poses(const uint64_t* entities, const fbe_pose_t* poses, int32_t count,
                                  int32_t space) {
    if (!fbe::scene::valid_space(space)) {
        return false;
    }
    bool all = true;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uint32_t slot = entity_slot(entities[i]);
        if (slot == none) {
            all = false;
            continue;
        }
        const fbe_pose_t& pose = poses[i];
        fbe::scene::set_rotation_in(
            slot, { pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w }, space);
        fbe::scene::set_position_in(slot, fbe::scene::to_vec(pose.position), space);
    }
    return all || fbe::fail("an entity in the batch was freed; the rest were set");
}

FBE_API bool fbe_entity_poses(const uint64_t* entities, fbe_pose_t* poses, int32_t count,
                              int32_t space) {
    if (!fbe::scene::valid_space(space)) {
        return false;
    }
    bool all = true;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uint32_t slot = entity_slot(entities[i]);
        if (slot == none) {
            all = false;
            poses[i] = {};
            continue;
        }
        poses[i].position = fbe::scene::from_vec(fbe::scene::position_in(slot, space));
        poses[i].rotation = fbe::scene::from_quat(fbe::scene::rotation_in(slot, space));
    }
    return all || fbe::fail("an entity in the batch was freed; its pose is all zeros");
}

} // extern "C"
