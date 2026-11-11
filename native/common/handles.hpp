#pragma once

// Handles, as every package's DLL gives them: a uint64_t of a 32-bit slot, a 24-bit generation
// above it, and an 8-bit kind at the top; 0 is none. Each pool has a kind of its own, so a handle
// of one kind given to a call for another is refused, never taken for another object; a freed
// slot's generation goes up, so an old handle is caught, never aliased; and a slot whose
// generation would wrap is retired. This header is the one list of kinds, each package in a range
// of its own (docs/architecture.md): fbengine 1 to 63, fbengine_audio 64 to 95, fbengine_physics
// 96 to 127, fbengine_video 128 to 143.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace fbe {

/// The kind of a pool's handles.
enum handle_kind_t : std::uint8_t {
    // fbengine, 1 to 63.
    WORLD_KIND = 1,
    ENTITY_KIND = 2,
    GEOMETRY_KIND = 3,
    MATERIAL_KIND = 4,
    TEXTURE_KIND = 5,
    ENVIRONMENT_KIND = 6,
    // E7's animation, apart from the kinds assets' loads and the GPU's objects take.
    CLIP_KIND = 20,
    SKIN_KIND = 21,
    PLAYER_KIND = 22,
    SHADER_KIND = 23,
    // fbengine_audio, 64 to 95.
    // fbengine_physics, 96 to 127.
    PHYSICS_WORLD_KIND = 96,
    PHYSICS_SHAPE_KIND = 97,
    PHYSICS_BODY_KIND = 98,
    // fbengine_video, 128 to 143.
};

/// The kind a handle says it is.
constexpr handle_kind_t kind_of(std::uint64_t handle) {
    return static_cast<handle_kind_t>(handle >> 56U);
}

/// The slots of one kind of handle, which a pool's data is indexed by.
class handle_slots_t {
public:
    /// A slot's generation is 24 bits; one freed this many times is retired.
    static constexpr std::uint32_t last_generation = (1U << 24U) - 1U;

    explicit handle_slots_t(handle_kind_t kind) : m_kind(kind) {}

    /// A free slot, or a new one; its handle is handle(slot). A new one has room made for it in
    /// the free list first, so freeing it never allocates: a release can't run out of memory.
    std::uint32_t make() {
        if (!m_free.empty()) {
            const std::uint32_t slot = m_free.back();
            m_free.pop_back();
            m_live[slot] = true;
            return slot;
        }
        m_free.reserve(m_generations.size() + 1);
        m_live.push_back(true);
        try {
            m_generations.push_back(1);
        } catch (...) {
            m_live.pop_back();
            throw;
        }
        return static_cast<std::uint32_t>(m_generations.size() - 1);
    }

    /// Frees a slot; its handles are caught from now on. It never allocates, as make made room.
    void free(std::uint32_t slot) {
        m_live[slot] = false;
        if (m_generations[slot] < last_generation) {
            ++m_generations[slot];
            m_free.push_back(slot);
        }
    }

    /// The live slot a handle is for, if it is one of this kind's and its slot's generation.
    [[nodiscard]] std::optional<std::uint32_t> find(std::uint64_t handle) const {
        const auto slot = static_cast<std::uint32_t>(handle);
        const auto generation = static_cast<std::uint32_t>(handle >> 32U) & last_generation;
        if (kind_of(handle) != m_kind || slot >= m_generations.size() || !m_live[slot] ||
            m_generations[slot] != generation) {
            return std::nullopt;
        }
        return slot;
    }

    [[nodiscard]] std::uint64_t handle(std::uint32_t slot) const {
        return (static_cast<std::uint64_t>(m_kind) << 56U) |
               (static_cast<std::uint64_t>(m_generations[slot]) << 32U) | slot;
    }

    [[nodiscard]] handle_kind_t kind() const {
        return m_kind;
    }

    /// How many slots there are, live or free, which the field arrays are sized to.
    [[nodiscard]] std::uint32_t size() const {
        return static_cast<std::uint32_t>(m_generations.size());
    }

    /// How many of them are live.
    [[nodiscard]] std::uint32_t live() const {
        return static_cast<std::uint32_t>(std::ranges::count(m_live, true));
    }

private:
    handle_kind_t m_kind;
    std::vector<std::uint32_t> m_generations;
    std::vector<bool> m_live;
    std::vector<std::uint32_t> m_free;
};

} // namespace fbe
