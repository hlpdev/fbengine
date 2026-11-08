// fbengine_physics soaked: thousands of shapes, bodies, characters, joints, and worlds made,
// stepped, and freed, over and over, as a long game's would be, leave none of them behind: once the
// first rounds have warmed it, the count of live allocations, its C++'s and Jolt's, is exactly what
// it was after every round. What malloc holds is said, not held to anything, as other threads, a
// driver's among them, and malloc's own caches move it.

#include "fbengine_physics.h"
#include "physics/live.hpp"
#include "stability.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

using fbe::tests::live_allocations;
using fbe::tests::malloc_bytes;

namespace {

constexpr fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };

/// How many more allocations are live after each of `rounds` of `round` than before them, once
/// `warm` of them have warmed what starts with the first.
std::vector<std::int64_t> left_by(const std::function<void(int)>& round, int warm, int rounds) {
    for (int i = 0; i < warm; ++i) {
        round(i);
    }
    const std::int64_t before = live_allocations();
    std::vector<std::int64_t> left;
    left.reserve(static_cast<std::size_t>(rounds));
    const std::int64_t reserved = live_allocations() - before;
    for (int i = 0; i < rounds; ++i) {
        round(warm + i);
        left.push_back(live_allocations() - before - reserved);
    }
    return left;
}

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
    // Jolt, and its threads, start with the first world.
    REQUIRE(fbe_physics_free(fbe_physics_new(1)));
    fbe::tests::count_jolt();
    const std::int32_t shapes = fbe_shape_count();
    const std::int32_t worlds = fbe_physics_worlds();
    const std::uint64_t start = malloc_bytes();
    const std::vector<std::int64_t> left = left_by([](int i) { soak(i % 2 == 0); }, 4, 40);
    CHECK(fbe_shape_count() == shapes);
    CHECK(fbe_physics_worlds() == worlds);
    WARN("what malloc holds moved by "
         << (static_cast<double>(malloc_bytes()) - static_cast<double>(start))
         << " bytes over the rounds");
    for (std::size_t i = 0; i < left.size(); ++i) {
        INFO("after round " << i);
        CHECK(left[i] == 0);
    }
}

// The count's own check: a round that leaves one allocation behind leaves one more each round.
TEST_CASE("the soak's count sees one allocation left a round") {
    std::vector<std::unique_ptr<int>> kept;
    kept.reserve(64);
    const std::vector<std::int64_t> left =
        left_by([&kept](int) { kept.push_back(std::make_unique<int>(0)); }, 4, 20);
    for (std::size_t i = 0; i < left.size(); ++i) {
        INFO("after round " << i);
        CHECK(left[i] == static_cast<std::int64_t>(i) + 1);
    }
}
