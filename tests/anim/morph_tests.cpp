// Morph targets through the C API: what's refused, weights moved by clips of each interpolation,
// what a pivot makes of them, and a model's clip of them.

#include "fbengine.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
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
