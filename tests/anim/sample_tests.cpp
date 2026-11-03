// Sampling a clip's channels, apart from the DLL: each interpolation, the ends, a rotation the
// short way, glTF's cubic spline, and finding keys playing on and after a jump.

#include "clip/clip.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

using fbe::anim::channel_t;
using fbe::anim::interpolation_t;
using fbe::anim::key_at;
using fbe::anim::property_t;
using fbe::anim::sample;

namespace {

channel_t channel(property_t property, interpolation_t interpolation, std::uint32_t width,
                  std::vector<float> times, std::vector<float> values) {
    channel_t made;
    made.property = property;
    made.interpolation = interpolation;
    made.width = width;
    made.times = std::move(times);
    made.values = std::move(values);
    return made;
}

std::array<float, 4> at(const channel_t& from, float time) {
    std::array<float, 4> out{};
    std::size_t cursor = 0;
    sample(from, time, cursor, out);
    return out;
}

} // namespace

TEST_CASE("a linear channel goes in a line between its keys, and holds its ends") {
    const channel_t moving = channel(property_t::TRANSLATION, interpolation_t::LINEAR, 3,
                                     { 1.0F, 3.0F }, { 0.0F, 0.0F, 0.0F, 4.0F, -2.0F, 8.0F });
    CHECK(at(moving, 2.0F)[0] == Catch::Approx(2.0F));
    CHECK(at(moving, 2.0F)[1] == Catch::Approx(-1.0F));
    CHECK(at(moving, 2.5F)[2] == Catch::Approx(6.0F));
    // Before the first key, and after the last, each holds.
    CHECK(at(moving, 0.0F)[0] == 0.0F);
    CHECK(at(moving, -5.0F)[2] == 0.0F);
    CHECK(at(moving, 3.0F)[0] == 4.0F);
    CHECK(at(moving, 100.0F)[1] == -2.0F);
}

TEST_CASE("a step channel holds the key before, until the next") {
    const channel_t stepping =
        channel(property_t::SCALE, interpolation_t::STEP, 3, { 0.0F, 1.0F, 2.0F },
                { 1.0F, 1.0F, 1.0F, 2.0F, 2.0F, 2.0F, 3.0F, 3.0F, 3.0F });
    CHECK(at(stepping, 0.5F)[0] == 1.0F);
    CHECK(at(stepping, 0.999F)[1] == 1.0F);
    CHECK(at(stepping, 1.0F)[2] == 2.0F);
    CHECK(at(stepping, 1.5F)[0] == 2.0F);
    CHECK(at(stepping, 7.0F)[0] == 3.0F);
}

TEST_CASE("a rotation goes the short way round, at length 1") {
    // From no turn to a quarter turn about Y written as its negative, -q, the same rotation, which
    // a line through the four floats would go the long way round to.
    const float half = std::sqrt(0.5F);
    const channel_t turning =
        channel(property_t::ROTATION, interpolation_t::LINEAR, 4, { 0.0F, 1.0F },
                { 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, -half, 0.0F, -half });
    const std::array<float, 4> middle = at(turning, 0.5F);
    // Halfway to a quarter turn is an eighth: sin and cos of a sixteenth of a turn.
    const float length = std::sqrt((middle[0] * middle[0]) + (middle[1] * middle[1]) +
                                   (middle[2] * middle[2]) + (middle[3] * middle[3]));
    CHECK(length == Catch::Approx(1.0F));
    CHECK(std::abs(middle[1]) == Catch::Approx(std::sin(0.3926991F)));
    CHECK(std::abs(middle[3]) == Catch::Approx(std::cos(0.3926991F)));
    // Its keys' own values come back at length 1, as they're given in a file.
    const channel_t long_key = channel(property_t::ROTATION, interpolation_t::STEP, 4, { 0.0F },
                                       { 0.0F, 0.0F, 0.0F, 2.0F });
    CHECK(at(long_key, 0.0F)[3] == Catch::Approx(1.0F));
}

TEST_CASE("a cubic channel is glTF's spline, its tangents scaled by the time between keys") {
    // One float, keys 2 seconds apart at 0 and 0, each written in tangent, value, out tangent.
    const channel_t rising = channel(property_t::WEIGHTS, interpolation_t::CUBIC, 1, { 0.0F, 2.0F },
                                     { 0.0F, 0.0F, 1.0F, -1.0F, 0.0F, 0.0F });
    // Halfway, s = 0.5: (s^3 - 2s^2 + s) * 2 * 1 + (s^3 - s^2) * 2 * -1 = 0.25 + 0.25.
    CHECK(at(rising, 1.0F)[0] == Catch::Approx(0.5F));
    // At a key, its value, not its tangents.
    CHECK(at(rising, 0.0F)[0] == 0.0F);
    CHECK(at(rising, 2.0F)[0] == 0.0F);
    CHECK(at(rising, 9.0F)[0] == 0.0F);
    // With no tangents, it eases between values: 3s^2 - 2s^3 of the way.
    const channel_t easing = channel(property_t::WEIGHTS, interpolation_t::CUBIC, 1, { 0.0F, 1.0F },
                                     { 0.0F, 0.0F, 0.0F, 0.0F, 8.0F, 0.0F });
    CHECK(at(easing, 0.25F)[0] == Catch::Approx(8.0F * ((3.0F * 0.0625F) - (2.0F * 0.015625F))));
}

TEST_CASE("weights are as wide as a channel's targets") {
    const channel_t weights = channel(property_t::WEIGHTS, interpolation_t::LINEAR, 2,
                                      { 0.0F, 1.0F }, { 0.0F, 1.0F, 1.0F, 0.0F });
    const std::array<float, 4> middle = at(weights, 0.25F);
    CHECK(middle[0] == Catch::Approx(0.25F));
    CHECK(middle[1] == Catch::Approx(0.75F));
}

TEST_CASE("keys are found from the last one, playing on, and searched for after a jump") {
    std::vector<float> times;
    times.reserve(100);
    for (int i = 0; i < 100; ++i) {
        times.push_back(static_cast<float>(i) * 0.1F);
    }
    CHECK(key_at(times, -1.0F, 50) == 0);
    CHECK(key_at(times, 0.0F, 50) == 0);
    CHECK(key_at(times, 0.15F, 0) == 1);
    // On from where it was.
    CHECK(key_at(times, 0.25F, 1) == 2);
    // Far on, and back.
    CHECK(key_at(times, 8.05F, 2) == 80);
    CHECK(key_at(times, 0.35F, 80) == 3);
    CHECK(key_at(times, 50.0F, 3) == 99);
    // A hint past the end is searched from nothing.
    CHECK(key_at(times, 0.55F, 1000) == 5);
    // Playing on through a channel, the cursor follows its keys.
    const channel_t moving =
        channel(property_t::TRANSLATION, interpolation_t::LINEAR, 3, { 0.0F, 1.0F, 2.0F, 3.0F },
                { 0, 0, 0, 1, 0, 0, 2, 0, 0, 3, 0, 0 });
    std::size_t cursor = 0;
    std::array<float, 3> out{};
    for (std::size_t quarter = 0; quarter < 12; ++quarter) {
        const float time = static_cast<float>(quarter) * 0.25F;
        sample(moving, time, cursor, out);
        CHECK(out[0] == Catch::Approx(time));
        CHECK(cursor == quarter / 4);
    }
    sample(moving, 0.5F, cursor, out);
    CHECK(out[0] == Catch::Approx(0.5F));
    CHECK(cursor == 0);
}

TEST_CASE("a step rotation between keys comes back at length 1") {
    const channel_t stepping =
        channel(property_t::ROTATION, interpolation_t::STEP, 4, { 0.0F, 1.0F },
                { 0.0F, 0.0F, 2.0F, 0.0F, 0.0F, 2.0F, 0.0F, 0.0F });
    CHECK(at(stepping, 0.5F)[2] == Catch::Approx(1.0F));
}
