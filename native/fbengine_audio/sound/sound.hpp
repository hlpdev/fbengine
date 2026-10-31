#pragma once

// A sound, decoded whole or kept as its file to decode as it plays, which the sounds' pool and each
// channel playing it share, so it stays while any of them has it.

#include <cstdint>
#include <memory>
#include <vector>

namespace fbe::audio {

struct sound_data_t {
    /// Its frames, each its channels' samples, one after another, when it's decoded whole.
    std::vector<float> samples;
    /// Its file, when it's streamed.
    std::vector<std::uint8_t> encoded;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t frames = 0;

    /// Whether each channel decodes it as it plays.
    [[nodiscard]] bool streamed() const {
        return !encoded.empty();
    }
};

/// What the handle fbe_sound_load gave is for, or null when it's gone.
std::shared_ptr<const sound_data_t> find_sound(std::uint64_t sound);

} // namespace fbe::audio
