// A video's thread and main thread, on a decoder of this test's, built in from video.cpp, which
// says how many seeks there are and holds the thread where a test asks: playing costs no seeks, and
// a loop turned on or off with the thread anywhere near its end does what it says.

#include "fbengine_video.h"
#include "video/decoder.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

namespace {

constexpr std::int64_t frames = 50;
constexpr double rate = 25.0;
constexpr std::int64_t group = 12;
constexpr double frame = 1.0 / rate;

// What a test sees of its decoder, and how it holds it: at the end of its pictures, as next() finds
// none, or in a seek.
struct control_t {
    std::mutex lock;
    std::condition_variable changed;
    int seeks = 0;
    bool hold_end = false;
    bool hold_seek = false;
    bool held = false;

    // Stops the thread here, saying so, while `holding` is.
    void stop_while(const bool& holding) {
        std::unique_lock hold(lock);
        held = holding;
        changed.notify_all();
        changed.wait(hold, [&] { return !holding; });
        held = false;
    }

    void wait_until_held() {
        std::unique_lock hold(lock);
        changed.wait(hold, [&] { return held; });
    }

    void release() {
        const std::scoped_lock hold(lock);
        hold_end = false;
        hold_seek = false;
        changed.notify_all();
    }

    int seeks_so_far() {
        const std::scoped_lock hold(lock);
        return seeks;
    }

    void set_hold_end() {
        const std::scoped_lock hold(lock);
        hold_end = true;
    }

    void set_hold_seek() {
        const std::scoped_lock hold(lock);
        hold_seek = true;
    }

    // Lets the thread on from the end, to stop in the seek it starts the next loop with.
    void hold_seek_after_end() {
        {
            const std::scoped_lock hold(lock);
            hold_end = false;
            held = false;
            hold_seek = true;
            changed.notify_all();
        }
        wait_until_held();
    }
};

control_t* control = nullptr;

class fake_t final : public fbe::video::decoder_t {
public:
    [[nodiscard]] std::int32_t width() const override {
        return 1;
    }

    [[nodiscard]] std::int32_t height() const override {
        return 1;
    }

    [[nodiscard]] double frame_rate() const override {
        return rate;
    }

    [[nodiscard]] std::int64_t frames() const override {
        return ::frames;
    }

    [[nodiscard]] double duration() const override {
        return static_cast<double>(::frames) * frame;
    }

    [[nodiscard]] std::int64_t next_start(std::int64_t number) const override {
        const std::int64_t start = ((number / group) + 1) * group;
        return start < ::frames ? start : INT64_MAX;
    }

    bool next(fbe::video::picture_t& picture) override {
        if (m_next == ::frames) {
            control->stop_while(control->hold_end);
            return false;
        }
        give(picture);
        return true;
    }

    bool seek(double seconds, fbe::video::picture_t& picture) override {
        {
            const std::scoped_lock hold(control->lock);
            ++control->seeks;
        }
        control->stop_while(control->hold_seek);
        const auto due = static_cast<std::int64_t>(std::floor((seconds * rate) + 0.001));
        m_next = std::min(due, ::frames - 1);
        give(picture);
        return true;
    }

    [[nodiscard]] fbe::video::sound_t take_sound() override {
        return {};
    }

private:
    void give(fbe::video::picture_t& picture) {
        picture.pixels.assign(4, 255);
        picture.number = m_next;
        picture.time = static_cast<double>(m_next) * frame;
        ++m_next;
    }

    std::int64_t m_next = 0;
};

// A video on the fake decoder, freed as it goes.
class video_handle_t {
public:
    explicit video_handle_t(control_t& controlled) {
        control = &controlled;
        const std::array<std::uint8_t, 1> byte{ 0 };
        m_video = fbe_video_open(byte.data(), 1);
    }

    video_handle_t(const video_handle_t&) = delete;
    video_handle_t& operator=(const video_handle_t&) = delete;
    video_handle_t(video_handle_t&&) = delete;
    video_handle_t& operator=(video_handle_t&&) = delete;

    ~video_handle_t() {
        fbe_video_free(m_video);
    }

    [[nodiscard]] std::uint64_t get() const {
        return m_video;
    }

private:
    std::uint64_t m_video = 0;
};

// Long enough for the thread to have decoded as far ahead as it does.
void let_it_decode() {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
}

} // namespace

namespace fbe::video {

std::unique_ptr<decoder_t> open_decoder(std::span<const std::uint8_t> /*bytes*/,
                                        bool& /*too_long*/) {
    return std::make_unique<fake_t>();
}

} // namespace fbe::video

// Each Update past a group's start on the way there, however fast the thread is, costs no seek,
// which would keep the main thread waiting.
TEST_CASE("playing a frame at a time seeks only as it starts") {
    control_t control;
    const video_handle_t video(control);
    fbe_video_play(video.get());
    for (std::int64_t i = 1; i < frames; ++i) {
        REQUIRE(fbe_video_update(video.get(), frame));
        CHECK(fbe_video_frame_number(video.get()) == i);
    }
    CHECK(control.seeks_so_far() == 1);
    fbe_video_update(video.get(), frame);
    CHECK(fbe_video_ended(video.get()));
    CHECK(control.seeks_so_far() == 1);
}

// With time for the thread to be ahead, and frames skipped within what it's decoded or past a start
// by no more than it'd decode: no seek.
TEST_CASE("frames skipped that decoding on gets to as soon cost no seek") {
    control_t control;
    const video_handle_t video(control);
    fbe_video_play(video.get());
    for (std::int64_t i = 2; i < frames; i += 2) {
        let_it_decode();
        REQUIRE(fbe_video_update(video.get(), 2 * frame));
        CHECK(fbe_video_frame_number(video.get()) == i);
    }
    CHECK(control.seeks_so_far() == 1);
}

TEST_CASE("a clock moved past the next group's start seeks") {
    control_t control;
    const video_handle_t video(control);
    fbe_video_play(video.get());
    let_it_decode();
    REQUIRE(fbe_video_update(video.get(), 30 * frame));
    CHECK(fbe_video_frame_number(video.get()) == 30);
    CHECK(control.seeks_so_far() == 2);
}

// A loop's only seeks are the thread's, one at each of its ends.
TEST_CASE("playing a loop a frame at a time seeks once a loop") {
    control_t control;
    const video_handle_t video(control);
    fbe_video_set_loop(video.get(), true);
    fbe_video_play(video.get());
    for (std::int64_t i = 1; i <= (2 * frames) + 10; ++i) {
        REQUIRE(fbe_video_update(video.get(), frame));
        CHECK(fbe_video_frame_number(video.get()) == i % frames);
    }
    CHECK(control.seeks_so_far() == 3);
}

// Mid-loop, what the thread's doing is what's wanted.
TEST_CASE("a loop turned off before its thread's in the next leaves the thread as it is") {
    control_t control;
    const video_handle_t video(control);
    fbe_video_set_loop(video.get(), true);
    fbe_video_play(video.get());
    REQUIRE(fbe_video_update(video.get(), 3 * frame));
    let_it_decode();
    fbe_video_set_loop(video.get(), false);
    for (std::int64_t i = 4; i <= frames; ++i) {
        REQUIRE(fbe_video_update(video.get(), frame));
    }
    CHECK(fbe_video_ended(video.get()));
    CHECK(fbe_video_frame_number(video.get()) == frames - 1);
    CHECK(control.seeks_so_far() == 1);
}

// The thread finding the end as looping's turned on goes on into the next loop.
TEST_CASE("a loop turned on as the thread finds the end goes on") {
    control_t control;
    const video_handle_t video(control);
    control.set_hold_end();
    fbe_video_seek(video.get(), static_cast<double>(frames - 1) * frame);
    control.wait_until_held();
    fbe_video_set_loop(video.get(), true);
    control.release();
    fbe_video_play(video.get());
    REQUIRE(fbe_video_update(video.get(), frame));
    CHECK_FALSE(fbe_video_ended(video.get()));
    CHECK(fbe_video_looping(video.get()));
    CHECK(fbe_video_frame_number(video.get()) == 0);
    REQUIRE(fbe_video_update(video.get(), frame));
    CHECK(fbe_video_frame_number(video.get()) == 1);
}

// Looping turned off as the seek looping turned on queued is under way lets go of its picture.
TEST_CASE("a loop turned off as the thread seeks to the next ends at the end") {
    control_t control;
    const video_handle_t video(control);
    fbe_video_seek(video.get(), static_cast<double>(frames - 1) * frame);
    let_it_decode();
    control.set_hold_seek();
    fbe_video_set_loop(video.get(), true);
    control.wait_until_held();
    fbe_video_set_loop(video.get(), false);
    control.release();
    fbe_video_play(video.get());
    REQUIRE(fbe_video_update(video.get(), frame));
    CHECK(fbe_video_ended(video.get()));
    CHECK(fbe_video_frame_number(video.get()) == frames - 1);
    // And on again, it goes on, from the start of the next.
    fbe_video_set_loop(video.get(), true);
    fbe_video_play(video.get());
    CHECK(fbe_video_frame_number(video.get()) == 0);
    REQUIRE(fbe_video_update(video.get(), frame));
    CHECK(fbe_video_frame_number(video.get()) == 1);
}

// A seek to a looping video's end is the next loop's start, whose frames go on by the clock, though
// looping's turned off then.
TEST_CASE("a seek to a looping video's end keeps its clock in the loop it shows") {
    control_t control;
    const video_handle_t video(control);
    fbe_video_set_loop(video.get(), true);
    fbe_video_seek(video.get(), static_cast<double>(frames) * frame);
    let_it_decode();
    fbe_video_set_loop(video.get(), false);
    fbe_video_play(video.get());
    CHECK(fbe_video_frame_number(video.get()) == 0);
    REQUIRE(fbe_video_update(video.get(), frame));
    CHECK(fbe_video_frame_number(video.get()) == 1);
    CHECK(std::abs(fbe_video_time(video.get()) - frame) < 1e-6);
}

// Looping turned off as the thread starts the next loop by itself ends this one.
TEST_CASE("a loop turned off as the thread starts the next by itself ends at the end") {
    control_t control;
    const video_handle_t video(control);
    fbe_video_set_loop(video.get(), true);
    control.set_hold_end();
    fbe_video_seek(video.get(), static_cast<double>(frames - 2) * frame);
    control.wait_until_held();
    control.hold_seek_after_end();
    fbe_video_set_loop(video.get(), false);
    control.release();
    fbe_video_play(video.get());
    REQUIRE(fbe_video_update(video.get(), frame));
    CHECK(fbe_video_frame_number(video.get()) == frames - 1);
    REQUIRE(fbe_video_update(video.get(), frame));
    CHECK(fbe_video_ended(video.get()));
    CHECK(fbe_video_frame_number(video.get()) == frames - 1);
}
