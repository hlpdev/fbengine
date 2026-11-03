// take_contacts, fed by hand what Jolt's threads say of contacts in a collision step: pairs begin
// when their first parts touch and end when their last stop, come out in their pairs' order
// however they arrived, and keep touching while their bodies sleep, until they wake and don't.

#include "fbengine_physics.h"
#include "physics/physics.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

using namespace fbe::physics;

namespace {

constexpr fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };

// A world of a static floor and three dynamic balls, which never steps: what touches what is only
// what the test says.
struct scene_t {
    std::uint64_t handle = 0;
    world_t* world = nullptr;
    std::uint64_t ball_shape = 0;
    std::uint64_t floor = 0;
    std::array<std::uint64_t, 3> balls{};

    scene_t() {
        handle = fbe_physics_new(16);
        world = find_world(handle);
        ball_shape = fbe_shape_sphere(0.5F);
        const fbe_body_vec3_t at{ .x = 0.0F, .y = 0.0F, .z = 0.0F };
        floor = fbe_body_add(handle, ball_shape, FBE_BODY_STATIC, &at, &unturned);
        for (std::uint64_t& ball : balls) {
            ball = fbe_body_add(handle, ball_shape, FBE_BODY_DYNAMIC, &at, &unturned);
        }
    }

    scene_t(const scene_t&) = delete;
    scene_t& operator=(const scene_t&) = delete;
    scene_t(scene_t&&) = delete;
    scene_t& operator=(scene_t&&) = delete;

    ~scene_t() {
        fbe_physics_free(handle);
        fbe_shape_free(ball_shape);
    }

    [[nodiscard]] static JPH::Body& body(std::uint64_t of) {
        world_t* in = nullptr;
        return *find_body(of, in);
    }

    // Jolt's thread says `a`'s part `a_part` began touching `b`'s `b_part`, at height `y`.
    void add(std::uint64_t a, std::uint64_t b, std::uint32_t a_part, std::uint32_t b_part,
             float y = 0.0F) const {
        JPH::ContactManifold manifold;
        manifold.mBaseOffset = JPH::RVec3(0.0F, y, 0.0F);
        manifold.mWorldSpaceNormal = JPH::Vec3::sAxisY();
        manifold.mRelativeContactPointsOn1.push_back(JPH::Vec3::sZero());
        manifold.mSubShapeID1.SetValue(a_part);
        manifold.mSubShapeID2.SetValue(b_part);
        JPH::ContactSettings settings;
        world->contact_listener.OnContactAdded(body(a), body(b), manifold, settings);
    }

    // Jolt's thread says they stopped.
    void remove(std::uint64_t a, std::uint64_t b, std::uint32_t a_part,
                std::uint32_t b_part) const {
        JPH::SubShapeID first;
        JPH::SubShapeID second;
        first.SetValue(a_part);
        second.SetValue(b_part);
        world->contact_listener.OnContactRemoved(
            JPH::SubShapeIDPair(body(a).GetID(), first, body(b).GetID(), second));
    }

    // The collision step ends.
    void take() const {
        world->began.clear();
        world->ended.clear();
        take_contacts(*world);
        before_contacts(*world);
    }
};

} // namespace

TEST_CASE("a pair begins as its first part touches and ends as its last stops") {
    const scene_t scene;
    scene.add(scene.floor, scene.balls[0], 1, 2);
    scene.take();
    REQUIRE(scene.world->began.size() == 1);
    CHECK(scene.world->touching.size() == 1);
    scene.add(scene.floor, scene.balls[0], 1, 3);
    scene.take();
    // A second part of a pair already touching begins nothing.
    CHECK(scene.world->began.empty());
    scene.remove(scene.floor, scene.balls[0], 1, 2);
    scene.take();
    CHECK(scene.world->ended.empty());
    CHECK(scene.world->touching.size() == 1);
    scene.remove(scene.floor, scene.balls[0], 1, 3);
    scene.take();
    REQUIRE(scene.world->ended.size() == 2);
    CHECK(scene.world->touching.empty());
}

TEST_CASE("a pair that begins in one collision step and ends in the next says both") {
    const scene_t scene;
    scene.add(scene.balls[0], scene.balls[1], 0, 0);
    scene.take();
    const std::size_t began = scene.world->began.size();
    scene.remove(scene.balls[0], scene.balls[1], 0, 0);
    scene.take();
    CHECK(began == 1);
    CHECK(scene.world->ended.size() == 2);
    CHECK(scene.world->touching.empty());
}

TEST_CASE("pairs come out in their order however their events arrived") {
    const std::array<std::array<int, 3>, 2> orders{ { { 0, 1, 2 }, { 2, 0, 1 } } };
    std::vector<std::vector<float>> heights;
    for (const std::array<int, 3>& order : orders) {
        const scene_t scene;
        for (const int i : order) {
            scene.add(scene.floor, scene.balls.at(static_cast<std::size_t>(i)), 0, 0,
                      static_cast<float>(i));
        }
        scene.take();
        std::vector<float>& said = heights.emplace_back();
        for (const fbe_physics_contact_t& contact : scene.world->began) {
            said.push_back(contact.position.y);
        }
    }
    REQUIRE(heights[0].size() == 3);
    CHECK(heights[0] == heights[1]);
}

TEST_CASE("a pair whose bodies fall asleep still touches, until one wakes and doesn't") {
    const scene_t scene;
    scene.add(scene.floor, scene.balls[0], 0, 0);
    scene.add(scene.floor, scene.balls[1], 0, 0);
    scene.take();
    // Jolt forgets what bodies touch as they sleep.
    fbe_body_sleep(scene.balls[0]);
    fbe_body_sleep(scene.balls[1]);
    scene.remove(scene.floor, scene.balls[0], 0, 0);
    scene.remove(scene.floor, scene.balls[1], 0, 0);
    scene.take();
    CHECK(scene.world->ended.empty());
    CHECK(scene.world->touching.size() == 2);
    // Woken, one finds its floor again; the other doesn't, so it's gone.
    fbe_body_wake(scene.balls[0]);
    fbe_body_wake(scene.balls[1]);
    scene.take();
    scene.add(scene.floor, scene.balls[0], 0, 0);
    scene.take();
    REQUIRE(scene.world->ended.size() == 2);
    CHECK(scene.world->ended[1] == scene.balls[1]);
    CHECK(scene.world->began.empty());
    CHECK(scene.world->touching.size() == 1);
}

TEST_CASE("a remove of a pair that never began ends nothing") {
    const scene_t scene;
    scene.remove(scene.floor, scene.balls[2], 0, 0);
    scene.take();
    CHECK(scene.world->ended.empty());
    CHECK(scene.world->touching.empty());
}
