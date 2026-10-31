// Skins through the C API: joints bound to entities by name, the palette a mesh is drawn with in
// its own space, what's refused, and living while a mesh is skinned.

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

std::uint64_t entity(std::uint64_t world, std::uint64_t parent, std::int32_t kind,
                     const char* name) {
    const std::uint64_t made = fbe_entity_create(world, parent, kind);
    fbe_entity_set_name(made, name);
    return made;
}

/// The matrix that moves by (x, y, z), column by column.
std::array<float, 16> moving(float x, float y, float z) {
    return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1 };
}

/// Where `palette`'s joint `joint` takes the point (x, y, z).
std::array<float, 3> through(const std::array<float, 32>& palette, int joint, float x, float y,
                             float z) {
    const float* m = &palette.at(static_cast<std::size_t>(joint) * 16);
    return {
        (m[0] * x) + (m[4] * y) + (m[8] * z) + m[12],
        (m[1] * x) + (m[5] * y) + (m[9] * z) + m[13],
        (m[2] * x) + (m[6] * y) + (m[10] * z) + m[14],
    };
}

/// A leg: a body with a mesh and hips at (0, 1, 0), a knee half a unit under the hips, and a
/// skin binding them where they are, so its palette starts as identities.
struct leg_t {
    std::uint64_t world = fbe_world_create();
    std::uint64_t body = entity(world, 0, FBE_ENTITY_PIVOT, "body");
    std::uint64_t mesh = entity(world, body, FBE_ENTITY_MESH, "skin");
    std::uint64_t hips = entity(world, body, FBE_ENTITY_PIVOT, "hips");
    std::uint64_t knee = entity(world, hips, FBE_ENTITY_PIVOT, "knee");
    std::uint64_t skin = fbe_skin_create();

    leg_t() {
        fbe_entity_set_position(hips, 0.0F, 1.0F, 0.0F, FBE_SPACE_LOCAL);
        fbe_entity_set_position(knee, 0.0F, -0.5F, 0.0F, FBE_SPACE_LOCAL);
        fbe_skin_add_joint(skin, "hips", moving(0.0F, -1.0F, 0.0F).data());
        fbe_skin_add_joint(skin, "knee", moving(0.0F, -0.5F, 0.0F).data());
    }

    leg_t(const leg_t&) = delete;
    leg_t& operator=(const leg_t&) = delete;
    leg_t(leg_t&&) = delete;
    leg_t& operator=(leg_t&&) = delete;

    ~leg_t() {
        fbe_world_free(world);
        fbe_skin_release(skin);
    }
};

} // namespace

TEST_CASE("a skinned mesh's palette is each joint's world matrix times its inverse bind") {
    const leg_t leg;
    CHECK(fbe_skin_joint_count(leg.skin) == 2);
    CHECK(std::string_view(fbe_skin_joint_name(leg.skin, 1)) == "knee");
    REQUIRE(fbe_skin_attach(leg.mesh, leg.skin, leg.body));
    CHECK(fbe_skin_of(leg.mesh) == leg.skin);
    std::array<float, 32> palette{};
    REQUIRE(fbe_skin_palette(leg.mesh, palette.data(), 2) == 2);
    // Where they were bound, nothing moves.
    for (std::size_t i = 0; i < palette.size(); ++i) {
        CHECK(palette[i] == Catch::Approx(moving(0, 0, 0)[i % 16]).margin(1e-6));
    }
    // The hips turned a quarter round Z take the knee, and a vertex bound at it, to (0.5, 1, 0).
    const float half = std::sqrt(0.5F);
    fbe_entity_set_rotation(leg.hips, 0.0F, 0.0F, half, half, FBE_SPACE_LOCAL);
    fbe_skin_palette(leg.mesh, palette.data(), 2);
    const std::array<float, 3> at_knee = through(palette, 1, 0.0F, 0.5F, 0.0F);
    CHECK(at_knee[0] == Catch::Approx(0.5F));
    CHECK(at_knee[1] == Catch::Approx(1.0F));
    // A vertex at the hips stays there.
    const std::array<float, 3> at_hips = through(palette, 0, 0.0F, 1.0F, 0.0F);
    CHECK(at_hips[0] == Catch::Approx(0.0F).margin(1e-6));
    CHECK(at_hips[1] == Catch::Approx(1.0F));
    // The body moving the mesh with the bones changes nothing in the mesh's space.
    fbe_entity_set_position(leg.body, 5.0F, 0.0F, 3.0F, FBE_SPACE_LOCAL);
    std::array<float, 32> moved{};
    fbe_skin_palette(leg.mesh, moved.data(), 2);
    for (std::size_t i = 0; i < palette.size(); ++i) {
        CHECK(moved[i] == Catch::Approx(palette[i]).margin(1e-5));
    }
}

TEST_CASE("a joint no entity has, or one freed, is where it was bound") {
    const leg_t leg;
    const std::uint64_t other = fbe_skin_create();
    fbe_skin_add_joint(other, "hips", moving(0.0F, -1.0F, 0.0F).data());
    fbe_skin_add_joint(other, "tail", moving(0.0F, 0.0F, 0.0F).data());
    REQUIRE(fbe_skin_attach(leg.mesh, other, leg.body));
    fbe_entity_set_position(leg.hips, 0.0F, 3.0F, 0.0F, FBE_SPACE_LOCAL);
    std::array<float, 32> palette{};
    REQUIRE(fbe_skin_palette(leg.mesh, palette.data(), 2) == 2);
    CHECK(through(palette, 0, 0.0F, 1.0F, 0.0F)[1] == Catch::Approx(3.0F));
    CHECK(through(palette, 1, 0.0F, 1.0F, 0.0F)[1] == 1.0F);
    fbe_entity_free(leg.hips);
    fbe_skin_palette(leg.mesh, palette.data(), 2);
    CHECK(through(palette, 0, 0.0F, 1.0F, 0.0F)[1] == 1.0F);
    // A joint added since is bound at the next palette.
    const std::uint64_t knee = entity(leg.world, leg.body, FBE_ENTITY_PIVOT, "knee2");
    fbe_entity_set_position(knee, 0.0F, 7.0F, 0.0F, FBE_SPACE_LOCAL);
    fbe_skin_add_joint(other, "knee2", moving(0.0F, 0.0F, 0.0F).data());
    std::array<float, 48> three{};
    REQUIRE(fbe_skin_palette(leg.mesh, three.data(), 3) == 3);
    CHECK(three[32 + 13] == Catch::Approx(7.0F));
    fbe_skin_release(other);
}

TEST_CASE("what a skin can't take is refused, saying why") {
    const leg_t leg;
    CHECK(!fbe_skin_add_joint(leg.skin, "", moving(0, 0, 0).data()));
    CHECK(says("a joint has a name"));
    CHECK(!fbe_skin_add_joint(leg.skin, "hips", moving(0, 0, 0).data()));
    CHECK(says("a joint \"hips\" already"));
    std::array<float, 16> broken = moving(0, 0, 0);
    broken[5] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!fbe_skin_add_joint(leg.skin, "toe", broken.data()));
    CHECK(says("matrix is numbers"));
    CHECK(fbe_skin_joint_count(leg.skin) == 2);
    CHECK(std::string_view(fbe_skin_joint_name(leg.skin, 2)).empty());
    // Only a mesh is skinned.
    CHECK(!fbe_skin_attach(leg.hips, leg.skin, leg.body));
    CHECK(says("the entity isn't a mesh"));
    CHECK(fbe_skin_of(leg.hips) == 0);
    std::array<float, 32> palette{};
    CHECK(fbe_skin_palette(leg.mesh, palette.data(), 2) == -1);
    CHECK(says("the mesh isn't skinned"));
    fbe_skin_attach(leg.mesh, leg.skin, leg.body);
    CHECK(fbe_skin_palette(leg.mesh, palette.data(), 1) == -1);
    CHECK(says("the mesh's palette is 2 joints"));
    CHECK(!fbe_skin_attach(leg.mesh, leg.skin, leg.world));
    CHECK(!fbe_skin_attach(leg.mesh, leg.world, leg.body));
}

TEST_CASE("a skin lives while a mesh is skinned by it, until it's taken off or the mesh freed") {
    const leg_t leg;
    const std::uint64_t skin = fbe_skin_create();
    fbe_skin_add_joint(skin, "hips", moving(0, 0, 0).data());
    REQUIRE(fbe_skin_attach(leg.mesh, skin, leg.body));
    fbe_skin_release(skin);
    CHECK(fbe_skin_alive(skin));
    // Skinned again by it, it isn't let go of first.
    REQUIRE(fbe_skin_attach(leg.mesh, skin, leg.body));
    CHECK(fbe_skin_alive(skin));
    REQUIRE(fbe_skin_attach(leg.mesh, 0, 0));
    CHECK(!fbe_skin_alive(skin));
    CHECK(fbe_skin_of(leg.mesh) == 0);
    CHECK(fbe_skin_attach(leg.mesh, 0, 0));
    // A mesh freed lets go of its skin at the next skin made.
    const std::uint64_t kept = fbe_skin_create();
    fbe_skin_attach(leg.mesh, kept, leg.body);
    fbe_skin_release(kept);
    fbe_entity_free(leg.mesh);
    CHECK(fbe_skin_alive(kept));
    const std::uint64_t next = fbe_skin_create();
    CHECK(!fbe_skin_alive(kept));
    CHECK(!fbe_skin_release(kept));
    CHECK(says("the skin was freed"));
    fbe_skin_release(next);
}
