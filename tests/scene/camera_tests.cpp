// Cameras through the C API: the pool their settings live in, as cameras come and go, and what
// isn't a camera.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>
#include <vector>

namespace {

std::uint64_t camera(std::uint64_t world, std::uint64_t parent = 0) {
    return fbe_entity_create(world, parent, FBE_ENTITY_CAMERA);
}

std::vector<std::uint64_t> cameras(std::uint64_t world) {
    std::vector<std::uint64_t> found(
        static_cast<std::size_t>(fbe_world_cameras(world, nullptr, 0)));
    fbe_world_cameras(world, found.data(), static_cast<std::int32_t>(found.size()));
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

TEST_CASE("freeing a camera leaves the others' settings theirs", "[scene]") {
    const test_world_t world;
    std::vector<std::uint64_t> made;
    for (int i = 0; i < 5; ++i) {
        made.push_back(camera(world.handle));
        fbe_camera_set_order(made.back(), i * 10);
    }
    fbe_entity_free(made[1]);
    fbe_entity_free(made[3]);
    CHECK(fbe_camera_order(made[0]) == 0);
    CHECK(fbe_camera_order(made[2]) == 20);
    CHECK(fbe_camera_order(made[4]) == 40);
    CHECK(cameras(world.handle) == std::vector{ made[0], made[2], made[4] });

    // A parent freed takes its cameras with it.
    const std::uint64_t rig = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    const std::uint64_t left = camera(world.handle, rig);
    camera(world.handle, rig);
    fbe_camera_set_order(left, -5);
    CHECK(cameras(world.handle).front() == left);
    fbe_entity_free(rig);
    CHECK(cameras(world.handle).size() == 3);
}

TEST_CASE("a slot that held a camera holds what's made there next", "[scene]") {
    const test_world_t world;
    const std::uint64_t old = camera(world.handle);
    fbe_entity_free(old);
    const std::uint64_t pivot = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    REQUIRE(static_cast<std::uint32_t>(pivot) == static_cast<std::uint32_t>(old));
    CHECK_FALSE(fbe_camera_set_order(pivot, 1));
    CHECK(std::string_view(fbe_error()) == "the entity isn't a camera");
    CHECK_FALSE(fbe_camera_set_order(old, 1));
    CHECK(std::string_view(fbe_error()) == "the entity was freed");
    CHECK(cameras(world.handle).empty());
}

TEST_CASE("a copied camera has its own settings", "[scene]") {
    const test_world_t world;
    const std::uint64_t original = camera(world.handle);
    fbe_camera_set_range(original, 0.05F, 28.0F);
    fbe_camera_set_viewport(original, 0.0F, 0.0F, 512.0F, 512.0F);
    const std::uint64_t copy = fbe_entity_copy(original, 0, 0);
    CHECK(fbe_camera_farthest(copy) == 28.0F);
    CHECK_FALSE(fbe_camera_whole_target(copy));
    fbe_camera_set_range(copy, 1.0F, 2.0F);
    CHECK(fbe_camera_farthest(original) == 28.0F);
    CHECK(cameras(world.handle) == std::vector{ original, copy });
}

TEST_CASE("a camera's settings take only what they can be", "[scene]") {
    const test_world_t world;
    const std::uint64_t made = camera(world.handle);
    CHECK_FALSE(fbe_camera_set_field_of_view(made, 0.0F));
    CHECK_FALSE(fbe_camera_set_field_of_view(made, 3.2F));
    CHECK_FALSE(fbe_camera_set_range(made, 0.0F, 1.0F));
    CHECK_FALSE(fbe_camera_set_range(made, 2.0F, 1.0F));
    CHECK_FALSE(fbe_camera_set_orthographic_height(made, -1.0F));
    CHECK_FALSE(fbe_camera_set_viewport(made, 0.0F, 0.0F, 0.0F, 10.0F));
    CHECK_FALSE(fbe_camera_set_projection(made, 2));
    CHECK(std::string_view(fbe_error()) == "2 isn't a Projection");
    fbe_vec3_t out{};
    CHECK(fbe_camera_project(made, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F, &out) == 0);
    CHECK(fbe_camera_project(made, 100.0F, 100.0F, 0.0F, 0.0F, -1.0F, &out) == 1);
    CHECK(fbe_world_cameras(0, nullptr, 0) == -1);
}

TEST_CASE("a target with no size projects nothing, and has a square's shape", "[scene]") {
    const test_world_t world;
    const std::uint64_t made = camera(world.handle);
    fbe_vec3_t out{};
    // A window dragged to nothing high.
    CHECK(fbe_camera_project(made, 100.0F, 0.0F, 0.0F, 0.0F, -5.0F, &out) == 0);
    CHECK(fbe_camera_unproject(made, 100.0F, 0.0F, 50.0F, 0.0F, 5.0F, &out) == 0);
    fbe_mat4_t projection{};
    REQUIRE(fbe_camera_projection_matrix(made, 100.0F, 0.0F, &projection));
    CHECK(projection.m[0] == projection.m[5]);
    CHECK(fbe_camera_point_in_view(made, 100.0F, 0.0F, 0.0F, 0.0F, -5.0F) == 1);
    CHECK(fbe_camera_unproject(made, 100.0F, 100.0F, 50.0F, 50.0F, 5.0F, &out) == 1);
}

TEST_CASE("a field of view or height too small to project is refused", "[scene]") {
    const test_world_t world;
    const std::uint64_t made = camera(world.handle);
    CHECK_FALSE(fbe_camera_set_field_of_view(made, 1e-45F));
    CHECK_FALSE(fbe_camera_set_orthographic_height(made, 1e-45F));
    CHECK(fbe_camera_set_field_of_view(made, 1e-6F));
    CHECK(fbe_camera_set_orthographic_height(made, 1e-6F));
}
