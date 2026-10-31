#pragma once

// What the scene's files share: the handle tables, and the entities' data, kept by field in arrays
// indexed by slot, so what runs over many entities a frame reads only what it needs.

#include "scene/math.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fbe::scene {

/// No slot: no parent, no world.
constexpr std::uint32_t none = UINT32_MAX;

/// The slots of one kind of handle, each a 32-bit slot and a generation above it. A freed slot's
/// generation goes up, so its old handles are caught, never taken for the next one made there.
class slots_t {
public:
    /// A free slot, or a new one; its handle is handle(slot).
    std::uint32_t make();
    void free(std::uint32_t slot);
    /// The live slot a handle is for, if it is.
    [[nodiscard]] std::optional<std::uint32_t> find(std::uint64_t handle) const;
    [[nodiscard]] std::uint64_t handle(std::uint32_t slot) const;
    /// How many slots there are, live or free, which the field arrays are sized to.
    [[nodiscard]] std::uint32_t size() const;

private:
    std::vector<std::uint32_t> m_generations;
    std::vector<bool> m_live;
    std::vector<std::uint32_t> m_free;
};

/// A world: the entities at its top, and how many it has.
struct world_t {
    std::vector<std::uint32_t> roots;
    std::int32_t count = 0;
};

/// Every entity's data, a field to an array, indexed by slot.
struct entities_t {
    slots_t slots;
    std::vector<std::int32_t> kind;
    std::vector<std::uint32_t> world;
    std::vector<std::uint32_t> parent;
    std::vector<std::vector<std::uint32_t>> children;
    std::vector<std::string> name;
    std::vector<bool> hidden;

    // The transform in the parent's space, and in the world's, which is worked out again only once
    // something above it or it moved: a moved entity marks itself and what's under it, and stops at
    // one marked already, since what's under a marked entity is marked too.
    std::vector<vec3_t> position;
    std::vector<quat_t> rotation;
    std::vector<vec3_t> scale;
    std::vector<mat4_t> world_matrix;
    /// The parent's world rotation times the entity's own, and its scales multiplied.
    std::vector<quat_t> world_rotation;
    std::vector<vec3_t> world_scale;
    std::vector<bool> moved;
};

struct scene_t {
    slots_t world_slots;
    std::vector<world_t> worlds;
    std::uint32_t main_world = none;
    entities_t entities;
    /// The handles freed since fbe_entity_freed last took them.
    std::vector<std::uint64_t> freed;
};

/// The one scene, which every World is in.
scene_t& scene();

/// The live entity a handle is for, or none, with fbe_error saying so.
std::uint32_t entity_slot(std::uint64_t handle);
/// The live world a handle is for, or none, with fbe_error saying so.
std::uint32_t world_slot(std::uint64_t handle);

/// Gives an entity a transform of nothing: at its parent's origin, unturned, at scale 1.
void reset_transform(std::uint32_t entity);
/// Marks an entity, and what's under it, to have its world transform worked out again.
void mark_moved(std::uint32_t entity);
/// Works out an entity's world transform, and its parents', if they moved.
void update_world(std::uint32_t entity);
/// Sets an entity's local transform so its world one is this, under its parent as it is.
void set_world_position(std::uint32_t entity, vec3_t position);
void set_world_rotation(std::uint32_t entity, quat_t rotation);
void set_world_scale(std::uint32_t entity, vec3_t scale);

} // namespace fbe::scene
