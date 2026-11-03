// How a World's moves are run, by the scene's insides: here or on the job system's workers, they
// come out the same; and what a long sweep leaves in a thread's scratch.

#include "fbengine.h"
#include "scene/scene.hpp"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace {

constexpr std::int32_t mover_group = 1;
constexpr std::int32_t level_group = 2;

/// Back to what's worth it at a test's end.
struct path_t {
    explicit path_t(fbe::scene::move_path_t path) {
        fbe::scene::set_move_path(path);
    }
    path_t(const path_t&) = delete;
    path_t& operator=(const path_t&) = delete;
    path_t(path_t&&) = delete;
    path_t& operator=(path_t&&) = delete;
    ~path_t() {
        fbe::scene::set_move_path(fbe::scene::move_path_t::AS_WORTH);
    }
};

void free_world(std::uint64_t world) {
    fbe_world_free(world);
    std::array<std::uint64_t, 64> some{};
    while (fbe_entity_freed(some.data(), some.size()) != 0) {}
}

/// Every bit of where the movers end and what they touched.
std::vector<std::uint32_t> walk(int count) {
    const std::uint64_t world = fbe_world_create();
    fbe_world_set_collision(world, mover_group, mover_group, FBE_RESPONSE_SLIDE);
    fbe_world_set_collision(world, mover_group, level_group, FBE_RESPONSE_SLIDE);
    const std::uint64_t floor = fbe_entity_create(world, 0, FBE_ENTITY_PLANE);
    fbe_entity_set_collision_group(floor, level_group);
    std::mt19937 random(5); // NOLINT(bugprone-random-generator-seed)
    std::uniform_real_distribution<float> pace(-0.1F, 0.1F);
    std::vector<std::uint64_t> movers;
    std::vector<std::array<float, 3>> paces;
    for (int i = 0; i < count; ++i) {
        const std::uint64_t mover = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
        fbe_entity_set_collision_group(mover, mover_group);
        fbe_entity_set_collision_radius(mover, 0.3F, 0.3F);
        const int across = i % 15;
        const int along = i / 15;
        fbe_entity_set_position(mover, 1.25F * static_cast<float>(across), 1.0F,
                                1.25F * static_cast<float>(along), FBE_SPACE_WORLD);
        movers.push_back(mover);
        const float x = pace(random);
        const float z = pace(random);
        paces.push_back({ x, -0.05F, z });
    }
    fbe_world_collide(world);
    std::vector<std::uint32_t> bits;
    for (int frame = 0; frame < 60; ++frame) {
        for (std::size_t i = 0; i < movers.size(); ++i) {
            fbe_entity_translate(movers[i], paces[i][0], paces[i][1], paces[i][2], FBE_SPACE_WORLD);
        }
        fbe_world_collide(world);
        for (const std::uint64_t mover : movers) {
            fbe_vec3_t at{};
            REQUIRE(fbe_entity_position(mover, FBE_SPACE_WORLD, &at));
            bits.push_back(std::bit_cast<std::uint32_t>(at.x));
            bits.push_back(std::bit_cast<std::uint32_t>(at.y));
            bits.push_back(std::bit_cast<std::uint32_t>(at.z));
            std::array<fbe_collision_t, 16> touched{};
            const std::int32_t contacts =
                fbe_entity_collisions(mover, touched.data(), touched.size());
            bits.push_back(static_cast<std::uint32_t>(contacts));
            for (std::int32_t c = 0; c < contacts && c < 16; ++c) {
                const fbe_collision_t& one = touched[static_cast<std::size_t>(c)];
                bits.push_back(std::bit_cast<std::uint32_t>(one.point.x));
                bits.push_back(std::bit_cast<std::uint32_t>(one.point.y));
                bits.push_back(std::bit_cast<std::uint32_t>(one.point.z));
                bits.push_back(std::bit_cast<std::uint32_t>(one.normal.x));
                bits.push_back(std::bit_cast<std::uint32_t>(one.normal.y));
                bits.push_back(std::bit_cast<std::uint32_t>(one.normal.z));
                bits.push_back(std::bit_cast<std::uint32_t>(one.time));
            }
        }
    }
    free_world(world);
    return bits;
}

} // namespace

TEST_CASE("moves come out the same here and on the workers, positions and contacts", "[scene]") {
    for (const int count : { 16, 17, 63, 64, 200 }) {
        std::vector<std::uint32_t> here;
        std::vector<std::uint32_t> workers;
        std::vector<std::uint32_t> worth;
        {
            const path_t path(fbe::scene::move_path_t::HERE);
            here = walk(count);
        }
        {
            const path_t path(fbe::scene::move_path_t::WORKERS);
            workers = walk(count);
        }
        worth = walk(count);
        CAPTURE(count);
        CHECK(here == workers);
        CHECK(here == worth);
    }
}

TEST_CASE("a long sweep leaves no more than a cap in a thread's scratch", "[scene]") {
    const path_t path(fbe::scene::move_path_t::HERE);
    const std::uint64_t world = fbe_world_create();
    fbe_world_set_collision(world, mover_group, level_group, FBE_RESPONSE_SLIDE);
    const std::uint64_t terrain = fbe_entity_create(world, 0, FBE_ENTITY_TERRAIN);
    fbe_terrain_set_size(terrain, 512);
    fbe_entity_set_collision_group(terrain, level_group);
    const std::uint64_t mover = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_collision_group(mover, mover_group);
    fbe_entity_set_collision_radius(mover, 0.5F, 0.5F);
    fbe_entity_set_position(mover, 1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD);
    fbe_world_collide(world);
    // From corner to corner, its box over every square of the grid.
    fbe_entity_set_position(mover, 511.0F, 1.0F, 511.0F, FBE_SPACE_WORLD);
    fbe_world_collide(world);
    CHECK(fbe::scene::move_scratch_kept() <= std::size_t{ 1 } << 16U);
    free_world(world);
}
