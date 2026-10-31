#pragma once

// What the scene's files share: the handle tables, and the entities' data, kept by field in arrays
// indexed by slot, so what runs over many entities a frame reads only what it needs.

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

} // namespace fbe::scene
