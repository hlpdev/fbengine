// Materials through the C API: how long they live as surfaces, meshes, and handles hold them, and
// what their settings take.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string_view>

namespace {

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

} // namespace

TEST_CASE("a material lives while a surface draws in it, through clones", "[scene]") {
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t geometry = fbe_geometry_cube();
    REQUIRE(fbe_surface_set_material(geometry, 0, material));
    const std::uint64_t clone = fbe_geometry_clone(geometry);
    REQUIRE(fbe_material_release(material));
    CHECK(fbe_material_alive(material));
    fbe_geometry_release(geometry);
    CHECK(fbe_material_alive(material));
    CHECK(fbe_surface_material(clone, 0) == material);
    fbe_geometry_release(clone);
    CHECK_FALSE(fbe_material_alive(material));
}

TEST_CASE("a mesh's material lives with it and its copies", "[scene]") {
    const test_world_t world;
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    REQUIRE(fbe_mesh_set_material(mesh, material));
    fbe_mesh_set_tint(mesh, 1.0F, 0.0F, 0.0F, 0.5F);
    const std::uint64_t copy = fbe_entity_copy(mesh, 0, 0);
    fbe_material_release(material);
    fbe_entity_free(mesh);
    CHECK(fbe_material_alive(material));
    CHECK(fbe_mesh_material(copy) == material);
    fbe_color_t tint{};
    fbe_mesh_tint(copy, &tint);
    CHECK(tint.a == 0.5F);
    REQUIRE(fbe_mesh_set_material(copy, material));
    CHECK(fbe_material_alive(material));
    REQUIRE(fbe_mesh_set_material(copy, 0));
    CHECK_FALSE(fbe_material_alive(material));
}

TEST_CASE("a material's settings take only what they can be", "[scene]") {
    const std::uint64_t material = fbe_material_create();
    CHECK_FALSE(fbe_material_set_layer(material, 8, FBE_LAYER_ADD, 0));
    CHECK(std::string_view(fbe_error()) == "a material's layers are 0 to 7, not 8");
    CHECK_FALSE(fbe_material_set_layer(material, 0, 4, 0));
    CHECK_FALSE(fbe_material_set_layer(material, 0, FBE_LAYER_ADD, 2));
    CHECK(fbe_material_layer_blend(material, -1) == -1);
    CHECK_FALSE(fbe_material_set_shininess(material, std::numeric_limits<float>::quiet_NaN()));
    CHECK_FALSE(fbe_material_set_alpha_cutoff(material, 1.5F));
    CHECK_FALSE(fbe_material_set_shading(material, 3));
    CHECK_FALSE(fbe_material_set_blend(material, 5));
    CHECK_FALSE(
        fbe_material_set_color(material, 0.0F, 0.0F, 0.0F, std::numeric_limits<float>::infinity()));
    const std::uint64_t geometry = fbe_geometry_cube();
    CHECK_FALSE(fbe_surface_set_material(geometry, 1, material));
    CHECK_FALSE(fbe_surface_set_material(geometry, 0, geometry));
    CHECK(std::string_view(fbe_error()) == "that handle isn't a material's");
    fbe_geometry_release(geometry);
    fbe_material_release(material);
}

TEST_CASE("what Add copies holds its surfaces' materials", "[scene]") {
    const std::uint64_t material = fbe_material_create();
    const std::uint64_t from = fbe_geometry_cube();
    REQUIRE(fbe_surface_set_material(from, 0, material));
    const std::uint64_t into = fbe_geometry_create();
    const fbe_mat4_t identity{ .m = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } };
    REQUIRE(fbe_geometry_add(into, from, &identity));
    fbe_material_release(material);
    fbe_geometry_release(from);
    CHECK(fbe_material_alive(material));
    CHECK(fbe_surface_material(into, 0) == material);
    // A new material may take the freed slot; letting go of `into` must not touch it.
    const std::uint64_t other = fbe_material_create();
    fbe_geometry_release(into);
    CHECK(fbe_material_alive(other));
    CHECK_FALSE(fbe_material_alive(material));
    fbe_material_release(other);
}

TEST_CASE("alpha is 0 to 1, and colors any finite value", "[scene]") {
    const test_world_t world;
    const std::uint64_t material = fbe_material_create();
    CHECK(fbe_material_set_color(material, -2.0F, 5.0F, 0.0F, 1.0F));
    CHECK_FALSE(fbe_material_set_color(material, 1.0F, 1.0F, 1.0F, 1.5F));
    CHECK(std::string_view(fbe_error()) == "a material's alpha is 0 to 1, not 1.5");
    CHECK_FALSE(fbe_material_set_color(material, 1.0F, 1.0F, 1.0F, -0.1F));
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    CHECK_FALSE(fbe_mesh_set_tint(mesh, 1.0F, 1.0F, 1.0F, 2.0F));
    CHECK(fbe_mesh_set_tint(mesh, 3.0F, 1.0F, 1.0F, 0.0F));
    fbe_material_release(material);
}

TEST_CASE("a mesh's surfaces are drawn with blends its alpha settles", "[scene]") {
    const test_world_t world;
    const std::uint64_t geometry = fbe_geometry_cube();
    const std::uint64_t mesh = fbe_entity_create(world.handle, 0, FBE_ENTITY_MESH);
    fbe_mesh_set_geometry(mesh, geometry);
    // The default material is AUTO and opaque.
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_OPAQUE);
    fbe_mesh_set_tint(mesh, 1.0F, 1.0F, 1.0F, 0.5F);
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_ALPHA);
    const std::uint64_t opaque = fbe_material_create();
    fbe_material_set_blend(opaque, FBE_BLEND_OPAQUE);
    fbe_material_set_color(opaque, 1.0F, 1.0F, 1.0F, 0.25F);
    fbe_surface_set_material(geometry, 0, opaque);
    // OPAQUE leaves its own alpha out, but the mesh's still fades it.
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_ALPHA);
    fbe_mesh_set_tint(mesh, 1.0F, 1.0F, 1.0F, 1.0F);
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_OPAQUE);
    fbe_material_set_blend(opaque, FBE_BLEND_AUTO);
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_ALPHA);
    const std::uint64_t glow = fbe_material_create();
    fbe_material_set_blend(glow, FBE_BLEND_ADD);
    fbe_mesh_set_material(mesh, glow);
    CHECK(fbe_mesh_blend(mesh, 0) == FBE_BLEND_ADD);
    CHECK(fbe_mesh_blend(mesh, 3) == FBE_BLEND_ADD);
    fbe_mesh_set_material(mesh, 0);
    CHECK(fbe_mesh_blend(mesh, 1) == -1);
    fbe_material_release(opaque);
    fbe_material_release(glow);
    fbe_geometry_release(geometry);
}
