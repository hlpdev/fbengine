// Videos: each decoded on a thread of its own, a few pictures ahead of its clock, which the main
// thread moves on, showing each picture as it falls due. A seek starts the thread again where it
// says, and a looping video's thread starts again at its start each time it ends, each loop's
// pictures due a loop's length after the last's. A clock moved on past the next picture a seek
// starts from seeks there, rather than decoding every picture between.

#include "fbengine_video.h"
#include "video/decoder.hpp"
#include "video/handles.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <format>
#include <limits>
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

// The most seconds a video's clock moves on by at a time.
constexpr double most_update = 1e9;

bool fail(std::string_view message) {
    last_error = message;
    return false;
}

// Where a thread starts again: a time in a loop, and which loop.
struct seek_t {
    double to = 0.0;
    std::int64_t loop = 0;
};

struct video_t {
    explicit video_t(std::unique_ptr<decoder_t> made)
        : decoder(std::move(made)), width(decoder->width()), height(decoder->height()),
          frame_rate(decoder->frame_rate()), frames(decoder->frames()),
          duration(decoder->duration()),
          // A picture's due a little before its time, so a clock moved on by a frame's time at a
          // time, which rounds, never falls a hair short of it.
          slack(0.001 / frame_rate) {
        sound_t sound = decoder->take_sound();
        sound_rate = sound.rate;
        sound_channels = sound.channels;
        wav = std::move(sound.wav);
        worker = std::thread([this] { work(); });
    }

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
    const std::int64_t frames;
    const double duration;
    const double slack;
    std::int32_t sound_rate = 0;
    std::int32_t sound_channels = 0;
    // Its sound, as a WAV, made once.
    std::vector<std::uint8_t> wav;

    // The thread's and the main thread's, under the lock.
    std::mutex lock;
    std::condition_variable changed;
    std::deque<picture_t> ready;
    std::vector<picture_t> spare;
    // Which seek the thread's pictures are of, and where the next is to.
    std::uint64_t generation = 0;
    std::optional<seek_t> seek_to = seek_t{};
    bool loop = false;
    // Whether the thread's decoded to the end, not looping.
    bool decoded_all = false;
    bool quit = false;

    // The main thread's.
    picture_t shown;
    // Whether the next picture ready is shown whenever it's due, as a seek's first is.
    bool showing_next = true;
    // The clock: a time in a loop, which a looping video's clock is kept within, and which loop,
    // so a clock moved on by years is as exact as at the start.
    double clock = 0.0;
    std::int64_t clock_loop = 0;
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
        // Which loop the pictures are of.
        std::int64_t loop_of = 0;
        while (!quit) {
            if (seek_to.has_value()) {
                const seek_t to = *seek_to;
                seek_to.reset();
                of = generation;
                loop_of = to.loop;
                picture_t picture = take_spare();
                hold.unlock();
                const bool found = decoder->seek(to.to, picture);
                hold.lock();
                if (of == generation) {
                    decoded(found, std::move(picture), loop_of);
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
                // A loop turned off meanwhile ends here, and one turned on again seeks.
                if (!loop) {
                    decoded(false, std::move(picture), loop_of);
                    continue;
                }
                ++loop_of;
            }
            decoded(found, std::move(picture), loop_of);
        }
    }

    // Keeps a picture the thread decoded, if it `found` one; the lock's held.
    void decoded(bool found, picture_t picture, std::int64_t loop_of) {
        if (found) {
            picture.loop = loop_of;
            ready.push_back(std::move(picture));
        } else {
            decoded_all = true;
            spare.push_back(std::move(picture));
        }
        changed.notify_all();
    }

    // Starts the thread again at `to`, the picture it decodes first shown whenever it's ready.
    void restart(seek_t to) {
        {
            const std::scoped_lock hold(lock);
            for (picture_t& picture : ready) {
                spare.push_back(std::move(picture));
            }
            ready.clear();
            ++generation;
            seek_to = to;
            decoded_all = false;
        }
        changed.notify_all();
    }

    // How far the clock is past `picture`'s time, in seconds.
    [[nodiscard]] double past(const picture_t& picture) const {
        return (static_cast<double>(clock_loop - picture.loop) * duration) + clock - picture.time;
    }

    // Moves the clock on by `seconds`, a looping video's whole loops into its count of them.
    void move_clock(double seconds) {
        clock += seconds;
        if (loop && clock >= duration) {
            const double loops = std::floor(clock / duration);
            clock_loop += static_cast<std::int64_t>(loops);
            clock = std::max(0.0, clock - (loops * duration));
        }
    }

    // Seeks to the picture the clock's at, if that's past the next picture a seek starts from,
    // or a loop on, which decoding every picture between would be slow to get to.
    void skip_ahead() {
        if (showing_next) {
            return;
        }
        // How many loops on from the one shown the clock is, and how far into that loop.
        std::int64_t loops = std::max<std::int64_t>(0, clock_loop - shown.loop);
        double into = clock;
        if (!loop) {
            into = std::min(duration, (static_cast<double>(loops) * duration) + clock);
            loops = 0;
        } else if (into >= duration) {
            const double whole = std::floor(into / duration);
            loops += static_cast<std::int64_t>(whole);
            into = std::max(0.0, into - (whole * duration));
        }
        const std::int64_t due =
            std::clamp(static_cast<std::int64_t>(std::floor((into * frame_rate) + 0.001)),
                       std::int64_t{ 0 }, frames - 1);
        // Where the thread gets to as soon as a seek would: the next start, or into the next loop
        // as far as its first, which two loops on is past.
        std::int64_t start = decoder->next_start(shown.number);
        if (start == std::numeric_limits<std::int64_t>::max()) {
            if (!loop) {
                return;
            }
            start = frames + std::min(decoder->next_start(0), frames);
        }
        if (loops < 2 && (loops * frames) + due < start) {
            return;
        }
        restart(seek_t{ .to = into, .loop = shown.loop + loops });
        showing_next = true;
    }

    // Shows the picture due by the clock, waiting for the thread to decode it if it's behind.
    void show_due() {
        skip_ahead();
        const double frame = 1.0 / frame_rate;
        std::unique_lock hold(lock);
        for (;;) {
            while (!ready.empty() && (showing_next || past(ready.front()) + slack >= 0.0)) {
                spare.push_back(std::exchange(shown, std::move(ready.front())));
                ready.pop_front();
                showing_next = false;
                ++frames_shown;
                changed.notify_all();
            }
            // A seek that found nothing to show leaves what's shown.
            if (showing_next && ready.empty() && decoded_all) {
                showing_next = false;
            }
            // The next is ready but not due, or there's none; or the next isn't decoded yet but
            // is due, as the one shown has had its time.
            const bool due = showing_next || past(shown) + slack >= frame;
            if (!ready.empty() || decoded_all || !due) {
                break;
            }
            changed.wait(hold);
        }
        if (decoded_all && ready.empty() && !showing_next && past(shown) + slack >= frame) {
            ended = true;
            playing = false;
            clock = shown.time + frame;
            clock_loop = shown.loop;
        }
    }

    // Starts the thread again at `seconds`, and shows the picture due then.
    void seek(double seconds) {
        restart(seek_t{ .to = seconds, .loop = 0 });
        clock = seconds;
        clock_loop = 0;
        ended = false;
        showing_next = true;
        show_due();
    }

    // Makes the video loop, or not: a loop turned off ends at its end, and the thread, if it's
    // gone on into the next, lets go of what it decoded of it; a loop turned on with the thread
    // at the end starts the next.
    void set_loop(bool looping) {
        {
            const std::scoped_lock hold(lock);
            if (loop && !looping) {
                end_loop();
            } else if (looping && decoded_all && !seek_to.has_value()) {
                const std::int64_t last = ready.empty() ? shown.loop : ready.back().loop;
                seek_to = seek_t{ .to = 0.0, .loop = last + 1 };
                decoded_all = false;
            }
            loop = looping;
        }
        changed.notify_all();
    }

    // Lets go of the pictures the thread's decoded of the loop after the one showing, and stops
    // it, if it has any; the lock's held. A seek's pictures still to come are of the loop it says.
    void end_loop() {
        if (ready.empty()) {
            return;
        }
        const std::int64_t showing = showing_next ? ready.front().loop : shown.loop;
        if (ready.back().loop == showing) {
            return;
        }
        while (ready.back().loop != showing) {
            spare.push_back(std::move(ready.back()));
            ready.pop_back();
            if (ready.empty()) {
                break;
            }
        }
        ++generation;
        decoded_all = true;
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
    bool too_long = false;
    std::unique_ptr<decoder_t> decoder =
        open_decoder(std::span(data, static_cast<std::size_t>(size)), too_long);
    if (too_long) {
        fail("the video's sound is longer than a WAV holds, 4 GB of it");
        return 0;
    }
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
    return found != nullptr && found->sound_rate > 0;
}

FBE_VIDEO_API int32_t fbe_video_sound_rate(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr ? found->sound_rate : 0;
}

FBE_VIDEO_API int32_t fbe_video_sound_channels(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr ? found->sound_channels : 0;
}

FBE_VIDEO_API int64_t fbe_video_sound(uint64_t video, uint8_t* bytes, int64_t capacity) {
    const video_t* found = find(video);
    if (found == nullptr) {
        return -1;
    }
    if (found->sound_rate == 0) {
        fail("the video has no sound");
        return -1;
    }
    if (std::cmp_greater_equal(capacity, found->wav.size())) {
        std::ranges::copy(found->wav, bytes);
    }
    return static_cast<int64_t>(found->wav.size());
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
    found->set_loop(loop);
    return true;
}

FBE_VIDEO_API bool fbe_video_looping(uint64_t video) {
    const video_t* found = find(video);
    return found != nullptr && found->loop;
}

FBE_VIDEO_API bool fbe_video_update(uint64_t video, double seconds) {
    video_t* found = find(video);
    if (found == nullptr) {
        return false;
    }
    // More than a billion seconds, 31 years, is a mistake, as a time in nanoseconds would be.
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > most_update) {
        return fail(std::format(
            "a video's clock moves on by 0 to a billion seconds at a time, not {}", seconds));
    }
    if (found->playing) {
        found->move_clock(seconds);
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
    const double since =
        (static_cast<double>(found->clock_loop - found->shown.loop) * found->duration) +
        found->clock;
    return std::clamp(since, 0.0, found->duration);
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
