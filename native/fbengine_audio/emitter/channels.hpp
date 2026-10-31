#pragma once

// An emitter's channels, by handle: those that have finished go whenever a change goes over them,
// and as more are added, so an emitter that never moves doesn't keep every channel it played.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbe::audio {

class channel_list_t {
public:
    /// Adds `channel`, first letting go of those `playing` says have finished, once the list has
    /// doubled since it last did, so adding stays cheap however many are playing.
    template <typename Playing>
    void add(std::uint64_t channel, Playing playing) {
        if (m_channels.size() >= m_prune_at) {
            each(playing);
            m_prune_at = std::max(least_prune, m_channels.size() * 2);
        }
        m_channels.push_back(channel);
    }

    /// Runs `change` on each channel, which says whether it's still playing; those that aren't go.
    template <typename Change>
    void each(Change change) {
        std::erase_if(m_channels, [&](std::uint64_t channel) { return !change(channel); });
    }

    /// Makes room for one more, so the next add can't run out of memory.
    void reserve() {
        m_channels.reserve(m_channels.size() + 1);
    }

    /// How many handles it keeps, finished ones it hasn't let go of yet too.
    [[nodiscard]] std::size_t size() const {
        return m_channels.size();
    }

private:
    static constexpr std::size_t least_prune = 16;

    std::vector<std::uint64_t> m_channels;
    std::size_t m_prune_at = least_prune;
};

} // namespace fbe::audio
