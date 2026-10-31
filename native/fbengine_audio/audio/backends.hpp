#pragma once

// The backends each device of fbe_audio_start's is opened on.

#include <array>
#include <cstddef>
#include <miniaudio.h>
#include <vector>

namespace fbe::audio {

/// What the OS's default output is tried on: every backend miniaudio was built with but its null
/// one, which miniaudio would otherwise fall back to last, so a machine with no output fails to
/// start rather than playing to nothing.
inline std::vector<ma_backend> output_backends() {
    std::array<ma_backend, MA_BACKEND_COUNT> enabled{};
    std::size_t count = 0;
    if (ma_get_enabled_backends(enabled.data(), enabled.size(), &count) != MA_SUCCESS) {
        return {};
    }
    std::vector<ma_backend> outputs;
    for (std::size_t i = 0; i < count; ++i) {
        if (enabled.at(i) != ma_backend_null) {
            outputs.push_back(enabled.at(i));
        }
    }
    return outputs;
}

} // namespace fbe::audio
