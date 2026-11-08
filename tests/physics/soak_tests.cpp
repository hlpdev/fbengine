// fbengine_physics soaked: thousands of shapes, bodies, characters, joints, and worlds made,
// stepped, and freed, over and over, as a long game's would be, leave none of them behind, and what
// malloc holds grows, once the first rounds have warmed it, by next to nothing a round.

#include "fbengine_physics.h"
#include "stability.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

using fbe::tests::malloc_bytes;
using fbe::tests::sanitized;

namespace {

/// The most what malloc holds grows a round, over a soak's second half, in bytes.
constexpr double most_slope = 64.0;

/// The least-squares slope of the second half of `held`, what malloc held after each round, in
/// bytes a round.
double late_slope(const std::vector<std::uint64_t>& held) {
    const std::size_t from = held.size() / 2;
    const auto count = static_cast<double>(held.size() - from);
    double mean_x = 0.0;
    double mean_y = 0.0;
    for (std::size_t i = from; i < held.size(); ++i) {
        mean_x += static_cast<double>(i);
        mean_y += static_cast<double>(held[i]);
    }
    mean_x /= count;
    mean_y /= count;
    double across = 0.0;
    double spread = 0.0;
    for (std::size_t i = from; i < held.size(); ++i) {
        const double x = static_cast<double>(i) - mean_x;
        across += x * (static_cast<double>(held[i]) - mean_y);
        spread += x * x;
    }
    return spread > 0.0 ? across / spread : 0.0;
}

constexpr fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };

// A world of 1000 bodies of their own shapes, on a floor, falling on each other, 200 joints
// between them, and 50 characters walking among them, stepped a while, saved and restored, then
// freed a piece at a time, or all at once with the world.
void soak(bool piece_by_piece) {
    const std::uint64_t world = fbe_physics_new(1200);
    REQUIRE(world != 0);
    const fbe_shape_vec3_t floor_size{ .x = 200.0F, .y = 1.0F, .z = 200.0F };
    const fbe_body_vec3_t floor_at{ .x = 0.0F, .y = -0.5F, .z = 0.0F };
    std::vector<std::uint64_t> shapes{ fbe_shape_box(&floor_size) };
    std::vector<std::uint64_t> bodies{
        fbe_body_add(world, shapes[0], FBE_BODY_STATIC, &floor_at, &unturned),
    };
    for (int i = 0; i < 1000; ++i) {
        const float size = 0.5F + (static_cast<float>(i % 7) * 0.1F);
        const fbe_shape_vec3_t box{ .x = size, .y = size, .z = size };
        const std::uint64_t shape = i % 2 == 0 ? fbe_shape_box(&box) : fbe_shape_sphere(size);
        shapes.push_back(shape);
        const fbe_body_vec3_t where{
            .x = (static_cast<float>(i % 20) * 2.0F) - 20.0F,
            .y = 1.0F + std::floor(static_cast<float>(i) / 400.0F),
            .z = (static_cast<float>((i / 20) % 20) * 2.0F) - 20.0F,
        };
        bodies.push_back(fbe_body_add(world, shape, FBE_BODY_DYNAMIC, &where, &unturned));
    }
    std::vector<std::uint64_t> joints;
    for (std::size_t i = 1; i + 1 < bodies.size() && joints.size() < 200; i += 5) {
        const fbe_joint_vec3_t point{ .x = 0.0F, .y = 1.0F, .z = 0.0F };
        joints.push_back(i % 2 == 0 ? fbe_joint_fixed(bodies[i], bodies[i + 1])
                                    : fbe_joint_point(bodies[i], bodies[i + 1], &point));
    }
    const std::uint64_t capsule = fbe_shape_capsule(1.8F, 0.3F);
    shapes.push_back(capsule);
    std::vector<std::uint64_t> characters;
    for (int i = 0; i < 50; ++i) {
        const fbe_character_vec3_t where{
            .x = static_cast<float>(i) - 25.0F,
            .y = 1.0F,
            .z = 30.0F,
        };
        characters.push_back(fbe_character_new(world, capsule, &where));
    }
    for (const std::uint64_t made : shapes) {
        REQUIRE(made != 0);
    }
    for (const std::uint64_t made : bodies) {
        REQUIRE(made != 0);
    }
    for (const std::uint64_t made : joints) {
        REQUIRE(made != 0);
    }
    for (const std::uint64_t made : characters) {
        REQUIRE(made != 0);
    }
    const auto step = [&] {
        for (const std::uint64_t character : characters) {
            const fbe_character_vec3_t walking{ .x = 0.0F, .y = -1.0F, .z = -1.0F };
            fbe_character_move(character, &walking, 1.0F / 60);
        }
        REQUIRE(fbe_physics_step(world, 1.0F / 60, 1));
    };
    for (int i = 0; i < 5; ++i) {
        step();
    }
    std::vector<std::uint8_t> state(static_cast<std::size_t>(fbe_physics_save(world, 0.0)));
    fbe_physics_saved(world, state.data(), static_cast<std::int64_t>(state.size()));
    step();
    double carried = 0.0;
    REQUIRE(fbe_physics_restore(world, state.data(), static_cast<std::int64_t>(state.size()),
                                &carried));
    step();
    if (piece_by_piece) {
        for (const std::uint64_t joint : joints) {
            REQUIRE(fbe_joint_free(joint));
        }
        for (const std::uint64_t character : characters) {
            REQUIRE(fbe_character_free(character));
        }
        for (const std::uint64_t body : bodies) {
            REQUIRE(fbe_body_free(body));
        }
        CHECK(fbe_physics_bodies(world) == 0);
        step();
    }
    REQUIRE(fbe_physics_free(world));
    for (const std::uint64_t shape : shapes) {
        REQUIRE(fbe_shape_free(shape));
    }
}

} // namespace

TEST_CASE("thousands of bodies, characters, joints, and worlds made and freed leave nothing",
          "[stability]") {
    const std::int32_t shapes = fbe_shape_count();
    const std::int32_t worlds = fbe_physics_worlds();
    // Jolt on the thread that asks alone, as malloc's caches for each thread (glibc's tcache,
    // macOS's magazines) fill as its threads work, slowly, at their threads' pace; the caller's
    // own fill in the warming rounds.
    const std::int32_t threads = fbe_physics_threads();
    REQUIRE(fbe_physics_set_threads(0));
    for (int i = 0; i < 60; ++i) {
        soak(i % 2 == 0);
    }
    std::vector<std::uint64_t> held;
    for (int i = 0; i < 40; ++i) {
        soak(i % 2 == 0);
        held.push_back(malloc_bytes());
    }
    REQUIRE(fbe_physics_set_threads(threads));
    CHECK(fbe_shape_count() == shapes);
    CHECK(fbe_physics_worlds() == worlds);
    // A round of about 1000 shapes, 1050 bodies, 200 joints, and 50 characters, and a world for
    // them, holds about 2 MB: what malloc holds grows by no more than 64 bytes a round over the
    // second half, as a cache that fills flattens and a leak keeps on, as about 18 KB a world did
    // on MinGW. A sanitizer's allocator counts its own way, and LeakSanitizer and leak_tests'
    // count of every allocation see a leak there.
    const double slope = late_slope(held);
    INFO("the second half's slope, " << slope << " bytes a round");
    if (!sanitized) {
        CHECK(slope <= most_slope);
    }
}

// The slope's own check: a leak of 256 bytes a round is seen, and a fill that flattens isn't.
TEST_CASE("the soak's slope sees a leak a round, and not a fill that flattens") {
    std::vector<std::uint64_t> leaking;
    std::vector<std::uint64_t> filling;
    for (std::uint64_t round = 0; round < 40; ++round) {
        leaking.push_back(std::uint64_t{ 19000000 } + (256 * round));
        // Half of what's left to fill each round, as caches fill: 20 KB in all.
        filling.push_back(std::uint64_t{ 19020480 } -
                          (std::uint64_t{ 20480 } >> std::min<std::uint64_t>(round, 63)));
    }
    CHECK(late_slope(leaking) > most_slope);
    CHECK(late_slope(filling) <= most_slope);
}
