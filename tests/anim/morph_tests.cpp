// Morph targets through the C API: what's refused, weights moved by clips of each interpolation,
// what a pivot makes of them, and a model's clip of them.

#include "fbengine.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>

namespace {

bool says(std::string_view message) {
    return std::string_view(fbe_error()).contains(message);
}

/// A geometry of a surface of two vertices, with a target moving the first by (1, 2, 3).
std::uint64_t pair_of_vertices() {
    const std::uint64_t geometry = fbe_geometry_create();
    const std::int32_t surface = fbe_geometry_add_surface(geometry);
    const std::array<fbe_vertex_t, 2> vertices{};
    fbe_surface_add_vertices(geometry, surface, vertices.data(), vertices.size());
    const std::array<float, 6> moved{ 1, 2, 3, 0, 0, 0 };
    fbe_morph_add_target(geometry, surface, moved.data(), nullptr, 2);
    return geometry;
}

} // namespace

TEST_CASE("a target has a vector for each of its surface's vertices, of numbers") {
    const std::uint64_t geometry = pair_of_vertices();
    const std::array<float, 6> moved{ 0, 0, 0, 0, 0, 0 };
    CHECK(fbe_morph_target_count(geometry, 0) == 1);
    CHECK(fbe_morph_add_target(geometry, 0, moved.data(), nullptr, 1) == -1);
    CHECK(says("each of the surface's 2 vertices, not 1"));
    CHECK(fbe_morph_add_target(geometry, 1, moved.data(), nullptr, 2) == -1);
    CHECK(says("there's no surface 1"));
    CHECK(fbe_morph_target_count(geometry, 1) == -1);
    std::array<float, 6> broken = moved;
    broken[4] = std::numeric_limits<float>::quiet_NaN();
    CHECK(fbe_morph_add_target(geometry, 0, broken.data(), nullptr, 2) == -1);
    CHECK(says("vectors are numbers"));
    CHECK(fbe_morph_add_target(geometry, 0, moved.data(), broken.data(), 2) == -1);
    CHECK(fbe_morph_add_target(geometry, 0, moved.data(), moved.data(), 2) == 1);
    fbe_geometry_release(geometry);
    CHECK(fbe_morph_target_count(geometry, 0) == -1);
    CHECK(fbe_morph_add_target(geometry, 0, moved.data(), nullptr, 2) == -1);
}

TEST_CASE("a mesh's weights are its own, numbers, and move what it draws") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t geometry = pair_of_vertices();
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t pivot = fbe_entity_create(world, 0, FBE_ENTITY_PIVOT);
    const std::array<float, 2> weights{ 2.0F, 7.0F };
    std::array<float, 2> out{};
    CHECK(fbe_morph_weights(mesh, 1.0F, out.data(), out.size()) == 0);
    REQUIRE(fbe_morph_set_weights(mesh, weights.data(), 2));
    REQUIRE(fbe_morph_weights(mesh, 1.0F, out.data(), out.size()) == 2);
    CHECK(out[1] == 7.0F);
    // Room for fewer than there are: as many as fit, and how many there are.
    CHECK(fbe_morph_weights(mesh, 1.0F, out.data(), 1) == 2);
    CHECK(fbe_morph_weights(mesh, 1.5F, out.data(), out.size()) == -1);
    CHECK(says("a tween is from 0 to 1, not 1.5"));
    CHECK(fbe_morph_weights(mesh, std::nanf(""), out.data(), out.size()) == -1);
    const std::array<float, 1> broken{ std::numeric_limits<float>::infinity() };
    CHECK(!fbe_morph_set_weights(mesh, broken.data(), 1));
    CHECK(says("weights are numbers"));
    CHECK(!fbe_morph_set_weights(pivot, weights.data(), 2));
    CHECK(says("the entity isn't a mesh"));
    CHECK(fbe_morph_weights(pivot, 1.0F, out.data(), 1) == -1);
    std::array<float, 6> positions{};
    CHECK(fbe_morph_vertices(mesh, 0, positions.data(), nullptr, 2) == -1);
    CHECK(says("the mesh draws nothing"));
    REQUIRE(fbe_mesh_set_geometry(mesh, geometry));
    CHECK(fbe_morph_vertices(mesh, 3, positions.data(), nullptr, 2) == -1);
    CHECK(fbe_morph_vertices(mesh, 0, positions.data(), nullptr, 1) == -1);
    CHECK(says("the surface has 2 vertices"));
    // Weights past its targets are nothing to it.
    REQUIRE(fbe_morph_vertices(mesh, 0, positions.data(), nullptr, 2) == 2);
    CHECK(positions[1] == 4.0F);
    CHECK(positions[4] == 0.0F);
    fbe_geometry_release(geometry);
    fbe_world_free(world);
}

TEST_CASE("clips mix weights of every interpolation, and any but a mesh's are left out") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    fbe_entity_set_name(mesh, "face");
    const std::uint64_t pivot = fbe_entity_create(world, mesh, FBE_ENTITY_PIVOT);
    fbe_entity_set_name(pivot, "bone");
    const std::array<float, 2> times{ 0.0F, 2.0F };
    // Three targets wide, stepping, then in a line.
    const std::uint64_t step = fbe_clip_create("", 0.0F);
    const std::array<float, 6> stepped{ 1, 2, 3, 5, 6, 7 };
    REQUIRE(fbe_clip_add_channel(step, "face", FBE_CLIP_WEIGHTS, FBE_CLIP_STEP, 3, times.data(),
                                 stepped.data(), 2));
    REQUIRE(fbe_clip_add_channel(step, "bone", FBE_CLIP_WEIGHTS, FBE_CLIP_STEP, 3, times.data(),
                                 stepped.data(), 2));
    const std::uint64_t player = fbe_player_create(mesh);
    REQUIRE(fbe_player_play(player, 0, step, FBE_PLAYER_ONCE, 1.0F));
    std::array<float, 8> out{};
    CHECK(fbe_morph_weights(mesh, 1.0F, out.data(), out.size()) == 3);
    CHECK(out[2] == 3.0F);
    // A crossfade to a wider clip: 5 targets, cubic with no tangents, its third at 9 at its end.
    const std::uint64_t curve = fbe_clip_create("", 0.0F);
    std::array<float, 30> curved{};
    curved[15 + 5 + 2] = 9.0F;
    REQUIRE(fbe_clip_add_channel(curve, "face", FBE_CLIP_WEIGHTS, FBE_CLIP_CUBIC, 5, times.data(),
                                 curved.data(), 2));
    REQUIRE(fbe_player_crossfade(player, 0, curve, 1.0F, FBE_PLAYER_ONCE, 1.0F));
    fbe_player_advance(world, 1.0F);
    // Halfway along the curve, and all the way faded: (3s^2 - 2s^3) of 9, at a half.
    REQUIRE(fbe_morph_weights(mesh, 1.0F, out.data(), out.size()) == 5);
    CHECK(out[2] == Catch::Approx(4.5F));
    CHECK(out[4] == 0.0F);
    fbe_player_free(player);
    fbe_clip_release(step);
    fbe_clip_release(curve);
    fbe_world_free(world);
}

TEST_CASE("a mesh freed takes its weights, and a copy starts with them") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t first = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t second = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::array<float, 1> one{ 1.0F };
    const std::array<float, 1> two{ 2.0F };
    fbe_morph_set_weights(first, one.data(), 1);
    fbe_morph_set_weights(second, two.data(), 1);
    const std::uint64_t copy = fbe_entity_copy(first, 0, 0);
    // The last mesh moves into the first's place: its weights go with it.
    fbe_entity_free(first);
    std::array<float, 1> out{};
    CHECK(fbe_morph_weights(second, 1.0F, out.data(), 1) == 1);
    CHECK(out[0] == 2.0F);
    CHECK(fbe_morph_weights(copy, 1.0F, out.data(), 1) == 1);
    CHECK(out[0] == 1.0F);
    CHECK(fbe_morph_weights(first, 1.0F, out.data(), 1) == -1);
    fbe_world_free(world);
}

namespace {

/// A clip of the first `count` targets of `node` held at `values`.
std::uint64_t held_weights(const char* node, std::span<const float> values) {
    const std::uint64_t clip = fbe_clip_create("", 0.0F);
    const std::array<float, 1> times{ 0.0F };
    fbe_clip_add_channel(clip, node, FBE_CLIP_WEIGHTS, FBE_CLIP_STEP,
                         static_cast<std::int32_t>(values.size()), times.data(), values.data(), 1);
    return clip;
}

/// A mesh named "face" with `rest` weights.
std::uint64_t face(std::uint64_t world, std::span<const float> rest) {
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    fbe_entity_set_name(mesh, "face");
    fbe_morph_set_weights(mesh, rest.data(), static_cast<std::int32_t>(rest.size()));
    return mesh;
}

float weight_of(std::uint64_t mesh, std::size_t target, float tween = 1.0F) {
    std::array<float, 8> out{};
    fbe_morph_weights(mesh, tween, out.data(), out.size());
    return out.at(target);
}

} // namespace

TEST_CASE("a crossfade between clips of other widths goes target by target, with no step") {
    const std::uint64_t world = fbe_world_create();
    const std::array<float, 2> rest{ 0.0F, 2.0F };
    const std::uint64_t mesh = face(world, rest);
    const std::array<float, 2> two{ 0.5F, 1.0F };
    const std::array<float, 1> one{ 0.25F };
    const std::uint64_t wide = held_weights("face", two);
    const std::uint64_t narrow = held_weights("face", one);
    const std::uint64_t player = fbe_player_create(mesh);
    fbe_player_play(player, 0, wide, FBE_PLAYER_LOOP, 1.0F);
    CHECK(weight_of(mesh, 1) == 1.0F);
    fbe_player_crossfade(player, 0, narrow, 1.0F, FBE_PLAYER_LOOP, 1.0F);
    // The second target goes from the wide clip's back to where it began, as the wide clip fades.
    for (int i = 1; i <= 10; ++i) {
        fbe_player_advance(world, 0.1F);
        CHECK(weight_of(mesh, 1) == Catch::Approx(1.0F + (0.1F * static_cast<float>(i))));
    }
    CHECK(weight_of(mesh, 0) == Catch::Approx(0.25F));
    // And dropped, it's where it was going.
    fbe_player_advance(world, 0.1F);
    CHECK(weight_of(mesh, 1) == Catch::Approx(2.0F));
    fbe_player_free(player);
    fbe_clip_release(wide);
    fbe_clip_release(narrow);
    fbe_world_free(world);
}

TEST_CASE("a crossfade's clips mean by their weights, over where a target began") {
    const std::uint64_t world = fbe_world_create();
    const std::array<float, 1> rest{ 1.0F };
    const std::uint64_t mesh = face(world, rest);
    const std::array<float, 1> low{ 1.0F };
    const std::array<float, 1> high{ 3.0F };
    const std::uint64_t first = held_weights("face", low);
    const std::uint64_t second = held_weights("face", high);
    const std::uint64_t player = fbe_player_create(mesh);
    fbe_player_play(player, 0, first, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_crossfade(player, 0, second, 1.0F, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_advance(world, 0.25F);
    CHECK(weight_of(mesh, 0) == Catch::Approx(1.5F));
    fbe_player_advance(world, 0.25F);
    CHECK(weight_of(mesh, 0) == Catch::Approx(2.0F));
    fbe_player_free(player);
    fbe_clip_release(first);
    fbe_clip_release(second);
    fbe_world_free(world);
}

TEST_CASE("a target no channel reaches is the game's, until a clip moves it from there") {
    const std::uint64_t world = fbe_world_create();
    const std::array<float, 2> rest{ 0.0F, 0.0F };
    const std::uint64_t mesh = face(world, rest);
    const std::array<float, 1> one{ 1.0F };
    const std::uint64_t narrow = held_weights("face", one);
    const std::uint64_t player = fbe_player_create(mesh);
    fbe_player_play(player, 0, narrow, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_advance(world, 0.1F);
    const std::array<float, 2> blink{ 1.0F, 0.7F };
    fbe_morph_set_weights(mesh, blink.data(), 2);
    fbe_player_advance(world, 0.1F);
    CHECK(weight_of(mesh, 1) == 0.7F);
    // Fading out takes back only what the clip moved.
    fbe_player_stop(player, 0, 1.0F);
    fbe_player_advance(world, 0.5F);
    CHECK(weight_of(mesh, 0) == Catch::Approx(0.5F));
    CHECK(weight_of(mesh, 1) == 0.7F);
    fbe_player_advance(world, 0.6F);
    CHECK(weight_of(mesh, 0) == Catch::Approx(0.0F));
    CHECK(weight_of(mesh, 1) == 0.7F);
    // A wider clip starts that target from where the game left it, and fades back to it.
    const std::array<float, 2> two{ 0.0F, 0.1F };
    const std::uint64_t wide = held_weights("face", two);
    fbe_player_play(player, 0, wide, FBE_PLAYER_LOOP, 1.0F);
    CHECK(weight_of(mesh, 1) == 0.1F);
    fbe_player_stop(player, 0, 1.0F);
    fbe_player_advance(world, 0.5F);
    CHECK(weight_of(mesh, 1) == Catch::Approx(0.4F));
    fbe_player_advance(world, 0.6F);
    CHECK(weight_of(mesh, 1) == Catch::Approx(0.7F));
    // A clip that no longer moves it, whether stopped or played over, leaves it the game's again.
    fbe_player_play(player, 0, wide, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_play(player, 0, narrow, FBE_PLAYER_LOOP, 1.0F);
    const std::array<float, 2> after_play{ 1.0F, 0.6F };
    fbe_morph_set_weights(mesh, after_play.data(), 2);
    fbe_player_play(player, 0, wide, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_stop(player, 0, 1.0F);
    fbe_player_advance(world, 1.1F);
    CHECK(weight_of(mesh, 1) == Catch::Approx(0.6F));
    fbe_player_play(player, 0, wide, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_stop(player, 0, 0.0F);
    const std::array<float, 2> after_stop{ 1.0F, 0.5F };
    fbe_morph_set_weights(mesh, after_stop.data(), 2);
    fbe_player_play(player, 0, wide, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_stop(player, 0, 1.0F);
    fbe_player_advance(world, 1.1F);
    CHECK(weight_of(mesh, 1) == Catch::Approx(0.5F));
    fbe_player_free(player);
    fbe_clip_release(narrow);
    fbe_clip_release(wide);
    fbe_world_free(world);
}

TEST_CASE("a clip fading out takes a target back to where it began, whatever that was") {
    const std::uint64_t world = fbe_world_create();
    const std::array<float, 1> rest{ 0.5F };
    const std::uint64_t mesh = face(world, rest);
    const std::array<float, 1> one{ 1.5F };
    const std::uint64_t clip = held_weights("face", one);
    const std::uint64_t player = fbe_player_create(mesh);
    fbe_player_play(player, 0, clip, FBE_PLAYER_LOOP, 1.0F);
    CHECK(weight_of(mesh, 0) == 1.5F);
    fbe_player_stop(player, 0, 1.0F);
    fbe_player_advance(world, 0.25F);
    CHECK(weight_of(mesh, 0) == Catch::Approx(1.25F));
    fbe_player_advance(world, 1.0F);
    CHECK(weight_of(mesh, 0) == Catch::Approx(0.5F));
    fbe_player_free(player);
    fbe_clip_release(clip);
    fbe_world_free(world);
}

TEST_CASE("a layer's weight lays a target over the layers before it") {
    const std::uint64_t world = fbe_world_create();
    const std::array<float, 2> rest{ 0.0F, 0.0F };
    const std::uint64_t mesh = face(world, rest);
    const std::array<float, 2> base_at{ 1.0F, 1.0F };
    const std::array<float, 1> over_at{ 3.0F };
    const std::uint64_t base = held_weights("face", base_at);
    const std::uint64_t over = held_weights("face", over_at);
    const std::uint64_t player = fbe_player_create(mesh);
    fbe_player_play(player, 0, base, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_set_weight(player, 1, 0.5F);
    fbe_player_play(player, 1, over, FBE_PLAYER_LOOP, 1.0F);
    fbe_player_advance(world, 0.1F);
    CHECK(weight_of(mesh, 0) == Catch::Approx(2.0F));
    CHECK(weight_of(mesh, 1) == Catch::Approx(1.0F));
    fbe_world_capture(world);
    fbe_player_stop(player, 1, 0.0F);
    fbe_player_advance(world, 0.1F);
    CHECK(weight_of(mesh, 0) == Catch::Approx(1.0F));
    CHECK(weight_of(mesh, 0, 0.5F) == Catch::Approx(1.5F));
    fbe_player_free(player);
    fbe_clip_release(base);
    fbe_clip_release(over);
    fbe_world_free(world);
}

namespace {

/// A surface of a vertex whose normal is (0.6, 0, 0.8), with a target turning it by (0, 1, 0).
std::uint64_t turning_vertex() {
    const std::uint64_t geometry = fbe_geometry_create();
    const std::int32_t surface = fbe_geometry_add_surface(geometry);
    fbe_vertex_t vertex{};
    vertex.normal = { .x = 0.6F, .y = 0.0F, .z = 0.8F };
    fbe_surface_add_vertices(geometry, surface, &vertex, 1);
    const std::array<float, 3> turned{ 0.0F, 1.0F, 0.0F };
    fbe_morph_add_target(geometry, surface, nullptr, turned.data(), 1);
    return geometry;
}

fbe_vec3_t morphed_normal(std::uint64_t mesh) {
    std::array<float, 3> normal{};
    REQUIRE(fbe_morph_vertices(mesh, 0, nullptr, normal.data(), 1) == 1);
    return { .x = normal[0], .y = normal[1], .z = normal[2] };
}

void check_near(fbe_vec3_t found, fbe_vec3_t wanted) {
    CHECK(found.x == Catch::Approx(wanted.x).margin(1e-5));
    CHECK(found.y == Catch::Approx(wanted.y).margin(1e-5));
    CHECK(found.z == Catch::Approx(wanted.z).margin(1e-5));
}

/// (x, y, z) made length 1.
fbe_vec3_t unit(float x, float y, float z) {
    const float size = std::sqrt((x * x) + (y * y) + (z * z));
    return { .x = x / size, .y = y / size, .z = z / size };
}

} // namespace

TEST_CASE("a geometry moved takes its target normals with it, at any weight") {
    // A matrix by its columns' first three each and a translation, and what it makes of the normal
    // (0.6, y, 0.8) at each weight y, by its normal matrix: its inverse transpose.
    struct moved_t {
        std::array<float, 16> matrix;
        std::function<fbe_vec3_t(float)> wanted;
    };
    const std::array<moved_t, 5> moves{
        moved_t{
            .matrix = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1 },
            .wanted = [](float y) { return unit(0.6F, y, 0.8F); },
        },
        // A quarter turn about Y: X goes to -Z, and Z to X.
        moved_t{
            .matrix = { 0, 0, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1 },
            .wanted = [](float y) { return unit(0.8F, y, -0.6F); },
        },
        moved_t{
            .matrix = { 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 },
            .wanted = [](float y) { return unit(0.3F, y, 0.8F); },
        },
        moved_t{
            .matrix = { 3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 1 },
            .wanted = [](float y) { return unit(0.6F, y, 0.8F); },
        },
        // A mirror in X.
        moved_t{
            .matrix = { -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 },
            .wanted = [](float y) { return unit(-0.6F, y, 0.8F); },
        },
    };
    const std::uint64_t world = fbe_world_create();
    for (const moved_t& move : moves) {
        const std::uint64_t geometry = turning_vertex();
        const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
        fbe_mesh_set_geometry(mesh, geometry);
        fbe_mat4_t matrix{};
        std::ranges::copy(move.matrix, std::begin(matrix.m));
        REQUIRE(fbe_geometry_transform(geometry, &matrix));
        for (const float weight : { 0.0F, 0.5F, 1.0F, 2.0F }) {
            const std::array<float, 1> weights{ weight };
            fbe_morph_set_weights(mesh, weights.data(), 1);
            check_near(morphed_normal(mesh), move.wanted(weight));
        }
        fbe_geometry_release(geometry);
    }
    fbe_world_free(world);
}

TEST_CASE("a target can turn normals and move nothing, or cancel them") {
    const std::uint64_t world = fbe_world_create();
    const std::uint64_t geometry = turning_vertex();
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    fbe_mesh_set_geometry(mesh, geometry);
    const std::array<float, 1> half{ 0.75F };
    fbe_morph_set_weights(mesh, half.data(), 1);
    // Back to length 1, and it's moved nothing.
    check_near(morphed_normal(mesh), unit(0.6F, 0.75F, 0.8F));
    std::array<float, 3> place{ 9.0F, 9.0F, 9.0F };
    REQUIRE(fbe_morph_vertices(mesh, 0, place.data(), nullptr, 1) == 1);
    CHECK(place == std::array<float, 3>{ 0.0F, 0.0F, 0.0F });
    // A target that does neither is one that moves nothing.
    CHECK(fbe_morph_add_target(geometry, 0, nullptr, nullptr, 1) == 1);
    CHECK(fbe_morph_add_target(geometry, 0, nullptr, nullptr, 2) == -1);
    // One that cancels the normal leaves the vertex's own.
    const std::array<float, 3> cancels{ -0.6F, 0.0F, -0.8F };
    CHECK(fbe_morph_add_target(geometry, 0, nullptr, cancels.data(), 1) == 2);
    const std::array<float, 3> weights{ 0.0F, 0.0F, 1.0F };
    fbe_morph_set_weights(mesh, weights.data(), 3);
    check_near(morphed_normal(mesh), { .x = 0.6F, .y = 0.0F, .z = 0.8F });
    fbe_geometry_release(geometry);
    fbe_world_free(world);
}

TEST_CASE("values as large as a float holds mix without overflowing") {
    const float most = std::numeric_limits<float>::max();
    const std::uint64_t world = fbe_world_create();
    // A channel's keys.
    const std::uint64_t clip = fbe_clip_create("", 0.0F);
    const std::array<float, 2> times{ 0.0F, 1.0F };
    const std::array<float, 2> keys{ -most, most };
    REQUIRE(fbe_clip_add_channel(clip, "face", FBE_CLIP_WEIGHTS, FBE_CLIP_LINEAR, 1, times.data(),
                                 keys.data(), 2));
    std::array<float, 1> sampled{};
    REQUIRE(fbe_clip_sample(clip, 0, 0.5F, sampled.data(), 1) == 1);
    CHECK(sampled[0] == 0.0F);
    // A mesh's weights, tweened from the capture.
    const std::array<float, 1> first{ -most };
    const std::uint64_t mesh = face(world, first);
    fbe_world_capture(world);
    const std::array<float, 1> second{ most };
    fbe_morph_set_weights(mesh, second.data(), 1);
    CHECK(weight_of(mesh, 0, 0.5F) == 0.0F);
    // A layer at a half over where they began, of a weight and a position.
    fbe_morph_set_weights(mesh, first.data(), 1);
    const std::array<float, 1> held{ most };
    const std::uint64_t over = held_weights("face", held);
    const std::array<float, 3> place{ most, 0.0F, 0.0F };
    const std::array<float, 1> start{ 0.0F };
    REQUIRE(fbe_clip_add_channel(over, "face", FBE_CLIP_TRANSLATION, FBE_CLIP_STEP, 3, start.data(),
                                 place.data(), 1));
    fbe_entity_set_position(mesh, -most, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    const std::uint64_t player = fbe_player_create(mesh);
    fbe_player_set_weight(player, 0, 0.5F);
    REQUIRE(fbe_player_play(player, 0, over, FBE_PLAYER_LOOP, 1.0F));
    CHECK(weight_of(mesh, 0) == 0.0F);
    fbe_vec3_t at{};
    fbe_entity_position(mesh, FBE_SPACE_LOCAL, &at);
    CHECK(at.x == 0.0F);
    fbe_player_free(player);
    fbe_clip_release(clip);
    fbe_clip_release(over);
    fbe_world_free(world);
}
