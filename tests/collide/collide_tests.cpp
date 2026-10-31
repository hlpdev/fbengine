// collide's masks, apart from the DLL: what the fast paths (whole words of rows, each row's span)
// give against a brute force over every screen pixel, on seeded random masks and placements, the
// handles' and the canvas's rules on whole pixels, and how long many sprites take.

#include "collide/fbe_collide.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <numbers>
#include <random>
#include <vector>

namespace {

/// A place, as the C API takes it.
fbe_collide_place_t place_of(float x, float y, float handle_x, float handle_y, float rotation,
                             float scale_x, float scale_y) {
    return {
        .x = x,
        .y = y,
        .handle_x = handle_x,
        .handle_y = handle_y,
        .rotation = rotation,
        .scale_x = scale_x,
        .scale_y = scale_y,
    };
}

fbe_collide_rect_t rect_of(float x, float y, float width, float height) {
    return { .x = x, .y = y, .width = width, .height = height };
}

/// A mask's pixels as the test keeps them: solid or not, and its handle.
struct pixels_t {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<bool> solid;
    std::uint64_t mask = 0;
};

/// RGBA8 pixels of `width` by `height`, each solid with chance `fill`, and their mask.
pixels_t made(std::int32_t width, std::int32_t height, double fill, std::mt19937& random) {
    pixels_t made;
    made.width = width;
    made.height = height;
    std::bernoulli_distribution solid(fill);
    std::uniform_int_distribution<int> faint(0, 127);
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> rgba(count * 4, 0);
    for (std::size_t at = 0; at < count; ++at) {
        const bool is = solid(random);
        made.solid.push_back(is);
        // Below the threshold, 128, isn't solid, whatever its color.
        rgba[(at * 4) + 3] = static_cast<std::uint8_t>(is ? 128 + faint(random) : faint(random));
        rgba[at * 4] = 200;
    }
    made.mask =
        fbe_collide_mask(rgba.data(), width, height, static_cast<std::int64_t>(width) * 4, 128);
    REQUIRE(made.mask != 0);
    return made;
}

/// Whether screen pixel `x`, `y` is solid for `pixels` placed at `place`, worked out from the
/// canvas's own forward rule: a mask pixel p is drawn at position + turn((p - handle) * scale).
bool reference(const pixels_t& pixels, const fbe_collide_place_t& place, std::int64_t x,
               std::int64_t y) {
    if (place.scale_x == 0.0F || place.scale_y == 0.0F) {
        return false;
    }
    const double turn = static_cast<double>(place.rotation) * std::numbers::pi / 180.0;
    // The pixel's middle, from the position, turned back, and scaled back.
    const double dx = (static_cast<double>(x) + 0.5) - place.x;
    const double dy = (static_cast<double>(y) + 0.5) - place.y;
    const double back_x = (std::cos(turn) * dx) + (std::sin(turn) * dy);
    const double back_y = (-std::sin(turn) * dx) + (std::cos(turn) * dy);
    const double u = (back_x / place.scale_x) + place.handle_x;
    const double v = (back_y / place.scale_y) + place.handle_y;
    if (u < 0.0 || v < 0.0 || u >= pixels.width || v >= pixels.height) {
        return false;
    }
    const auto column = static_cast<std::size_t>(std::floor(u));
    const auto row = static_cast<std::size_t>(std::floor(v));
    return pixels.solid[(row * static_cast<std::size_t>(pixels.width)) + column];
}

/// The screen pixels a placed mask may reach, from its bounds, a few more each way.
void reach(const pixels_t& pixels, const fbe_collide_place_t& place, std::int64_t& left,
           std::int64_t& top, std::int64_t& right, std::int64_t& bottom) {
    fbe_collide_rect_t bounds{};
    REQUIRE(fbe_collide_bounds(pixels.mask, &place, &bounds));
    left = static_cast<std::int64_t>(std::floor(bounds.x)) - 2;
    top = static_cast<std::int64_t>(std::floor(bounds.y)) - 2;
    right = static_cast<std::int64_t>(std::ceil(bounds.x + bounds.width)) + 2;
    bottom = static_cast<std::int64_t>(std::ceil(bounds.y + bounds.height)) + 2;
}

/// A random place: a position with fractions, a handle, and a rotation and scale, plain a third of
/// the time, quarter turns and mirrors a third, and anything the rest.
fbe_collide_place_t placed(const pixels_t& pixels, std::mt19937& random) {
    std::uniform_real_distribution<float> at(-40.0F, 40.0F);
    std::uniform_real_distribution<float> any(-1.0F, 1.0F);
    std::uniform_int_distribution<int> kind(0, 2);
    std::uniform_int_distribution<int> quarter(0, 3);
    const float x = at(random);
    const float y = at(random);
    const float handle_x = (static_cast<float>(pixels.width) * (any(random) + 1.0F)) / 2.0F;
    const float handle_y = (static_cast<float>(pixels.height) * (any(random) + 1.0F)) / 2.0F;
    fbe_collide_place_t place = place_of(x, y, handle_x, handle_y, 0.0F, 1.0F, 1.0F);
    const int which = kind(random);
    if (which == 1) {
        place.rotation = static_cast<float>(quarter(random) * 90);
        place.scale_x = any(random) < 0.0F ? -1.0F : 1.0F;
        place.scale_y = any(random) < 0.0F ? -2.0F : 1.0F;
    } else if (which == 2) {
        place.rotation = any(random) * 360.0F;
        place.scale_x = any(random) * 2.5F;
        place.scale_y = any(random) * 2.5F;
    }
    return place;
}

} // namespace

TEST_CASE("placed masks meet where a brute force over every pixel says they do") {
    // Seeded, so a failure comes again.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 random(20261031);
    std::uniform_int_distribution<int> size(1, 140);
    std::uniform_real_distribution<double> fill(0.02, 0.6);
    std::size_t met = 0;
    std::size_t cases = 0;
    for (int round = 0; round < 300; ++round) {
        // Drawn one at a time, so every compiler draws them in the same order.
        const int wide = size(random);
        const int low = (size(random) / 2) + 1;
        const pixels_t a = made(wide, low, fill(random), random);
        const int narrow = (size(random) / 2) + 1;
        const int tall = size(random);
        const pixels_t b = made(narrow, tall, fill(random), random);
        for (int pair = 0; pair < 4; ++pair) {
            const fbe_collide_place_t at_a = placed(a, random);
            const fbe_collide_place_t at_b = placed(b, random);
            std::int64_t left = 0;
            std::int64_t top = 0;
            std::int64_t right = 0;
            std::int64_t bottom = 0;
            reach(a, at_a, left, top, right, bottom);
            bool expected = false;
            for (std::int64_t y = top; y < bottom && !expected; ++y) {
                for (std::int64_t x = left; x < right && !expected; ++x) {
                    expected = reference(a, at_a, x, y) && reference(b, at_b, x, y);
                }
            }
            INFO("round " << round << " pair " << pair << ": a " << a.width << "x" << a.height
                          << " at " << at_a.x << "," << at_a.y << " turned " << at_a.rotation
                          << " scaled " << at_a.scale_x << "," << at_a.scale_y << "; b " << b.width
                          << "x" << b.height << " at " << at_b.x << "," << at_b.y << " turned "
                          << at_b.rotation << " scaled " << at_b.scale_x << "," << at_b.scale_y);
            CHECK(fbe_collide_masks(a.mask, &at_a, b.mask, &at_b) == (expected ? 1 : 0));
            CHECK(fbe_collide_masks(b.mask, &at_b, a.mask, &at_a) == (expected ? 1 : 0));
            met += expected ? 1 : 0;
            ++cases;
            // A rectangle over part of it, and a point.
            std::uniform_real_distribution<float> corner(-45.0F, 45.0F);
            std::uniform_real_distribution<float> side(0.0F, 30.0F);
            const float rect_x = corner(random);
            const float rect_y = corner(random);
            const float rect_width = side(random);
            const fbe_collide_rect_t rect = rect_of(rect_x, rect_y, rect_width, side(random));
            bool inside = false;
            const auto from_x = static_cast<std::int64_t>(std::floor(rect.x));
            const auto to_x = static_cast<std::int64_t>(std::ceil(rect.x + rect.width));
            const auto from_y = static_cast<std::int64_t>(std::floor(rect.y));
            const auto to_y = static_cast<std::int64_t>(std::ceil(rect.y + rect.height));
            for (std::int64_t y = from_y; y < to_y && !inside; ++y) {
                for (std::int64_t x = from_x; x < to_x && !inside; ++x) {
                    inside = reference(a, at_a, x, y);
                }
            }
            CHECK(fbe_collide_rect(a.mask, &at_a, &rect) == (inside ? 1 : 0));
            const float point_x = corner(random);
            const float point_y = corner(random);
            const bool on = reference(a, at_a, static_cast<std::int64_t>(std::floor(point_x)),
                                      static_cast<std::int64_t>(std::floor(point_y)));
            CHECK(fbe_collide_point(a.mask, &at_a, point_x, point_y) == (on ? 1 : 0));
        }
        fbe_collide_free(a.mask);
        fbe_collide_free(b.mask);
    }
    // Both kinds of answer came up often.
    CHECK(met > cases / 5);
    CHECK(met < cases - (cases / 5));
    CHECK(fbe_collide_count() == 0);
}

TEST_CASE("a mask's pixels, threshold, and refusals") {
    std::vector<std::uint8_t> rgba(std::size_t{ 24 }, 0);
    rgba[3] = 255;
    rgba[(4 * 4) + 3] = 10;
    const std::uint64_t mask = fbe_collide_mask(rgba.data(), 3, 2, 12, 10);
    REQUIRE(mask != 0);
    CHECK(fbe_collide_width(mask) == 3);
    CHECK(fbe_collide_height(mask) == 2);
    CHECK(fbe_collide_solid_count(mask) == 2);
    CHECK(fbe_collide_solid(mask, 0, 0));
    CHECK(fbe_collide_solid(mask, 1, 1));
    CHECK_FALSE(fbe_collide_solid(mask, 2, 1));
    CHECK_FALSE(fbe_collide_solid(mask, -1, 0));
    CHECK_FALSE(fbe_collide_solid(mask, 3, 0));
    // Touching isn't meeting: the next pixel along doesn't meet it, its own does.
    const fbe_collide_place_t at = place_of(10.0F, 10.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F);
    const fbe_collide_place_t beside = place_of(13.0F, 10.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F);
    const fbe_collide_place_t over = place_of(11.0F, 11.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F);
    CHECK(fbe_collide_masks(mask, &at, mask, &beside) == 0);
    CHECK(fbe_collide_masks(mask, &at, mask, &over) == 1);
    const fbe_collide_rect_t touching = rect_of(13.0F, 10.0F, 5.0F, 5.0F);
    const fbe_collide_rect_t reaching = rect_of(10.5F, 9.0F, 0.1F, 1.1F);
    CHECK(fbe_collide_rect(mask, &at, &touching) == 0);
    CHECK(fbe_collide_rect(mask, &at, &reaching) == 1);
    CHECK(fbe_collide_point(mask, &at, 10.99F, 10.0F) == 1);
    CHECK(fbe_collide_point(mask, &at, 11.0F, 10.0F) == 0);
    // Refused: a bad size, stride, or threshold, a place that isn't finite, a freed mask.
    CHECK(fbe_collide_mask(rgba.data(), 3, 2, 11, 10) == 0);
    CHECK(fbe_collide_mask(rgba.data(), 3, 2, 12, 0) == 0);
    CHECK(fbe_collide_mask(rgba.data(), -1, 2, 12, 10) == 0);
    // An empty one is a mask, of nothing.
    const std::uint64_t empty = fbe_collide_mask(nullptr, 0, 0, 0, 10);
    CHECK(empty != 0);
    CHECK(fbe_collide_masks(empty, &at, mask, &at) == 0);
    CHECK(fbe_collide_free(empty));
    fbe_collide_place_t bad = at;
    bad.rotation = NAN;
    CHECK(fbe_collide_masks(mask, &at, mask, &bad) == -1);
    CHECK(fbe_collide_free(mask));
    CHECK(fbe_collide_masks(mask, &at, mask, &at) == -1);
    CHECK(fbe_collide_width(mask) == -1);
}

TEST_CASE("many sprites take little time") {
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 random(7);
    const pixels_t ship = made(32, 32, 0.5, random);
    const pixels_t rock = made(48, 48, 0.5, random);
    std::uniform_real_distribution<float> at(0.0F, 1280.0F);
    std::vector<fbe_collide_place_t> rocks;
    rocks.reserve(1000);
    for (int each = 0; each < 1000; ++each) {
        rocks.push_back(place_of(at(random), at(random) * 0.5625F, 24.0F, 24.0F, 0.0F, 1.0F, 1.0F));
    }
    int hits = 0;
    const auto started = std::chrono::steady_clock::now();
    // A ship against 1,000 rocks, 100 times, plain, then turned.
    for (int frame = 0; frame < 100; ++frame) {
        const fbe_collide_place_t place =
            place_of(at(random), at(random) * 0.5625F, 16.0F, 16.0F, 0.0F, 1.0F, 1.0F);
        for (const fbe_collide_place_t& each : rocks) {
            hits += fbe_collide_masks(ship.mask, &place, rock.mask, &each);
        }
    }
    const auto plain = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 100; ++frame) {
        const fbe_collide_place_t place = place_of(at(random), at(random) * 0.5625F, 16.0F, 16.0F,
                                                   static_cast<float>(frame) * 3.7F, 1.0F, 1.0F);
        for (const fbe_collide_place_t& each : rocks) {
            hits += fbe_collide_masks(ship.mask, &place, rock.mask, &each);
        }
    }
    const auto turned = std::chrono::steady_clock::now();
    // Ten ships over one rock each, overlapping, plain and turned.
    const fbe_collide_place_t under = place_of(100.0F, 100.0F, 24.0F, 24.0F, 0.0F, 1.0F, 1.0F);
    const fbe_collide_place_t near = place_of(101.5F, 99.25F, 16.0F, 16.0F, 0.0F, 1.0F, 1.0F);
    const fbe_collide_place_t near_turned =
        place_of(101.5F, 99.25F, 16.0F, 16.0F, 33.0F, 1.0F, 1.0F);
    for (int each = 0; each < 10000; ++each) {
        hits += fbe_collide_masks(ship.mask, &near, rock.mask, &under);
    }
    const auto overlapping = std::chrono::steady_clock::now();
    for (int each = 0; each < 10000; ++each) {
        hits += fbe_collide_masks(ship.mask, &near_turned, rock.mask, &under);
    }
    const auto done = std::chrono::steady_clock::now();
    const auto ms = [](auto from, auto to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    std::fputs(std::format("100,000 pairs, mostly apart: {:.2f} ms plain, {:.2f} ms turned; 10,000 "
                           "overlapping: {:.2f} ms plain, {:.2f} ms turned ({} hits)\n",
                           ms(started, plain), ms(plain, turned), ms(turned, overlapping),
                           ms(overlapping, done), hits)
                   .c_str(),
               stdout);
    CHECK(hits > 0);
    fbe_collide_free(ship.mask);
    fbe_collide_free(rock.mask);
}
