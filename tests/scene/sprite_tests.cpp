// Sprites as entities, and how a camera sees one, through the C API; and the World's capture, which
// drawing tweens from.

#include "fbengine.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>

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

bool near(float a, float b) {
    return std::fabs(a - b) < 0.0001F;
}

/// Where a point of the quad, in its own -1 to 1, is in the world through a sprite's matrix.
fbe_vec3_t corner(const fbe_mat4_t& m, float x, float y) {
    return {
        .x = (m.m[0] * x) + (m.m[4] * y) + m.m[12],
        .y = (m.m[1] * x) + (m.m[5] * y) + m.m[13],
        .z = (m.m[2] * x) + (m.m[6] * y) + m.m[14],
    };
}

bool near(fbe_vec3_t a, float x, float y, float z) {
    return near(a.x, x) && near(a.y, y) && near(a.z, z);
}

} // namespace

TEST_CASE("a sprite faces the camera as its view mode says", "[scene]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t sprite = fbe_entity_create(world.handle, 0, FBE_ENTITY_SPRITE);
    CHECK(fbe_entity_kind(sprite) == FBE_ENTITY_SPRITE);
    fbe_entity_set_position(sprite, 0.0F, 0.0F, -5.0F, FBE_SPACE_WORLD);
    CHECK(fbe_sprite_view_mode(sprite) == FBE_SPRITE_BILLBOARD);
    fbe_vec2_t size{};
    fbe_sprite_size(sprite, &size);
    CHECK((size.x == 1.0F && size.y == 1.0F));
    // A camera turned a quarter about Y: the billboard's corners lie across its view.
    const float half = 0.70710678F;
    fbe_entity_set_rotation(camera, 0.0F, half, 0.0F, half, FBE_SPACE_WORLD);
    // The entity's own turn and scale don't count.
    fbe_entity_set_rotation(sprite, half, 0.0F, 0.0F, half, FBE_SPACE_WORLD);
    fbe_entity_set_scale(sprite, 5.0F, 5.0F, 5.0F, FBE_SPACE_LOCAL);
    fbe_mat4_t m{};
    REQUIRE(fbe_sprite_matrix(sprite, camera, &m));
    // The camera's right is now -Z, its up Y.
    CHECK(near(corner(m, 1.0F, 0.0F), 0.0F, 0.0F, -6.0F));
    CHECK(near(corner(m, 0.0F, 1.0F), 0.0F, 1.0F, -5.0F));
    // Sized, rolled a quarter counterclockwise, and with its bottom middle at its place.
    fbe_sprite_set_size(sprite, 2.0F, 0.5F);
    fbe_sprite_set_handle(sprite, 0.0F, -1.0F);
    fbe_sprite_set_roll(sprite, 1.5707963F);
    REQUIRE(fbe_sprite_matrix(sprite, camera, &m));
    // Rolled, the quad's +X points up and its +Y the camera's left (+Z), 2 and 0.5 long.
    CHECK(near(corner(m, 0.0F, -1.0F), 0.0F, 0.0F, -5.0F));
    CHECK(near(corner(m, 1.0F, -1.0F), 0.0F, 2.0F, -5.0F));
    CHECK(near(corner(m, 0.0F, 0.0F), 0.0F, 0.0F, -4.5F));

    // FIXED: as the entity is turned and scaled.
    fbe_sprite_set_view_mode(sprite, FBE_SPRITE_FIXED);
    fbe_sprite_set_size(sprite, 1.0F, 1.0F);
    fbe_sprite_set_handle(sprite, 0.0F, 0.0F);
    fbe_sprite_set_roll(sprite, 0.0F);
    REQUIRE(fbe_sprite_matrix(sprite, camera, &m));
    // Turned a quarter about X: its +Y goes to +Z.
    CHECK(near(corner(m, 0.0F, 1.0F), 0.0F, 0.0F, 0.0F));
    CHECK(near(corner(m, 1.0F, 0.0F), 5.0F, 0.0F, -5.0F));

    // UPRIGHT: on its own up, facing the camera about it.
    fbe_sprite_set_view_mode(sprite, FBE_SPRITE_UPRIGHT);
    fbe_entity_set_rotation(sprite, 0.0F, 0.0F, 0.0F, 1.0F, FBE_SPACE_WORLD);
    // The camera tilted down too: an upright sprite stays upright.
    fbe_entity_set_rotation(camera, -0.25F, 0.0F, 0.0F, 0.9682458F, FBE_SPACE_WORLD);
    REQUIRE(fbe_sprite_matrix(sprite, camera, &m));
    CHECK(near(corner(m, 0.0F, 1.0F), 0.0F, 1.0F, -5.0F));
    CHECK(near(corner(m, 1.0F, 0.0F), 1.0F, 0.0F, -5.0F));
    // UPRIGHT_YAW: its own turn and scale, then the camera's yaw alone, here a quarter to the left.
    fbe_sprite_set_view_mode(sprite, FBE_SPRITE_UPRIGHT_YAW);
    fbe_entity_set_scale(sprite, 2.0F, 3.0F, 1.0F, FBE_SPACE_WORLD);
    const float half_turn = std::numbers::pi_v<float> * 0.25F;
    fbe_entity_set_rotation(camera, 0.0F, std::sin(half_turn), 0.0F, std::cos(half_turn),
                            FBE_SPACE_WORLD);
    REQUIRE(fbe_sprite_matrix(sprite, camera, &m));
    CHECK(near(corner(m, 0.0F, 0.0F), 0.0F, 0.0F, -5.0F));
    CHECK(near(corner(m, 1.0F, 0.0F), 0.0F, 0.0F, -7.0F));
    CHECK(near(corner(m, 0.0F, 1.0F), 0.0F, 3.0F, -5.0F));
    // Pitched as well, the same: only its yaw counts.
    const float tilt = 0.5F;
    fbe_entity_turn(camera, std::sin(tilt), 0.0F, 0.0F, std::cos(tilt), FBE_SPACE_LOCAL);
    REQUIRE(fbe_sprite_matrix(sprite, camera, &m));
    CHECK(near(corner(m, 1.0F, 0.0F), 0.0F, 0.0F, -7.0F));
    CHECK(near(corner(m, 0.0F, 1.0F), 0.0F, 3.0F, -5.0F));
    // Its reach has its scale in it, as it's how far its corners go.
    fbe_aabb_t reach{};
    REQUIRE(fbe_entity_bounds(sprite, &reach));
    CHECK(reach.max.y >= 3.0F);
    fbe_entity_set_scale(sprite, 1.0F, 1.0F, 1.0F, FBE_SPACE_WORLD);
    CHECK_FALSE(fbe_sprite_set_view_mode(sprite, 4));
    CHECK_FALSE(fbe_sprite_set_size(sprite, -1.0F, 1.0F));
}

TEST_CASE("a sprite's material, tint, and settings copy, and its bounds reach its corners",
          "[scene]") {
    const test_world_t world;
    const std::uint64_t sprite = fbe_entity_create(world.handle, 0, FBE_ENTITY_SPRITE);
    const std::uint64_t material = fbe_material_create();
    REQUIRE(fbe_sprite_set_material(sprite, material));
    CHECK(fbe_sprite_material(sprite) == material);
    // The sprite holds it.
    fbe_material_release(material);
    CHECK(fbe_material_alive(material));
    fbe_sprite_set_tint(sprite, 1.0F, 0.5F, 0.25F, 0.5F);
    CHECK_FALSE(fbe_sprite_set_tint(sprite, 1.0F, 1.0F, 1.0F, 2.0F));
    fbe_sprite_set_size(sprite, 2.0F, 1.0F);
    fbe_sprite_set_view_mode(sprite, FBE_SPRITE_FIXED);
    const std::uint64_t copy = fbe_entity_copy(sprite, world.handle, 0);
    CHECK(fbe_entity_kind(copy) == FBE_ENTITY_SPRITE);
    CHECK(fbe_sprite_material(copy) == material);
    CHECK(fbe_sprite_view_mode(copy) == FBE_SPRITE_FIXED);
    fbe_color_t tint{};
    fbe_sprite_tint(copy, &tint);
    CHECK((tint.g == 0.5F && tint.a == 0.5F));
    // Its material lives while either draws in it.
    fbe_entity_free(sprite);
    CHECK(fbe_material_alive(material));
    fbe_entity_free(copy);
    CHECK_FALSE(fbe_material_alive(material));

    // However a billboard turns, its bounds hold its corners: 2 by 1 is under 2.24 out.
    const std::uint64_t other = fbe_entity_create(world.handle, 0, FBE_ENTITY_SPRITE);
    fbe_sprite_set_size(other, 2.0F, 1.0F);
    fbe_entity_set_position(other, 10.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    fbe_aabb_t bounds{};
    REQUIRE(fbe_entity_bounds(other, &bounds));
    CHECK(near(bounds.max.x, 10.0F + std::sqrt(5.0F)));
    CHECK(near(bounds.min.y, -std::sqrt(5.0F)));
    // A camera sees it by those bounds.
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    // Looking down -Z past it: its place is out of view, its corners' reach in.
    fbe_entity_set_position(camera, 12.5F, 0.0F, 2.0F, FBE_SPACE_WORLD);
    CHECK(fbe_camera_entity_in_view(camera, 100.0F, 100.0F, other) == 1);
    fbe_entity_set_position(camera, 16.0F, 0.0F, 2.0F, FBE_SPACE_WORLD);
    CHECK(fbe_camera_entity_in_view(camera, 100.0F, 100.0F, other) == 0);
}

TEST_CASE("a tweened matrix goes from the capture to where an entity is", "[scene]") {
    const test_world_t world;
    const std::uint64_t parent = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    const std::uint64_t child = fbe_entity_create(world.handle, parent, FBE_ENTITY_PIVOT);
    fbe_entity_set_position(child, 1.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    REQUIRE(fbe_world_capture(world.handle));
    // The parent moves 10 along X and turns a half about Y; the child grows 3 times.
    fbe_entity_set_position(parent, 10.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    fbe_entity_set_rotation(parent, 0.0F, 1.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    fbe_entity_set_scale(child, 3.0F, 3.0F, 3.0F, FBE_SPACE_LOCAL);
    fbe_mat4_t m{};
    REQUIRE(fbe_entity_tweened_matrix(parent, 0.5F, &m));
    CHECK(near(m.m[12], 5.0F));
    // Halfway through a half turn, a quarter: its X axis is -Z... or +Z, by the short way.
    CHECK(near(std::fabs(m.m[2]), 1.0F));
    REQUIRE(fbe_entity_tweened_matrix(child, 0.5F, &m));
    // The child is 1 out along its parent's turned X, and scaled 2.
    CHECK(near(m.m[12], 5.0F));
    CHECK(near(std::fabs(m.m[14]), 1.0F));
    CHECK(near(std::hypot(m.m[0], m.m[2]), 2.0F));
    // At 0, where it was captured; at 1, where it is.
    REQUIRE(fbe_entity_tweened_matrix(child, 0.0F, &m));
    CHECK(near(m.m[12], 1.0F));
    REQUIRE(fbe_entity_tweened_matrix(child, 1.0F, &m));
    fbe_mat4_t now{};
    fbe_entity_matrix(child, FBE_SPACE_WORLD, &now);
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(near(m.m[i], now.m[i]));
    }
    // One made since the capture is where it is.
    const std::uint64_t late = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_position(late, 4.0F, 0.0F, 0.0F, FBE_SPACE_LOCAL);
    REQUIRE(fbe_entity_tweened_matrix(late, 0.0F, &m));
    CHECK(near(m.m[12], 4.0F));
    CHECK_FALSE(fbe_entity_tweened_matrix(late, 1.5F, &m));
}

TEST_CASE("a tween holds across a new parent and a new World", "[scene]") {
    const test_world_t world;
    const std::uint64_t moved = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_position(moved, 5.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    REQUIRE(fbe_world_capture(world.handle));
    // A parent made since, turned, scaled, and far off; the entity kept where it is under it.
    const std::uint64_t parent = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_position(parent, 0.0F, 0.0F, -10.0F, FBE_SPACE_WORLD);
    fbe_entity_set_rotation(parent, 0.0F, 0.70710678F, 0.0F, 0.70710678F, FBE_SPACE_WORLD);
    fbe_entity_set_scale(parent, 2.0F, 2.0F, 2.0F, FBE_SPACE_LOCAL);
    REQUIRE(fbe_entity_set_parent(moved, parent, true));
    fbe_mat4_t m{};
    REQUIRE(fbe_entity_tweened_matrix(moved, 0.0F, &m));
    CHECK((near(m.m[12], 5.0F) && near(m.m[13], 0.0F) && near(m.m[14], 0.0F)));
    CHECK((near(m.m[0], 1.0F) && near(m.m[5], 1.0F) && near(m.m[10], 1.0F)));
    // Moved under one in another World, still from where it was.
    const test_world_t other;
    const std::uint64_t there = fbe_entity_create(other.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_position(there, 3.0F, 3.0F, 3.0F, FBE_SPACE_WORLD);
    REQUIRE(fbe_entity_set_parent(moved, there, false));
    REQUIRE(fbe_entity_tweened_matrix(moved, 0.0F, &m));
    CHECK((near(m.m[12], 5.0F) && near(m.m[13], 0.0F) && near(m.m[14], 0.0F)));
    // And at 1, where it is.
    fbe_mat4_t now{};
    fbe_entity_matrix(moved, FBE_SPACE_WORLD, &now);
    REQUIRE(fbe_entity_tweened_matrix(moved, 1.0F, &m));
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(near(m.m[i], now.m[i]));
    }
}

TEST_CASE("a fixed sprite's bounds hold its corners under a shearing parent", "[scene]") {
    const test_world_t world;
    const std::uint64_t parent = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_scale(parent, 3.0F, 1.0F, 1.0F, FBE_SPACE_LOCAL);
    const std::uint64_t sprite = fbe_entity_create(world.handle, parent, FBE_ENTITY_SPRITE);
    fbe_sprite_set_view_mode(sprite, FBE_SPRITE_FIXED);
    fbe_entity_set_rotation(sprite, 0.0F, 0.0F, 0.38268343F, 0.92387953F, FBE_SPACE_LOCAL);
    fbe_mat4_t m{};
    REQUIRE(fbe_sprite_matrix(sprite, sprite, &m));
    fbe_aabb_t bounds{};
    REQUIRE(fbe_entity_bounds(sprite, &bounds));
    for (const float x : { -1.0F, 1.0F }) {
        for (const float y : { -1.0F, 1.0F }) {
            const fbe_vec3_t p = corner(m, x, y);
            CHECK(p.x >= bounds.min.x - 0.0001F);
            CHECK(p.x <= bounds.max.x + 0.0001F);
            CHECK(p.y >= bounds.min.y - 0.0001F);
            CHECK(p.y <= bounds.max.y + 0.0001F);
        }
    }
}

TEST_CASE("a billboard seen by a camera under a mirror isn't mirrored", "[scene]") {
    const test_world_t world;
    const std::uint64_t mirror = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    fbe_entity_set_scale(mirror, 1.0F, 1.0F, -1.0F, FBE_SPACE_LOCAL);
    const std::uint64_t camera = fbe_entity_create(world.handle, mirror, FBE_ENTITY_CAMERA);
    const std::uint64_t sprite = fbe_entity_create(world.handle, 0, FBE_ENTITY_SPRITE);
    fbe_entity_set_position(sprite, 0.0F, 0.0F, -5.0F, FBE_SPACE_WORLD);
    for (const std::int32_t mode : {
             FBE_SPRITE_BILLBOARD,
             FBE_SPRITE_UPRIGHT,
             FBE_SPRITE_UPRIGHT_YAW,
         }) {
        fbe_sprite_set_view_mode(sprite, mode);
        fbe_mat4_t m{};
        REQUIRE(fbe_sprite_matrix(sprite, camera, &m));
        // It faces the camera, at +Z from it, and its axes keep their hand: X cross Y is along Z.
        const std::array<float, 3> x{ m.m[0], m.m[1], m.m[2] };
        const std::array<float, 3> y{ m.m[4], m.m[5], m.m[6] };
        const std::array<float, 3> z{ m.m[8], m.m[9], m.m[10] };
        const float hand = (((x[1] * y[2]) - (x[2] * y[1])) * z[0]) +
                           (((x[2] * y[0]) - (x[0] * y[2])) * z[1]) +
                           (((x[0] * y[1]) - (x[1] * y[0])) * z[2]);
        CHECK(z[2] > 0.0F);
        CHECK(hand > 0.0F);
    }
}
