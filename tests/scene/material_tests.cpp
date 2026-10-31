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
    CHECK(std::string_view(fbe_error()) == "the material was freed");
    fbe_geometry_release(geometry);
    fbe_material_release(material);
}
