// Clips through the C API: made a channel at a time, what's refused and why, what they say of
// themselves, sampling, and living while they're held.

#include "fbengine.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

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
