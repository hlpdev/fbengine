// Videos: each decoded on a thread of its own, a few pictures ahead of its clock, which the main
// thread moves on, showing each picture as it falls due. A seek starts the thread again where it
// says, and a looping video's thread starts again at its start each time it ends, each loop's
// pictures due a loop's length after the last's.

#include "fbengine_video.h"
#include "video/decoder.hpp"
#include "video/handles.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace fbe::video;

thread_local std::string last_error;

// How many pictures a video's thread decodes ahead of what's shown.
constexpr std::size_t ahead = 3;

bool fail(std::string_view message) {
    last_error = message;
    return false;
}

struct video_t {
    explicit video_t(std::unique_ptr<decoder_t> made)
        : decoder(std::move(made)), width(decoder->width()), height(decoder->height()),
          frame_rate(decoder->frame_rate()), duration(decoder->duration()),
          // A picture's due a little before its time, so a clock moved on by a frame's time at a
          // time, which rounds, never falls a hair short of it.
          slack(0.001 / frame_rate), worker([this] { work(); }) {}

    ~video_t() {
        {
            const std::scoped_lock hold(lock);
            quit = true;
        }
        changed.notify_all();
        worker.join();
    }

    video_t(const video_t&) = delete;
    video_t& operator=(const video_t&) = delete;
    video_t(video_t&&) = delete;
    video_t& operator=(video_t&&) = delete;

    std::unique_ptr<decoder_t> decoder;
    const std::int32_t width;
    const std::int32_t height;
    const double frame_rate;
    const double duration;
    const double slack;

    // The thread's and the main thread's, under the lock.
    std::mutex lock;
    std::condition_variable changed;
    std::deque<picture_t> ready;
    std::vector<picture_t> spare;
    // Which seek the thread's pictures are of, and where the next is to.
    std::uint64_t generation = 0;
    std::optional<double> seek_to = 0.0;
    bool loop = false;
    // Whether the thread's decoded to the end, not looping.
    bool decoded_all = false;
    bool quit = false;

    // The main thread's.
    picture_t shown;
    // Whether the next picture ready is shown whenever it's due, as a seek's first is.
    bool showing_next = true;
    double clock = 0.0;
    bool playing = false;
    bool ended = false;
    std::int64_t frames_shown = 0;

    std::thread worker;

    // A picture to decode into, one shown before if there's one; the lock's held.
    picture_t take_spare() {
        if (spare.empty()) {
            return {};
        }
        picture_t picture = std::move(spare.back());
        spare.pop_back();
        return picture;
    }

    // The thread: decodes pictures ahead of what's shown, from where each seek says.
    void work() {
        std::unique_lock hold(lock);
        std::uint64_t of = generation;
        // Where this loop's pictures are due from, and the last picture's time in it.
        double loop_start = 0.0;
        double last = 0.0;
        while (!quit) {
            if (seek_to.has_value()) {
                const double to = *seek_to;
                seek_to.reset();
                of = generation;
                loop_start = 0.0;
                picture_t picture = take_spare();
                hold.unlock();
                const bool found = decoder->seek(to, picture);
                hold.lock();
                if (of == generation) {
                    decoded(found, std::move(picture), loop_start, last);
                }
                continue;
            }
            if (ready.size() >= ahead || decoded_all) {
                changed.wait(hold);
                continue;
            }
            picture_t picture = take_spare();
            const bool looping = loop;
            hold.unlock();
            bool found = decoder->next(picture);
            bool again = false;
            if (!found && looping) {
                found = decoder->seek(0.0, picture);
                again = found;
            }
            hold.lock();
            if (of != generation) {
                spare.push_back(std::move(picture));
                continue;
            }
            if (again) {
                // The next loop's start, a frame after the last's last picture.
                loop_start += last + (1.0 / frame_rate);
            }
            decoded(found, std::move(picture), loop_start, last);
        }
    }

    // Keeps a picture the thread decoded, if it `found` one; the lock's held.
    void decoded(bool found, picture_t picture, double loop_start, double& last) {
        if (found) {
            last = picture.time;
            picture.loop_start = loop_start;
            picture.time += loop_start;
            ready.push_back(std::move(picture));
        } else {
            decoded_all = true;
            spare.push_back(std::move(picture));
        }
        changed.notify_all();
    }

    // Shows the picture due by the clock, waiting for the thread to decode it if it's behind.
    void show_due() {
        std::unique_lock hold(lock);
        for (;;) {
            while (!ready.empty() && (showing_next || ready.front().time <= clock + slack)) {
                spare.push_back(std::exchange(shown, std::move(ready.front())));
                ready.pop_front();
                showing_next = false;
                ++frames_shown;
                changed.notify_all();
            }
            // The next is ready but not due, or there's none; or the next isn't decoded yet but
            // is due, as the one shown has had its time.
            const bool due = showing_next || shown.time + (1.0 / frame_rate) <= clock + slack;
            if (!ready.empty() || decoded_all || !due) {
                break;
            }
            changed.wait(hold);
        }
        if (decoded_all && ready.empty() && !showing_next &&
            shown.time + (1.0 / frame_rate) <= clock + slack) {
            ended = true;
            playing = false;
            clock = shown.time + (1.0 / frame_rate);
        }
    }

    // Starts the thread again at `seconds`, and shows the picture due then.
    void seek(double seconds) {
        {
            const std::scoped_lock hold(lock);
            for (picture_t& picture : ready) {
                spare.push_back(std::move(picture));
            }
            ready.clear();
            ++generation;
            seek_to = seconds;
            decoded_all = false;
        }
        changed.notify_all();
        clock = seconds;
        ended = false;
        showing_next = true;
        show_due();
    }
};

pool_t<video_t>& videos() {
    // Never freed: statics freed as the process ends go in any order.
    static auto* pool = new pool_t<video_t>(VIDEO_KIND);
    return *pool;
}

video_t* find(std::uint64_t handle) {
    video_t* video = videos().get(handle);
    if (video == nullptr) {
        fail(handle == 0 ? "there's no video" : "the video was freed");
    }
    return video;
}

// A RIFF WAV file of `sound`'s 32-bit float samples.
std::vector<std::uint8_t> wav_of(const sound_t& sound) {
    std::vector<std::uint8_t> wav;
    const auto put = [&wav](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            wav.push_back(static_cast<std::uint8_t>(value >> (8U * static_cast<unsigned>(i))));
        }
    };
    const auto tag = [&wav](std::string_view text) {
        wav.insert(wav.end(), text.begin(), text.end());
    };
    const auto data = static_cast<std::uint32_t>(sound.samples.size() * sizeof(float));
    const auto channels = static_cast<std::uint32_t>(sound.channels);
    const auto rate = static_cast<std::uint32_t>(sound.rate);
    tag("RIFF");
    put(36 + data, 4);
    tag("WAVE");
    tag("fmt ");
    put(16, 4);
    // IEEE float.
    put(3, 2);
    put(channels, 2);
    put(rate, 4);
    put(rate * channels * 4, 4);
    put(channels * 4, 2);
    put(32, 2);
    tag("data");
    put(data, 4);
    const std::size_t start = wav.size();
    wav.resize(start + data);
    std::memcpy(wav.data() + start, sound.samples.data(), data);
    return wav;
}

} // namespace

extern "C" {

FBE_VIDEO_API const char* fbe_video_version(void) {
    return FBE_VERSION;
}

FBE_VIDEO_API const char* fbe_video_error(void) {
    return last_error.c_str();
}

FBE_VIDEO_API uint64_t fbe_video_open(const uint8_t* data, int64_t size) {
    if (size <= 0) {
        fail("a video is some bytes");
        return 0;
    }
    std::unique_ptr<decoder_t> decoder =
        open_decoder(std::vector<std::uint8_t>(data, data + static_cast<std::size_t>(size)));
    if (decoder == nullptr) {
        fail("that isn't a video fbengine_video decodes: an MPEG-1 program stream");
        return 0;
    }
    auto video = std::make_unique<video_t>(std::move(decoder));
    video->show_due();
    if (video->frames_shown == 0) {
        fail("the video has no frames");
        return 0;
    }
    return videos().add(std::move(video));
}

FBE_VIDEO_API bool fbe_video_free(uint64_t video) {
    return find(video) != nullptr && videos().remove(video);
}

FBE_VIDEO_API int32_t fbe_video_count(void) {
    return static_cast<int32_t>(videos().count());
}

FBE_VIDEO_API int32_t fbe_video_width(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr ? found->width : 0;
}

FBE_VIDEO_API int32_t fbe_video_height(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr ? found->height : 0;
}

FBE_VIDEO_API double fbe_video_frame_rate(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr ? found->frame_rate : 0.0;
}

FBE_VIDEO_API double fbe_video_duration(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr ? found->duration : 0.0;
}

FBE_VIDEO_API bool fbe_video_has_sound(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr && found->decoder->has_sound();
}

FBE_VIDEO_API int32_t fbe_video_sound_rate(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr ? found->decoder->sound_rate() : 0;
}

FBE_VIDEO_API int32_t fbe_video_sound_channels(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr ? found->decoder->sound_channels() : 0;
}

FBE_VIDEO_API int64_t fbe_video_sound(uint64_t video, uint8_t* bytes, int64_t capacity) {
    const video_t* found = find(video);
    if (found == nullptr) {
        return -1;
    }
    if (!found->decoder->has_sound()) {
        fail("the video has no sound");
        return -1;
    }
    const std::vector<std::uint8_t> wav = wav_of(found->decoder->sound());
    if (std::cmp_greater_equal(capacity, wav.size())) {
        std::ranges::copy(wav, bytes);
    }
    return static_cast<int64_t>(wav.size());
}

FBE_VIDEO_API bool fbe_video_play(uint64_t video) {
    video_t* found = find(video);
    if (found == nullptr) {
        return false;
    }
    if (found->ended) {
        found->seek(0.0);
    }
    found->playing = true;
    return true;
}

FBE_VIDEO_API bool fbe_video_pause(uint64_t video) {
    video_t* found = find(video);
    if (found == nullptr) {
        return false;
    }
    found->playing = false;
    return true;
}

FBE_VIDEO_API bool fbe_video_playing(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr && found->playing;
}

FBE_VIDEO_API bool fbe_video_set_loop(uint64_t video, bool loop) {
    video_t* found = find(video);
    if (found == nullptr) {
        return false;
    }
    {
        const std::scoped_lock hold(found->lock);
        found->loop = loop;
        // A thread at the end goes on into the next loop.
        if (loop && found->decoded_all) {
            found->decoded_all = false;
        }
    }
    found->changed.notify_all();
    return true;
}

FBE_VIDEO_API bool fbe_video_looping(uint64_t video) {
    video_t* found = find(video);
    if (found == nullptr) {
        return false;
    }
    const std::scoped_lock hold(found->lock);
    return found->loop;
}

FBE_VIDEO_API bool fbe_video_update(uint64_t video, double seconds) {
    video_t* found = find(video);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return fail(std::format("a video's clock moves on by 0 seconds or more, not {}", seconds));
    }
    if (found->playing) {
        found->clock += seconds;
        found->show_due();
    }
    return true;
}

FBE_VIDEO_API bool fbe_video_seek(uint64_t video, double seconds) {
    video_t* found = find(video);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > found->duration) {
        return fail(std::format("a video seeks to 0 to its length, {} seconds, not {}",
                                found->duration, seconds));
    }
    found->seek(seconds);
    return true;
}

FBE_VIDEO_API double fbe_video_time(uint64_t video) {
    const video_t* found = find(video);
    if (found == nullptr) {
        return -1.0;
    }
    return std::clamp(found->clock - found->shown.loop_start, 0.0, found->duration);
}

FBE_VIDEO_API bool fbe_video_ended(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr && found->ended;
}

FBE_VIDEO_API int64_t fbe_video_frame_number(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr ? found->shown.number : -1;
}

FBE_VIDEO_API int64_t fbe_video_frames_shown(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr ? found->frames_shown : -1;
}

FBE_VIDEO_API bool fbe_video_frame(uint64_t video, uint8_t* pixels, int64_t size) {
    const video_t* found = find(video);
    if (found == nullptr) {
        return false;
    }
    if (std::cmp_not_equal(size, found->shown.pixels.size())) {
        return fail(std::format("a frame of the video is {} bytes, not {}",
                                found->shown.pixels.size(), size));
    }
    std::ranges::copy(found->shown.pixels, pixels);
    return true;
}

} // extern "C"
