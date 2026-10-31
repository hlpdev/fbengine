#pragma once

// A decoder of a format of video, which a video's thread decodes it with: pl_mpeg's MPEG-1 now;
// another format joins as another decoder_t, which open_decoder tries in turn.

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace fbe::video {

/// A picture decoded: when it's due, from the video's start, its number, from 0, and its pixels,
/// 4 bytes each, red, green, blue, and alpha, in rows from the top.
struct picture_t {
    double time = 0.0;
    std::int64_t number = 0;
    std::vector<std::uint8_t> pixels;
    // How far into the video's loops it is, which time counts from: 0 in its first.
    double loop_start = 0.0;
};

/// A video's whole sound: its frames a second, its channels, and its samples, interleaved, the
/// first at the video's time 0.
struct sound_t {
    std::int32_t rate = 0;
    std::int32_t channels = 0;
    std::vector<float> samples;
};

/// The most bytes of samples a WAV holds, less its header.
constexpr std::uint64_t most_sound_bytes = std::numeric_limits<std::uint32_t>::max() - 44;

/// Whether `bytes` of samples fit a WAV, whose sizes are 32 bits.
constexpr bool wav_holds(std::uint64_t bytes) {
    return bytes <= most_sound_bytes;
}

/// A video's decoder: what it's like, and its pictures in order, one thread's at a time but for
/// what's const, which any thread calls as another decodes.
class decoder_t {
public:
    decoder_t() = default;
    virtual ~decoder_t() = default;
    decoder_t(const decoder_t&) = delete;
    decoder_t& operator=(const decoder_t&) = delete;
    decoder_t(decoder_t&&) = delete;
    decoder_t& operator=(decoder_t&&) = delete;

    [[nodiscard]] virtual std::int32_t width() const = 0;
    [[nodiscard]] virtual std::int32_t height() const = 0;
    [[nodiscard]] virtual double frame_rate() const = 0;
    /// How many pictures it has.
    [[nodiscard]] virtual std::int64_t frames() const = 0;
    /// How long it is, in seconds.
    [[nodiscard]] virtual double duration() const = 0;

    /// The number of the first picture after `number` that a seek starts decoding at, past which
    /// a seek's quicker than decoding on; the most an int64_t holds if there's none.
    [[nodiscard]] virtual std::int64_t next_start(std::int64_t number) const = 0;

    /// Decodes the next picture into `picture`, whose pixels are its size; false at the end.
    virtual bool next(picture_t& picture) = 0;

    /// Starts again at `seconds`, decoding the picture due then into `picture` from the key frame
    /// before it, or the last there is before it, the next() ones after it; false if there's none.
    virtual bool seek(double seconds, picture_t& picture) = 0;

    /// Its sound, decoded as it was opened, which it gives up; none, rate 0, if it has none.
    [[nodiscard]] virtual sound_t take_sound() = 0;
};

/// A decoder of `bytes`, of the first format that takes them, or nullptr; `too_long` says if it's
/// because its sound is longer than a WAV holds.
std::unique_ptr<decoder_t> open_decoder(std::span<const std::uint8_t> bytes, bool& too_long);

/// MPEG-1's, or nullptr if `bytes` aren't an MPEG-1 program stream with video, or its sound is
/// too long, as `too_long` says.
std::unique_ptr<decoder_t> open_mpeg1(std::span<const std::uint8_t> bytes, bool& too_long);

} // namespace fbe::video
