// Clips through the C API: made a channel at a time, what's refused and why, what they say of
// themselves, sampling, and living while they're held.

#include "fbengine.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace {

constexpr std::array<float, 2> two_times{ 0.0F, 2.0F };
constexpr std::array<float, 6> two_places{ 0.0F, 0.0F, 0.0F, 4.0F, 2.0F, 0.0F };

bool says(std::string_view message) {
    return std::string_view(fbe_error()).contains(message);
}

} // namespace

TEST_CASE("a clip is made a channel at a time, and says what it is") {
    const std::uint64_t clip = fbe_clip_create("walk", 30.0F);
    REQUIRE(clip != 0);
    CHECK(std::string_view(fbe_clip_name(clip)) == "walk");
    CHECK(fbe_clip_frame_rate(clip) == 30.0F);
    CHECK(fbe_clip_length(clip) == 0.0F);
    CHECK(fbe_clip_channel_count(clip) == 0);
    CHECK(fbe_clip_add_channel(clip, "hips", FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3,
                               two_times.data(), two_places.data(), 2));
    const std::array<float, 3> times{ 0.0F, 1.0F, 3.5F };
    const std::array<float, 12> turns{ 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1 };
    CHECK(fbe_clip_add_channel(clip, "hips", FBE_CLIP_ROTATION, FBE_CLIP_STEP, 4, times.data(),
                               turns.data(), 3));
    // Its length is its last key's, whichever channel's it is.
    CHECK(fbe_clip_length(clip) == 3.5F);
    CHECK(fbe_clip_channel_count(clip) == 2);
    std::array<float, 4> out{};
    CHECK(fbe_clip_sample(clip, 0, 1.0F, out.data(), out.size()) == 3);
    CHECK(out[0] == Catch::Approx(2.0F));
    CHECK(out[1] == Catch::Approx(1.0F));
    CHECK(fbe_clip_sample(clip, 1, 2.0F, out.data(), out.size()) == 4);
    CHECK(out[1] == 1.0F);
    CHECK(fbe_clip_release(clip));
    CHECK(!fbe_clip_alive(clip));
}

TEST_CASE("a channel that can't be sampled is refused, saying why") {
    const std::uint64_t clip = fbe_clip_create("", 0.0F);
    const float* times = two_times.data();
    const float* values = two_places.data();
    CHECK(!fbe_clip_add_channel(clip, "a", 4, FBE_CLIP_LINEAR, 3, times, values, 2));
    CHECK(says("4 isn't a property"));
    CHECK(!fbe_clip_add_channel(clip, "a", FBE_CLIP_SCALE, 3, 3, times, values, 2));
    CHECK(says("3 isn't an interpolation"));
    CHECK(!fbe_clip_add_channel(clip, "a", FBE_CLIP_SCALE, FBE_CLIP_LINEAR, 3, times, values, 0));
    CHECK(says("a key or more"));
    CHECK(
        !fbe_clip_add_channel(clip, "a", FBE_CLIP_ROTATION, FBE_CLIP_LINEAR, 3, times, values, 2));
    CHECK(says("rotation is 4 floats a key, not 3"));
    CHECK(!fbe_clip_add_channel(clip, "a", FBE_CLIP_WEIGHTS, FBE_CLIP_LINEAR, 0, times, values, 2));
    CHECK(says("weights are 1 or more floats a key, not 0"));
    const std::array<float, 2> falling{ 1.0F, 1.0F };
    CHECK(!fbe_clip_add_channel(clip, "a", FBE_CLIP_SCALE, FBE_CLIP_LINEAR, 3, falling.data(),
                                values, 2));
    CHECK(says("1 isn't after 1"));
    const std::array<float, 1> early{ -1.0F };
    CHECK(!fbe_clip_add_channel(clip, "a", FBE_CLIP_SCALE, FBE_CLIP_LINEAR, 3, early.data(), values,
                                1));
    CHECK(says("0 seconds or more, not -1"));
    std::array<float, 6> broken = two_places;
    broken[4] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!fbe_clip_add_channel(clip, "a", FBE_CLIP_SCALE, FBE_CLIP_LINEAR, 3, times, broken.data(),
                                2));
    CHECK(says("values are numbers"));
    // A CUBIC channel's keys are three times as wide, each with its tangents.
    std::array<float, 18> curve{};
    curve[16] = std::numeric_limits<float>::infinity();
    CHECK(!fbe_clip_add_channel(clip, "a", FBE_CLIP_SCALE, FBE_CLIP_CUBIC, 3, times, curve.data(),
                                2));
    CHECK(fbe_clip_channel_count(clip) == 0);
    // One node's property moved twice is refused; another property, or node, isn't.
    CHECK(fbe_clip_add_channel(clip, "a", FBE_CLIP_SCALE, FBE_CLIP_LINEAR, 3, times, values, 2));
    CHECK(!fbe_clip_add_channel(clip, "a", FBE_CLIP_SCALE, FBE_CLIP_STEP, 3, times, values, 2));
    CHECK(says("moves the scale of \"a\" already"));
    CHECK(fbe_clip_add_channel(clip, "a", FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3, times, values,
                               2));
    CHECK(fbe_clip_add_channel(clip, "b", FBE_CLIP_SCALE, FBE_CLIP_LINEAR, 3, times, values, 2));
    CHECK(fbe_clip_channel_count(clip) == 3);
    fbe_clip_release(clip);
}

TEST_CASE("sampling a channel out of range, or at no time, is refused") {
    const std::uint64_t clip = fbe_clip_create("", 0.0F);
    fbe_clip_add_channel(clip, "a", FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3, two_times.data(),
                         two_places.data(), 2);
    std::array<float, 3> out{};
    CHECK(fbe_clip_sample(clip, 1, 0.0F, out.data(), out.size()) == -1);
    CHECK(says("there's no channel 1"));
    CHECK(fbe_clip_sample(clip, -1, 0.0F, out.data(), out.size()) == -1);
    CHECK(fbe_clip_sample(clip, 0, 0.0F, out.data(), 2) == -1);
    CHECK(says("3 floats"));
    CHECK(fbe_clip_sample(clip, 0, std::nanf(""), out.data(), out.size()) == -1);
    fbe_clip_release(clip);
}

TEST_CASE("a clip lives while it's held, and a freed one's calls fail") {
    CHECK(fbe_clip_create("", -1.0F) == 0);
    CHECK(says("0 or more, not -1"));
    CHECK(fbe_clip_create("", std::numeric_limits<float>::infinity()) == 0);
    const std::uint64_t clip = fbe_clip_create(nullptr, 0.0F);
    CHECK(std::string_view(fbe_clip_name(clip)).empty());
    CHECK(fbe_clip_retain(clip));
    CHECK(fbe_clip_release(clip));
    CHECK(fbe_clip_alive(clip));
    CHECK(fbe_clip_release(clip));
    CHECK(!fbe_clip_alive(clip));
    CHECK(!fbe_clip_release(clip));
    CHECK(says("the clip was freed"));
    CHECK(!fbe_clip_add_channel(clip, "a", FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3,
                                two_times.data(), two_places.data(), 2));
    CHECK(std::string_view(fbe_clip_name(clip)).empty());
    CHECK(fbe_clip_length(clip) == 0.0F);
    // Another kind's handle is refused, never taken for a clip.
    const std::uint64_t world = fbe_world_create();
    CHECK(!fbe_clip_retain(world));
    fbe_world_free(world);
}

TEST_CASE("a clip's events are kept in the order of their times, and lengthen it") {
    const std::uint64_t clip = fbe_clip_create("", 0.0F);
    CHECK(fbe_clip_add_event(clip, "land", 1.0F));
    CHECK(fbe_clip_add_event(clip, "step", 0.5F));
    CHECK(fbe_clip_add_event(clip, "again", 1.0F));
    CHECK(fbe_clip_event_count(clip) == 3);
    CHECK(std::string_view(fbe_clip_event_name(clip, 0)) == "step");
    // At one time, in the order they were added.
    CHECK(std::string_view(fbe_clip_event_name(clip, 1)) == "land");
    CHECK(std::string_view(fbe_clip_event_name(clip, 2)) == "again");
    CHECK(fbe_clip_event_time(clip, 2) == 1.0F);
    CHECK(fbe_clip_length(clip) == 1.0F);
    CHECK(!fbe_clip_add_event(clip, "", 1.0F));
    CHECK(says("an event has a name"));
    CHECK(!fbe_clip_add_event(clip, "x", -0.5F));
    CHECK(says("0 seconds or more, not -0.5"));
    CHECK(!fbe_clip_add_event(clip, "x", std::nanf("")));
    CHECK(std::string_view(fbe_clip_event_name(clip, 3)).empty());
    CHECK(says("there's no event 3"));
    fbe_clip_release(clip);
}

namespace {

/// Whether channel `channel` of `part`, sampled from 0 to its length, is `clip`'s from `start`.
bool samples_as(std::uint64_t part, std::uint64_t clip, int32_t channel, float start) {
    const float length = fbe_clip_length(part);
    for (int step = 0; step <= 40; ++step) {
        const float time = length * static_cast<float>(step) / 40.0F;
        std::array<float, 4> cut{};
        std::array<float, 4> whole{};
        const int32_t width = fbe_clip_sample(part, channel, time, cut.data(), cut.size());
        fbe_clip_sample(clip, channel, start + time, whole.data(), whole.size());
        for (int32_t i = 0; i < width; ++i) {
            const auto at = static_cast<std::size_t>(i);
            if (std::abs(cut.at(at) - whole.at(at)) > 1e-4F) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

TEST_CASE("a slice of a clip samples as the clip did there, whatever its interpolation") {
    const std::uint64_t clip = fbe_clip_create("walk", 30.0F);
    const std::array<float, 3> times{ 1.0F, 2.0F, 3.5F };
    const std::array<float, 9> places{ 0, 0, 0, 4, 2, 0, -1, 5, 3 };
    fbe_clip_add_channel(clip, "a", FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3, times.data(),
                         places.data(), 3);
    fbe_clip_add_channel(clip, "a", FBE_CLIP_SCALE, FBE_CLIP_STEP, 3, times.data(), places.data(),
                         3);
    // In tangent, value, out tangent, for each of three keys, tangents not used before the first
    // key or after the last set, as a file might have them.
    const std::array<float, 27> curve{
        9,  9, 9, 0,  0, 0, 1, -2, 3, // the first key
        -1, 0, 2, 4,  2, 0, 0, 1,  0, // the second
        2,  2, 2, -1, 5, 3, 9, 9,  9, // the last
    };
    fbe_clip_add_channel(clip, "b", FBE_CLIP_SCALE, FBE_CLIP_CUBIC, 3, times.data(), curve.data(),
                         3);
    const std::array<float, 36> turning{
        0, 0,    0, 0, 0, 0, 0, 1, 0, 0.2F, 0,    0, 0, 0,    0, 0, 0, 0.7F,
        0, 0.7F, 0, 0, 0, 0, 0, 0, 0, 0,    0.7F, 0, 0, 0.7F, 0, 0, 0, 0,
    };
    fbe_clip_add_channel(clip, "b", FBE_CLIP_ROTATION, FBE_CLIP_CUBIC, 4, times.data(),
                         turning.data(), 3);
    fbe_clip_add_event(clip, "early", 0.2F);
    fbe_clip_add_event(clip, "start", 1.5F);
    fbe_clip_add_event(clip, "middle", 2.0F);
    fbe_clip_add_event(clip, "end", 2.75F);
    fbe_clip_add_event(clip, "late", 3.0F);
    // A part across keys, one cut at a key, and one from before the first key to after the last.
    for (const auto& [start, end] : std::array<std::pair<float, float>, 3>{
             { { 1.5F, 2.75F }, { 2.0F, 3.5F }, { 0.25F, 4.5F } },
         }) {
        const std::uint64_t part = fbe_clip_slice(clip, start, end);
        REQUIRE(part != 0);
        CHECK(fbe_clip_length(part) == Catch::Approx(end - start));
        CHECK(fbe_clip_frame_rate(part) == 30.0F);
        CHECK(std::string_view(fbe_clip_name(part)) == "walk");
        for (int32_t channel = 0; channel < 4; ++channel) {
            CHECK(samples_as(part, clip, channel, start));
        }
        fbe_clip_release(part);
    }
    // Its events between its ends, or at them, from its start.
    const std::uint64_t part = fbe_clip_slice(clip, 1.5F, 2.75F);
    REQUIRE(fbe_clip_event_count(part) == 3);
    CHECK(std::string_view(fbe_clip_event_name(part, 0)) == "start");
    CHECK(fbe_clip_event_time(part, 0) == 0.0F);
    CHECK(fbe_clip_event_time(part, 1) == 0.5F);
    CHECK(std::string_view(fbe_clip_event_name(part, 2)) == "end");
    fbe_clip_release(part);
    CHECK(fbe_clip_slice(clip, -1.0F, 1.0F) == 0);
    CHECK(says("not -1 to 1"));
    CHECK(fbe_clip_slice(clip, 2.0F, 1.0F) == 0);
    CHECK(fbe_clip_slice(clip, 0.0F, std::numeric_limits<float>::infinity()) == 0);
    // A part of no length is its start's keys.
    const std::uint64_t moment = fbe_clip_slice(clip, 2.0F, 2.0F);
    CHECK(fbe_clip_length(moment) == 0.0F);
    CHECK(samples_as(moment, clip, 0, 2.0F));
    fbe_clip_release(moment);
    fbe_clip_release(clip);
}
