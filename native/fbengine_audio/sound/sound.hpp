#pragma once

// A decoded sound, which the sounds' pool and each channel playing it share, so it stays while
// any of them has it.

#include <cstdint>
#include <memory>
#include <vector>

namespace fbe::audio {

struct sound_data_t {
    /// Its frames, each its channels' samples, one after another.
    std::vector<float> samples;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t frames = 0;
};

/// What the handle fbe_sound_load gave is for, or null when it's gone.
std::shared_ptr<const sound_data_t> find_sound(std::uint64_t sound);

} // namespace fbe::audio
