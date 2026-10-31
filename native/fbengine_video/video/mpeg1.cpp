// MPEG-1, by pl_mpeg: a program stream of MPEG-1 video and MPEG-1 audio layer II, decoded from
// memory. The video's thread has a decoder of its own; the sound is decoded by another, over the
// same bytes, which neither changes.
//
// pl_mpeg's own seek can miss in a short video, and shows an open group of pictures' first
// B-frames, which are made from the group before's pictures, made from nothing, and its times
// for them a frame or two out. So its video decoder reads the video's elementary stream, which
// is taken out of the program stream on opening, its groups of pictures found: a seek starts
// it again at a group's start and numbers its pictures from there.

#include "video/decoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
// pl_mpeg's header takes stdio.h's FILE.
#include <cstdio>
#include <memory>
#include <pl_mpeg.h>
#include <span>
#include <utility>
#include <vector>

namespace fbe::video {

namespace {

// pl_mpeg's sound is two channels, interleaved.
constexpr std::int32_t sound_channels_of_mpeg1 = 2;

// Start codes, 00 00 01 and a byte, which MPEG-1 video has nowhere else.
constexpr std::uint32_t picture_start_code = 0x00000100U;
constexpr std::uint32_t group_start_code = 0x000001B8U;

// How much of the elementary stream pl_mpeg's video decoder is given at a time.
constexpr std::size_t chunk_size = std::size_t{ 64 } * 1024;

struct plm_deleter_t {
    void operator()(plm_t* plm) const {
        plm_destroy(plm);
    }
};

struct demux_deleter_t {
    void operator()(plm_demux_t* demux) const {
        plm_demux_destroy(demux);
    }
};

struct video_deleter_t {
    void operator()(plm_video_t* video) const {
        plm_video_destroy(video);
    }
};

using plm_ptr_t = std::unique_ptr<plm_t, plm_deleter_t>;
using demux_ptr_t = std::unique_ptr<plm_demux_t, demux_deleter_t>;
using video_ptr_t = std::unique_ptr<plm_video_t, video_deleter_t>;

// A group of pictures in a video's elementary stream, which its decoder can start at.
struct group_t {
    // Where its start code is.
    std::size_t offset = 0;
    // The number of its first picture shown.
    std::int64_t first = 0;
    // How many of its pictures are shown before its key frame, which is decoded first.
    std::int64_t leading = 0;
    // Whether those are made from its own pictures only, not the group before's.
    bool closed = true;
};

// A video's elementary stream: its pictures and their groups.
struct stream_t {
    std::vector<std::uint8_t> bytes;
    std::vector<group_t> groups;
    std::int64_t pictures = 0;
};

// A decoder of `bytes`, which it reads but never frees, with video or sound.
plm_ptr_t decoder_of(std::vector<std::uint8_t>& bytes, bool video) {
    plm_ptr_t plm(plm_create_with_memory(bytes.data(), bytes.size(), 0));
    if (plm != nullptr) {
        plm_set_video_enabled(plm.get(), video ? 1 : 0);
        plm_set_audio_enabled(plm.get(), video ? 0 : 1);
    }
    return plm;
}

// The video `bytes`' elementary stream, its video packets' data, with its pictures counted and
// its groups found, without decoding them.
stream_t stream_of(std::vector<std::uint8_t>& bytes) {
    stream_t stream;
    // The demuxer frees the buffer.
    const demux_ptr_t demux(
        plm_demux_create(plm_buffer_create_with_memory(bytes.data(), bytes.size(), 0), 1));
    for (const plm_packet_t* packet = plm_demux_decode(demux.get()); packet != nullptr;
         packet = plm_demux_decode(demux.get())) {
        if (packet->type == PLM_DEMUX_PACKET_VIDEO_1) {
            const std::span data(packet->data, packet->length);
            stream.bytes.insert(stream.bytes.end(), data.begin(), data.end());
        }
    }
    const std::span video(stream.bytes);
    bool first_of_group = false;
    std::uint32_t last = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < video.size(); ++i) {
        last = (last << 8U) | video[i];
        // The bits after a start code, if there are 4 bytes of them.
        const auto after = [&](std::size_t bits) {
            std::uint32_t word = 0;
            for (std::size_t j = 1; j <= 4; ++j) {
                word = (word << 8U) | (i + j < video.size() ? video[i + j] : 0U);
            }
            return word >> (32U - bits);
        };
        if (last == group_start_code) {
            // A 25-bit time code, then whether it's closed.
            stream.groups.push_back({
                .offset = i - 3,
                .first = stream.pictures,
                .leading = 0,
                .closed = (after(26) & 1U) != 0,
            });
            first_of_group = true;
        } else if (last == picture_start_code) {
            // Its key frame's 10-bit temporal reference is how many are shown before it.
            if (first_of_group) {
                stream.groups.back().leading = after(10);
                first_of_group = false;
            }
            ++stream.pictures;
        }
    }
    // Pictures before a first group, if a stream has them, are decoded from its start.
    if (stream.groups.empty() || stream.groups.front().first != 0) {
        stream.groups.insert(stream.groups.begin(), group_t{});
    }
    return stream;
}

class mpeg1_t final : public decoder_t {
public:
    mpeg1_t(std::vector<std::uint8_t> bytes, stream_t stream, bool has_sound,
            std::int32_t sound_rate)
        : m_bytes(std::move(bytes)), m_stream(std::move(stream)), m_has_sound(has_sound),
          m_sound_rate(sound_rate) {
        // The decoder reads its sequence header now, from the start of the stream.
        plm_buffer_t* buffer = plm_buffer_create_with_capacity(chunk_size);
        plm_buffer_set_load_callback(buffer, &mpeg1_t::load, this);
        m_video.reset(plm_video_create_with_buffer(buffer, 1));
        if (plm_video_has_header(m_video.get()) != 0) {
            m_width = plm_video_get_width(m_video.get());
            m_height = plm_video_get_height(m_video.get());
            m_frame_rate = plm_video_get_framerate(m_video.get());
        }
    }

    mpeg1_t(const mpeg1_t&) = delete;
    mpeg1_t& operator=(const mpeg1_t&) = delete;
    mpeg1_t(mpeg1_t&&) = delete;
    mpeg1_t& operator=(mpeg1_t&&) = delete;
    ~mpeg1_t() override = default;

    // Whether its decoder found the video's size and frame rate.
    [[nodiscard]] bool decodes() const {
        return m_width > 0 && m_height > 0 && m_frame_rate > 0.0 && m_stream.pictures > 0;
    }

    [[nodiscard]] std::int32_t width() const override {
        return m_width;
    }

    [[nodiscard]] std::int32_t height() const override {
        return m_height;
    }

    [[nodiscard]] double frame_rate() const override {
        return m_frame_rate;
    }

    [[nodiscard]] double duration() const override {
        return static_cast<double>(m_stream.pictures) / m_frame_rate;
    }

    [[nodiscard]] bool has_sound() const override {
        return m_has_sound;
    }

    [[nodiscard]] std::int32_t sound_rate() const override {
        return m_sound_rate;
    }

    [[nodiscard]] std::int32_t sound_channels() const override {
        return m_has_sound ? sound_channels_of_mpeg1 : 0;
    }

    bool next(picture_t& picture) override {
        plm_frame_t* frame = plm_video_decode(m_video.get());
        if (frame == nullptr) {
            return false;
        }
        convert(*frame, picture, m_next++);
        return true;
    }

    // From the start of the last group to start at or before the picture due, or of the one
    // before if it's one of an open group's shown before its key frame.
    bool seek(double seconds, picture_t& picture) override {
        const auto due =
            std::clamp(static_cast<std::int64_t>(std::floor((seconds * m_frame_rate) + 0.001)),
                       std::int64_t{ 0 }, m_stream.pictures - 1);
        auto group = std::ranges::upper_bound(m_stream.groups, due, {}, &group_t::first) - 1;
        if (group != m_stream.groups.begin() && !group->closed &&
            due < group->first + group->leading) {
            --group;
        }
        plm_video_rewind(m_video.get());
        m_cursor = group->offset;
        m_next = group->first;
        for (plm_frame_t* frame = plm_video_decode(m_video.get()); frame != nullptr;
             frame = plm_video_decode(m_video.get())) {
            if (m_next == due) {
                convert(*frame, picture, m_next++);
                return true;
            }
            ++m_next;
        }
        return false;
    }

    [[nodiscard]] sound_t sound() const override {
        sound_t sound{ .rate = m_sound_rate, .channels = sound_channels(), .samples = {} };
        if (!m_has_sound) {
            return sound;
        }
        // Its own decoder of the bytes, which it only reads.
        auto& bytes = const_cast<std::vector<std::uint8_t>&>(m_bytes);
        const plm_ptr_t plm = decoder_of(bytes, false);
        bool first = true;
        for (plm_samples_t* samples = plm_decode_audio(plm.get()); samples != nullptr;
             samples = plm_decode_audio(plm.get())) {
            // Silence until the first sound, so the first sample is at time 0.
            if (first && samples->time > 0.0) {
                const auto silent = static_cast<std::size_t>(
                    std::llround(samples->time * m_sound_rate) * sound_channels_of_mpeg1);
                sound.samples.resize(silent, 0.0F);
            }
            first = false;
            sound.samples.insert(sound.samples.end(), samples->interleaved,
                                 samples->interleaved +
                                     (std::size_t{ samples->count } * sound_channels_of_mpeg1));
        }
        return sound;
    }

private:
    // Gives pl_mpeg's video decoder the next of the stream, from where a seek puts it.
    static void load(plm_buffer_t* buffer, void* user) {
        auto* self = static_cast<mpeg1_t*>(user);
        const std::size_t count =
            std::min(chunk_size, self->m_stream.bytes.size() - self->m_cursor);
        if (count > 0) {
            plm_buffer_write(buffer, self->m_stream.bytes.data() + self->m_cursor, count);
            self->m_cursor += count;
        }
        if (self->m_cursor == self->m_stream.bytes.size()) {
            plm_buffer_signal_end(buffer);
        }
    }

    // `frame` as picture `number`'s pixels, alpha 255.
    void convert(plm_frame_t& frame, picture_t& picture, std::int64_t number) const {
        const auto size =
            static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height) * 4;
        if (picture.pixels.size() != size) {
            picture.pixels.assign(size, 255);
        }
        // pl_mpeg writes red, green, and blue, and leaves alpha.
        plm_frame_to_rgba(&frame, picture.pixels.data(), m_width * 4);
        picture.number = number;
        picture.time = static_cast<double>(number) / m_frame_rate;
    }

    std::vector<std::uint8_t> m_bytes;
    stream_t m_stream;
    bool m_has_sound;
    std::int32_t m_sound_rate;
    video_ptr_t m_video;
    std::int32_t m_width = 0;
    std::int32_t m_height = 0;
    double m_frame_rate = 0.0;
    // Where in the stream the decoder's given the next of it from.
    std::size_t m_cursor = 0;
    // The number of the next picture the decoder gives.
    std::int64_t m_next = 0;
};

} // namespace

std::unique_ptr<decoder_t> open_mpeg1(std::vector<std::uint8_t>& bytes) {
    const plm_ptr_t plm = decoder_of(bytes, true);
    if (plm == nullptr || plm_has_headers(plm.get()) == 0 ||
        plm_get_num_video_streams(plm.get()) == 0 || plm_get_width(plm.get()) <= 0 ||
        plm_get_height(plm.get()) <= 0 || !(plm_get_framerate(plm.get()) > 0.0)) {
        return nullptr;
    }
    stream_t stream = stream_of(bytes);
    if (stream.pictures == 0) {
        return nullptr;
    }
    const bool has_sound = plm_get_num_audio_streams(plm.get()) > 0;
    const std::int32_t sound_rate = has_sound ? plm_get_samplerate(plm.get()) : 0;
    auto decoder =
        std::make_unique<mpeg1_t>(std::move(bytes), std::move(stream), has_sound, sound_rate);
    // Its own decoder of the stream finds what the program stream's did, or it isn't one.
    if (!decoder->decodes()) {
        return nullptr;
    }
    return decoder;
}

std::unique_ptr<decoder_t> open_decoder(std::vector<std::uint8_t> bytes) {
    // Each format's in turn; the bytes go to the first that takes them.
    return open_mpeg1(bytes);
}

} // namespace fbe::video
