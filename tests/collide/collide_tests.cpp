// collide's masks, apart from the DLL: what the fast paths (whole words of rows, each row's span)
// give against a brute force over every screen pixel, on seeded random masks and placements, the
// handles' and the canvas's rules on whole pixels, and how long many sprites take.

#include "collide/fbe_collide.h"

#include <array>
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

/// Pi, and a quarter turn, in radians, as a game's floats have them.
constexpr auto pi = static_cast<float>(std::numbers::pi);
constexpr float half_pi = pi / 2.0F;

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

/// Whether screen pixel `x`, `y` is solid for `pixels` placed at `place`, as the per-pixel answer,
/// fbe_collide_point at its middle, gives it.
bool reference(const pixels_t& pixels, const fbe_collide_place_t& place, std::int64_t x,
               std::int64_t y) {
    const int hit = fbe_collide_point(pixels.mask, &place, static_cast<float>(x) + 0.5F,
                                      static_cast<float>(y) + 0.5F);
    REQUIRE(hit >= 0);
    return hit == 1;
}

/// Whether a GPU draws screen pixel `x`, `y` of `pixels` placed at `place` as the canvas does: its
/// corners as floats (the canvas's own arithmetic), snapped to the 1/256 pixel a rasterizer takes,
/// its two triangles (0 1 2 and 0 2 3) covering the pixel's middle by the top-left rule, and its uv
/// there, kept exact, sampled NEAREST, clamped.
bool gpu_at(const pixels_t& pixels, const fbe_collide_place_t& place, std::int64_t x,
            std::int64_t y) {
    // The canvas's own: the cosine and sine of the float it's given, in radians.
    const auto turn = static_cast<double>(place.rotation);
    const std::array<std::array<float, 2>, 4> part{
        {
            { 0.0F, 0.0F },
            { static_cast<float>(pixels.width), 0.0F },
            { static_cast<float>(pixels.width), static_cast<float>(pixels.height) },
            { 0.0F, static_cast<float>(pixels.height) },
        },
    };
    std::array<std::int64_t, 4> cx{};
    std::array<std::int64_t, 4> cy{};
    for (std::size_t at = 0; at < 4; ++at) {
        const double from_x = (part[at][0] - place.handle_x) * static_cast<double>(place.scale_x);
        const double from_y = (part[at][1] - place.handle_y) * static_cast<double>(place.scale_y);
        const auto corner_x =
            static_cast<float>(place.x + (from_x * std::cos(turn)) - (from_y * std::sin(turn)));
        const auto corner_y =
            static_cast<float>(place.y + (from_x * std::sin(turn)) + (from_y * std::cos(turn)));
        cx[at] = std::llround(static_cast<double>(corner_x) * 256.0);
        cy[at] = std::llround(static_cast<double>(corner_y) * 256.0);
    }
    const std::int64_t px = (x * 256) + 128;
    const std::int64_t py = (y * 256) + 128;
    const std::array<std::array<std::size_t, 3>, 2> triangles{ { { 0, 1, 2 }, { 0, 2, 3 } } };
    const std::array<std::int64_t, 4> u_of{ 0, 1, 1, 0 };
    const std::array<std::int64_t, 4> v_of{ 0, 0, 1, 1 };
    for (const auto& triangle : triangles) {
        std::array<std::size_t, 3> v = triangle;
        const std::int64_t area = ((cx[v[1]] - cx[v[0]]) * (cy[v[2]] - cy[v[0]])) -
                                  ((cy[v[1]] - cy[v[0]]) * (cx[v[2]] - cx[v[0]]));
        if (area == 0) {
            continue;
        }
        if (area < 0) {
            std::swap(v[1], v[2]);
        }
        std::array<std::int64_t, 3> weight{};
        bool covered = true;
        for (std::size_t edge = 0; edge < 3 && covered; ++edge) {
            const std::size_t from = v[(edge + 1) % 3];
            const std::size_t to = v[(edge + 2) % 3];
            const std::int64_t dx = cx[to] - cx[from];
            const std::int64_t dy = cy[to] - cy[from];
            const std::int64_t value = (dx * (py - cy[from])) - (dy * (px - cx[from]));
            // Its inward normal is (-dy, dx): a left edge's points right, a top edge's down.
            const bool top_left = -dy > 0 || (dy == 0 && dx > 0);
            covered = value > 0 || (value == 0 && top_left);
            weight[edge] = value;
        }
        if (!covered) {
            continue;
        }
        const std::int64_t total = weight[0] + weight[1] + weight[2];
        std::int64_t u = 0;
        std::int64_t w = 0;
        for (std::size_t at = 0; at < 3; ++at) {
            u += weight[at] * u_of[v[at]];
            w += weight[at] * v_of[v[at]];
        }
        const std::int64_t column =
            std::clamp<std::int64_t>((u * pixels.width) / total, 0, pixels.width - 1);
        const std::int64_t row =
            std::clamp<std::int64_t>((w * pixels.height) / total, 0, pixels.height - 1);
        return pixels.solid[static_cast<std::size_t>((row * pixels.width) + column)];
    }
    return false;
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

/// A random place: half the time on the GPU's grid, its position on quarter pixels, its handle
/// on halves, ends, or the middle, a quarter turn or none, and a scale of a half, 1, or 2, either
/// way; and the rest anything, plain a third of that, quarter turns and mirrors a third, and any
/// turn and scale the rest. Whether it's on the grid, where the GPU's answer is exact.
fbe_collide_place_t placed(const pixels_t& pixels, std::mt19937& random, bool& gridded) {
    std::uniform_real_distribution<float> at(-40.0F, 40.0F);
    std::uniform_real_distribution<float> any(-1.0F, 1.0F);
    std::uniform_int_distribution<int> whole(-40, 40);
    std::uniform_int_distribution<int> kind(0, 5);
    std::uniform_int_distribution<int> quarter(-1, 4);
    std::uniform_int_distribution<int> four(0, 3);
    const int which = kind(random);
    gridded = which >= 3;
    if (gridded) {
        const auto x =
            static_cast<float>(whole(random)) + (static_cast<float>(four(random)) / 4.0F);
        const auto y =
            static_cast<float>(whole(random)) + (static_cast<float>(four(random)) / 4.0F);
        const std::array<float, 4> handles_x{
            0.0F,
            static_cast<float>(pixels.width) / 2.0F,
            static_cast<float>(pixels.width),
            0.5F,
        };
        const std::array<float, 4> handles_y{
            0.0F,
            static_cast<float>(pixels.height) / 2.0F,
            static_cast<float>(pixels.height),
            1.5F,
        };
        const float handle_x = handles_x[static_cast<std::size_t>(four(random))];
        const float handle_y = handles_y[static_cast<std::size_t>(four(random))];
        const std::array<float, 6> scales{ 1.0F, -1.0F, 2.0F, -2.0F, 0.5F, -0.5F };
        std::uniform_int_distribution<int> scale(0, 5);
        const auto rotation = static_cast<float>(quarter(random)) * half_pi;
        const float scale_x = scales[static_cast<std::size_t>(scale(random))];
        return place_of(x, y, handle_x, handle_y, rotation, scale_x,
                        scales[static_cast<std::size_t>(scale(random))]);
    }
    const float x = at(random);
    const float y = at(random);
    const float handle_x = (static_cast<float>(pixels.width) * (any(random) + 1.0F)) / 2.0F;
    const float handle_y = (static_cast<float>(pixels.height) * (any(random) + 1.0F)) / 2.0F;
    fbe_collide_place_t place = place_of(x, y, handle_x, handle_y, 0.0F, 1.0F, 1.0F);
    if (which == 1) {
        place.rotation = static_cast<float>(four(random)) * half_pi;
        place.scale_x = any(random) < 0.0F ? -1.0F : 1.0F;
        place.scale_y = any(random) < 0.0F ? -2.0F : 1.0F;
    } else if (which == 2) {
        place.rotation = any(random) * 2.0F * pi;
        place.scale_x = any(random) * 2.5F;
        place.scale_y = any(random) * 2.5F;
    }
    return place;
}

} // namespace

TEST_CASE(
    "placed masks meet where a brute force over every pixel says they do, as a GPU draws them") {
    // Seeded, so a failure comes again.
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 random(20261031);
    std::uniform_int_distribution<int> size(1, 140);
    std::uniform_real_distribution<double> fill(0.02, 0.6);
    std::size_t met = 0;
    std::size_t cases = 0;
    std::size_t gridded_cases = 0;
    for (int round = 0; round < 300; ++round) {
        // Drawn one at a time, so every compiler draws them in the same order.
        const int wide = size(random);
        const int low = (size(random) / 2) + 1;
        const pixels_t a = made(wide, low, fill(random), random);
        const int narrow = (size(random) / 2) + 1;
        const int tall = size(random);
        const pixels_t b = made(narrow, tall, fill(random), random);
        for (int pair = 0; pair < 4; ++pair) {
            bool gridded_a = false;
            bool gridded_b = false;
            const fbe_collide_place_t at_a = placed(a, random, gridded_a);
            const fbe_collide_place_t at_b = placed(b, random, gridded_b);
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
            // On the grid, each pixel is the GPU's, ties and all.
            if (gridded_a) {
                ++gridded_cases;
                std::int64_t differ = 0;
                for (std::int64_t y = top; y < bottom; ++y) {
                    for (std::int64_t x = left; x < right; ++x) {
                        differ += reference(a, at_a, x, y) != gpu_at(a, at_a, x, y) ? 1 : 0;
                    }
                }
                CHECK(differ == 0);
            }
        }
        fbe_collide_free(a.mask);
        fbe_collide_free(b.mask);
    }
    // Both kinds of answer came up often.
    CHECK(met > cases / 5);
    CHECK(met < cases - (cases / 5));
    CHECK(gridded_cases > 300);
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
                                                   static_cast<float>(frame) * 0.0646F, 1.0F, 1.0F);
        for (const fbe_collide_place_t& each : rocks) {
            hits += fbe_collide_masks(ship.mask, &place, rock.mask, &each);
        }
    }
    const auto turned = std::chrono::steady_clock::now();
    // Ten ships over one rock each, overlapping, plain and turned.
    const fbe_collide_place_t under = place_of(100.0F, 100.0F, 24.0F, 24.0F, 0.0F, 1.0F, 1.0F);
    const fbe_collide_place_t near = place_of(101.5F, 99.25F, 16.0F, 16.0F, 0.0F, 1.0F, 1.0F);
    const fbe_collide_place_t near_turned =
        place_of(101.5F, 99.25F, 16.0F, 16.0F, 0.576F, 1.0F, 1.0F);
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

TEST_CASE("an odd sprite, mid-handled, mirrored, and turned, is the GPU's pixels") {
    // 5 by 3, every pixel solid but its first column, its handle at 2.5, 1.5: its edges' middles
    // land on pixels' middles.
    std::vector<std::uint8_t> rgba(std::size_t{ 60 }, 0);
    pixels_t ship;
    ship.width = 5;
    ship.height = 3;
    for (std::size_t at = 0; at < 15; ++at) {
        const bool solid = at % 5 != 0;
        ship.solid.push_back(solid);
        rgba[(at * 4) + 3] = solid ? 255 : 0;
    }
    ship.mask = fbe_collide_mask(rgba.data(), 5, 3, 20, 1);
    REQUIRE(ship.mask != 0);
    for (const float rotation : { 0.0F, half_pi, pi, 3.0F * half_pi, -half_pi, 2.0F * pi }) {
        for (const float scale : { 1.0F, -1.0F }) {
            const fbe_collide_place_t place =
                place_of(10.0F, 10.0F, 2.5F, 1.5F, rotation, scale, 1.0F);
            for (std::int64_t y = 4; y < 17; ++y) {
                for (std::int64_t x = 4; x < 17; ++x) {
                    INFO("turned " << rotation << " scaled " << scale << " at " << x << "," << y);
                    CHECK(reference(ship, place, x, y) == gpu_at(ship, place, x, y));
                }
            }
        }
    }
    fbe_collide_free(ship.mask);
}

TEST_CASE("a quarter turn's pixels agree, by point, rectangle, and mask") {
    std::vector<std::uint8_t> rgba(std::size_t{ 92 }, 255);
    const std::uint64_t bar = fbe_collide_mask(rgba.data(), 1, 23, 4, 1);
    const std::uint64_t dot = fbe_collide_mask(rgba.data(), 1, 1, 4, 1);
    const fbe_collide_place_t place = place_of(17.0F, -3.0F, 1.0F, 23.0F, half_pi, 0.5F, 2.0F);
    int solid = 0;
    for (std::int64_t y = -20; y < 20; ++y) {
        for (std::int64_t x = -40; x < 60; ++x) {
            const auto fx = static_cast<float>(x);
            const auto fy = static_cast<float>(y);
            const int point = fbe_collide_point(bar, &place, fx + 0.5F, fy + 0.5F);
            const fbe_collide_rect_t inside = rect_of(fx + 0.25F, fy + 0.25F, 0.5F, 0.5F);
            const fbe_collide_place_t on = place_of(fx, fy, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F);
            INFO("pixel " << x << "," << y);
            CHECK(fbe_collide_rect(bar, &place, &inside) == point);
            CHECK(fbe_collide_masks(bar, &place, dot, &on) == point);
            solid += point;
        }
    }
    // 23 pixels down, scaled 2, turned to across, scaled a half: 46 by a half, so 46 pixels'
    // middles, or none, along one row.
    CHECK(solid > 0);
    fbe_collide_free(bar);
    fbe_collide_free(dot);
}

TEST_CASE("a mask scaled up costs its own pixels, not the screen's") {
    std::vector<std::uint8_t> one(std::size_t{ 4096 }, 0);
    std::vector<std::uint8_t> two(std::size_t{ 4096 }, 0);
    one[3] = 255;
    two[(((31 * 32) + 31) * 4) + 3] = 255;
    const std::uint64_t a = fbe_collide_mask(one.data(), 32, 32, 128, 1);
    const std::uint64_t b = fbe_collide_mask(two.data(), 32, 32, 128, 1);
    // Turned, it's a row at a time over what the canvas reaches: a huge one's slow, if not forever.
    for (const float scale : { 1.0F, 30.0F, 1000.0F, 1e6F, 3.4e38F }) {
        for (const float rotation : { 0.0F, half_pi, 0.576F }) {
            if (rotation == 0.576F && scale > 1000.0F) {
                continue;
            }
            // Each solid only at an opposite corner, on one spot: no pixel meets, so every row of
            // the overlap is gone through.
            const fbe_collide_place_t place =
                place_of(0.0F, 0.0F, 16.0F, 16.0F, rotation, scale, scale);
            const auto started = std::chrono::steady_clock::now();
            CHECK(fbe_collide_masks(a, &place, b, &place) == 0);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
            INFO("scale " << scale << " turned " << rotation);
            CHECK(ms < 500.0);
        }
    }
    // Where they do meet, scaled up, each a pixel solid at its middle.
    std::vector<std::uint8_t> middle(std::size_t{ 4096 }, 0);
    middle[(((16 * 32) + 16) * 4) + 3] = 255;
    const std::uint64_t c = fbe_collide_mask(middle.data(), 32, 32, 128, 1);
    const fbe_collide_place_t big = place_of(0.0F, 0.0F, 16.0F, 16.0F, 0.0F, 1000.0F, 1000.0F);
    const fbe_collide_place_t near = place_of(500.0F, 500.0F, 16.0F, 16.0F, 0.0F, 1.0F, 1.0F);
    CHECK(fbe_collide_masks(c, &big, c, &near) == 1);
    fbe_collide_free(a);
    fbe_collide_free(b);
    fbe_collide_free(c);
}
