// Entities' transforms through the C API: the dirty flags that keep world transforms current,
// parents that shear or scale to nothing, and the batches.

#include "fbengine.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace {

constexpr float quarter = 1.5707963F;

std::uint64_t pivot(std::uint64_t world, std::uint64_t parent = 0) {
    return fbe_entity_create(world, parent, FBE_ENTITY_PIVOT);
}

fbe_vec3_t world_position(std::uint64_t entity) {
    fbe_vec3_t found{};
    fbe_entity_position(entity, FBE_SPACE_WORLD, &found);
    return found;
}

bool near(fbe_vec3_t a, fbe_vec3_t b) {
    return std::fabs(a.x - b.x) < 1e-4F && std::fabs(a.y - b.y) < 1e-4F &&
           std::fabs(a.z - b.z) < 1e-4F;
}

/// A turn about Y, left for a positive angle.
fbe_quat_t yaw(float angle) {
    return { .x = 0.0F, .y = std::sin(angle / 2.0F), .z = 0.0F, .w = std::cos(angle / 2.0F) };
}

/// A pose at x along +X, turned about Y.
fbe_pose_t pose(float x, float angle) {
    return { .position = { .x = x, .y = 0.0F, .z = 0.0F }, .rotation = yaw(angle) };
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

TEST_CASE("a moved ancestor moves what's under it, however often it's asked", "[scene]") {
    const test_world_t world;
    const std::uint64_t root = pivot(world.handle);
    std::uint64_t at = root;
    for (int i = 0; i < 20; ++i) {
        at = pivot(world.handle, at);
        fbe_entity_set_position(at, 1.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    }
    CHECK(near(world_position(at), { 20.0F, 0.0F, 0.0F }));

    // A middle entity worked out on its own, then the root moved: the leaf still follows.
    const std::uint64_t middle = fbe_entity_parent(fbe_entity_parent(at));
    CHECK(near(world_position(middle), { 18.0F, 0.0F, 0.0F }));
    fbe_entity_set_position(root, 0.0F, 5.0F, 0.0F, FBE_SPACE_LOCAL);
    CHECK(near(world_position(at), { 20.0F, 5.0F, 0.0F }));
    CHECK(near(world_position(middle), { 18.0F, 5.0F, 0.0F }));
    fbe_entity_turn(root, yaw(quarter).x, yaw(quarter).y, yaw(quarter).z, yaw(quarter).w,
                    FBE_SPACE_LOCAL);
    CHECK(near(world_position(middle), { 0.0F, 5.0F, -18.0F }));
    CHECK(near(world_position(at), { 0.0F, 5.0F, -20.0F }));
}

TEST_CASE("an entity reparented or copied moves with its new parent", "[scene]") {
    const test_world_t world;
    const std::uint64_t first = pivot(world.handle);
    const std::uint64_t second = pivot(world.handle);
    fbe_entity_set_position(second, 0.0F, 0.0F, 7.0F, FBE_SPACE_LOCAL);
    const std::uint64_t child = pivot(world.handle, first);
    const std::uint64_t grandchild = pivot(world.handle, child);
    CHECK(near(world_position(grandchild), {}));
    REQUIRE(fbe_entity_set_parent(child, second, false));
    CHECK(near(world_position(grandchild), { 0.0F, 0.0F, 7.0F }));

    const std::uint64_t copy = fbe_entity_copy(child, 0, first);
    fbe_entity_set_position(first, 3.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    CHECK(near(world_position(fbe_entity_child(copy, 0)), { 3.0F, 0.0F, 0.0F }));
}

TEST_CASE("a slot used again starts with no transform", "[scene]") {
    const test_world_t world;
    const std::uint64_t old = pivot(world.handle);
    fbe_entity_set_position(old, 1.0F, 2.0F, 3.0F, FBE_SPACE_LOCAL);
    fbe_entity_set_scale(old, 2.0F, 2.0F, 2.0F, FBE_SPACE_LOCAL);
    CHECK(near(world_position(old), { 1.0F, 2.0F, 3.0F }));
    fbe_entity_free(old);
    const std::uint64_t made = pivot(world.handle);
    REQUIRE(static_cast<std::uint32_t>(made) == static_cast<std::uint32_t>(old));
    CHECK(near(world_position(made), {}));
    fbe_vec3_t scale{};
    fbe_entity_scale(made, FBE_SPACE_WORLD, &scale);
    CHECK(near(scale, { 1.0F, 1.0F, 1.0F }));
}

TEST_CASE("a world position round-trips under a shearing parent", "[scene]") {
    const test_world_t world;
    const std::uint64_t top = pivot(world.handle);
    fbe_entity_set_scale(top, 3.0F, 1.0F, 1.0F, FBE_SPACE_LOCAL);
    const std::uint64_t middle = pivot(world.handle, top);
    const fbe_quat_t turn = yaw(0.7F);
    fbe_entity_set_rotation(middle, turn.x, turn.y, turn.z, turn.w, FBE_SPACE_LOCAL);
    const std::uint64_t child = pivot(world.handle, middle);
    REQUIRE(fbe_entity_set_position(child, 4.0F, -2.0F, 9.0F, FBE_SPACE_WORLD));
    CHECK(near(world_position(child), { 4.0F, -2.0F, 9.0F }));
}

TEST_CASE("a parent scaled to nothing doesn't make nonsense of its children", "[scene]") {
    const test_world_t world;
    const std::uint64_t parent = pivot(world.handle);
    fbe_entity_set_position(parent, 1.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    fbe_entity_set_scale(parent, 0.0F, 1.0F, 1.0F, FBE_SPACE_LOCAL);
    const std::uint64_t child = pivot(world.handle, parent);
    REQUIRE(fbe_entity_set_position(child, 5.0F, 2.0F, 3.0F, FBE_SPACE_WORLD));
    fbe_vec3_t local{};
    fbe_entity_position(child, FBE_SPACE_LOCAL, &local);
    CHECK(near(local, { 0.0F, 2.0F, 3.0F }));
    CHECK(near(world_position(child), { 1.0F, 2.0F, 3.0F }));
    REQUIRE(fbe_entity_set_scale(child, 2.0F, 2.0F, 2.0F, FBE_SPACE_WORLD));
    fbe_vec3_t scale{};
    fbe_entity_scale(child, FBE_SPACE_LOCAL, &scale);
    CHECK(near(scale, { 0.0F, 2.0F, 2.0F }));

    fbe_vec3_t out{};
    CHECK_FALSE(fbe_entity_transform(0, parent, FBE_TRANSFORM_POINT, 1.0F, 1.0F, 1.0F, &out));
    CHECK(std::string_view(fbe_error()).contains("scaled to nothing"));
    // A wall facing the flattened axis still faces it; a floor, flattened to a line, faces nowhere.
    CHECK(fbe_entity_transform(parent, 0, FBE_TRANSFORM_NORMAL, 2.0F, 0.0F, 0.0F, &out));
    CHECK(near(out, { 1.0F, 0.0F, 0.0F }));
    CHECK(fbe_entity_transform(parent, 0, FBE_TRANSFORM_NORMAL, 0.0F, 1.0F, 0.0F, &out));
    CHECK(near(out, {}));
}

TEST_CASE("rotations are kept at length 1", "[scene]") {
    const test_world_t world;
    const std::uint64_t entity = pivot(world.handle);
    fbe_entity_set_rotation(entity, 0.0F, 2.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    fbe_quat_t found{};
    fbe_entity_rotation(entity, FBE_SPACE_LOCAL, &found);
    CHECK(found.y == Catch::Approx(1.0F));
    fbe_entity_set_rotation(entity, 0.0F, 0.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    fbe_entity_rotation(entity, FBE_SPACE_LOCAL, &found);
    CHECK(found.w == 1.0F);
}

TEST_CASE("a space or axis that isn't one fails", "[scene]") {
    const test_world_t world;
    const std::uint64_t entity = pivot(world.handle);
    CHECK_FALSE(fbe_entity_set_position(entity, 0.0F, 0.0F, 0.0F, 2));
    CHECK(std::string_view(fbe_error()) == "2 isn't a Space");
    CHECK_FALSE(fbe_entity_align(entity, 1.0F, 0.0F, 0.0F, 3, 1.0F));
    CHECK(std::string_view(fbe_error()) == "3 isn't an Axis");
    fbe_vec3_t out{};
    CHECK_FALSE(fbe_entity_transform(0, 0, 3, 0.0F, 0.0F, 0.0F, &out));
}

TEST_CASE("a batch sets every live entity, and says one was freed", "[scene]") {
    const test_world_t world;
    const std::uint64_t first = pivot(world.handle);
    const std::uint64_t second = pivot(world.handle);
    const std::array<std::uint64_t, 3> entities{ first, second, pivot(world.handle) };
    fbe_entity_free(entities[1]);
    const fbe_pose_t turned = pose(3.0F, quarter);
    const std::array<fbe_pose_t, 3> poses{ pose(1.0F, 0.0F), pose(2.0F, 0.0F), turned };
    CHECK_FALSE(fbe_entity_set_poses(entities.data(), poses.data(), 3, FBE_SPACE_WORLD));
    CHECK(near(world_position(entities[0]), { 1.0F, 0.0F, 0.0F }));
    CHECK(near(world_position(entities[2]), { 3.0F, 0.0F, 0.0F }));

    std::array<fbe_pose_t, 3> back{};
    CHECK_FALSE(fbe_entity_poses(entities.data(), back.data(), 3, FBE_SPACE_LOCAL));
    CHECK(back[1].rotation.w == 0.0F);
    CHECK(back[2].rotation.y == Catch::Approx(yaw(quarter).y));
    CHECK(fbe_entity_poses(entities.data(), back.data(), 1, FBE_SPACE_LOCAL));
}
