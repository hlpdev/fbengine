#pragma once

// A sound, decoded whole, kept as its file to decode as it plays, or fed its frames as they come,
// which the sounds' pool and each channel playing it share, so it stays while any of them has it.

#include <cstdint>
#include <memory>
#include <miniaudio.h>
#include <vector>

namespace fbe::audio {

/// A fed sound's frames, which the main thread feeds and the one channel playing it reads, on the
/// mixing thread: miniaudio's ring buffer, which one thread writes and another reads without a
/// lock. miniaudio's init clears it first.
struct feed_t {
    ma_pcm_rb ring;
    bool has_ring = false;
    /// The frames it waits to have, each time it runs dry, before it plays again, so a feed that
    /// comes unevenly doesn't stutter.
    std::uint32_t buffered = 0;
    /// The main thread's: whether a channel plays it, as only one can.
    bool playing = false;
    /// The mixing thread's: whether it's waiting for `buffered` frames, as it does at first.
    bool waiting = true;

    feed_t() = default;
    feed_t(const feed_t&) = delete;
    feed_t& operator=(const feed_t&) = delete;
    feed_t(feed_t&&) = delete;
    feed_t& operator=(feed_t&&) = delete;

    ~feed_t() {
        if (has_ring) {
            ma_pcm_rb_uninit(&ring);
        }
    }
};

struct sound_data_t {
    /// Its frames, each its channels' samples, one after another, when it's decoded whole.
    std::vector<float> samples;
    /// Its file, when it's streamed.
    std::vector<std::uint8_t> encoded;
    /// Its frames as they come, when it's fed.
    std::shared_ptr<feed_t> feed;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t frames = 0;

    /// Whether each channel decodes it as it plays.
    [[nodiscard]] bool streamed() const {
        return !encoded.empty();
    }

    /// Whether it's fed its frames as they come.
    [[nodiscard]] bool fed() const {
        return feed != nullptr;
    }
};

/// What the handle fbe_sound_load gave is for, or null when it's gone.
std::shared_ptr<const sound_data_t> find_sound(std::uint64_t sound);

} // namespace fbe::audio
