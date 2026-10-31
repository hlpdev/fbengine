#pragma once

// A decoder of a format of video, which a video's thread decodes it with: pl_mpeg's MPEG-1 now;
// another format joins as another decoder_t, which open_decoder tries in turn.

#include <cstdint>
#include <memory>
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

/// A video's whole sound: its frames a second, its channels, and its samples, interleaved.
struct sound_t {
    std::int32_t rate = 0;
    std::int32_t channels = 0;
    std::vector<float> samples;
};

/// A video's decoder: what it's like, and its pictures in order, one thread's at a time but for
/// sound(), which any thread calls as the other decodes.
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
    /// How long it is, in seconds.
    [[nodiscard]] virtual double duration() const = 0;
    [[nodiscard]] virtual bool has_sound() const = 0;
    [[nodiscard]] virtual std::int32_t sound_rate() const = 0;
    [[nodiscard]] virtual std::int32_t sound_channels() const = 0;

    /// Decodes the next picture into `picture`, whose pixels are its size; false at the end.
    virtual bool next(picture_t& picture) = 0;

    /// Starts again at `seconds`, decoding the picture due then into `picture` from the key frame
    /// before it, the next() ones after it; false if there's none.
    virtual bool seek(double seconds, picture_t& picture) = 0;

    /// Its whole sound, decoded from its start, which time 0 is the first sample of.
    [[nodiscard]] virtual sound_t sound() const = 0;
};

/// A decoder of `bytes`, which it keeps, of the first format that takes them, or nullptr.
std::unique_ptr<decoder_t> open_decoder(std::vector<std::uint8_t> bytes);

/// MPEG-1's, or nullptr if `bytes` aren't an MPEG-1 program stream with video.
std::unique_ptr<decoder_t> open_mpeg1(std::vector<std::uint8_t>& bytes);

} // namespace fbe::video
