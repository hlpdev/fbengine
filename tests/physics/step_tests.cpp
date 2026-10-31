// fbengine_physics' steps through its C API, at their largest: Jolt makes all of an update's jobs
// as it starts, from a fixed number, so a step of many collision steps with many bodies awake, on
// Jolt's widest job system, has to be taken in several.

#include "fbengine_physics.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace {

constexpr fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };

} // namespace

TEST_CASE("jolt's threads are what's set, from 0 to 31") {
    CHECK(fbe_physics_set_threads(2));
    CHECK(fbe_physics_threads() == 2);
    CHECK_FALSE(fbe_physics_set_threads(32));
    CHECK_FALSE(fbe_physics_set_threads(-1));
    CHECK(fbe_physics_threads() == 2);
    // Once Jolt's started, its pool changes between steps.
    const std::uint64_t world = fbe_physics_new(10);
    REQUIRE(world != 0);
    CHECK(fbe_physics_threads() == 2);
    CHECK(fbe_physics_set_threads(5));
    CHECK(fbe_physics_threads() == 5);
    CHECK(fbe_physics_step(world, 1.0F / 60.0F, 1));
    CHECK(fbe_physics_free(world));
}

TEST_CASE("16 collision steps with 16000 bodies awake fit, on 31 threads") {
    // Jolt's widest, which a CI runner of few cores is given too.
    REQUIRE(fbe_physics_set_threads(31));
    const std::uint64_t world = fbe_physics_new(16384);
    REQUIRE(world != 0);
    const fbe_shape_vec3_t size{ .x = 1.0F, .y = 1.0F, .z = 1.0F };
    const std::uint64_t box = fbe_shape_box(&size);
    REQUIRE(box != 0);
    // Rows of boxes touching side by side, so they collide as well as fall.
    for (int i = 0; i < 16000; ++i) {
        const int row = i / 128;
        const fbe_body_vec3_t at{
            .x = static_cast<float>(i % 128),
            .y = 100.0F,
            .z = static_cast<float>(row) * 1.5F,
        };
        REQUIRE(fbe_body_add(world, box, FBE_BODY_DYNAMIC, &at, &unturned) != 0);
    }
    CHECK(fbe_physics_bodies(world) == 16000);
    REQUIRE(fbe_physics_step(world, 1.0F, FBE_PHYSICS_MAX_COLLISION_STEPS));
    CHECK(fbe_physics_steps(world) == 1);
    // So many touching are more than a world of 16384 has room for, which it says.
    CHECK(fbe_physics_overflowed(world));
    CHECK(fbe_physics_threads() == 31);
    CHECK(fbe_physics_free(world));
    CHECK(fbe_shape_free(box));
}
