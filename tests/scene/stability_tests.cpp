// The scene through what a long-running game does to it: transforms that aren't finite refused, or
// past a float as they're put together, picked, collided, and bounded all the same; what's freed
// while it plays, is picked, or is under something; and thousands of frames of making and freeing,
// with what's kept not growing.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string_view>
#include <vector>

#ifdef _WIN32
// NOLINTBEGIN(llvm-include-order): windows.h before psapi.h.
#include <windows.h>
#include <psapi.h>
// NOLINTEND(llvm-include-order)
#else
#include <fstream>
#include <unistd.h>
#endif

namespace {

constexpr float not_a_number = std::numeric_limits<float>::quiet_NaN();
constexpr float endless = std::numeric_limits<float>::infinity();

/// A world of the test's own, freed at its end, with what it queued as freed let go of.
struct test_world_t {
    std::uint64_t handle = fbe_world_create();

    test_world_t() = default;
    test_world_t(const test_world_t&) = delete;
    test_world_t& operator=(const test_world_t&) = delete;
    test_world_t(test_world_t&&) = delete;
    test_world_t& operator=(test_world_t&&) = delete;

    ~test_world_t() {
        fbe_world_free(handle);
        drain();
    }

    static void drain() {
        std::array<std::uint64_t, 64> some{};
        while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    }
};

bool says(std::string_view message) {
    return std::string_view(fbe_error()).contains(message);
}

fbe_vec3_t position_of(std::uint64_t entity, std::int32_t space = FBE_SPACE_LOCAL) {
    fbe_vec3_t at{};
    fbe_entity_position(entity, space, &at);
    return at;
}

std::uint64_t cube(std::uint64_t world, std::uint64_t parent = 0) {
    const std::uint64_t made = fbe_entity_create(world, parent, FBE_ENTITY_MESH);
    const std::uint64_t geometry = fbe_geometry_cube();
    fbe_mesh_set_geometry(made, geometry);
    fbe_geometry_release(geometry);
    return made;
}

/// How much memory the process holds now, in bytes.
std::int64_t held_now() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                         sizeof counters);
    return static_cast<std::int64_t>(counters.PrivateUsage);
#else
    std::ifstream statm("/proc/self/statm");
    std::int64_t pages = 0;
    std::int64_t resident = 0;
    statm >> pages >> resident;
    return resident * sysconf(_SC_PAGESIZE);
#endif
}

} // namespace

TEST_CASE("what isn't a finite number, given as where an entity is, its turn, or its scale, is "
          "refused, and it's left as it was",
          "[scene]") {
    const test_world_t world;
    const std::uint64_t entity = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    REQUIRE(fbe_entity_set_position(entity, 1.0F, 2.0F, 3.0F, FBE_SPACE_LOCAL));
    for (const float bad : { not_a_number, endless, -endless }) {
        for (const std::int32_t space : { FBE_SPACE_LOCAL, FBE_SPACE_WORLD }) {
            CHECK_FALSE(fbe_entity_set_position(entity, bad, 0.0F, 0.0F, space));
            CHECK(says("isn't finite"));
            CHECK_FALSE(fbe_entity_set_position(entity, 0.0F, 0.0F, bad, space));
            CHECK_FALSE(fbe_entity_set_rotation(entity, 0.0F, bad, 0.0F, 1.0F, space));
            CHECK(says("isn't finite"));
            CHECK_FALSE(fbe_entity_set_scale(entity, 1.0F, bad, 1.0F, space));
            CHECK_FALSE(fbe_entity_translate(entity, bad, 0.0F, 0.0F, space));
            CHECK_FALSE(fbe_entity_turn(entity, bad, 0.0F, 0.0F, 1.0F, space));
        }
        CHECK_FALSE(fbe_entity_move(entity, 0.0F, bad, 0.0F));
        CHECK_FALSE(fbe_entity_point_at(entity, bad, 0.0F, 0.0F, 0.0F));
        CHECK_FALSE(fbe_entity_point_at(entity, 0.0F, 0.0F, 5.0F, bad));
        const fbe_pose_t pose{ .position = { .x = bad, .y = 0.0F, .z = 0.0F },
                               .rotation = { .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F } };
        CHECK_FALSE(fbe_entity_set_poses(&entity, &pose, 1, FBE_SPACE_LOCAL));
        CHECK(says("isn't finite"));
    }
    // Nor a move that ends past a float.
    REQUIRE(fbe_entity_set_position(entity, 3.0e38F, 2.0F, 3.0F, FBE_SPACE_LOCAL));
    CHECK_FALSE(fbe_entity_translate(entity, 3.0e38F, 0.0F, 0.0F, FBE_SPACE_LOCAL));
    CHECK(says("where the translation goes isn't finite"));
    CHECK_FALSE(fbe_entity_translate(entity, 3.0e38F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    CHECK_FALSE(fbe_entity_move(entity, 3.0e38F, 0.0F, 0.0F));
    CHECK(position_of(entity).x == 3.0e38F);
    REQUIRE(fbe_entity_set_position(entity, 1.0F, 2.0F, 3.0F, FBE_SPACE_LOCAL));
    // A rotation of no length is none, as one that's made length 1 can't be.
    REQUIRE(fbe_entity_set_rotation(entity, 0.0F, 0.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL));
    const fbe_vec3_t at = position_of(entity);
    CHECK((at.x == 1.0F && at.y == 2.0F && at.z == 3.0F));
    fbe_quat_t turned{};
    REQUIRE(fbe_entity_rotation(entity, FBE_SPACE_LOCAL, &turned));
    CHECK(turned.w == 1.0F);
    fbe_vec3_t scale{};
    REQUIRE(fbe_entity_scale(entity, FBE_SPACE_LOCAL, &scale));
    CHECK((scale.x == 1.0F && scale.y == 1.0F && scale.z == 1.0F));
}

TEST_CASE("an Instances' matrices that aren't finite are refused, the others kept", "[scene]") {
    const test_world_t world;
    const std::uint64_t made = fbe_entity_create(world.handle, 0, FBE_ENTITY_INSTANCES);
    const std::uint64_t geometry = fbe_geometry_cube();
    REQUIRE(fbe_instances_set_geometry(made, geometry));
    fbe_geometry_release(geometry);
    fbe_instance_t good{};
    for (const int i : { 0, 5, 10, 15 }) {
        good.matrix.m[i] = 1.0F;
    }
    good.color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F };
    std::array<fbe_instance_t, 2> two{ good, good };
    REQUIRE(fbe_instances_set(made, two.data(), 2));
    fbe_instance_t bad = good;
    bad.matrix.m[12] = not_a_number;
    two[1] = bad;
    CHECK_FALSE(fbe_instances_set(made, two.data(), 2));
    CHECK(says("isn't finite"));
    CHECK(fbe_instances_count(made) == 2);
    CHECK_FALSE(fbe_instances_set_one(made, 0, &bad));
    CHECK_FALSE(fbe_instances_set(made, nullptr, 2));
    CHECK_FALSE(fbe_instances_set_one(made, 0, nullptr));
    bad.matrix.m[12] = 0.0F;
    bad.matrix.m[5] = endless;
    CHECK_FALSE(fbe_instances_set_one(made, 1, &bad));
    fbe_instance_t kept{};
    REQUIRE(fbe_instances_get(made, 1, &kept));
    CHECK(kept.matrix.m[5] == 1.0F);
}

TEST_CASE("entities past a float once put together are picked, collided, and bounded all the "
          "same",
          "[scene]") {
    const test_world_t world;
    // Finite each, but past a float in the world: far, scaled far, under what's far and scaled.
    const std::uint64_t distant = cube(world.handle);
    REQUIRE(fbe_entity_set_position(distant, 3.0e38F, 0.0F, 0.0F, FBE_SPACE_LOCAL));
    REQUIRE(fbe_entity_set_scale(distant, 1.0e30F, 1.0e30F, 1.0e30F, FBE_SPACE_LOCAL));
    const std::uint64_t under = cube(world.handle, distant);
    REQUIRE(fbe_entity_set_position(under, 3.0e38F, 3.0e38F, 0.0F, FBE_SPACE_LOCAL));
    const std::uint64_t close = cube(world.handle);
    for (const std::uint64_t each : { distant, under, close }) {
        fbe_entity_set_pick_mode(each, FBE_PICK_MESH);
        fbe_entity_set_collision_group(each, 1);
        fbe_entity_set_collision_radius(each, 0.5F, 0.5F);
    }
    REQUIRE(fbe_world_set_collision(world.handle, 1, 1, FBE_RESPONSE_SLIDE));
    REQUIRE(fbe_entity_set_position(close, 0.0F, 0.0F, -5.0F, FBE_SPACE_LOCAL));
    const fbe_vec3_t from{ .x = 0.0F, .y = 0.0F, .z = 0.0F };
    const fbe_vec3_t to{ .x = 0.0F, .y = 0.0F, .z = -10.0F };
    fbe_pick_t found{};
    CHECK(fbe_world_pick(world.handle, &from, &to, 0.0F, 0, &found) == 1);
    CHECK(found.entity == close);
    CHECK(fbe_world_pick(world.handle, &from, &to, 0.5F, 0, &found) == 1);
    // A pick from or to what isn't finite hits nothing.
    const fbe_vec3_t nowhere{ .x = not_a_number, .y = 0.0F, .z = 0.0F };
    const fbe_vec3_t forever{ .x = 0.0F, .y = 0.0F, .z = -endless };
    CHECK(fbe_world_pick(world.handle, &nowhere, &to, 0.0F, 0, &found) <= 0);
    CHECK(fbe_world_pick(world.handle, &from, &forever, 0.0F, 0, &found) <= 0);
    CHECK(fbe_world_pick(world.handle, &from, &to, not_a_number, 0, &found) <= 0);
    CHECK(fbe_world_pick(world.handle, &from, &to, endless, 0, &found) <= 0);
    REQUIRE(fbe_entity_set_position(close, 0.0F, 0.0F, -4.0F, FBE_SPACE_LOCAL));
    CHECK(fbe_world_collide(world.handle));
    fbe_aabb_t bounds{};
    (void)fbe_entity_bounds(under, &bounds);
    (void)fbe_entity_can_see(close, under);
    (void)fbe_entity_can_see_point(close, not_a_number, 0.0F, 0.0F);
    const fbe_vec3_t middle = position_of(close, FBE_SPACE_WORLD);
    CHECK(std::isfinite(middle.z));
}

TEST_CASE("an entity freed while its clip plays, or under one that is, leaves the rest playing",
          "[scene]") {
    const test_world_t world;
    const std::uint64_t root = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    const std::uint64_t arm = fbe_entity_create(world.handle, root, FBE_ENTITY_PIVOT);
    const std::uint64_t hand = fbe_entity_create(world.handle, arm, FBE_ENTITY_PIVOT);
    const std::uint64_t leg = fbe_entity_create(world.handle, root, FBE_ENTITY_PIVOT);
    REQUIRE(fbe_entity_set_name(arm, "arm"));
    REQUIRE(fbe_entity_set_name(hand, "hand"));
    REQUIRE(fbe_entity_set_name(leg, "leg"));
    const std::uint64_t clip = fbe_clip_create("walk", 30.0F);
    const std::array<float, 2> times{ 0.0F, 1.0F };
    const std::array<float, 6> across{ 0.0F, 0.0F, 0.0F, 2.0F, 0.0F, 0.0F };
    for (const char* node : { "arm", "hand", "leg" }) {
        REQUIRE(fbe_clip_add_channel(clip, node, FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3,
                                     times.data(), across.data(), 2));
    }
    const std::uint64_t player = fbe_player_create(root);
    REQUIRE(fbe_player_play(player, 0, clip, FBE_PLAYER_LOOP, 1.0F));
    REQUIRE(fbe_player_advance(world.handle, 0.25F));
    // The arm, and the hand under it, freed between advances; then the leg moved to another World.
    REQUIRE(fbe_entity_free(arm));
    CHECK_FALSE(fbe_entity_alive(hand));
    REQUIRE(fbe_player_advance(world.handle, 0.25F));
    CHECK(position_of(leg).x == 1.0F);
    const test_world_t other;
    const std::uint64_t there = fbe_entity_create(other.handle, 0, FBE_ENTITY_PIVOT);
    REQUIRE(fbe_entity_set_parent(leg, there, true));
    REQUIRE(fbe_player_advance(world.handle, 0.25F));
    REQUIRE(fbe_player_advance(other.handle, 0.25F));
    // An entity made again with the freed one's name is played, as the clip goes by names.
    const std::uint64_t again = fbe_entity_create(world.handle, root, FBE_ENTITY_PIVOT);
    REQUIRE(fbe_entity_set_name(again, "arm"));
    REQUIRE(fbe_player_advance(world.handle, 0.25F));
    CHECK(std::isfinite(position_of(again).x));
    // The root freed: the player's left playing nothing, and its calls fail.
    REQUIRE(fbe_entity_free(root));
    REQUIRE(fbe_player_advance(world.handle, 0.25F));
    CHECK_FALSE(fbe_player_play(player, 0, clip, FBE_PLAYER_LOOP, 1.0F));
    fbe_player_free(player);
    fbe_clip_release(clip);
}

TEST_CASE("what a pick or a collision found, freed after, is refused, not taken for another",
          "[scene]") {
    const test_world_t world;
    const std::uint64_t wall = cube(world.handle);
    REQUIRE(fbe_entity_set_position(wall, 0.0F, 0.0F, -5.0F, FBE_SPACE_LOCAL));
    REQUIRE(fbe_entity_set_pick_mode(wall, FBE_PICK_MESH));
    REQUIRE(fbe_entity_set_collision_group(wall, 2));
    const std::uint64_t ball = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    REQUIRE(fbe_entity_set_collision_group(ball, 1));
    REQUIRE(fbe_entity_set_collision_radius(ball, 0.5F, 0.5F));
    REQUIRE(fbe_world_set_collision(world.handle, 1, 2, FBE_RESPONSE_STOP));
    REQUIRE(fbe_world_collide(world.handle));
    REQUIRE(fbe_entity_set_position(ball, 0.0F, 0.0F, -5.0F, FBE_SPACE_LOCAL));
    REQUIRE(fbe_world_collide(world.handle));
    std::array<fbe_collision_t, 4> hits{};
    const std::int32_t count = fbe_entity_collisions(ball, hits.data(), 4);
    REQUIRE(count >= 1);
    const fbe_vec3_t from{ .x = 0.0F, .y = 0.0F, .z = 0.0F };
    const fbe_vec3_t to{ .x = 0.0F, .y = 0.0F, .z = -10.0F };
    fbe_pick_t found{};
    REQUIRE(fbe_world_pick(world.handle, &from, &to, 0.0F, ball, &found) == 1);
    REQUIRE(found.entity == wall);
    REQUIRE(fbe_entity_free(wall));
    // Made again in its slot, the old handle still isn't it.
    const std::uint64_t made = cube(world.handle);
    CHECK(made != wall);
    CHECK_FALSE(fbe_entity_alive(found.entity));
    CHECK_FALSE(fbe_entity_alive(hits[0].entity));
    fbe_vec3_t at{};
    CHECK_FALSE(fbe_entity_position(hits[0].entity, FBE_SPACE_LOCAL, &at));
    // The ball's collisions are reset by its next collide, the freed wall's gone from them.
    REQUIRE(fbe_world_collide(world.handle));
    const std::int32_t after = fbe_entity_collisions(ball, hits.data(), 4);
    for (std::int32_t i = 0; i < after; ++i) {
        CHECK(fbe_entity_alive(hits[static_cast<std::size_t>(i)].entity));
    }
}

TEST_CASE("a parent made a child of what's under it is refused, and freeing it first frees all",
          "[scene]") {
    const test_world_t world;
    const std::uint64_t top = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    const std::uint64_t middle = fbe_entity_create(world.handle, top, FBE_ENTITY_PIVOT);
    const std::uint64_t bottom = fbe_entity_create(world.handle, middle, FBE_ENTITY_PIVOT);
    CHECK_FALSE(fbe_entity_set_parent(top, bottom, false));
    CHECK_FALSE(fbe_entity_set_parent(top, top, true));
    CHECK_FALSE(fbe_entity_set_parent(middle, bottom, true));
    CHECK(fbe_entity_parent(bottom) == middle);
    CHECK(fbe_entity_parent(top) == 0);
    REQUIRE(fbe_entity_free(top));
    for (const std::uint64_t each : { top, middle, bottom }) {
        CHECK_FALSE(fbe_entity_alive(each));
        CHECK_FALSE(fbe_entity_set_parent(each, 0, false));
    }
    CHECK(fbe_world_entity_count(world.handle) == 0);
    std::array<std::uint64_t, 8> freed{};
    CHECK(fbe_entity_freed(freed.data(), freed.size()) == 3);
}

TEST_CASE("thousands of frames of making, moving, playing, picking, and freeing keep what's held "
          "the same",
          "[scene]") {
    const test_world_t world;
    std::mt19937 random(5);
    const std::uint64_t clip = fbe_clip_create("bob", 30.0F);
    const std::array<float, 2> times{ 0.0F, 1.0F };
    const std::array<float, 6> up{ 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F };
    REQUIRE(fbe_clip_add_channel(clip, "", FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3, times.data(),
                                 up.data(), 2));
    std::vector<std::uint64_t> live;
    std::vector<std::uint64_t> players;
    constexpr std::array<std::int32_t, 5> kinds{ FBE_ENTITY_PIVOT, FBE_ENTITY_MESH,
                                                 FBE_ENTITY_LIGHT, FBE_ENTITY_CAMERA,
                                                 FBE_ENTITY_INSTANCES };
    std::int64_t held_at_start = 0;
    const auto frame = [&](int number) {
        for (int i = 0; i < 12; ++i) {
            const std::uint64_t parent = live.empty() || random() % 3 == 0 ? 0 : live[random() % live.size()];
            const std::int32_t kind = kinds[random() % kinds.size()];
            const std::uint64_t made = kind == FBE_ENTITY_MESH ? cube(world.handle, parent)
                                                               : fbe_entity_create(world.handle, parent, kind);
            REQUIRE(made != 0);
            if (kind == FBE_ENTITY_INSTANCES) {
                const std::uint64_t geometry = fbe_geometry_cube();
                fbe_instances_set_geometry(made, geometry);
                fbe_geometry_release(geometry);
                fbe_instance_t one{};
                for (const int at : { 0, 5, 10, 15 }) {
                    one.matrix.m[at] = 1.0F;
                }
                fbe_instances_set(made, &one, 1);
            }
            fbe_entity_set_pick_mode(made, FBE_PICK_BOX);
            fbe_entity_set_position(made, static_cast<float>(random() % 41) - 20.0F, 0.0F,
                                    static_cast<float>(random() % 41) - 20.0F, FBE_SPACE_WORLD);
            if (random() % 4 == 0) {
                const std::uint64_t player = fbe_player_create(made);
                fbe_player_play(player, 0, clip, FBE_PLAYER_LOOP, 1.0F);
                players.push_back(player);
            }
            live.push_back(made);
        }
        // About as many freed, any of them, which frees what's under it too.
        for (int i = 0; i < 12 && !live.empty(); ++i) {
            const std::size_t at = random() % live.size();
            fbe_entity_free(live[at]);
            live[at] = live.back();
            live.pop_back();
        }
        std::erase_if(live, [](std::uint64_t each) { return !fbe_entity_alive(each); });
        std::erase_if(players, [](std::uint64_t each) {
            if (fbe_entity_alive(fbe_player_entity(each))) {
                return false;
            }
            fbe_player_free(each);
            return true;
        });
        if (!live.empty() && random() % 2 == 0) {
            fbe_entity_set_parent(live[random() % live.size()], live[random() % live.size()], true);
        }
        REQUIRE(fbe_player_advance(world.handle, 1.0F / 60.0F));
        REQUIRE(fbe_world_collide(world.handle));
        const fbe_vec3_t from{ .x = -30.0F, .y = 0.0F, .z = 0.0F };
        const fbe_vec3_t to{ .x = 30.0F, .y = 0.0F, .z = 0.0F };
        fbe_pick_t found{};
        (void)fbe_world_pick(world.handle, &from, &to, 0.25F, 0, &found);
        test_world_t::drain();
        if (number == 1000) {
            held_at_start = held_now();
        }
    };
    for (int number = 0; number < 6000; ++number) {
        frame(number);
    }
    const std::int64_t held_at_end = held_now();
    CAPTURE(held_at_start, held_at_end, live.size(), players.size());
    // As many live as there were, give or take, and what's held about what it was then.
    CHECK(live.size() < 400);
    CHECK(held_at_end < held_at_start + (8 << 20));
    for (const std::uint64_t player : players) {
        fbe_player_free(player);
    }
    fbe_clip_release(clip);
}
