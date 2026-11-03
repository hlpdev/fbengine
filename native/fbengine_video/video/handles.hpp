#pragma once

// The handles fbengine_video's C API gives for its objects, as docs/architecture.md has them: a
// 32-bit slot, above it a 24-bit generation, which goes up as the slot's freed, so a freed
// object's handle is never another's, and above that an 8-bit kind, so a handle of one kind is
// never taken for another. 0 is none. Its layout is native/common/handles.hpp's, which it gives
// way to once that's on master.

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace fbe::video {

/// The kinds of fbengine_video's handles, in its range of architecture.md's, 128 to 143.
enum handle_kind_t : std::uint8_t {
    VIDEO_KIND = 128,
};

/// The objects of one kind, each owned by its slot, by handle.
template <typename T>
class pool_t {
public:
    /// A pool of `kind`'s handles, whose slots are retired once freed `last_generation` times.
    explicit pool_t(handle_kind_t kind, std::uint32_t last_generation = (1U << 24U) - 1U)
        : m_kind(kind), m_last_generation(last_generation) {}

    /// Keeps `item`, giving its handle.
    std::uint64_t add(std::unique_ptr<T> item) {
        std::uint32_t slot = 0;
        if (m_free.empty()) {
            slot = static_cast<std::uint32_t>(m_slots.size());
            m_slots.push_back({ nullptr, 1 });
        } else {
            slot = m_free.back();
            m_free.pop_back();
        }
        m_slots[slot].item = std::move(item);
        ++m_count;
        return (static_cast<std::uint64_t>(m_kind) << 56U) |
               (static_cast<std::uint64_t>(m_slots[slot].generation) << 32U) | slot;
    }

    /// The object `handle` is, or nullptr for one that's freed, never was, or is of another kind.
    [[nodiscard]] T* get(std::uint64_t handle) const {
        const auto slot = static_cast<std::uint32_t>(handle);
        const auto generation = static_cast<std::uint32_t>(handle >> 32U) & 0xFFFFFFU;
        const auto kind = static_cast<std::uint8_t>(handle >> 56U);
        if (kind != m_kind || slot >= m_slots.size() || m_slots[slot].generation != generation) {
            return nullptr;
        }
        return m_slots[slot].item.get();
    }

    /// Frees the object `handle` is; false if there's none.
    bool remove(std::uint64_t handle) {
        if (get(handle) == nullptr) {
            return false;
        }
        const auto slot = static_cast<std::uint32_t>(handle);
        m_slots[slot].item.reset();
        --m_count;
        // A slot of the last generation is retired, so no handle comes round again.
        if (m_slots[slot].generation == m_last_generation) {
            m_slots[slot].generation = 0;
            return true;
        }
        ++m_slots[slot].generation;
        m_free.push_back(slot);
        return true;
    }

    /// How many objects there are.
    [[nodiscard]] std::size_t count() const {
        return m_count;
    }

private:
    struct slot_t {
        std::unique_ptr<T> item;
        // 0 for a retired slot.
        std::uint32_t generation;
    };

    handle_kind_t m_kind;
    std::uint32_t m_last_generation;
    std::vector<slot_t> m_slots;
    std::vector<std::uint32_t> m_free;
    std::size_t m_count = 0;
};

} // namespace fbe::video
