// Players through the C API: a clip's nodes bound to entities by name, posed at a time, moved on by
// a world's update, and what a player's, or its clip's, or its entity's freeing leaves.

#include "fbengine.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace {

bool says(std::string_view message) {
    return std::string_view(fbe_error()).contains(message);
}

std::uint64_t pivot(std::uint64_t world, std::uint64_t parent, const char* name) {
    const std::uint64_t made = fbe_entity_create(world, parent, FBE_ENTITY_PIVOT);
    fbe_entity_set_name(made, name);
    return made;
}

fbe_vec3_t position(std::uint64_t entity) {
    fbe_vec3_t found{};
    fbe_entity_position(entity, FBE_SPACE_LOCAL, &found);
    return found;
}

/// A clip moving "hips" from x 0 to 4 over 2 seconds, and turning "head" a half turn about Y.
std::uint64_t walk() {
    const std::uint64_t clip = fbe_clip_create("walk", 0.0F);
    const std::array<float, 2> times{ 0.0F, 2.0F };
    const std::array<float, 6> places{ 0.0F, 0.0F, 0.0F, 4.0F, 0.0F, 0.0F };
    fbe_clip_add_channel(clip, "hips", FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3, times.data(),
                         places.data(), 2);
    const std::array<float, 8> turns{ 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F };
    fbe_clip_add_channel(clip, "head", FBE_CLIP_ROTATION, FBE_CLIP_LINEAR, 4, times.data(),
                         turns.data(), 2);
    return clip;
}

} // namespace

TEST_CASE("a player poses the entities its clip names, at a time") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    const std::uint64_t spine = pivot(world, hips, "spine");
    const std::uint64_t head = pivot(world, spine, "head");
    fbe_entity_set_position(head, 0.0F, 1.0F, 0.0F, FBE_SPACE_LOCAL);
    const std::uint64_t player = fbe_player_create(hips);
    REQUIRE(player != 0);
    CHECK(fbe_player_entity(player) == hips);
    CHECK(fbe_player_clip(player) == 0);
    const std::uint64_t clip = walk();
    REQUIRE(fbe_player_play(player, clip));
    CHECK(fbe_player_clip(player) == clip);
    CHECK(fbe_player_time(player) == 0.0F);
    // Its own name, and one two under it, each found.
    REQUIRE(fbe_player_set_time(player, 1.0F));
    CHECK(position(hips).x == Catch::Approx(2.0F));
    fbe_quat_t turned{};
    fbe_entity_rotation(head, FBE_SPACE_LOCAL, &turned);
    CHECK(std::abs(turned.y) == Catch::Approx(std::sqrt(0.5F)));
    // What the clip doesn't move stays as it was.
    CHECK(position(head).y == 1.0F);
    CHECK(position(spine).x == 0.0F);
    // Its world transform follows.
    fbe_vec3_t world_head{};
    fbe_entity_position(head, FBE_SPACE_WORLD, &world_head);
    CHECK(world_head.x == Catch::Approx(2.0F));
    // Before its first key and after its last, it holds them.
    fbe_player_set_time(player, -3.0F);
    CHECK(position(hips).x == 0.0F);
    fbe_player_set_time(player, 9.0F);
    CHECK(position(hips).x == 4.0F);
    CHECK(fbe_player_time(player) == 9.0F);
    fbe_player_free(player);
    fbe_clip_release(clip);
    fbe_world_free(world);
}

TEST_CASE("a world's update moves on its players, and holds a clip at its end") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t other = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    const std::uint64_t elsewhere = pivot(other, 0, "hips");
    const std::uint64_t clip = walk();
    const std::uint64_t player = fbe_player_create(hips);
    const std::uint64_t still = fbe_player_create(elsewhere);
    fbe_player_play(player, clip);
    fbe_player_play(still, clip);
    REQUIRE(fbe_player_advance(world, 0.5F));
    CHECK(fbe_player_time(player) == 0.5F);
    CHECK(position(hips).x == Catch::Approx(1.0F));
    // Only that world's.
    CHECK(fbe_player_time(still) == 0.0F);
    CHECK(position(elsewhere).x == 0.0F);
    fbe_player_advance(world, 10.0F);
    CHECK(fbe_player_time(player) == 2.0F);
    CHECK(position(hips).x == 4.0F);
    // Stopped, it leaves them where they are.
    REQUIRE(fbe_player_play(player, 0));
    CHECK(fbe_player_clip(player) == 0);
    CHECK(fbe_player_time(player) == 0.0F);
    fbe_player_advance(world, 1.0F);
    CHECK(position(hips).x == 4.0F);
    CHECK(!fbe_player_advance(world, -1.0F));
    CHECK(says("0 seconds or more, not -1"));
    CHECK(!fbe_player_advance(world, std::nanf("")));
    CHECK(!fbe_player_advance(player, 1.0F));
    fbe_player_free(player);
    fbe_player_free(still);
    fbe_clip_release(clip);
    fbe_world_free(world);
    fbe_world_free(other);
}

TEST_CASE("a node no entity has is left out, and a channel added since is bound when it poses") {
    const std::uint64_t world = fbe_world_create();
    // Named as a node, but not under the player's entity, so not found.
    const std::uint64_t hips = pivot(world, 0, "root");
    const std::uint64_t head = pivot(world, 0, "head");
    const std::uint64_t clip = walk();
    const std::uint64_t player = fbe_player_create(hips);
    REQUIRE(fbe_player_play(player, clip));
    fbe_player_set_time(player, 1.0F);
    CHECK(position(hips).x == 0.0F);
    fbe_quat_t turned{};
    fbe_entity_rotation(head, FBE_SPACE_LOCAL, &turned);
    CHECK(turned.w == 1.0F);
    const std::array<float, 1> times{ 0.0F };
    const std::array<float, 3> big{ 2.0F, 2.0F, 2.0F };
    fbe_clip_add_channel(clip, "root", FBE_CLIP_SCALE, FBE_CLIP_STEP, 3, times.data(), big.data(),
                         1);
    fbe_player_set_time(player, 1.5F);
    fbe_vec3_t scale{};
    fbe_entity_scale(hips, FBE_SPACE_LOCAL, &scale);
    CHECK(scale.y == 2.0F);
    fbe_player_free(player);
    fbe_clip_release(clip);
    fbe_world_free(world);
}

TEST_CASE("a played clip lives on after its handle goes, until it's stopped") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    const std::uint64_t clip = walk();
    const std::uint64_t player = fbe_player_create(hips);
    fbe_player_play(player, clip);
    REQUIRE(fbe_clip_release(clip));
    CHECK(fbe_clip_alive(clip));
    fbe_player_advance(world, 1.0F);
    CHECK(position(hips).x == Catch::Approx(2.0F));
    // Played again, it isn't let go of before it's held again.
    REQUIRE(fbe_player_play(player, clip));
    CHECK(fbe_clip_alive(clip));
    fbe_player_play(player, 0);
    CHECK(!fbe_clip_alive(clip));
    CHECK(!fbe_player_play(player, clip));
    CHECK(says("the clip was freed"));
    // Freeing a player lets go of what it plays.
    const std::uint64_t again = walk();
    fbe_player_play(player, again);
    fbe_clip_release(again);
    REQUIRE(fbe_player_free(player));
    CHECK(!fbe_clip_alive(again));
    CHECK(!fbe_player_alive(player));
    CHECK(!fbe_player_free(player));
    CHECK(says("the player was freed"));
    fbe_world_free(world);
}

TEST_CASE("a player's entity freed leaves it playing nothing, and its calls failing") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    const std::uint64_t leg = pivot(world, hips, "leg");
    const std::uint64_t clip = walk();
    const std::uint64_t player = fbe_player_create(hips);
    fbe_player_play(player, clip);
    // A bound entity freed is left out.
    fbe_entity_free(leg);
    CHECK(fbe_player_set_time(player, 1.0F));
    fbe_entity_free(hips);
    CHECK(fbe_player_alive(player));
    CHECK(!fbe_player_set_time(player, 1.0F));
    CHECK(says("the player's entity was freed"));
    CHECK(!fbe_player_play(player, clip));
    // The world's update lets go of what it played.
    fbe_clip_release(clip);
    CHECK(fbe_clip_alive(clip));
    fbe_player_advance(world, 0.1F);
    CHECK(!fbe_clip_alive(clip));
    CHECK(fbe_player_free(player));
    CHECK(fbe_player_create(hips) == 0);
    // Another kind's handle isn't a player's.
    CHECK(!fbe_player_alive(world));
    CHECK(fbe_player_entity(world) == 0);
    fbe_world_free(world);
}
