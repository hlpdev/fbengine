// fbengine_physics' determinism: a world given the same calls steps the same, bit for bit, again,
// on any number of threads, and on every target, compiler, and build, which a hash of where
// everything is checked in here says; and a world restored from a state steps on exactly as it
// did from there.

#include "fbengine_physics.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <format>
#include <map>
#include <string>
#include <vector>

namespace {

// What every target's step of the scene comes to; a change to it is a change to how worlds step.
constexpr std::uint64_t golden = 0xC8E32DE2056558FFULL;

constexpr fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };
constexpr int steps = 300;

// FNV-1a, over the bits of what's hashed.
class hash_t {
public:
    void add(std::uint64_t value) {
        for (unsigned int i = 0; i < 8; ++i) {
            m_hash = (m_hash ^ ((value >> (i * 8U)) & 0xFFU)) * 0x100000001B3ULL;
        }
    }

    void add(float value) {
        add(std::uint64_t{ std::bit_cast<std::uint32_t>(value) });
    }

    void add(const fbe_body_vec3_t& v) {
        add(v.x);
        add(v.y);
        add(v.z);
    }

    [[nodiscard]] std::uint64_t value() const {
        return m_hash;
    }

private:
    std::uint64_t m_hash = 0xCBF29CE484222325ULL;
};

fbe_body_vec3_t at(float x, float y, float z) {
    return { .x = x, .y = y, .z = z };
}

// A scene of every kind of shape, body, joint, sensor, and a character, which it steps, hashing
// what it's like at each step's end: its bodies by the order they were made, not their handles,
// which another scene before it changes.
struct scene_t {
    std::uint64_t world = 0;
    std::vector<std::uint64_t> shapes;
    std::vector<std::uint64_t> bodies;
    std::map<std::uint64_t, std::uint64_t> order;
    std::uint64_t mover = 0;
    std::uint64_t character = 0;

    scene_t() {
        world = fbe_physics_new(4096);
        const fbe_shape_vec3_t floor_size{ .x = 40.0F, .y = 1.0F, .z = 40.0F };
        const fbe_shape_vec3_t crate_size{ .x = 1.0F, .y = 1.0F, .z = 1.0F };
        const fbe_shape_vec3_t bar_size{ .x = 0.2F, .y = 1.0F, .z = 0.2F };
        const std::uint64_t floor = shape(fbe_shape_box(&floor_size));
        const std::uint64_t crate = shape(fbe_shape_box(&crate_size));
        const std::uint64_t bar = shape(fbe_shape_box(&bar_size));
        const std::uint64_t ball = shape(fbe_shape_sphere(0.4F));
        const std::uint64_t capsule = shape(fbe_shape_capsule(1.2F, 0.3F));
        const std::uint64_t cylinder = shape(fbe_shape_cylinder(0.8F, 0.4F));
        const std::array<fbe_shape_vec3_t, 5> rock{
            {
                { .x = 0.0F, .y = 0.0F, .z = 0.0F },
                { .x = 0.8F, .y = 0.1F, .z = 0.0F },
                { .x = 0.2F, .y = 0.7F, .z = 0.1F },
                { .x = 0.3F, .y = 0.2F, .z = 0.9F },
                { .x = 0.7F, .y = 0.6F, .z = 0.7F },
            },
        };
        const std::uint64_t hull = shape(fbe_shape_convex_hull(rock.data(), 5));
        // A ramp: a mesh of two triangles, tilted toward -X.
        const std::array<fbe_shape_vec3_t, 4> ramp{
            {
                { .x = 6.0F, .y = 0.0F, .z = -3.0F },
                { .x = 12.0F, .y = 3.0F, .z = -3.0F },
                { .x = 12.0F, .y = 3.0F, .z = 3.0F },
                { .x = 6.0F, .y = 0.0F, .z = 3.0F },
            },
        };
        const std::array<std::uint32_t, 6> ramp_triangles{ 0, 2, 1, 0, 3, 2 };
        const std::uint64_t slope = shape(fbe_shape_mesh(ramp.data(), 4, ramp_triangles.data(), 6));

        body(floor, FBE_BODY_STATIC, at(0.0F, -0.5F, 0.0F));
        body(slope, FBE_BODY_STATIC, at(0.0F, 0.0F, 0.0F));
        // A leaning stack of crates, which topples.
        for (int i = 0; i < 48; ++i) {
            const auto column = static_cast<float>(i % 4);
            const auto row = static_cast<float>((i / 4) % 4);
            const int level = i / 16;
            const auto up = static_cast<float>(level);
            body(crate, FBE_BODY_DYNAMIC,
                 at((column * 1.02F) + (up * 0.3F), 0.5F + (up * 1.01F), (row * 1.02F) - 6.0F));
        }
        // Balls, capsules, cylinders, and rocks raining onto it and the ramp.
        for (int i = 0; i < 40; ++i) {
            const std::array<std::uint64_t, 4> kinds{ ball, capsule, cylinder, hull };
            const int level = i / 8;
            body(kinds[static_cast<std::size_t>(i % 4)], FBE_BODY_DYNAMIC,
                 at((static_cast<float>(i % 8) * 1.3F) - 2.0F, 6.0F + static_cast<float>(level),
                    (static_cast<float>(i % 5) * 1.1F) - 6.0F));
        }
        // A chain of bars hung from the world on a hinge, each on a point under the last.
        std::uint64_t last = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint64_t link =
                body(bar, FBE_BODY_DYNAMIC, at(-8.0F, 7.5F - static_cast<float>(i), 0.0F));
            const fbe_joint_vec3_t top{ .x = -8.0F, .y = 8.0F - static_cast<float>(i), .z = 0.0F };
            const fbe_joint_vec3_t axis{ .x = 0.0F, .y = 0.0F, .z = 1.0F };
            if (last == 0) {
                fbe_joint_hinge(link, 0, &top, &axis);
            } else {
                fbe_joint_point(link, last, &top);
            }
            last = link;
        }
        fbe_body_add_impulse(last, &kick);
        // A ball on a rope, and a crate on a slider its motor drives.
        const std::uint64_t swinging = body(ball, FBE_BODY_DYNAMIC, at(-4.0F, 6.0F, 3.0F));
        const fbe_joint_vec3_t on_ball{ .x = -4.0F, .y = 6.0F, .z = 3.0F };
        const fbe_joint_vec3_t hook{ .x = -5.5F, .y = 7.0F, .z = 3.0F };
        fbe_joint_set_limits(fbe_joint_distance(swinging, 0, &on_ball, &hook), 0.0F, 2.0F);
        const std::uint64_t sliding = body(crate, FBE_BODY_DYNAMIC, at(-6.0F, 0.5F, 8.0F));
        const fbe_joint_vec3_t along{ .x = 1.0F, .y = 0.0F, .z = 0.0F };
        const std::uint64_t slider = fbe_joint_slider(sliding, 0, &along);
        fbe_joint_set_motor(slider, FBE_JOINT_MOTOR_SPEED, 1.0F, 500.0F);
        // A kinematic platform going round, and a sensor.
        mover = body(crate, FBE_BODY_KINEMATIC, at(4.0F, 0.5F, 8.0F));
        const std::uint64_t zone = body(crate, FBE_BODY_STATIC, at(0.0F, 0.5F, 0.0F));
        fbe_body_set_sensor(zone, true);
        const fbe_character_vec3_t start{ .x = 0.0F, .y = 0.9F, .z = 6.0F };
        character = fbe_character_new(world, capsule, &start);
        add_order(fbe_character_body(character));
    }

    scene_t(const scene_t&) = delete;
    scene_t& operator=(const scene_t&) = delete;
    scene_t(scene_t&&) = delete;
    scene_t& operator=(scene_t&&) = delete;

    ~scene_t() {
        fbe_physics_free(world);
        for (const std::uint64_t shape : shapes) {
            fbe_shape_free(shape);
        }
    }

    std::uint64_t shape(std::uint64_t made) {
        shapes.push_back(made);
        return made;
    }

    std::uint64_t body(std::uint64_t shape, std::int32_t motion, const fbe_body_vec3_t& where) {
        const std::uint64_t made = fbe_body_add(world, shape, motion, &where, &unturned);
        bodies.push_back(made);
        add_order(made);
        return made;
    }

    void add_order(std::uint64_t handle) {
        order[handle] = order.size();
    }

    // Steps it from step `from` to step `to`, each step a 15th of a second in `collision_steps`
    // collision steps, or with 0, mostly a 60th, giving the hash of each step's end.
    [[nodiscard]] std::uint64_t run(int from, int to, std::int32_t collision_steps = 0) const {
        hash_t hash;
        for (int step = from; step < to; ++step) {
            const float turn = static_cast<float>(step) * 0.05F;
            const fbe_body_vec3_t round{ .x = turn, .y = 0.0F, .z = 0.0F };
            fbe_body_set_velocity(mover, &round);
            const fbe_character_vec3_t walk{ .x = 1.5F, .y = -2.0F, .z = -0.5F };
            fbe_character_move(character, &walk, 1.0F / 60.0F);
            // A step in several collision steps now and then, a long one in many.
            if (collision_steps > 0) {
                fbe_physics_step(world, 1.0F / 15.0F, collision_steps);
            } else if (step % 50 == 25) {
                fbe_physics_step(world, 0.2F, 12);
            } else {
                fbe_physics_step(world, 1.0F / 60.0F, step % 10 == 0 ? 2 : 1);
            }
            add_state(hash);
            add_contacts(hash);
        }
        return hash.value();
    }

    void add_state(hash_t& hash) const {
        for (const std::uint64_t body : bodies) {
            fbe_body_vec3_t v{};
            fbe_body_quat_t q{};
            fbe_body_position(body, &v);
            hash.add(v);
            fbe_body_rotation(body, &q);
            hash.add(q.x);
            hash.add(q.y);
            hash.add(q.z);
            hash.add(q.w);
            fbe_body_velocity(body, &v);
            hash.add(v);
            fbe_body_angular_velocity(body, &v);
            hash.add(v);
            hash.add(std::uint64_t{ fbe_body_active(body) });
        }
        fbe_character_vec3_t c{};
        fbe_character_position(character, &c);
        hash.add(c.x);
        hash.add(c.y);
        hash.add(c.z);
        hash.add(static_cast<std::uint64_t>(fbe_character_ground(character)));
    }

    void add_contacts(hash_t& hash) const {
        std::array<fbe_physics_contact_t, 256> began{};
        const std::int32_t count = fbe_physics_began(world, began.data(), 256);
        hash.add(static_cast<std::uint64_t>(count));
        for (int i = 0; i < std::min(count, 256); ++i) {
            const fbe_physics_contact_t& contact = began.at(static_cast<std::size_t>(i));
            hash.add(order.at(contact.a));
            hash.add(order.at(contact.b));
            hash.add(contact.position.x);
            hash.add(contact.position.y);
            hash.add(contact.position.z);
            hash.add(contact.speed);
        }
        std::array<std::uint64_t, 512> ended{};
        const std::int32_t pairs = fbe_physics_ended(world, ended.data(), 256);
        hash.add(static_cast<std::uint64_t>(pairs));
        for (int i = 0; i < std::min(pairs, 256) * 2; ++i) {
            hash.add(order.at(ended.at(static_cast<std::size_t>(i))));
        }
    }

    static constexpr fbe_body_vec3_t kick{ .x = 30.0F, .y = 0.0F, .z = 0.0F };
};

std::uint64_t stepped(std::int32_t threads, std::int32_t collision_steps = 0) {
    fbe_physics_set_threads(threads);
    const scene_t scene;
    return scene.run(0, collision_steps > 0 ? steps / 4 : steps, collision_steps);
}

} // namespace

TEST_CASE("a world given the same calls steps the same, bit for bit") {
    CHECK(stepped(3) == stepped(3));
}

TEST_CASE("a world steps the same on any number of threads") {
    const std::uint64_t alone = stepped(0);
    CHECK(stepped(1) == alone);
    CHECK(stepped(7) == alone);
    CHECK(stepped(31) == alone);
}

// What began and ended in each of a step's collision steps comes out in the same order, with the
// same positions and speeds, however Jolt's threads ran.
TEST_CASE("contacts are the same on any number of threads, in steps of many collision steps") {
    const std::uint64_t alone = stepped(0, 4);
    CHECK(stepped(15, 4) == alone);
    CHECK(stepped(15, 4) == alone);
    CHECK(stepped(31, 4) == alone);
}

TEST_CASE("a world steps the same on every target, compiler, and build") {
    const std::uint64_t hash = stepped(3);
    std::fputs(std::format("the scene's hash is 0x{:016X}\n", hash).c_str(), stdout);
    CHECK(hash == golden);
}

TEST_CASE("a world restored from a state steps on exactly as it did") {
    fbe_physics_set_threads(3);
    const scene_t scene;
    static_cast<void>(scene.run(0, steps / 2));
    const std::int64_t size = fbe_physics_save(scene.world, 0.25, nullptr, 0);
    REQUIRE(size > 0);
    std::vector<std::uint8_t> state(static_cast<std::size_t>(size));
    REQUIRE(fbe_physics_save(scene.world, 0.25, state.data(), size) == size);
    const std::uint64_t first = scene.run(steps / 2, steps);
    double carried = 0.0;
    REQUIRE(fbe_physics_restore(scene.world, state.data(), size, &carried));
    CHECK(carried == 0.25);
    CHECK(fbe_physics_steps(scene.world) == (steps / 2));
    CHECK(scene.run(steps / 2, steps) == first);
}

TEST_CASE("a state restores only into the world it's of, as it was") {
    fbe_physics_set_threads(3);
    const scene_t scene;
    static_cast<void>(scene.run(0, 30));
    std::vector<std::uint8_t> state(
        static_cast<std::size_t>(fbe_physics_save(scene.world, 0.0, nullptr, 0)));
    fbe_physics_save(scene.world, 0.0, state.data(), static_cast<std::int64_t>(state.size()));
    const auto size = static_cast<std::int64_t>(state.size());
    double carried = 0.0;
    // Cut short, it leaves the world as it was.
    static_cast<void>(scene.run(30, 40));
    fbe_body_vec3_t before{};
    fbe_body_position(scene.bodies.at(10), &before);
    CHECK_FALSE(fbe_physics_restore(scene.world, state.data(), size - 16, &carried));
    CHECK(std::string(fbe_physics_error()) == "the state ends too soon");
    fbe_body_vec3_t after{};
    fbe_body_position(scene.bodies.at(10), &after);
    CHECK(std::bit_cast<std::uint32_t>(before.y) == std::bit_cast<std::uint32_t>(after.y));
    CHECK(fbe_physics_steps(scene.world) == 40);
    // Bytes that aren't a state, and a world with another body since.
    std::vector<std::uint8_t> nothing(64, 7);
    CHECK_FALSE(fbe_physics_restore(scene.world, nothing.data(), 64, &carried));
    CHECK(std::string(fbe_physics_error()) == "that isn't a world's state");
    const fbe_body_vec3_t somewhere = at(20.0F, 5.0F, 20.0F);
    fbe_body_add(scene.world, scene.shapes.at(1), FBE_BODY_DYNAMIC, &somewhere, &unturned);
    CHECK_FALSE(fbe_physics_restore(scene.world, state.data(), size, &carried));
    CHECK(std::string(fbe_physics_error()) ==
          "the state is of other bodies, joints, or characters than the world has");
    CHECK(fbe_physics_steps(scene.world) == 40);
}
