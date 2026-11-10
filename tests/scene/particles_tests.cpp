// Particles by numbers, with no GPU, through fbe_particles_read: a start within a box, a velocity
// added within a box, a size of a random multiple and a shape, growth and fading by steps, smooth
// or held, and bursts by a schedule. The scene shader has the same formula, which the GPU tests
// hold to these numbers.

#include "fbengine.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string_view>
#include <vector>

namespace {

using Catch::Approx;

struct test_world_t {
    std::uint64_t handle = fbe_world_create();

    test_world_t() = default;
    test_world_t(const test_world_t&) = delete;
    test_world_t& operator=(const test_world_t&) = delete;
    test_world_t(test_world_t&&) = delete;
    test_world_t& operator=(test_world_t&&) = delete;

    ~test_world_t() {
        fbe_world_free(handle);
    }
};

bool says(std::string_view message) {
    return std::string_view(fbe_error()).contains(message);
}

/// An emitter that spawns none on its own, its particles still, of a half-size, opaque, living 100
/// seconds, read from now on.
std::uint64_t still_emitter(std::uint64_t world, float half = 0.5F) {
    const std::uint64_t emitter = fbe_entity_create(world, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_particles_set_rate(emitter, 0.0F));
    REQUIRE(fbe_particles_set_motion(emitter, 0.0F, 0.0F, 0.0F, 0.0F));
    REQUIRE(fbe_particles_set_sizes(emitter, half, half));
    const fbe_color_t white{ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F };
    REQUIRE(fbe_particles_set_colors(emitter, &white, &white));
    REQUIRE(fbe_particles_set_life(emitter, 100.0F, 100.0F));
    REQUIRE(fbe_particles_set_seed(emitter, 20261110));
    REQUIRE(fbe_particles_read(emitter, nullptr, 0) == 0);
    return emitter;
}

std::vector<fbe_particle_t> read(std::uint64_t emitter) {
    const std::int32_t count = fbe_particles_read(emitter, nullptr, 0);
    REQUIRE(count >= 0);
    std::vector<fbe_particle_t> all(static_cast<std::size_t>(count));
    REQUIRE(fbe_particles_read(emitter, all.data(), count) == count);
    return all;
}

} // namespace

TEST_CASE("an emitter's particles are read from the first read on, as they are now",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_particles_set_rate(emitter, 0.0F));
    REQUIRE(fbe_particles_set_life(emitter, 2.0F, 2.0F));
    REQUIRE(fbe_particles_set_motion(emitter, 3.0F, 3.0F, 0.0F, 0.0F));
    REQUIRE(fbe_particles_set_forces(emitter, 0.0F, -4.0F, 0.0F, 0.0F));
    REQUIRE(fbe_particles_set_sizes(emitter, 1.0F, 0.0F));
    REQUIRE(fbe_entity_set_position(emitter, 1.0F, 2.0F, 3.0F, FBE_SPACE_WORLD));
    // Born before the first read: alive, and not known to it.
    REQUIRE(fbe_particles_burst(emitter, 2));
    CHECK(fbe_particles_read(emitter, nullptr, 0) == 0);
    CHECK(read(emitter).empty());
    CHECK(fbe_particles_alive(emitter) == 2);
    REQUIRE(fbe_particles_burst(emitter, 3));
    REQUIRE(fbe_world_step_particles(world.handle, 0.5F));
    const std::vector<fbe_particle_t> all = read(emitter);
    REQUIRE(all.size() == 3);
    for (const fbe_particle_t& one : all) {
        // Half a second along -Z at 3, fallen by 4 a second a second; a quarter through its life.
        CHECK(one.position.x == Approx(1.0F));
        CHECK(one.position.y == Approx(2.0F - 0.5F));
        CHECK(one.position.z == Approx(3.0F - 1.5F));
        CHECK(one.half_width == Approx(0.75F));
        CHECK(one.half_height == Approx(0.75F));
        CHECK(one.alpha == Approx(0.75F));
        CHECK(one.age == Approx(0.5F));
    }
    // Up to what's asked for is written, and how many there are is given all the same.
    std::array<fbe_particle_t, 2> some{};
    CHECK(fbe_particles_read(emitter, some.data(), 2) == 3);
    CHECK(some[1].age == Approx(0.5F));
    // Dead, they're not read; nor what's lost when it holds another number.
    REQUIRE(fbe_world_step_particles(world.handle, 2.0F));
    CHECK(read(emitter).empty());
    REQUIRE(fbe_particles_burst(emitter, 2));
    CHECK(read(emitter).size() == 2);
    REQUIRE(fbe_particles_set_capacity(emitter, 8));
    CHECK(read(emitter).empty());
    REQUIRE(fbe_particles_burst(emitter, 20));
    CHECK(read(emitter).size() == 8);
    CHECK(fbe_particles_read(0, nullptr, 0) == -1);
    // A copy has none, and reads its own from its first read.
    const std::uint64_t copy = fbe_entity_copy(emitter, world.handle, 0);
    REQUIRE(fbe_particles_burst(copy, 1));
    CHECK(fbe_particles_read(copy, nullptr, 0) == 0);
    REQUIRE(fbe_particles_burst(copy, 1));
    CHECK(read(copy).size() == 1);
}

TEST_CASE("particles are born within a box about their emitter, on its own axes",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle);
    REQUIRE(fbe_particles_set_start_box(emitter, 2.0F, 0.0F, 0.5F));
    fbe_vec3_t box{};
    REQUIRE(fbe_particles_start_box(emitter, &box));
    CHECK((box.x == 2.0F && box.y == 0.0F && box.z == 0.5F));
    REQUIRE(fbe_entity_set_position(emitter, 10.0F, 20.0F, 30.0F, FBE_SPACE_WORLD));
    REQUIRE(fbe_particles_burst(emitter, 400));
    std::vector<fbe_particle_t> all = read(emitter);
    REQUIRE(all.size() == 400);
    float least_x = 100.0F;
    float most_x = -100.0F;
    float least_z = 100.0F;
    float most_z = -100.0F;
    for (const fbe_particle_t& one : all) {
        least_x = std::fmin(least_x, one.position.x);
        most_x = std::fmax(most_x, one.position.x);
        least_z = std::fmin(least_z, one.position.z);
        most_z = std::fmax(most_z, one.position.z);
        CHECK(one.position.y == 20.0F);
    }
    // All of it, and most of it used: 400 of them reach within a twentieth of each side.
    CHECK((least_x >= 8.0F && least_x < 8.2F && most_x <= 12.0F && most_x > 11.8F));
    CHECK((least_z >= 29.5F && least_z < 29.55F && most_z <= 30.5F && most_z > 30.45F));
    // They're in its bounds.
    fbe_aabb_t bounds{};
    REQUIRE(fbe_entity_bounds(emitter, &bounds));
    CHECK((bounds.min.x <= least_x && bounds.max.x >= most_x));
    // The same seed's are the same ones again.
    REQUIRE(fbe_particles_set_capacity(emitter, 1024));
    REQUIRE(fbe_particles_set_seed(emitter, 20261110));
    REQUIRE(fbe_particles_burst(emitter, 400));
    const std::vector<fbe_particle_t> again = read(emitter);
    REQUIRE(again.size() == 400);
    CHECK(again[7].position.x == all[7].position.x);
    CHECK(again[399].position.z == all[399].position.z);
    // Turned a quarter turn about Y, its X is the world's -Z.
    const float half = std::numbers::pi_v<float> / 4.0F;
    REQUIRE(fbe_entity_set_rotation(emitter, 0.0F, std::sin(half), 0.0F, std::cos(half),
                                    FBE_SPACE_WORLD));
    REQUIRE(fbe_particles_set_capacity(emitter, 1024));
    REQUIRE(fbe_particles_burst(emitter, 400));
    all = read(emitter);
    most_x = -100.0F;
    most_z = -100.0F;
    for (const fbe_particle_t& one : all) {
        most_x = std::fmax(most_x, std::fabs(one.position.x - 10.0F));
        most_z = std::fmax(most_z, std::fabs(one.position.z - 30.0F));
    }
    CHECK((most_x <= 0.5001F && most_x > 0.45F));
    CHECK((most_z <= 2.0001F && most_z > 1.8F));
    CHECK_FALSE(fbe_particles_set_start_box(emitter, -1.0F, 0.0F, 0.0F));
    CHECK(says("a particle's start box is 0 or more each way"));
    CHECK_FALSE(fbe_particles_set_start_box(emitter, 0.0F, std::nanf(""), 0.0F));
    CHECK_FALSE(fbe_particles_set_start_box(0, 1.0F, 1.0F, 1.0F));
}

TEST_CASE("a velocity within a box is added to each particle's own, on the world's axes",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle);
    // Turned, which the world's axes aren't.
    const float half = std::numbers::pi_v<float> / 4.0F;
    REQUIRE(fbe_entity_set_rotation(emitter, 0.0F, std::sin(half), 0.0F, std::cos(half),
                                    FBE_SPACE_WORLD));
    const fbe_vec3_t one_way{ .x = 1.0F, .y = 2.0F, .z = -3.0F };
    REQUIRE(fbe_particles_set_velocity_box(emitter, &one_way, &one_way));
    REQUIRE(fbe_particles_burst(emitter, 3));
    REQUIRE(fbe_world_step_particles(world.handle, 2.0F));
    for (const fbe_particle_t& one : read(emitter)) {
        CHECK(one.position.x == Approx(2.0F));
        CHECK(one.position.y == Approx(4.0F));
        CHECK(one.position.z == Approx(-6.0F));
    }
    // With no speed of its own, where that takes it is in its bounds all the same.
    fbe_aabb_t added{};
    REQUIRE(fbe_entity_bounds(emitter, &added));
    CHECK((added.max.x >= 2.0F && added.max.y >= 4.0F && added.min.z <= -6.0F));
    // Between two, with its own speed along its forward axis, the world's -X as it's turned.
    REQUIRE(fbe_particles_set_capacity(emitter, 1024));
    REQUIRE(fbe_particles_set_motion(emitter, 5.0F, 5.0F, 0.0F, 0.0F));
    const fbe_vec3_t least{ .x = 0.0F, .y = -1.0F, .z = 2.0F };
    const fbe_vec3_t most{ .x = 0.0F, .y = 1.0F, .z = 4.0F };
    REQUIRE(fbe_particles_set_velocity_box(emitter, &least, &most));
    fbe_vec3_t low{};
    fbe_vec3_t high{};
    REQUIRE(fbe_particles_velocity_box(emitter, &low, &high));
    CHECK((low.y == -1.0F && high.z == 4.0F));
    REQUIRE(fbe_particles_burst(emitter, 300));
    REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    const std::vector<fbe_particle_t> all = read(emitter);
    REQUIRE(all.size() == 300);
    float least_y = 100.0F;
    float most_y = -100.0F;
    float least_z = 100.0F;
    float most_z = -100.0F;
    for (const fbe_particle_t& one : all) {
        CHECK(one.position.x == Approx(-5.0F));
        least_y = std::fmin(least_y, one.position.y);
        most_y = std::fmax(most_y, one.position.y);
        least_z = std::fmin(least_z, one.position.z);
        most_z = std::fmax(most_z, one.position.z);
    }
    CHECK((least_y >= -1.0F && least_y < -0.9F && most_y <= 1.0F && most_y > 0.9F));
    CHECK((least_z >= 2.0F && least_z < 2.1F && most_z <= 4.0F && most_z > 3.9F));
    // Where they can go by it is in its bounds.
    fbe_aabb_t bounds{};
    REQUIRE(fbe_entity_bounds(emitter, &bounds));
    CHECK((bounds.min.x <= -5.0F && bounds.max.z >= most_z && bounds.min.y <= least_y));
    const fbe_vec3_t wrong{ .x = 0.0F, .y = -2.0F, .z = 2.0F };
    CHECK_FALSE(fbe_particles_set_velocity_box(emitter, &least, &wrong));
    CHECK(says("least to most on each axis"));
}

TEST_CASE("a particle's size is times a number chosen at its birth, its width and height its own",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle, 0.5F);
    REQUIRE(fbe_particles_set_size_range(emitter, 2.0F, 4.0F));
    REQUIRE(fbe_particles_set_shape(emitter, 1.0F, 3.0F));
    REQUIRE(fbe_particles_burst(emitter, 300));
    const std::vector<fbe_particle_t> all = read(emitter);
    REQUIRE(all.size() == 300);
    float least = 100.0F;
    float most = 0.0F;
    for (const fbe_particle_t& one : all) {
        least = std::fmin(least, one.half_width);
        most = std::fmax(most, one.half_width);
        CHECK(one.half_height == Approx(one.half_width * 3.0F));
    }
    CHECK((least >= 1.0F && least < 1.05F && most <= 2.0F && most > 1.95F));
    // It keeps the one it was born with.
    REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    CHECK(read(emitter)[5].half_width == all[5].half_width);
    // One number, and none's taken: a seed's run is what it was without.
    REQUIRE(fbe_particles_set_size_range(emitter, 3.0F, 3.0F));
    REQUIRE(fbe_particles_set_capacity(emitter, 4));
    REQUIRE(fbe_particles_burst(emitter, 1));
    CHECK(read(emitter)[0].half_width == Approx(1.5F));
    fbe_particles_settings_t settings{};
    REQUIRE(fbe_particles_settings(emitter, &settings));
    CHECK((settings.size_least == 3.0F && settings.size_most == 3.0F));
    CHECK((settings.width == 1.0F && settings.height == 3.0F));
    // Its bounds are by the biggest it can be.
    fbe_aabb_t bounds{};
    REQUIRE(fbe_entity_bounds(emitter, &bounds));
    CHECK(bounds.max.y >= 4.5F);
    CHECK_FALSE(fbe_particles_set_size_range(emitter, 2.0F, 1.0F));
    CHECK(says("what a particle's size is times is 0 or more, least to most"));
    CHECK_FALSE(fbe_particles_set_size_range(emitter, -1.0F, 1.0F));
    CHECK_FALSE(fbe_particles_set_shape(emitter, -1.0F, 1.0F));
    CHECK(says("a particle's width and height are 0 or more times its size"));
}

TEST_CASE("each step adds to a particle's width and height and multiplies them, smooth or held",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle, 0.5F);
    REQUIRE(fbe_particles_set_shape(emitter, 1.0F, 2.0F));
    REQUIRE(fbe_particles_set_steps(emitter, 0.1F, false));
    REQUIRE(fbe_particles_set_growth(emitter, 0.02F, 1.1F));
    REQUIRE(fbe_particles_burst(emitter, 1));
    // As a game stepping by hand has it, a tick at a time.
    float width = 0.5F;
    float height = 1.0F;
    CHECK(read(emitter)[0].half_width == Approx(width));
    for (int step = 1; step <= 5; ++step) {
        REQUIRE(fbe_world_step_particles(world.handle, 0.1F));
        width = (width + 0.02F) * 1.1F;
        height = (height + 0.02F) * 1.1F;
        const fbe_particle_t one = read(emitter)[0];
        CAPTURE(step);
        CHECK(one.half_width == Approx(width).epsilon(0.001));
        CHECK(one.half_height == Approx(height).epsilon(0.001));
    }
    // Half a step on: smooth, it's between this step's and the next; held, it's this step's still.
    REQUIRE(fbe_world_step_particles(world.handle, 0.05F));
    const float next = (width + 0.02F) * 1.1F;
    const float between = read(emitter)[0].half_width;
    CHECK((between > width + 0.01F && between < next - 0.01F));
    REQUIRE(fbe_particles_set_steps(emitter, 0.1F, true));
    CHECK(read(emitter)[0].half_width == Approx(width).epsilon(0.001));
    REQUIRE(fbe_world_step_particles(world.handle, 0.06F));
    CHECK(read(emitter)[0].half_width == Approx(next).epsilon(0.001));
    // Added alone, and multiplied alone.
    REQUIRE(fbe_particles_set_growth(emitter, 0.25F, 1.0F));
    CHECK(read(emitter)[0].half_width == Approx(0.5F + (6.0F * 0.25F)));
    REQUIRE(fbe_particles_set_growth(emitter, 0.0F, 0.5F));
    CHECK(read(emitter)[0].half_width == Approx(0.5F / 64.0F));
    // Shrunk past nothing, it's nothing, not less.
    REQUIRE(fbe_particles_set_growth(emitter, -0.2F, 1.0F));
    CHECK(read(emitter)[0].half_width == 0.0F);
    CHECK(read(emitter)[0].half_height == 0.0F);
    // With no step, neither does anything.
    REQUIRE(fbe_particles_set_steps(emitter, 0.0F, false));
    CHECK(read(emitter)[0].half_width == Approx(0.5F));
    // What it can grow to is in its bounds.
    REQUIRE(fbe_particles_set_steps(emitter, 1.0F, false));
    REQUIRE(fbe_particles_set_growth(emitter, 1.0F, 1.0F));
    fbe_aabb_t bounds{};
    REQUIRE(fbe_entity_bounds(emitter, &bounds));
    CHECK(bounds.max.y >= 100.0F);
    fbe_particles_settings_t settings{};
    REQUIRE(fbe_particles_settings(emitter, &settings));
    CHECK((settings.step == 1.0F && settings.added == 1.0F && settings.multiplied == 1.0F));
    CHECK_FALSE(settings.held);
    CHECK_FALSE(fbe_particles_set_growth(emitter, 0.0F, 0.0F));
    CHECK(says("what it's times above 0"));
    CHECK_FALSE(fbe_particles_set_steps(emitter, -0.1F, false));
    CHECK(says("a particle's step is 0 seconds or more"));
}

TEST_CASE("each step takes an amount off a particle's alpha, and one with none is gone",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle);
    REQUIRE(fbe_particles_set_steps(emitter, 0.1F, false));
    REQUIRE(fbe_particles_set_fade(emitter, 0.25F));
    REQUIRE(fbe_particles_burst(emitter, 1));
    CHECK(read(emitter)[0].alpha == Approx(1.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 0.2F));
    CHECK(read(emitter)[0].alpha == Approx(0.5F));
    // Smooth, half a step on is half a step's less; held, it's the last whole step's.
    REQUIRE(fbe_world_step_particles(world.handle, 0.05F));
    CHECK(read(emitter)[0].alpha == Approx(0.375F));
    REQUIRE(fbe_particles_set_steps(emitter, 0.1F, true));
    CHECK(read(emitter)[0].alpha == Approx(0.5F));
    // It's gone when it has none, at 0.4 seconds, long before its life's 100: read, and alive.
    REQUIRE(fbe_particles_set_steps(emitter, 0.1F, false));
    CHECK(fbe_particles_alive(emitter) == 1);
    REQUIRE(fbe_world_step_particles(world.handle, 0.1F));
    CHECK(fbe_particles_alive(emitter) == 1);
    REQUIRE(fbe_world_step_particles(world.handle, 0.1F));
    CHECK(fbe_particles_alive(emitter) == 0);
    CHECK(read(emitter).empty());
    // Held, one that would have none at 0.36 seconds is gone at the whole step after, 0.4.
    REQUIRE(fbe_particles_set_steps(emitter, 0.1F, true));
    REQUIRE(fbe_particles_set_fade(emitter, 0.28F));
    REQUIRE(fbe_particles_burst(emitter, 1));
    REQUIRE(fbe_world_step_particles(world.handle, 0.38F));
    CHECK(fbe_particles_alive(emitter) == 1);
    CHECK(read(emitter)[0].alpha == Approx(1.0F - (3.0F * 0.28F)));
    REQUIRE(fbe_world_step_particles(world.handle, 0.04F));
    CHECK(fbe_particles_alive(emitter) == 0);
    // With its colors' own fade it's from what they have it: 1 to 0 over 2 seconds, less 0.1 a
    // step.
    REQUIRE(fbe_particles_set_steps(emitter, 0.5F, false));
    REQUIRE(fbe_particles_set_fade(emitter, 0.1F));
    REQUIRE(fbe_particles_set_life(emitter, 2.0F, 2.0F));
    const fbe_color_t white{ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F };
    const fbe_color_t clear{ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 0.0F };
    REQUIRE(fbe_particles_set_colors(emitter, &white, &clear));
    REQUIRE(fbe_particles_burst(emitter, 1));
    REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    CHECK(read(emitter)[0].alpha == Approx(0.5F - 0.2F));
    // Gone at 1 / (0.2 + 0.5) seconds, 1.43, not its life's 2.
    REQUIRE(fbe_world_step_particles(world.handle, 0.5F));
    CHECK(fbe_particles_alive(emitter) == 0);
    // No fade, and it lives its life.
    REQUIRE(fbe_particles_set_fade(emitter, 0.0F));
    REQUIRE(fbe_particles_burst(emitter, 1));
    REQUIRE(fbe_world_step_particles(world.handle, 1.9F));
    CHECK(fbe_particles_alive(emitter) == 1);
    fbe_particles_settings_t settings{};
    REQUIRE(fbe_particles_settings(emitter, &settings));
    CHECK(settings.fade == 0.0F);
    CHECK_FALSE(fbe_particles_set_fade(emitter, -0.1F));
    CHECK(says("what's taken off a particle's alpha a step is 0 or more"));
}

TEST_CASE("bursts come by their schedule: so many, every so often, for so long, and again",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle);
    fbe_particles_settings_t settings{};
    REQUIRE(fbe_particles_settings(emitter, &settings));
    CHECK_FALSE(settings.bursting);
    // Three, every half second, for 1.2 seconds: at 0, 0.5, and 1.
    REQUIRE(fbe_particles_set_bursts(emitter, 3, 0.5F, 1.2F, 0.0F));
    CHECK(fbe_particles_alive(emitter) == 0);
    REQUIRE(fbe_particles_settings(emitter, &settings));
    CHECK(settings.bursting);
    CHECK((settings.burst_count == 3 && settings.burst_every == 0.5F));
    CHECK((settings.burst_lasts == 1.2F && settings.burst_again == 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 0.25F));
    CHECK(fbe_particles_alive(emitter) == 3);
    REQUIRE(fbe_world_step_particles(world.handle, 0.25F));
    CHECK(fbe_particles_alive(emitter) == 3);
    REQUIRE(fbe_world_step_particles(world.handle, 0.25F));
    CHECK(fbe_particles_alive(emitter) == 6);
    // Each is as old as it's been since its burst.
    std::vector<fbe_particle_t> all = read(emitter);
    REQUIRE(all.size() == 6);
    std::ranges::sort(all, {}, &fbe_particle_t::age);
    CHECK(all[0].age == Approx(0.25F));
    CHECK(all[5].age == Approx(0.75F));
    REQUIRE(fbe_world_step_particles(world.handle, 0.5F));
    CHECK(fbe_particles_alive(emitter) == 9);
    REQUIRE(fbe_particles_settings(emitter, &settings));
    CHECK_FALSE(settings.bursting);
    REQUIRE(fbe_world_step_particles(world.handle, 5.0F));
    CHECK(fbe_particles_alive(emitter) == 9);
}

TEST_CASE("bursts are set off again after so long, and one long step has them all, where it was",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle);
    // Two, every second, for half a second, set off again every 3: at 0, 3, 6, 9.
    REQUIRE(fbe_particles_set_bursts(emitter, 2, 1.0F, 0.5F, 3.0F));
    for (int second = 0; second < 7; ++second) {
        REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    }
    CHECK(fbe_particles_alive(emitter) == 6);
    fbe_particles_settings_t settings{};
    REQUIRE(fbe_particles_settings(emitter, &settings));
    CHECK(settings.bursting);
    // Set again, from the start, in one step of 10 seconds, the emitter going 10 along X in it.
    REQUIRE(fbe_particles_set_capacity(emitter, 1024));
    REQUIRE(fbe_world_step_particles(world.handle, 0.0F));
    REQUIRE(fbe_particles_set_bursts(emitter, 2, 1.0F, 0.5F, 3.0F));
    REQUIRE(fbe_entity_set_position(emitter, 10.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    REQUIRE(fbe_world_step_particles(world.handle, 10.0F));
    std::vector<fbe_particle_t> all = read(emitter);
    REQUIRE(all.size() == 8);
    std::ranges::sort(all, {}, &fbe_particle_t::age);
    for (std::size_t burst = 0; burst < 4; ++burst) {
        // The last was at 9 seconds, 1 ago, 9 along; the first at 0, 10 ago, where it began.
        const float ago = 1.0F + (3.0F * static_cast<float>(burst));
        CHECK(all[burst * 2].age == Approx(ago));
        CHECK(all[(burst * 2) + 1].age == Approx(ago));
        CHECK(all[burst * 2].position.x == Approx(10.0F - ago).margin(0.001));
    }
    // None, and they stop.
    REQUIRE(fbe_particles_set_bursts(emitter, 0, 1.0F, 0.0F, 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 5.0F));
    CHECK(fbe_particles_alive(emitter) == 8);
    REQUIRE(fbe_particles_settings(emitter, &settings));
    CHECK_FALSE(settings.bursting);
    // For ever, with no end.
    REQUIRE(fbe_particles_set_capacity(emitter, 1024));
    REQUIRE(fbe_particles_set_bursts(emitter, 1, 0.25F, 0.0F, 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 2.0F));
    CHECK(fbe_particles_alive(emitter) == 8);
    // A hidden emitter's don't come, and its copy's start from their start.
    REQUIRE(fbe_entity_set_hidden(emitter, true));
    REQUIRE(fbe_world_step_particles(world.handle, 2.0F));
    CHECK(fbe_particles_alive(emitter) == 8);
    const std::uint64_t copy = fbe_entity_copy(emitter, world.handle, 0);
    REQUIRE(fbe_entity_set_hidden(copy, false));
    REQUIRE(fbe_world_step_particles(world.handle, 0.1F));
    CHECK(fbe_particles_alive(copy) == 1);
}

TEST_CASE("bursts past what an emitter holds, or a life, in a huge step, come to nothing strange",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle);
    REQUIRE(fbe_particles_set_capacity(emitter, 16));
    REQUIRE(fbe_particles_read(emitter, nullptr, 0) == 0);
    REQUIRE(fbe_particles_set_bursts(emitter, 1000, 0.001F, 0.0F, 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 1000000.0F));
    CHECK(fbe_particles_alive(emitter) == 16);
    for (const fbe_particle_t& one : read(emitter)) {
        CHECK(one.age < 0.01F);
    }
    // Set off again a thousand times a second, each time a burst.
    REQUIRE(fbe_particles_set_bursts(emitter, 1, 10.0F, 0.0F, 0.001F));
    REQUIRE(fbe_world_step_particles(world.handle, 1000000.0F));
    CHECK(fbe_particles_alive(emitter) == 16);
    // A life of a second: only the last second's bursts are alive after a long step.
    REQUIRE(fbe_particles_set_capacity(emitter, 1024));
    REQUIRE(fbe_particles_set_life(emitter, 1.0F, 1.0F));
    REQUIRE(fbe_particles_set_bursts(emitter, 1, 0.25F, 0.0F, 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 100.0F));
    CHECK(fbe_particles_alive(emitter) == 3);
    CHECK_FALSE(fbe_particles_set_bursts(emitter, -1, 1.0F, 0.0F, 0.0F));
    CHECK(says("bursts are of 0 particles or more"));
    CHECK_FALSE(fbe_particles_set_bursts(emitter, 1, 0.0001F, 0.0F, 0.0F));
    CHECK_FALSE(fbe_particles_set_bursts(emitter, 1, 1.0F, -1.0F, 0.0F));
    CHECK_FALSE(fbe_particles_set_bursts(emitter, 1, 1.0F, 0.0F, 0.0001F));
    CHECK_FALSE(fbe_particles_set_bursts(emitter, 1, std::nanf(""), 0.0F, 0.0F));
}

TEST_CASE("an emitter that sets none of the new has a seed's run as it was", "[scene][particles]") {
    const test_world_t world;
    const auto run = [&](bool set) {
        const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
        REQUIRE(fbe_particles_set_rate(emitter, 0.0F));
        REQUIRE(fbe_particles_set_motion(emitter, 1.0F, 3.0F, 1.0F, 2.0F));
        REQUIRE(fbe_particles_set_life(emitter, 5.0F, 9.0F));
        REQUIRE(fbe_particles_set_seed(emitter, 77));
        REQUIRE(fbe_particles_read(emitter, nullptr, 0) == 0);
        if (set) {
            // Each to what it is at first, or to one number: none takes a number of the run.
            const fbe_vec3_t none{};
            REQUIRE(fbe_particles_set_start_box(emitter, 0.0F, 0.0F, 0.0F));
            REQUIRE(fbe_particles_set_velocity_box(emitter, &none, &none));
            REQUIRE(fbe_particles_set_size_range(emitter, 1.0F, 1.0F));
            REQUIRE(fbe_particles_set_shape(emitter, 1.0F, 1.0F));
            REQUIRE(fbe_particles_set_steps(emitter, 0.1F, true));
            REQUIRE(fbe_particles_set_roll_along(emitter, false));
        }
        REQUIRE(fbe_particles_burst(emitter, 50));
        REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
        return read(emitter);
    };
    const std::vector<fbe_particle_t> plain = run(false);
    const std::vector<fbe_particle_t> set = run(true);
    REQUIRE(plain.size() == 50);
    REQUIRE(set.size() == 50);
    for (std::size_t at = 0; at < plain.size(); ++at) {
        CHECK(plain[at].position.x == set[at].position.x);
        CHECK(plain[at].position.y == set[at].position.y);
        CHECK(plain[at].position.z == set[at].position.z);
        CHECK(plain[at].half_width == set[at].half_width);
    }
    // A range of sizes takes a number for each, after the first's place, so the run's another.
    const std::uint64_t ranged = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_particles_set_rate(ranged, 0.0F));
    REQUIRE(fbe_particles_set_motion(ranged, 1.0F, 3.0F, 1.0F, 2.0F));
    REQUIRE(fbe_particles_set_life(ranged, 5.0F, 9.0F));
    REQUIRE(fbe_particles_set_seed(ranged, 77));
    REQUIRE(fbe_particles_read(ranged, nullptr, 0) == 0);
    REQUIRE(fbe_particles_set_size_range(ranged, 1.0F, 3.0F));
    REQUIRE(fbe_particles_burst(ranged, 50));
    REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    const std::vector<fbe_particle_t> other = read(ranged);
    REQUIRE(other.size() == 50);
    std::size_t moved = 0;
    for (std::size_t at = 0; at < other.size(); ++at) {
        if (other[at].position.x != plain[at].position.x) {
            ++moved;
        }
    }
    CHECK(moved > 40);
}

TEST_CASE("a copy of an emitter has what's new of its settings", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle);
    REQUIRE(fbe_particles_set_start_box(emitter, 1.0F, 2.0F, 3.0F));
    REQUIRE(fbe_particles_set_size_range(emitter, 2.0F, 2.0F));
    REQUIRE(fbe_particles_set_shape(emitter, 2.0F, 3.0F));
    REQUIRE(fbe_particles_set_steps(emitter, 0.5F, true));
    REQUIRE(fbe_particles_set_growth(emitter, 0.1F, 2.0F));
    REQUIRE(fbe_particles_set_fade(emitter, 0.05F));
    REQUIRE(fbe_particles_set_roll_along(emitter, true));
    REQUIRE(fbe_particles_set_bursts(emitter, 4, 2.0F, 3.0F, 5.0F));
    const std::uint64_t copy = fbe_entity_copy(emitter, world.handle, 0);
    fbe_particles_settings_t settings{};
    REQUIRE(fbe_particles_settings(copy, &settings));
    CHECK((settings.size_least == 2.0F && settings.width == 2.0F && settings.height == 3.0F));
    CHECK((settings.step == 0.5F && settings.held && settings.added == 0.1F));
    CHECK((settings.multiplied == 2.0F && settings.fade == 0.05F && settings.roll_along));
    CHECK((settings.burst_count == 4 && settings.burst_again == 5.0F && settings.bursting));
    fbe_vec3_t box{};
    REQUIRE(fbe_particles_start_box(copy, &box));
    CHECK(box.z == 3.0F);
    CHECK_FALSE(fbe_particles_settings(0, &settings));
    CHECK_FALSE(fbe_particles_set_roll_along(0, true));
}

TEST_CASE("a copy of an emitter that's bursting bursts from its own start", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle);
    REQUIRE(fbe_particles_set_bursts(emitter, 4, 1.0F, 2.5F, 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    const std::size_t first = read(emitter).size();
    CHECK(first >= 4);
    // Past where they last: no more of them come.
    REQUIRE(fbe_world_step_particles(world.handle, 3.0F));
    const std::size_t all = read(emitter).size();
    CHECK(all == 12);
    const std::uint64_t copy = fbe_entity_copy(emitter, world.handle, 0);
    REQUIRE(fbe_particles_read(copy, nullptr, 0) == 0);
    REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    CHECK(read(copy).size() == first);
    CHECK(read(emitter).size() == all);
}

TEST_CASE("a long step loses no burst that nothing after it takes the place of",
          "[scene][particles]") {
    const test_world_t world;
    // One burst, as they last half a second of one every second: five seconds on, it's there.
    const std::uint64_t once = still_emitter(world.handle);
    REQUIRE(fbe_particles_set_capacity(once, 100));
    REQUIRE(fbe_particles_read(once, nullptr, 0) == 0);
    REQUIRE(fbe_particles_set_bursts(once, 100, 1.0F, 0.5F, 0.0F));
    // The only one of the shortest bursts there are, in a step far longer than they last.
    const std::uint64_t brief = still_emitter(world.handle);
    REQUIRE(fbe_particles_set_capacity(brief, 16));
    REQUIRE(fbe_particles_read(brief, nullptr, 0) == 0);
    REQUIRE(fbe_particles_set_bursts(brief, 16, 0.001F, 0.001F, 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 5.0F));
    const std::vector<fbe_particle_t> first = read(once);
    REQUIRE(first.size() == 100);
    CHECK(first[0].age == Approx(5.0F));
    CHECK(first[99].age == Approx(5.0F));
    const std::vector<fbe_particle_t> second = read(brief);
    REQUIRE(second.size() == 16);
    CHECK(second[0].age == Approx(5.0F));
    REQUIRE(fbe_particles_set_bursts(once, 0, 1.0F, 0.0F, 0.0F));
    REQUIRE(fbe_particles_set_bursts(brief, 0, 1.0F, 0.0F, 0.0F));
    // Set off again every 100 seconds, one burst each time: 150 on, the second's 50 old.
    const std::uint64_t again = still_emitter(world.handle);
    REQUIRE(fbe_particles_set_capacity(again, 4));
    REQUIRE(fbe_particles_read(again, nullptr, 0) == 0);
    REQUIRE(fbe_particles_set_bursts(again, 4, 10.0F, 0.5F, 100.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 150.0F));
    const std::vector<fbe_particle_t> third = read(again);
    REQUIRE(third.size() == 4);
    CHECK(third[0].age == Approx(50.0F));
    // Lives of a tenth of a second to 100, and a burst of all it holds every 10: of the last, 5
    // seconds old, some are dead already, and where they'd be are those of the one before, which
    // no burst was sure to take the place of.
    const std::uint64_t mixed = still_emitter(world.handle);
    REQUIRE(fbe_particles_set_capacity(mixed, 64));
    REQUIRE(fbe_particles_set_life(mixed, 0.1F, 100.0F));
    REQUIRE(fbe_particles_set_seed(mixed, 5));
    REQUIRE(fbe_particles_read(mixed, nullptr, 0) == 0);
    REQUIRE(fbe_particles_set_bursts(mixed, 64, 10.0F, 0.0F, 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 55.0F));
    const std::vector<fbe_particle_t> fourth = read(mixed);
    CHECK(fourth.size() == 64);
    std::size_t young = 0;
    std::size_t older = 0;
    for (const fbe_particle_t& one : fourth) {
        young += one.age < 5.5F ? 1 : 0;
        older += one.age > 14.5F ? 1 : 0;
    }
    CHECK(young > 48);
    CHECK(young < 64);
    CHECK(older == 64 - young);
}

TEST_CASE("a particle that starts with no alpha and fades by held steps shows through its first",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle);
    const fbe_color_t clear{ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 0.0F };
    const fbe_color_t white{ .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F };
    REQUIRE(fbe_particles_set_colors(emitter, &clear, &white));
    REQUIRE(fbe_particles_set_life(emitter, 1.0F, 1.0F));
    REQUIRE(fbe_particles_set_steps(emitter, 0.5F, true));
    REQUIRE(fbe_particles_set_fade(emitter, 0.6F));
    REQUIRE(fbe_particles_burst(emitter, 1));
    CHECK(fbe_particles_alive(emitter) == 1);
    REQUIRE(fbe_world_step_particles(world.handle, 0.25F));
    const std::vector<fbe_particle_t> all = read(emitter);
    REQUIRE(all.size() == 1);
    CHECK(all[0].alpha == Approx(0.25F));
    // Its first whole step takes more than it has: gone then.
    REQUIRE(fbe_world_step_particles(world.handle, 0.3F));
    CHECK(read(emitter).empty());
    CHECK(fbe_particles_alive(emitter) == 0);
}

TEST_CASE("a grown size is finite whatever its growth, and right for a multiple next to 1",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = still_emitter(world.handle);
    REQUIRE(fbe_particles_set_steps(emitter, 0.01F, false));
    REQUIRE(fbe_particles_set_growth(emitter, 0.0F, 2.0F));
    REQUIRE(fbe_particles_burst(emitter, 1));
    REQUIRE(fbe_world_step_particles(world.handle, 2.0F));
    std::vector<fbe_particle_t> all = read(emitter);
    REQUIRE(all.size() == 1);
    CHECK(all[0].half_width == 1e9F);
    fbe_aabb_t bounds{};
    REQUIRE(fbe_entity_bounds(emitter, &bounds));
    CHECK((std::isfinite(bounds.min.x) && std::isfinite(bounds.max.y)));
    // Of no size, times as much as there is: still none.
    REQUIRE(fbe_particles_set_sizes(emitter, 0.0F, 0.0F));
    all = read(emitter);
    CHECK(all[0].half_width == 0.0F);
    // A step too short to count, and all there is added a step.
    REQUIRE(fbe_particles_set_steps(emitter, std::numeric_limits<float>::denorm_min(), true));
    REQUIRE(fbe_particles_set_growth(emitter, 3e38F, 3e38F));
    all = read(emitter);
    CHECK(all[0].half_width == 1e9F);
    CHECK(all[0].alpha == 1.0F);
    REQUIRE(fbe_particles_set_growth(emitter, -3e38F, 3e38F));
    CHECK(read(emitter)[0].half_width == 0.0F);
    REQUIRE(fbe_entity_bounds(emitter, &bounds));
    CHECK((std::isfinite(bounds.min.x) && std::isfinite(bounds.max.y)));
    // 300 steps of a hundredth added, then times a float past 1: as stepping it by hand has it.
    for (const float times : { 1.000002F, 0.999999F, 1.0001F, 1.01F }) {
        CAPTURE(times);
        REQUIRE(fbe_particles_set_sizes(emitter, 0.5F, 0.5F));
        REQUIRE(fbe_particles_set_steps(emitter, 0.125F, true));
        REQUIRE(fbe_particles_set_growth(emitter, 0.01F, times));
        REQUIRE(fbe_particles_set_capacity(emitter, 16));
        REQUIRE(fbe_particles_burst(emitter, 1));
        REQUIRE(fbe_world_step_particles(world.handle, 37.5F));
        double width = 0.5;
        for (int step = 0; step < 300; ++step) {
            width = (width + static_cast<double>(0.01F)) * static_cast<double>(times);
        }
        all = read(emitter);
        REQUIRE(all.size() == 1);
        CHECK(all[0].half_width == Approx(width).epsilon(0.00001));
    }
}
