#pragma once

// The handles fbengine_physics' C API gives for its objects: a 32-bit slot and a 32-bit generation,
// which goes up as the slot's freed, so a freed object's handle is never another's. 0 is none.

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace fbe::physics {

/// The objects of one kind, each owned by its slot, by handle.
template <typename T>
class pool_t {
public:
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
        return (static_cast<std::uint64_t>(m_slots[slot].generation) << 32U) | slot;
    }

    /// The object `handle` is, or nullptr for one that's freed or never was.
    [[nodiscard]] T* get(std::uint64_t handle) const {
        const auto slot = static_cast<std::uint32_t>(handle);
        const auto generation = static_cast<std::uint32_t>(handle >> 32U);
        if (slot >= m_slots.size() || m_slots[slot].generation != generation) {
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
        // 0 is no generation, so no handle is ever 0.
        if (++m_slots[slot].generation == 0) {
            m_slots[slot].generation = 1;
        }
        m_free.push_back(slot);
        --m_count;
        return true;
    }

    /// How many objects there are.
    [[nodiscard]] std::size_t count() const {
        return m_count;
    }

private:
    struct slot_t {
        std::unique_ptr<T> item;
        std::uint32_t generation;
    };

    std::vector<slot_t> m_slots;
    std::vector<std::uint32_t> m_free;
    std::size_t m_count = 0;
};

} // namespace fbe::physics
