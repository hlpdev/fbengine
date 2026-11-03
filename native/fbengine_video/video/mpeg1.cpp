// MPEG-1, by pl_mpeg: a program stream of MPEG-1 video and MPEG-1 audio layer II, decoded from
// memory. Its video and sound are taken out of the program stream as it's opened, and its sound is
// decoded then, all of it, to be given as a WAV; its video's decoded by the video's thread.
//
// pl_mpeg's own seek can miss in a short video, and shows an open group of pictures' first
// B-frames, which are made from the group before's pictures, made from nothing, and its times
// for them a frame or two out. So its video decoder reads the video's elementary stream, its
// groups of pictures found as it's opened: a seek starts it again at a group's start and numbers
// its pictures from there. Its own sound's times start at 0 and run on through a gap, so the
// sound's frames are placed by the times of the packets they're in.

#include "video/decoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
// pl_mpeg's header takes stdio.h's FILE.
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <pl_mpeg.h>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace fbe::video {

namespace {

// pl_mpeg's sound is two channels, interleaved, in frames of 1152 samples each.
constexpr std::int32_t sound_channels_of_mpeg1 = 2;
constexpr std::int64_t samples_per_frame = 1152;

// Start codes, 00 00 01 and a byte, which MPEG-1 video has nowhere else.
constexpr std::uint32_t picture_start_code = 0x00000100U;
constexpr std::uint32_t group_start_code = 0x000001B8U;
constexpr std::uint32_t sequence_start_code = 0x000001B3U;

// How much silence a sound's times can make, past its own frames, in seconds: the video's length,
// but at least the one and at most the other.
constexpr double fewest_silence = 5.0;
constexpr double most_silence = 600.0;

// How much of a stream pl_mpeg's decoders are given at a time.
constexpr std::size_t chunk_size = std::size_t{ 64 } * 1024;

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

struct audio_deleter_t {
    void operator()(plm_audio_t* audio) const {
        plm_audio_destroy(audio);
    }
};

using demux_ptr_t = std::unique_ptr<plm_demux_t, demux_deleter_t>;
using video_ptr_t = std::unique_ptr<plm_video_t, video_deleter_t>;
using audio_ptr_t = std::unique_ptr<plm_audio_t, audio_deleter_t>;

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

// A packet of a video's sound: where its data starts in the sound's stream, and when it starts, if
// it says, or a negative.
struct sound_packet_t {
    std::size_t offset = 0;
    double time = -1.0;
};

// A video's program stream taken apart: its video's elementary stream, its pictures, and their
// groups, and its sound's stream and packets.
struct stream_t {
    bool headers = false;
    bool has_video = false;
    bool has_sound = false;
    std::vector<std::uint8_t> bytes;
    std::vector<group_t> groups;
    std::int64_t pictures = 0;
    // When the first picture to start in the first packet of the video that says is shown, or a
    // negative; where in the video's stream that packet starts; and that picture's number.
    double start = -1.0;
    std::size_t start_offset = 0;
    std::int64_t start_number = 0;
    std::vector<std::uint8_t> sound;
    std::vector<sound_packet_t> sound_packets;
};

// As many as `bits` of the bits after a start code that ends at `at` in `bytes`, as if 0s follow
// their end.
std::uint32_t bits_after(std::span<const std::uint8_t> bytes, std::size_t at, std::uint32_t bits) {
    std::uint32_t word = 0;
    for (std::size_t j = 1; j <= 4; ++j) {
        word = (word << 8U) | (at + j < bytes.size() ? bytes[at + j] : 0U);
    }
    return word >> (32U - bits);
}

// Counts `stream`'s pictures and finds their groups, without decoding them: a group only once a
// picture follows it, so start codes of groups of nothing take nothing. And finds the number of
// the picture its start is the time of.
void find_groups(stream_t& stream) {
    const std::span video(stream.bytes);
    std::optional<group_t> group;
    // The number of the first picture shown of the group the pictures are in.
    std::int64_t first = 0;
    bool started = false;
    std::uint32_t last = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < video.size(); ++i) {
        last = (last << 8U) | video[i];
        if (last == group_start_code) {
            // A 25-bit time code, then whether it's closed.
            group = group_t{
                .offset = i - 3,
                .first = stream.pictures,
                .leading = 0,
                .closed = (bits_after(video, i, 26) & 1U) != 0,
            };
            first = stream.pictures;
        } else if (last == picture_start_code) {
            // A picture's 10-bit temporal reference is how many of its group are shown before it,
            // which for its key frame are decoded after it.
            const std::uint32_t reference = bits_after(video, i, 10);
            if (group.has_value()) {
                group->leading = reference;
                stream.groups.push_back(*group);
                group.reset();
            }
            if (!started && i - 3 >= stream.start_offset) {
                stream.start_number = first + reference;
                started = true;
            }
            ++stream.pictures;
        }
    }
    // Pictures before a first group, if a stream has them, are decoded from its start.
    if (stream.groups.empty() || stream.groups.front().first != 0) {
        stream.groups.insert(stream.groups.begin(), group_t{});
    }
}

// How wide the pixels of the video's elementary stream `video` are shown against how high: its
// first sequence header's aspect code says how high against how wide. 1 if it says nothing.
double pixel_aspect_of(std::span<const std::uint8_t> video) {
    static constexpr std::array<double, 16> heights{
        1.0,    1.0,    0.6735, 0.7031, 0.7615, 0.8055, 0.8437, 0.8935,
        0.9157, 0.9815, 1.0255, 1.0695, 1.0950, 1.1575, 1.2015, 1.0,
    };
    std::uint32_t last = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < video.size(); ++i) {
        last = (last << 8U) | video[i];
        if (last == sequence_start_code) {
            // 12 bits of width and of height, then the code, 3 bytes on.
            return i + 4 < video.size() ? 1.0 / heights.at(video[i + 4] >> 4U) : 1.0;
        }
    }
    return 1.0;
}

// The program stream `bytes`, taken apart.
stream_t stream_of(std::span<const std::uint8_t> bytes) {
    stream_t stream;
    // The demuxer frees the buffer, which only reads the bytes.
    const demux_ptr_t demux(plm_demux_create(
        plm_buffer_create_with_memory(const_cast<std::uint8_t*>(bytes.data()), bytes.size(), 0),
        1));
    stream.headers = plm_demux_has_headers(demux.get()) != 0;
    if (!stream.headers) {
        return stream;
    }
    stream.has_video = plm_demux_get_num_video_streams(demux.get()) > 0;
    stream.has_sound = plm_demux_get_num_audio_streams(demux.get()) > 0;
    for (const plm_packet_t* packet = plm_demux_decode(demux.get()); packet != nullptr;
         packet = plm_demux_decode(demux.get())) {
        const std::span data(packet->data, packet->length);
        if (packet->type == PLM_DEMUX_PACKET_VIDEO_1) {
            if (stream.start < 0.0 && packet->pts >= 0.0) {
                stream.start = packet->pts;
                stream.start_offset = stream.bytes.size();
            }
            stream.bytes.insert(stream.bytes.end(), data.begin(), data.end());
        } else if (packet->type == PLM_DEMUX_PACKET_AUDIO_1 && !data.empty()) {
            stream.sound_packets.push_back({ .offset = stream.sound.size(), .time = packet->pts });
            stream.sound.insert(stream.sound.end(), data.begin(), data.end());
        }
    }
    find_groups(stream);
    return stream;
}

// A video's sound, decoded, its frames placed as its packets' times say, from its first
// picture's: a time more than the frames' own length, and the video's, or 5 seconds if that's more,
// or 10 minutes if that's less, either way is taken to be wrong, and its frame goes on from the one
// before; so a file of little sound and many bare pictures is at most 10 minutes of silence.
class sound_decoding_t {
public:
    sound_decoding_t(const stream_t& stream, double frame_rate, double duration)
        : m_stream(stream), m_duration(duration) {
        // Its times are from the first picture's, which can be before a stream's own 0, or from the
        // sound's own first if there's none.
        if (stream.start >= 0.0) {
            m_start = stream.start - (static_cast<double>(stream.start_number) / frame_rate);
            return;
        }
        for (const sound_packet_t& packet : stream.sound_packets) {
            if (packet.time >= 0.0) {
                m_start = packet.time;
                break;
            }
        }
    }

    // Decodes it all into `sound`; false if it's more than a WAV holds.
    bool decode(sound_t& sound) {
        // The whole stream in one buffer, which only reads it, so pl_mpeg finds each frame where
        // it is and never moves what's left of it.
        plm_buffer_t* buffer = plm_buffer_create_with_memory(
            const_cast<std::uint8_t*>(m_stream.sound.data()), m_stream.sound.size(), 0);
        const audio_ptr_t audio(plm_audio_create_with_buffer(buffer, 1));
        // Where the next frame's header is read from: the first is read as the decoder's made.
        std::size_t from = 0;
        m_wav.resize(wav_header_size);
        for (;;) {
            const plm_samples_t* samples = plm_audio_decode(audio.get());
            const std::size_t to = m_stream.sound.size() - plm_buffer_get_remaining(buffer);
            if (samples == nullptr) {
                // A header that isn't one is read past; the end isn't.
                if (to == from) {
                    break;
                }
                from = to;
                continue;
            }
            if (m_rate == 0) {
                m_rate = plm_audio_get_samplerate(audio.get());
                // A sound of no rate is none.
                if (m_rate <= 0) {
                    sound = {};
                    return true;
                }
            }
            if (!add(*samples, start_of(from))) {
                return false;
            }
            from = to;
        }
        if (m_frames.empty()) {
            sound = {};
            return true;
        }
        return place(sound);
    }

private:
    // A frame decoded: how many samples it has, and when its packet says it starts, from the
    // first picture's, if it's the first to start in a packet that says.
    struct frame_t {
        std::int64_t samples = 0;
        std::optional<double> time;
    };

    // Where a frame read from `from` starts, as pl_mpeg finds its header: after the zeros the
    // frame before ends in, a sync there, or else the next sync of a layer II frame after it, past
    // the rest of the frame before, which it reads only as far as its samples.
    [[nodiscard]] std::size_t start_of(std::size_t from) const {
        const std::span sound(m_stream.sound);
        while (from < sound.size() && sound[from] == 0) {
            ++from;
        }
        if (from + 1 >= sound.size() ||
            (sound[from] == 0xFF && (sound[from + 1] & 0xE0U) == 0xE0U)) {
            return from;
        }
        for (std::size_t at = from + 1; at + 1 < sound.size(); ++at) {
            if (sound[at] == 0xFF && (sound[at + 1] & 0xFEU) == 0xFCU) {
                return at;
            }
        }
        return from;
    }

    // Keeps a frame that starts at `start` in the sound's stream, after those before.
    bool add(const plm_samples_t& samples, std::size_t start) {
        if (!fits(m_decoded + samples.count)) {
            return false;
        }
        // A packet's time is that of the first frame to start in it.
        const std::vector<sound_packet_t>& packets = m_stream.sound_packets;
        const auto packet =
            std::ranges::upper_bound(packets, start, {}, &sound_packet_t::offset) - 1;
        const auto index = static_cast<std::size_t>(packet - packets.begin());
        frame_t frame{ .samples = samples.count };
        if (index != m_packet && packet->time >= 0.0 && m_start.has_value()) {
            frame.time = packet->time - *m_start;
        }
        m_packet = index;
        m_frames.push_back(frame);
        const auto bytes =
            std::as_bytes(std::span(samples.interleaved, static_cast<std::size_t>(samples.count) *
                                                             sound_channels_of_mpeg1));
        const std::size_t at = m_wav.size();
        m_wav.resize(at + bytes.size());
        std::memcpy(m_wav.data() + at, bytes.data(), bytes.size());
        m_decoded += samples.count;
        return true;
    }

    // Places the frames decoded as their times say, in the WAV they're decoded into, which it
    // gives `sound`: after silence, if the first is later than the first picture, or with its
    // samples before the first picture left out; and after silence where a frame's a gap after
    // the frame before. False if that's more than a WAV holds.
    bool place(sound_t& sound) {
        const double most = (static_cast<double>(m_decoded) / m_rate) +
                            std::max(fewest_silence, std::min(m_duration, most_silence));
        // Where silence goes, by the samples kept before it, and how much; and how many of the
        // first samples are left out.
        std::vector<std::pair<std::int64_t, std::int64_t>> gaps;
        std::int64_t dropped = 0;
        std::int64_t dropping = 0;
        std::int64_t written = 0;
        std::int64_t silence = 0;
        const auto pad = [&](std::int64_t count) {
            gaps.emplace_back(written - silence, count);
            written += count;
            silence += count;
        };
        for (std::size_t k = 0; k < m_frames.size(); ++k) {
            const frame_t& frame = m_frames[k];
            if (frame.time.has_value() && std::abs(*frame.time) <= most) {
                const std::int64_t at = std::llround(*frame.time * m_rate);
                if (k == 0 && at < 0) {
                    dropping = -at;
                } else if (k == 0 && at > 0) {
                    pad(at);
                } else if (k > 0 && at > written + (samples_per_frame / 2)) {
                    pad(at - written);
                }
            }
            const std::int64_t skipped = std::min(dropping, frame.samples);
            dropping -= skipped;
            dropped += skipped;
            written += frame.samples - skipped;
        }
        if (!fits(written)) {
            return false;
        }
        // A sound whose samples all come before the first picture is none.
        if (written == 0) {
            sound = {};
            return true;
        }
        constexpr std::size_t sample_bytes = sizeof(float) * sound_channels_of_mpeg1;
        const auto bytes_of = [](std::int64_t samples) {
            return static_cast<std::size_t>(samples) * sample_bytes;
        };
        const auto first = m_wav.begin() + static_cast<std::ptrdiff_t>(wav_header_size);
        m_wav.erase(first, first + static_cast<std::ptrdiff_t>(bytes_of(dropped)));
        const std::int64_t kept = m_decoded - dropped;
        m_wav.resize(wav_header_size + bytes_of(written));
        // Each run of samples after a gap moved on past it, the last first.
        std::uint8_t* samples = m_wav.data() + wav_header_size;
        std::int64_t end = kept;
        for (const auto& [at, count] : std::ranges::reverse_view(gaps)) {
            std::memmove(samples + bytes_of(at + silence), samples + bytes_of(at),
                         bytes_of(end - at));
            silence -= count;
            std::memset(samples + bytes_of(at + silence), 0, bytes_of(count));
            end = at;
        }
        write_wav_header(m_wav, m_rate, sound_channels_of_mpeg1);
        sound = {
            .rate = m_rate,
            .channels = sound_channels_of_mpeg1,
            .wav = std::move(m_wav),
        };
        return true;
    }

    // Whether `count` samples of each channel fit a WAV.
    static bool fits(std::int64_t count) {
        return count >= 0 && wav_holds(static_cast<std::uint64_t>(count) * sound_channels_of_mpeg1 *
                                       sizeof(float));
    }

    const stream_t& m_stream;
    double m_duration;
    std::optional<double> m_start;
    std::int32_t m_rate = 0;
    // A WAV's header, still to be written, and the frames' samples after it.
    std::vector<std::uint8_t> m_wav;
    std::vector<frame_t> m_frames;
    std::int64_t m_decoded = 0;
    // The packet the last frame started in.
    std::size_t m_packet = std::numeric_limits<std::size_t>::max();
};

class mpeg1_t final : public decoder_t {
public:
    explicit mpeg1_t(stream_t stream) : m_stream(std::move(stream)) {
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

    // Decodes its sound, after which it keeps none of the sound's stream; false if it's more than
    // a WAV holds.
    bool decode_sound() {
        if (m_stream.has_sound &&
            !sound_decoding_t(m_stream, m_frame_rate, duration()).decode(m_sound)) {
            return false;
        }
        m_stream.sound = {};
        m_stream.sound_packets = {};
        return true;
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

    [[nodiscard]] std::int64_t frames() const override {
        return m_stream.pictures;
    }

    [[nodiscard]] double duration() const override {
        return static_cast<double>(m_stream.pictures) / m_frame_rate;
    }

    [[nodiscard]] double pixel_aspect() const override {
        return pixel_aspect_of(m_stream.bytes);
    }

    [[nodiscard]] std::int64_t next_start(std::int64_t number) const override {
        // The next group's start, or past its first pictures if they're made from the group
        // before's, which a seek to them starts before.
        const auto after = std::ranges::upper_bound(m_stream.groups, number, {}, &group_t::first);
        if (after == m_stream.groups.end()) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return after->first + (after->closed ? 0 : after->leading);
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
    // before if it's one of an open group's shown before its key frame; the last there is, if the
    // stream ends before it.
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
        // The last picture decoded, which pl_mpeg leaves as it is once there's no more.
        plm_frame_t* last = nullptr;
        for (plm_frame_t* frame = plm_video_decode(m_video.get()); frame != nullptr;
             frame = plm_video_decode(m_video.get())) {
            if (m_next == due) {
                convert(*frame, picture, m_next++);
                return true;
            }
            last = frame;
            ++m_next;
        }
        if (last == nullptr) {
            return false;
        }
        convert(*last, picture, m_next - 1);
        return true;
    }

    [[nodiscard]] sound_t take_sound() override {
        return std::exchange(m_sound, {});
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

    stream_t m_stream;
    video_ptr_t m_video;
    std::int32_t m_width = 0;
    std::int32_t m_height = 0;
    double m_frame_rate = 0.0;
    sound_t m_sound;
    // Where in the stream the decoder's given the next of it from.
    std::size_t m_cursor = 0;
    // The number of the next picture the decoder gives.
    std::int64_t m_next = 0;
};

} // namespace

std::unique_ptr<decoder_t> open_mpeg1(std::span<const std::uint8_t> bytes, bool& too_long) {
    stream_t stream = stream_of(bytes);
    if (!stream.headers || !stream.has_video || stream.pictures == 0) {
        return nullptr;
    }
    auto decoder = std::make_unique<mpeg1_t>(std::move(stream));
    if (!decoder->decodes()) {
        return nullptr;
    }
    if (!decoder->decode_sound()) {
        too_long = true;
        return nullptr;
    }
    return decoder;
}

std::unique_ptr<decoder_t> open_decoder(std::span<const std::uint8_t> bytes, bool& too_long) {
    // Each format's in turn, each given the bytes as they are.
    return open_mpeg1(bytes, too_long);
}

} // namespace fbe::video
