// A WAV's header, which a decoder writes before the samples it decodes after it, so a video's
// sound is made once, in place.

#include "video/decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fbe::video {

void write_wav_header(std::span<std::uint8_t> wav, std::int32_t rate, std::int32_t channels) {
    std::size_t at = 0;
    const auto put = [&](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            wav[at++] = static_cast<std::uint8_t>(value >> (8U * static_cast<unsigned>(i)));
        }
    };
    const auto tag = [&](std::string_view text) {
        for (const char c : text) {
            wav[at++] = static_cast<std::uint8_t>(c);
        }
    };
    const auto data = static_cast<std::uint32_t>(wav.size() - wav_header_size);
    const auto count = static_cast<std::uint32_t>(channels);
    const auto per_second = static_cast<std::uint32_t>(rate);
    tag("RIFF");
    put(36 + data, 4);
    tag("WAVE");
    tag("fmt ");
    put(16, 4);
    // IEEE float.
    put(3, 2);
    put(count, 2);
    put(per_second, 4);
    put(per_second * count * 4, 4);
    put(count * 4, 2);
    put(32, 2);
    tag("data");
    put(data, 4);
}

} // namespace fbe::video
