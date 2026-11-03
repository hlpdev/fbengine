// Particle emitters' counts and limits and decals' triangles, through the C API, with no GPU.

#include "fbengine.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

namespace {

using Catch::Approx;

/// A world of the test's own, freed at its end.
struct test_world_t {
    std::uint64_t handle = fbe_world_create();

    test_world_t() = default;
    test_world_t(const test_world_t&) = delete;
    test_world_t& operator=(const test_world_t&) = delete;
    test_world_t(test_world_t&&) = delete;
    test_world_t& operator=(test_world_t&&) = delete;

    ~test_world_t() {
        fbe_world_free(handle);
        std::array<std::uint64_t, 16> some{};
        while (fbe_entity_freed(some.data(), some.size()) != 0) {}
    }
};

std::string last_error() {
    return fbe_error();
}

/// A box of half-sizes at a place, its axes the world's.
fbe_mat4_t box_at(float x, float y, float z, float half_x, float half_y, float half_z) {
    fbe_mat4_t box{};
    box.m[0] = half_x;
    box.m[5] = half_y;
    box.m[10] = half_z;
    box.m[12] = x;
    box.m[13] = y;
    box.m[14] = z;
    box.m[15] = 1.0F;
    return box;
}

/// A mesh of a cube, -1 to 1, in a world.
std::uint64_t cube_mesh(std::uint64_t world) {
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t geometry = fbe_geometry_cube();
    fbe_mesh_set_geometry(mesh, geometry);
    fbe_geometry_release(geometry);
    return mesh;
}

std::vector<fbe_vertex_t> vertices_of(std::uint64_t geometry) {
    const std::int32_t count = fbe_surface_vertex_count(geometry, 0);
    std::vector<fbe_vertex_t> vertices(static_cast<std::size_t>(count));
    fbe_surface_vertices(geometry, 0, 0, vertices.data(), count);
    return vertices;
}

} // namespace

TEST_CASE("a new emitter has its defaults, and holds none alive", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(emitter != 0);
    CHECK(fbe_entity_kind(emitter) == FBE_ENTITY_PARTICLES);
    CHECK(fbe_particles_capacity(emitter) == 1024);
    CHECK(fbe_particles_rate(emitter) == 32.0F);
    CHECK(fbe_particles_alive(emitter) == 0);
    CHECK(fbe_particles_material(emitter) == 0);
}

TEST_CASE("an emitter spawns at its rate as steps go by, and what's lived its life dies",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_particles_set_rate(emitter, 10.0F));
    REQUIRE(fbe_particles_set_life(emitter, 1.0F, 1.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 0.5F));
    CHECK(fbe_particles_alive(emitter) == 5);
    // A rate's fraction carries over: a tenth of a step at a time adds up.
    for (int i = 0; i < 2; ++i) {
        REQUIRE(fbe_world_step_particles(world.handle, 0.05F));
    }
    CHECK(fbe_particles_alive(emitter) == 6);
    // The first's born a twentieth of a second in, so a whole life on from then is only them gone.
    REQUIRE(fbe_world_step_particles(world.handle, 0.5F));
    CHECK(fbe_particles_alive(emitter) == 10);
    REQUIRE(fbe_particles_set_rate(emitter, 0.0F));
    REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    CHECK(fbe_particles_alive(emitter) == 0);
}

TEST_CASE("a burst spawns at once, up to what an emitter holds", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_particles_set_capacity(emitter, 4));
    REQUIRE(fbe_particles_set_rate(emitter, 0.0F));
    REQUIRE(fbe_particles_burst(emitter, 3));
    CHECK(fbe_particles_alive(emitter) == 3);
    REQUIRE(fbe_particles_burst(emitter, 100));
    CHECK(fbe_particles_alive(emitter) == 4);
    // Setting what it holds loses them.
    REQUIRE(fbe_particles_set_capacity(emitter, 8));
    CHECK(fbe_particles_alive(emitter) == 0);
}

TEST_CASE("a step longer than a life leaves only what could have lived through it",
          "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_particles_set_rate(emitter, 100.0F));
    REQUIRE(fbe_particles_set_life(emitter, 0.5F, 0.5F));
    REQUIRE(fbe_world_step_particles(world.handle, 10.0F));
    CHECK(fbe_particles_alive(emitter) == 50);
}

TEST_CASE("a hidden emitter spawns none, and no others in another World step",
          "[scene][particles]") {
    const test_world_t world;
    const test_world_t other;
    const std::uint64_t hidden = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    const std::uint64_t elsewhere = fbe_entity_create(other.handle, 0, FBE_ENTITY_PARTICLES);
    REQUIRE(fbe_entity_set_hidden(hidden, true));
    REQUIRE(fbe_world_step_particles(world.handle, 1.0F));
    CHECK(fbe_particles_alive(hidden) == 0);
    CHECK(fbe_particles_alive(elsewhere) == 0);
}

TEST_CASE("a copy of an emitter has its settings and none of its particles", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    const std::uint64_t material = fbe_material_create();
    REQUIRE(fbe_particles_set_material(emitter, material));
    REQUIRE(fbe_particles_set_capacity(emitter, 16));
    REQUIRE(fbe_particles_set_rate(emitter, 3.0F));
    REQUIRE(fbe_particles_burst(emitter, 5));
    const std::uint64_t copy = fbe_entity_copy(emitter, world.handle, 0);
    REQUIRE(copy != 0);
    CHECK(fbe_particles_capacity(copy) == 16);
    CHECK(fbe_particles_rate(copy) == 3.0F);
    CHECK(fbe_particles_material(copy) == material);
    CHECK(fbe_particles_alive(copy) == 0);
    CHECK(fbe_particles_alive(emitter) == 5);
    // Each holds the material: freeing one leaves the other's.
    REQUIRE(fbe_entity_free(emitter));
    CHECK(fbe_particles_material(copy) == material);
    fbe_material_release(material);
    CHECK(fbe_particles_material(copy) == material);
}

TEST_CASE("the same seed spawns the same, and another doesn't", "[scene][particles]") {
    // What it spawned shows in a box, whichever it reaches: how far it's seen to go.
    const auto run = [](std::uint32_t seed) {
        const test_world_t world;
        const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
        REQUIRE(fbe_particles_set_seed(emitter, seed));
        REQUIRE(fbe_particles_set_rate(emitter, 0.0F));
        REQUIRE(fbe_particles_set_life(emitter, 0.25F, 1.0F));
        std::vector<std::int32_t> alive;
        REQUIRE(fbe_particles_burst(emitter, 64));
        for (int i = 0; i < 8; ++i) {
            REQUIRE(fbe_world_step_particles(world.handle, 0.125F));
            alive.push_back(fbe_particles_alive(emitter));
        }
        return alive;
    };
    CHECK(run(7) == run(7));
    CHECK(run(7) != run(8));
}

TEST_CASE("an emitter's limits are checked", "[scene][particles]") {
    const test_world_t world;
    const std::uint64_t emitter = fbe_entity_create(world.handle, 0, FBE_ENTITY_PARTICLES);
    const float nan = std::nanf("");
    const float pi = std::numbers::pi_v<float>;
    CHECK_FALSE(fbe_particles_set_capacity(emitter, 0));
    CHECK(last_error().contains("not 0"));
    CHECK_FALSE(fbe_particles_set_capacity(emitter, 1048577));
    CHECK(fbe_particles_set_capacity(emitter, 1048576));
    CHECK(fbe_particles_set_capacity(emitter, 1024));
    CHECK_FALSE(fbe_particles_set_rate(emitter, -1.0F));
    CHECK_FALSE(fbe_particles_set_rate(emitter, nan));
    CHECK_FALSE(fbe_particles_set_life(emitter, 0.0F, 1.0F));
    CHECK_FALSE(fbe_particles_set_life(emitter, 2.0F, 1.0F));
    CHECK_FALSE(fbe_particles_set_life(emitter, 1.0F, 513.0F));
    CHECK(fbe_particles_set_life(emitter, 1.0F, 512.0F));
    CHECK_FALSE(fbe_particles_set_motion(emitter, -1.0F, 1.0F, 0.0F, 0.0F));
    CHECK_FALSE(fbe_particles_set_motion(emitter, 2.0F, 1.0F, 0.0F, 0.0F));
    CHECK_FALSE(fbe_particles_set_motion(emitter, 1.0F, 1.0F, pi + 1.0F, 0.0F));
    CHECK_FALSE(fbe_particles_set_motion(emitter, 1.0F, 1.0F, 0.0F, -1.0F));
    CHECK(fbe_particles_set_motion(emitter, 1.0F, 1.0F, pi, 1.0F));
    CHECK_FALSE(fbe_particles_set_forces(emitter, nan, 0.0F, 0.0F, 0.0F));
    CHECK_FALSE(fbe_particles_set_forces(emitter, 0.0F, 0.0F, 0.0F, -1.0F));
    CHECK_FALSE(fbe_particles_set_sizes(emitter, -1.0F, 1.0F));
    CHECK_FALSE(fbe_particles_burst(emitter, -1));
    CHECK_FALSE(fbe_world_step_particles(world.handle, -1.0F));
    CHECK_FALSE(fbe_world_step_particles(world.handle, nan));
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    CHECK_FALSE(fbe_particles_set_rate(mesh, 1.0F));
    CHECK(last_error() == "the entity isn't a particle emitter");
}

TEST_CASE("a sky's sun is a directional light of its World, and a brightness 0 or more",
          "[scene][sky]") {
    const test_world_t world;
    const test_world_t other;
    const std::uint64_t sun = fbe_entity_create(world.handle, 0, FBE_ENTITY_LIGHT);
    REQUIRE(fbe_light_set_type(sun, FBE_LIGHT_DIRECTIONAL));
    CHECK(fbe_world_set_sky_atmosphere(world.handle, sun, 20.0F));
    CHECK(fbe_world_set_sky_atmosphere(world.handle, 0, 20.0F));
    CHECK_FALSE(fbe_world_set_sky_atmosphere(world.handle, 0, -1.0F));
    CHECK_FALSE(fbe_world_set_sky_atmosphere(other.handle, sun, 1.0F));
    CHECK(last_error() == "a sky's sun is in its World");
    REQUIRE(fbe_light_set_type(sun, FBE_LIGHT_POINT));
    CHECK_FALSE(fbe_world_set_sky_atmosphere(world.handle, sun, 1.0F));
    CHECK(last_error() == "a sky's sun is a directional light");
    CHECK(fbe_world_clear_sky(world.handle));
}

TEST_CASE("a decal is what's in its box on faces toward it, clipped to it", "[scene][decal]") {
    const test_world_t world;
    cube_mesh(world.handle);
    // Over the cube's +Z face, whose corners it cuts in half.
    const fbe_mat4_t box = box_at(0.0F, 0.0F, 1.0F, 0.5F, 0.5F, 0.5F);
    const std::uint64_t made = fbe_geometry_decal(world.handle, &box, 0.01F);
    REQUIRE(made != 0);
    const std::vector<fbe_vertex_t> vertices = vertices_of(made);
    REQUIRE(!vertices.empty());
    for (const fbe_vertex_t& vertex : vertices) {
        CHECK(vertex.position.z == Approx(1.01F));
        CHECK(std::fabs(vertex.position.x) <= 0.5F + 0.0001F);
        CHECK(std::fabs(vertex.position.y) <= 0.5F + 0.0001F);
        CHECK(vertex.normal.z == Approx(1.0F));
        // Across its X and down its Y.
        CHECK(vertex.uv0.x == Approx(vertex.position.x + 0.5F));
        CHECK(vertex.uv0.y == Approx(0.5F - vertex.position.y));
        CHECK(vertex.uv1.x == vertex.uv0.x);
    }
    fbe_geometry_release(made);
}

TEST_CASE("a decal leaves out what faces away, or isn't in its box", "[scene][decal]") {
    const test_world_t world;
    cube_mesh(world.handle);
    // Turned to look at the cube's far side, none of it faces the box's +Z.
    const fbe_mat4_t away = box_at(0.0F, 0.0F, -1.0F, 0.5F, 0.5F, -0.5F);
    const std::uint64_t behind = fbe_geometry_decal(world.handle, &away, 0.0F);
    REQUIRE(behind != 0);
    const std::vector<fbe_vertex_t> back = vertices_of(behind);
    REQUIRE(!back.empty());
    for (const fbe_vertex_t& vertex : back) {
        CHECK(vertex.position.z == Approx(-1.0F));
    }
    fbe_geometry_release(behind);
    const fbe_mat4_t far = box_at(10.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F);
    const std::uint64_t nothing = fbe_geometry_decal(world.handle, &far, 0.0F);
    REQUIRE(nothing != 0);
    CHECK(fbe_surface_vertex_count(nothing, 0) == 0);
    fbe_geometry_release(nothing);
}

TEST_CASE("a decal goes on a moved, scaled, or mirrored mesh, as it shows", "[scene][decal]") {
    const test_world_t world;
    const std::uint64_t mesh = cube_mesh(world.handle);
    REQUIRE(fbe_entity_set_position(mesh, 5.0F, 0.0F, 0.0F, FBE_SPACE_WORLD));
    REQUIRE(fbe_entity_set_scale(mesh, 2.0F, 2.0F, 2.0F, FBE_SPACE_WORLD));
    const fbe_mat4_t box = box_at(5.0F, 0.0F, 2.0F, 0.5F, 0.5F, 0.5F);
    const std::uint64_t scaled = fbe_geometry_decal(world.handle, &box, 0.0F);
    const std::vector<fbe_vertex_t> on_scaled = vertices_of(scaled);
    REQUIRE(!on_scaled.empty());
    CHECK(on_scaled[0].position.z == Approx(2.0F));
    fbe_geometry_release(scaled);
    // Mirrored in x: its faces still face out.
    REQUIRE(fbe_entity_set_scale(mesh, -2.0F, 2.0F, 2.0F, FBE_SPACE_WORLD));
    const std::uint64_t mirrored = fbe_geometry_decal(world.handle, &box, 0.0F);
    const std::vector<fbe_vertex_t> on_mirrored = vertices_of(mirrored);
    REQUIRE(!on_mirrored.empty());
    CHECK(on_mirrored[0].normal.z == Approx(1.0F));
    fbe_geometry_release(mirrored);
}

TEST_CASE("a decal goes on what's opaque, so never on another", "[scene][decal]") {
    const test_world_t world;
    const std::uint64_t mesh = cube_mesh(world.handle);
    const std::uint64_t glass = fbe_material_create();
    REQUIRE(fbe_material_set_blend(glass, FBE_BLEND_ALPHA));
    REQUIRE(fbe_mesh_set_material(mesh, glass));
    const fbe_mat4_t box = box_at(0.0F, 0.0F, 1.0F, 0.5F, 0.5F, 0.5F);
    const std::uint64_t on_glass = fbe_geometry_decal(world.handle, &box, 0.0F);
    CHECK(fbe_surface_vertex_count(on_glass, 0) == 0);
    fbe_geometry_release(on_glass);
    REQUIRE(fbe_mesh_set_material(mesh, 0));
    const std::uint64_t first = fbe_geometry_decal(world.handle, &box, 0.01F);
    const std::int32_t alone = fbe_surface_vertex_count(first, 0);
    REQUIRE(alone > 0);
    const std::uint64_t drawn = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    REQUIRE(fbe_mesh_set_geometry(drawn, first));
    REQUIRE(fbe_mesh_set_material(drawn, glass));
    const std::uint64_t second = fbe_geometry_decal(world.handle, &box, 0.01F);
    CHECK(fbe_surface_vertex_count(second, 0) == alone);
    fbe_geometry_release(first);
    fbe_geometry_release(second);
    fbe_material_release(glass);
}

TEST_CASE("a decal goes on a terrain and a ground plane", "[scene][decal]") {
    const test_world_t world;
    // Facing up: its +Z is the world's +Y.
    fbe_mat4_t box{};
    box.m[0] = 1.0F;
    box.m[6] = -1.0F;
    box.m[9] = 1.0F;
    box.m[12] = 1.5F;
    box.m[13] = 0.0F;
    box.m[14] = 1.5F;
    box.m[15] = 1.0F;
    const std::uint64_t terrain = fbe_entity_create(world.handle, 0, FBE_ENTITY_TERRAIN);
    REQUIRE(fbe_terrain_set_size(terrain, 4));
    const std::uint64_t on_terrain = fbe_geometry_decal(world.handle, &box, 0.0F);
    const std::vector<fbe_vertex_t> over_terrain = vertices_of(on_terrain);
    REQUIRE(!over_terrain.empty());
    for (const fbe_vertex_t& vertex : over_terrain) {
        CHECK(vertex.position.y == Approx(0.0F).margin(0.0001));
        CHECK(vertex.normal.y == Approx(1.0F));
        CHECK(std::fabs(vertex.position.x - 1.5F) <= 1.0001F);
    }
    fbe_geometry_release(on_terrain);
    REQUIRE(fbe_entity_free(terrain));
    const std::uint64_t plane = fbe_entity_create(world.handle, 0, FBE_ENTITY_PLANE);
    REQUIRE(plane != 0);
    const std::uint64_t on_plane = fbe_geometry_decal(world.handle, &box, 0.0F);
    const std::vector<fbe_vertex_t> over_plane = vertices_of(on_plane);
    REQUIRE(!over_plane.empty());
    for (const fbe_vertex_t& vertex : over_plane) {
        CHECK(vertex.position.y == Approx(0.0F).margin(0.0001));
        CHECK(vertex.normal.y == Approx(1.0F));
    }
    fbe_geometry_release(on_plane);
}

TEST_CASE("a decal's box has an inverse, its lift's 0 or more, and its World's live",
          "[scene][decal]") {
    const test_world_t world;
    const fbe_mat4_t flat = box_at(0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 1.0F);
    CHECK(fbe_geometry_decal(world.handle, &flat, 0.0F) == 0);
    CHECK(last_error() == "a decal's box has an inverse");
    const fbe_mat4_t box = box_at(0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F);
    CHECK(fbe_geometry_decal(world.handle, &box, -1.0F) == 0);
    CHECK(fbe_geometry_decal(0, &box, 0.0F) == 0);
}
