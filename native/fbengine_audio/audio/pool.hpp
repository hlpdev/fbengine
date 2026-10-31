#pragma once

// The handles fbengine_audio's C API gives out, as handles.hpp lays them out: a pool's own kind,
// so a handle of another kind finds nothing, and a generation over its slot, so a handle whose
// object is gone finds nothing, even after its slot is taken again. 0 is none.

#include "audio/handles.hpp"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace fbe::audio {

/// Objects of one kind by handle, each where it was made until it's removed, since miniaudio
/// points at them.
template <typename Item>
class pool_t {
public:
    explicit pool_t(kind_t kind) : m_kind(kind) {}

    /// Keeps `item`, giving its handle.
    std::uint64_t add(std::unique_ptr<Item> item) {
        std::uint32_t slot = 0;
        if (m_free.empty()) {
            slot = static_cast<std::uint32_t>(m_slots.size());
            m_slots.emplace_back();
        } else {
            slot = m_free.back();
            m_free.pop_back();
        }
        m_slots[slot].item = std::move(item);
        ++m_count;
        return make_handle(m_kind, m_slots[slot].generation, slot);
    }

    /// The item `handle` is for, or null when it's gone, of another kind, or was never one.
    [[nodiscard]] Item* find(std::uint64_t handle) const {
        const std::uint32_t slot = handle_slot(handle);
        if (handle_kind(handle) != m_kind || slot >= m_slots.size() ||
            m_slots[slot].generation != handle_generation(handle)) {
            return nullptr;
        }
        return m_slots[slot].item.get();
    }

    /// Takes the item `handle` is for out, giving it to the caller to let go, or null when there's
    /// none; the handle finds nothing after.
    std::unique_ptr<Item> remove(std::uint64_t handle) {
        if (find(handle) == nullptr) {
            return nullptr;
        }
        return take(handle_slot(handle));
    }

    /// Removes each item `done` says is, in slot order.
    template <typename Done>
    void remove_if(Done done) {
        for (std::uint32_t slot = 0; slot < m_slots.size(); ++slot) {
            if (m_slots[slot].item != nullptr && done(*m_slots[slot].item)) {
                take(slot);
            }
        }
    }

    /// Runs `visit` on each item, in slot order.
    template <typename Visit>
    void each(Visit visit) {
        for (const slot_t& slot : m_slots) {
            if (slot.item != nullptr) {
                visit(*slot.item);
            }
        }
    }

    /// Removes every item, so no handle given out finds one.
    void clear() {
        remove_if([](const Item&) { return true; });
    }

    /// How many items it has.
    [[nodiscard]] std::uint32_t count() const {
        return m_count;
    }

    /// How many slots it has, used, free, and retired.
    [[nodiscard]] std::uint32_t slots() const {
        return static_cast<std::uint32_t>(m_slots.size());
    }

private:
    struct slot_t {
        std::unique_ptr<Item> item;
        // Never 0, so no handle is 0.
        std::uint32_t generation = 1;
    };

    std::unique_ptr<Item> take(std::uint32_t slot) {
        std::unique_ptr<Item> item = std::move(m_slots[slot].item);
        // A slot whose generation would wrap is never used again, so no old handle comes back; its
        // generation, past any a handle has, finds nothing.
        if (++m_slots[slot].generation <= most_generation) {
            m_free.push_back(slot);
        }
        --m_count;
        return item;
    }

    kind_t m_kind;
    std::vector<slot_t> m_slots;
    std::vector<std::uint32_t> m_free;
    std::uint32_t m_count = 0;
};

} // namespace fbe::audio
