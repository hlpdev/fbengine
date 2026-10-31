// Players through the C API: a clip's nodes bound to entities by name, posed at a time, moved on by
// a world's update, and what a player's, or its clip's, or its entity's freeing leaves.

#include "fbengine.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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
    CHECK(fbe_player_clip(player, 0) == 0);
    const std::uint64_t clip = walk();
    REQUIRE(fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F));
    CHECK(fbe_player_clip(player, 0) == clip);
    CHECK(fbe_player_time(player, 0) == 0.0F);
    // Its own name, and one two under it, each found.
    REQUIRE(fbe_player_set_time(player, 0, 1.0F));
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
    fbe_player_set_time(player, 0, -3.0F);
    CHECK(position(hips).x == 0.0F);
    fbe_player_set_time(player, 0, 9.0F);
    CHECK(position(hips).x == 4.0F);
    // Played once, its time is held at its end.
    CHECK(fbe_player_time(player, 0) == 2.0F);
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
    fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F);
    fbe_player_play(still, 0, clip, FBE_PLAYER_ONCE, 1.0F);
    REQUIRE(fbe_player_advance(world, 0.5F));
    CHECK(fbe_player_time(player, 0) == 0.5F);
    CHECK(position(hips).x == Catch::Approx(1.0F));
    // Only that world's.
    CHECK(fbe_player_time(still, 0) == 0.0F);
    CHECK(position(elsewhere).x == 0.0F);
    fbe_player_advance(world, 10.0F);
    CHECK(fbe_player_time(player, 0) == 2.0F);
    CHECK(position(hips).x == 4.0F);
    // Stopped, it leaves them where they are.
    REQUIRE(fbe_player_stop(player, 0, 0.0F));
    CHECK(fbe_player_clip(player, 0) == 0);
    CHECK(fbe_player_time(player, 0) == 0.0F);
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
    REQUIRE(fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F));
    fbe_player_set_time(player, 0, 1.0F);
    CHECK(position(hips).x == 0.0F);
    fbe_quat_t turned{};
    fbe_entity_rotation(head, FBE_SPACE_LOCAL, &turned);
    CHECK(turned.w == 1.0F);
    const std::array<float, 1> times{ 0.0F };
    const std::array<float, 3> big{ 2.0F, 2.0F, 2.0F };
    fbe_clip_add_channel(clip, "root", FBE_CLIP_SCALE, FBE_CLIP_STEP, 3, times.data(), big.data(),
                         1);
    fbe_player_set_time(player, 0, 1.5F);
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
    fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F);
    REQUIRE(fbe_clip_release(clip));
    CHECK(fbe_clip_alive(clip));
    fbe_player_advance(world, 1.0F);
    CHECK(position(hips).x == Catch::Approx(2.0F));
    // Played again, it isn't let go of before it's held again.
    REQUIRE(fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F));
    CHECK(fbe_clip_alive(clip));
    fbe_player_stop(player, 0, 0.0F);
    CHECK(!fbe_clip_alive(clip));
    CHECK(!fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F));
    CHECK(says("the clip was freed"));
    // Freeing a player lets go of what it plays.
    const std::uint64_t again = walk();
    fbe_player_play(player, 0, again, FBE_PLAYER_ONCE, 1.0F);
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
    fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F);
    // A bound entity freed is left out.
    fbe_entity_free(leg);
    CHECK(fbe_player_set_time(player, 0, 1.0F));
    fbe_entity_free(hips);
    CHECK(fbe_player_alive(player));
    CHECK(!fbe_player_set_time(player, 0, 1.0F));
    CHECK(says("the player's entity was freed"));
    CHECK(!fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F));
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

namespace {

/// A clip moving "hips" along x from `from` to `to` over `seconds`.
std::uint64_t sliding(float from, float to, float seconds) {
    const std::uint64_t clip = fbe_clip_create("slide", 0.0F);
    const std::array<float, 2> times{ 0.0F, seconds };
    const std::array<float, 6> places{ from, 0.0F, 0.0F, to, 0.0F, 0.0F };
    fbe_clip_add_channel(clip, "hips", FBE_CLIP_TRANSLATION, FBE_CLIP_LINEAR, 3, times.data(),
                         places.data(), 2);
    return clip;
}

/// Each event the player reached, as "name@time", in order, let go of.
std::vector<std::string> reached(std::uint64_t player) {
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(fbe_player_event_count(player)));
    for (int32_t i = 0; i < fbe_player_event_count(player); ++i) {
        names.push_back(std::format("{}@{}", fbe_player_event_name(player, i),
                                    fbe_player_event_time(player, i)));
    }
    fbe_player_clear_events(player);
    return names;
}

/// A clip putting "hips" and "head" at x `x`.
std::uint64_t both_at(float x) {
    const std::uint64_t clip = fbe_clip_create("", 0.0F);
    const std::array<float, 1> times{ 0.0F };
    const std::array<float, 3> place{ x, 0.0F, 0.0F };
    fbe_clip_add_channel(clip, "hips", FBE_CLIP_TRANSLATION, FBE_CLIP_STEP, 3, times.data(),
                         place.data(), 1);
    fbe_clip_add_channel(clip, "head", FBE_CLIP_TRANSLATION, FBE_CLIP_STEP, 3, times.data(),
                         place.data(), 1);
    return clip;
}

/// A clip turning "hips" to `rotation`.
std::uint64_t turned_to(std::array<float, 4> rotation) {
    const std::uint64_t clip = fbe_clip_create("", 0.0F);
    const std::array<float, 1> times{ 0.0F };
    fbe_clip_add_channel(clip, "hips", FBE_CLIP_ROTATION, FBE_CLIP_STEP, 4, times.data(),
                         rotation.data(), 1);
    return clip;
}

} // namespace

TEST_CASE("a clip plays once, round a loop, or there and back, at its speed") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    const std::uint64_t player = fbe_player_create(hips);
    const std::uint64_t clip = sliding(0.0F, 2.0F, 2.0F);
    REQUIRE(fbe_player_play(player, 0, clip, FBE_PLAYER_LOOP, 1.0F));
    fbe_player_advance(world, 2.5F);
    CHECK(fbe_player_time(player, 0) == Catch::Approx(0.5F));
    CHECK(position(hips).x == Catch::Approx(0.5F));
    CHECK(fbe_player_playing(player, 0));
    // There and back: 2.5 seconds in is half a second back from the end.
    fbe_player_play(player, 0, clip, FBE_PLAYER_PING_PONG, 1.0F);
    fbe_player_set_time(player, 0, 0.0F);
    fbe_player_advance(world, 2.5F);
    CHECK(fbe_player_time(player, 0) == Catch::Approx(1.5F));
    fbe_player_advance(world, 2.0F);
    CHECK(fbe_player_time(player, 0) == Catch::Approx(0.5F));
    fbe_player_advance(world, 0.25F);
    CHECK(fbe_player_time(player, 0) == Catch::Approx(0.75F));
    // Twice as fast, once: it holds its end, and isn't playing then.
    fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 2.0F);
    fbe_player_set_time(player, 0, 0.0F);
    fbe_player_advance(world, 0.5F);
    CHECK(fbe_player_time(player, 0) == Catch::Approx(1.0F));
    fbe_player_advance(world, 5.0F);
    CHECK(fbe_player_time(player, 0) == 2.0F);
    CHECK(!fbe_player_playing(player, 0));
    // Backward, from its end.
    fbe_player_stop(player, 0, 0.0F);
    fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, -1.0F);
    CHECK(fbe_player_time(player, 0) == 2.0F);
    fbe_player_advance(world, 0.5F);
    CHECK(fbe_player_time(player, 0) == Catch::Approx(1.5F));
    CHECK(fbe_player_speed(player, 0) == -1.0F);
    fbe_player_set_speed(player, 0, 2.0F);
    fbe_player_advance(world, 0.25F);
    CHECK(fbe_player_time(player, 0) == Catch::Approx(2.0F));
    fbe_player_free(player);
    fbe_clip_release(clip);
    fbe_world_free(world);
}

TEST_CASE("playing the clip a layer plays goes on where it is") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    const std::uint64_t player = fbe_player_create(hips);
    const std::uint64_t clip = sliding(0.0F, 2.0F, 2.0F);
    fbe_player_play(player, 0, clip, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_advance(world, 0.5F);
    fbe_player_play(player, 0, clip, FBE_PLAYER_LOOP, 2.0F);
    CHECK(fbe_player_time(player, 0) == Catch::Approx(0.5F));
    CHECK(fbe_player_speed(player, 0) == 2.0F);
    fbe_player_free(player);
    fbe_clip_release(clip);
    fbe_world_free(world);
}

TEST_CASE("a crossfade mixes one clip into the next, and a stop fades back to where it was") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    fbe_entity_set_position(hips, 10.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    const std::uint64_t player = fbe_player_create(hips);
    const std::uint64_t still = sliding(0.0F, 0.0F, 1.0F);
    const std::uint64_t away = sliding(4.0F, 4.0F, 1.0F);
    fbe_player_play(player, 0, still, FBE_PLAYER_LOOP, 1.0F);
    CHECK(position(hips).x == 0.0F);
    REQUIRE(fbe_player_crossfade(player, 0, away, 1.0F, FBE_PLAYER_LOOP, 1.0F));
    CHECK(fbe_player_clip(player, 0) == away);
    fbe_player_advance(world, 0.25F);
    CHECK(position(hips).x == Catch::Approx(1.0F));
    fbe_player_advance(world, 0.25F);
    CHECK(position(hips).x == Catch::Approx(2.0F));
    fbe_player_advance(world, 0.75F);
    CHECK(position(hips).x == Catch::Approx(4.0F));
    // The one faded out is let go of.
    fbe_clip_release(still);
    CHECK(!fbe_clip_alive(still));
    // Stopped over a second, it fades back to where it was before the player moved it.
    REQUIRE(fbe_player_stop(player, 0, 1.0F));
    CHECK(fbe_player_clip(player, 0) == 0);
    CHECK(!fbe_player_playing(player, 0));
    fbe_player_advance(world, 0.5F);
    CHECK(position(hips).x == Catch::Approx(7.0F));
    fbe_player_advance(world, 0.5F);
    CHECK(position(hips).x == Catch::Approx(10.0F));
    fbe_clip_release(away);
    CHECK(!fbe_clip_alive(away));
    // A crossfade of no time is a play.
    const std::uint64_t again = sliding(1.0F, 1.0F, 1.0F);
    REQUIRE(fbe_player_crossfade(player, 0, again, 0.0F, FBE_PLAYER_ONCE, 1.0F));
    CHECK(position(hips).x == 1.0F);
    fbe_player_free(player);
    fbe_clip_release(again);
    fbe_world_free(world);
}

TEST_CASE("a layer is over the ones before it, by its weight, and kept to its mask") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    const std::uint64_t spine = pivot(world, hips, "spine");
    const std::uint64_t head = pivot(world, spine, "head");
    const std::uint64_t player = fbe_player_create(hips);
    const std::uint64_t walk = both_at(1.0F);
    const std::uint64_t wave = both_at(5.0F);
    fbe_player_play(player, 0, walk, FBE_PLAYER_LOOP, 1.0F);
    REQUIRE(fbe_player_set_mask(player, 1, &spine, 1));
    REQUIRE(fbe_player_play(player, 1, wave, FBE_PLAYER_LOOP, 1.0F));
    // The head is under the spine, so the wave moves it; the hips aren't.
    CHECK(position(hips).x == 1.0F);
    CHECK(position(head).x == 5.0F);
    REQUIRE(fbe_player_set_weight(player, 1, 0.5F));
    CHECK(fbe_player_weight(player, 1) == 0.5F);
    fbe_player_advance(world, 0.0F);
    CHECK(position(head).x == Catch::Approx(3.0F));
    // With no mask, it moves all it plays.
    fbe_player_set_mask(player, 1, nullptr, 0);
    fbe_player_set_weight(player, 1, 1.0F);
    fbe_player_advance(world, 0.0F);
    CHECK(position(hips).x == 5.0F);
    fbe_player_free(player);
    fbe_clip_release(walk);
    fbe_clip_release(wave);
    fbe_world_free(world);
}

TEST_CASE("rotations mix the short way round, however their keys are written") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    const std::uint64_t player = fbe_player_create(hips);
    // A quarter turn about Y, then the same, written as its negative.
    const float half = std::sqrt(0.5F);
    const std::uint64_t first = turned_to({ 0.0F, half, 0.0F, half });
    const std::uint64_t second = turned_to({ 0.0F, -half, 0.0F, -half });
    fbe_player_play(player, 0, first, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_crossfade(player, 0, second, 1.0F, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_advance(world, 0.5F);
    fbe_quat_t rotation{};
    fbe_entity_rotation(hips, FBE_SPACE_LOCAL, &rotation);
    CHECK(std::abs(rotation.y) == Catch::Approx(half));
    CHECK(std::abs(rotation.w) == Catch::Approx(half));
    fbe_player_free(player);
    fbe_clip_release(first);
    fbe_clip_release(second);
    fbe_world_free(world);
}

TEST_CASE("events are reached in order, at the start, round loops, and there and back") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    const std::uint64_t player = fbe_player_create(hips);
    const std::uint64_t clip = sliding(0.0F, 1.0F, 1.0F);
    fbe_clip_add_event(clip, "start", 0.0F);
    fbe_clip_add_event(clip, "step", 0.5F);
    fbe_clip_add_event(clip, "end", 1.0F);
    fbe_player_play(player, 0, clip, FBE_PLAYER_LOOP, 1.0F);
    CHECK(fbe_player_event_count(player) == 0);
    fbe_player_advance(world, 0.6F);
    std::array<uint64_t, 4> evented{};
    REQUIRE(fbe_player_evented(evented.data(), evented.size()) == 1);
    CHECK(evented[0] == player);
    CHECK(fbe_player_event_layer(player, 0) == 0);
    CHECK(std::string_view(fbe_player_event_clip(player, 0)) == "slide");
    CHECK(reached(player) == std::vector<std::string>{ "start@0", "step@0.5" });
    CHECK(fbe_player_evented(evented.data(), evented.size()) == 0);
    // Round the loop: its end, then its start again.
    fbe_player_advance(world, 0.6F);
    CHECK(reached(player) == std::vector<std::string>{ "end@1", "start@0" });
    // There and back: on to the end, then back past the middle.
    fbe_player_play(player, 0, clip, FBE_PLAYER_PING_PONG, 1.0F);
    fbe_player_set_time(player, 0, 0.25F);
    fbe_player_advance(world, 1.5F);
    CHECK(reached(player) == std::vector<std::string>{ "step@0.5", "end@1", "step@0.5" });
    // Once, its end is reached once.
    fbe_player_stop(player, 0, 0.0F);
    fbe_player_play(player, 0, clip, FBE_PLAYER_ONCE, 1.0F);
    fbe_player_advance(world, 3.0F);
    CHECK(reached(player) == std::vector<std::string>{ "start@0", "step@0.5", "end@1" });
    fbe_player_advance(world, 3.0F);
    CHECK(fbe_player_event_count(player) == 0);
    // A step round a thousand loops reaches them the first time round and the last, not each.
    fbe_player_stop(player, 0, 0.0F);
    const std::uint64_t quick = sliding(0.0F, 1.0F, 0.01F);
    fbe_clip_add_event(quick, "tick", 0.005F);
    fbe_player_play(player, 0, quick, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_advance(world, 10.0F);
    CHECK(fbe_player_event_count(player) <= 3);
    CHECK(fbe_player_event_count(player) >= 1);
    CHECK(std::string_view(fbe_player_event_name(player, 9)).empty());
    CHECK(says("there's no event 9"));
    fbe_player_free(player);
    CHECK(fbe_player_evented(evented.data(), evented.size()) == 0);
    fbe_clip_release(clip);
    fbe_clip_release(quick);
    fbe_world_free(world);
}

TEST_CASE("what a layer can't take is refused, saying why") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t hips = pivot(world, 0, "hips");
    const std::uint64_t player = fbe_player_create(hips);
    const std::uint64_t clip = sliding(0.0F, 1.0F, 1.0F);
    CHECK(!fbe_player_play(player, 16, clip, FBE_PLAYER_LOOP, 1.0F));
    CHECK(says("layers are 0 to 15, not 16"));
    CHECK(!fbe_player_play(player, -1, clip, FBE_PLAYER_LOOP, 1.0F));
    CHECK(!fbe_player_play(player, 0, clip, 3, 1.0F));
    CHECK(says("3 isn't a mode"));
    CHECK(!fbe_player_play(player, 0, clip, FBE_PLAYER_LOOP, std::nanf("")));
    CHECK(says("speed that's a number"));
    CHECK(!fbe_player_crossfade(player, 0, clip, -1.0F, FBE_PLAYER_LOOP, 1.0F));
    CHECK(says("0 seconds or more, not -1"));
    CHECK(!fbe_player_stop(player, 0, std::numeric_limits<float>::infinity()));
    CHECK(!fbe_player_set_weight(player, 0, 1.5F));
    CHECK(says("from 0 to 1, not 1.5"));
    CHECK(!fbe_player_set_weight(player, 0, std::nanf("")));
    CHECK(fbe_player_weight(player, 0) == 1.0F);
    CHECK(!fbe_player_set_speed(player, 0, std::nanf("")));
    const std::uint64_t gone = pivot(world, 0, "gone");
    fbe_entity_free(gone);
    CHECK(!fbe_player_set_mask(player, 0, &gone, 1));
    CHECK(says("was freed"));
    fbe_player_free(player);
    fbe_clip_release(clip);
    fbe_world_free(world);
}
