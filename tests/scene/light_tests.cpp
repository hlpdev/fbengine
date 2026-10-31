// Lights through the C API: their pool beside the cameras', and what isn't a light.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace {

std::uint64_t light(std::uint64_t world, std::uint64_t parent = 0) {
    return fbe_entity_create(world, parent, FBE_ENTITY_LIGHT);
}

std::vector<std::uint64_t> lights(std::uint64_t world) {
    std::vector<std::uint64_t> found(static_cast<std::size_t>(fbe_world_lights(world, nullptr, 0)));
    fbe_world_lights(world, found.data(), static_cast<std::int32_t>(found.size()));
    return found;
}

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

TEST_CASE("lights and cameras keep their own pools", "[scene]") {
    const test_world_t world;
    const std::uint64_t first = light(world.handle);
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t second = light(world.handle);
    fbe_light_set_range(first, 1.0F);
    fbe_light_set_range(second, 2.0F);
    fbe_camera_set_order(camera, 7);
    fbe_entity_free(first);
    CHECK(fbe_light_range(second) == 2.0F);
    CHECK(fbe_camera_order(camera) == 7);
    CHECK(lights(world.handle) == std::vector{ second });
    CHECK_FALSE(fbe_light_set_range(camera, 1.0F));
    CHECK(std::string_view(fbe_error()) == "the entity isn't a light");
    CHECK_FALSE(fbe_camera_set_fog_mode(second, FBE_FOG_LINEAR));
    CHECK(std::string_view(fbe_error()) == "the entity isn't a camera");
}

TEST_CASE("a light's and a fog's settings take only what they can be", "[scene]") {
    const test_world_t world;
    const std::uint64_t made = light(world.handle);
    CHECK_FALSE(fbe_light_set_type(made, 3));
    CHECK(std::string_view(fbe_error()) == "3 isn't a LightType");
    CHECK_FALSE(fbe_light_set_cone(made, -0.1F, 0.5F));
    CHECK_FALSE(fbe_light_set_cone(made, 0.0F, 1.6F));
    CHECK(fbe_light_set_cone(made, 0.5F, 0.5F));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(fbe_light_set_cone(made, nan, 0.5F));
    CHECK_FALSE(fbe_light_set_cone(made, 0.1F, nan));
    CHECK(fbe_light_set_intensity(made, 0.0F));
    CHECK_FALSE(fbe_light_set_color(made, 1.0F, std::numeric_limits<float>::infinity(), 1.0F));
    CHECK_FALSE(fbe_world_set_ambient(world.handle, 0.0F, 0.0F, nan));
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    CHECK_FALSE(fbe_camera_set_fog_mode(camera, 4));
    CHECK(std::string_view(fbe_error()) == "4 isn't a FogMode");
    CHECK(fbe_camera_set_fog_range(camera, 0.0F, 0.1F));
    CHECK_FALSE(fbe_camera_set_fog_range(camera, -1.0F, 0.1F));
    CHECK_FALSE(fbe_camera_set_fog_range(camera, nan, 0.1F));
    CHECK_FALSE(fbe_camera_set_fog_density(camera, nan));
    CHECK(fbe_world_lights(0, nullptr, 0) == -1);
}
