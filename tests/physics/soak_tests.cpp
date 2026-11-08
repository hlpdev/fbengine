// fbengine_physics soaked: thousands of shapes, bodies, characters, joints, and worlds made,
// stepped, and freed, over and over, as a long game's would be, leave none of them behind, and what
// the process has allocated grows by no more than its heap's fragments.

#include "fbengine_physics.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

#ifdef _WIN32
// psapi.h needs windows.h's types first.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on
#elifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <unistd.h>
#endif

#ifdef __SANITIZE_ADDRESS__
#define FBE_UNDER_ASAN
#elifdef __has_feature
#if __has_feature(address_sanitizer)
#define FBE_UNDER_ASAN
#endif
#endif

namespace {

constexpr fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };

/// What the process has allocated: its committed memory on Windows, its malloc zones' bytes in use
/// on macOS, and its virtual size on Linux, where a large malloc maps its own.
std::uint64_t allocated() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): psapi's way to the _EX fields.
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                         sizeof(counters));
    return counters.PrivateUsage;
#elifdef __APPLE__
    malloc_statistics_t statistics{};
    malloc_zone_statistics(nullptr, &statistics);
    return statistics.size_in_use;
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t pages = 0;
    statm >> pages;
    return pages * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
}

// A world of 1000 bodies of their own shapes, on a floor, falling on each other, 200 joints
// between them, and 50 characters walking among them, stepped a while, saved and restored, then
// freed a piece at a time, or all at once with the world; giving how much it all held, made, more
// than before it.
std::uint64_t soak(bool piece_by_piece) {
    const std::uint64_t start = allocated();
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
    const std::uint64_t full = allocated();
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
    return full > start ? full - start : 0;
}

} // namespace

TEST_CASE("thousands of bodies, characters, joints, and worlds made and freed leave nothing",
          "[stability]") {
    const std::int32_t shapes = fbe_shape_count();
    const std::int32_t worlds = fbe_physics_worlds();
    // Jolt, its threads, and the allocator's pools start in the first rounds.
    for (int i = 0; i < 4; ++i) {
        soak(i % 2 == 0);
    }
    const std::uint64_t before = allocated();
    std::uint64_t held = 0;
    for (int i = 0; i < 40; ++i) {
        held = soak(i % 2 == 0);
    }
    CHECK(fbe_shape_count() == shapes);
    CHECK(fbe_physics_worlds() == worlds);
    // 40 rounds of about 1000 shapes, 1050 bodies, 200 joints, and 50 characters, and a world for
    // them, leave less than two rounds hold: what's left is the heap's own, about 40 KB a round on
    // Windows, which leak_tests' count of every allocation shows isn't a leak.
    INFO("a round held " << held << " bytes");
    CHECK(held > 0);
    // ASan keeps what's freed a while before it's used again, so bytes grow under it; its leak
    // check, and leak_tests' count, see a leak there.
#ifndef FBE_UNDER_ASAN
    CHECK(allocated() < before + (2 * held));
#endif
}
