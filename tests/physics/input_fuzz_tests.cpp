// fbengine_physics given numbers at the ends of what a float holds, as a game's bug or a file made
// to break it might: each shape, body, joint, and character call is refused, or what it makes
// steps on with every number finite; none crashes or hangs.

#include "fbengine_physics.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr fbe_body_quat_t unturned{ .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F };

// The ends of a float, either way: the least above 0, denormal, as small as a float is normal,
// small, at and past each of fbengine_physics' limits, and as large as a float holds.
const std::array<float, 20> extremes{ std::numeric_limits<float>::denorm_min(),
                                      std::numeric_limits<float>::min(),
                                      1.0e-20F,
                                      1.0e-6F,
                                      1.0e5F,
                                      1.0e6F,
                                      1.0e9F,
                                      1.0e12F,
                                      1.0e15F,
                                      1.0e30F,
                                      std::numeric_limits<float>::max(),
                                      -std::numeric_limits<float>::denorm_min(),
                                      -1.0e-6F,
                                      -1.0e5F,
                                      -1.0e6F,
                                      -1.0e9F,
                                      -1.0e12F,
                                      -1.0e15F,
                                      -1.0e30F,
                                      -std::numeric_limits<float>::max() };

bool finite(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

// A world with a floor and a few boxes and balls, which what's tried is added to or done to.
struct scene_t {
    std::uint64_t world = fbe_physics_new(256);
    std::vector<std::uint64_t> bodies;
    std::vector<std::uint64_t> shapes;
    std::uint64_t character = 0;

    scene_t() {
        const fbe_shape_vec3_t floor_size{ .x = 50.0F, .y = 1.0F, .z = 50.0F };
        const fbe_shape_vec3_t box_size{ .x = 1.0F, .y = 1.0F, .z = 1.0F };
        shapes.push_back(fbe_shape_box(&floor_size));
        shapes.push_back(fbe_shape_box(&box_size));
        shapes.push_back(fbe_shape_sphere(0.5F));
        const fbe_body_vec3_t floor_at{ .x = 0.0F, .y = -0.5F, .z = 0.0F };
        add(shapes[0], FBE_BODY_STATIC, floor_at);
        for (std::size_t i = 0; i < 4; ++i) {
            const fbe_body_vec3_t where{ .x = static_cast<float>(i) * 2.0F, .y = 2.0F, .z = 0.0F };
            add(shapes[1 + (i % 2)], FBE_BODY_DYNAMIC, where);
        }
        const fbe_character_vec3_t standing{ .x = -5.0F, .y = 1.0F, .z = 0.0F };
        shapes.push_back(fbe_shape_capsule(1.8F, 0.3F));
        character = fbe_character_new(world, shapes.back(), &standing);
    }

    ~scene_t() {
        fbe_physics_free(world);
        for (const std::uint64_t shape : shapes) {
            fbe_shape_free(shape);
        }
    }

    scene_t(const scene_t&) = delete;
    scene_t& operator=(const scene_t&) = delete;
    scene_t(scene_t&&) = delete;
    scene_t& operator=(scene_t&&) = delete;

    std::uint64_t add(std::uint64_t shape, int32_t motion, const fbe_body_vec3_t& where) {
        const std::uint64_t body = fbe_body_add(world, shape, motion, &where, &unturned);
        if (body != 0) {
            bodies.push_back(body);
        }
        return body;
    }

    // Steps a while, casting through the world, and checks every body and the character stay
    // somewhere, turned, and going at finite numbers.
    void step_soundly(const std::string& what) {
        for (int i = 0; i < 30; ++i) {
            const fbe_character_vec3_t walking{ .x = 1.0F, .y = -1.0F, .z = 0.0F };
            fbe_character_move(character, &walking, 1.0F / 60);
            fbe_physics_step(world, 1.0F / 60, 1);
            const fbe_physics_vec3_t from{ .x = -10.0F, .y = 1.0F, .z = 0.0F };
            const fbe_physics_vec3_t along{ .x = 1.0F, .y = 0.0F, .z = 0.0F };
            fbe_physics_hit_t hit{};
            fbe_physics_cast_ray(world, &from, &along, 20.0F, 0xFFFFFFFFU, 0, false, &hit);
        }
        for (const std::uint64_t body : bodies) {
            fbe_body_vec3_t where{};
            fbe_body_vec3_t going{};
            fbe_body_quat_t turned{};
            fbe_body_position(body, &where);
            fbe_body_velocity(body, &going);
            fbe_body_rotation(body, &turned);
            INFO(what);
            CHECK(finite(where.x, where.y, where.z));
            CHECK(finite(going.x, going.y, going.z));
            CHECK(finite(turned.x, turned.y, turned.w));
        }
        fbe_character_vec3_t standing{};
        fbe_character_position(character, &standing);
        INFO(what);
        CHECK(finite(standing.x, standing.y, standing.z));
    }

    // Adds `shape`, if it's made, as a body that moves, or doesn't if it can't, among the others,
    // and steps on.
    void try_shape(std::uint64_t shape, const std::string& what) {
        if (shape == 0) {
            return;
        }
        shapes.push_back(shape);
        const fbe_body_vec3_t where{ .x = 0.0F, .y = 5.0F, .z = 3.0F };
        if (add(shape, FBE_BODY_DYNAMIC, where) == 0) {
            add(shape, FBE_BODY_STATIC, where);
        }
        step_soundly(what);
    }
};

} // namespace

TEST_CASE("shapes of the ends of a float are refused, or step on soundly", "[stability]") {
    for (const float a : extremes) {
        scene_t scene;
        const fbe_shape_vec3_t cube{ .x = a, .y = a, .z = a };
        scene.try_shape(fbe_shape_box(&cube), std::format("a box {} across", a));
        const fbe_shape_vec3_t plank{ .x = a, .y = 0.1F, .z = 1.0F };
        scene.try_shape(fbe_shape_box(&plank), std::format("a box {} by 0.1 by 1", a));
        scene.try_shape(fbe_shape_sphere(a), std::format("a sphere of radius {}", a));
        scene.try_shape(fbe_shape_capsule(a, a / 4), std::format("a capsule {} long", a));
        scene.try_shape(fbe_shape_capsule(a * 2, a), std::format("a capsule of radius {}", a));
        scene.try_shape(fbe_shape_cylinder(a, 0.5F), std::format("a cylinder {} long", a));
        scene.try_shape(fbe_shape_cylinder(1.0F, a), std::format("a cylinder of radius {}", a));
        const std::array<fbe_shape_vec3_t, 4> corners{ {
            { .x = 0.0F, .y = 0.0F, .z = 0.0F },
            { .x = a, .y = 0.0F, .z = 0.0F },
            { .x = 0.0F, .y = a, .z = 0.0F },
            { .x = 0.0F, .y = 0.0F, .z = a },
        } };
        scene.try_shape(fbe_shape_convex_hull(corners.data(), 4),
                        std::format("a convex hull {} across", a));
        const std::array<std::uint32_t, 12> faces{ 0, 1, 2, 0, 2, 3, 0, 3, 1, 1, 3, 2 };
        scene.try_shape(fbe_shape_mesh(corners.data(), 4, faces.data(), 12),
                        std::format("a mesh {} across", a));
        const std::array<fbe_shape_part_t, 2> parts{ {
            { .shape = scene.shapes[1],
              .position = { .x = a, .y = 0.0F, .z = 0.0F },
              .rotation = { .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F } },
            { .shape = scene.shapes[2],
              .position = { .x = 0.0F, .y = 0.0F, .z = 0.0F },
              .rotation = { .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F } },
        } };
        scene.try_shape(fbe_shape_compound(parts.data(), 2),
                        std::format("a compound with a part {} away", a));
    }
}

TEST_CASE("convex hulls and meshes of points all in one place, on a line, or in a plane are "
          "refused, or step on soundly",
          "[stability]") {
    scene_t scene;
    const std::array<fbe_shape_vec3_t, 4> same{ {
        { .x = 1.0F, .y = 1.0F, .z = 1.0F },
        { .x = 1.0F, .y = 1.0F, .z = 1.0F },
        { .x = 1.0F, .y = 1.0F, .z = 1.0F },
        { .x = 1.0F, .y = 1.0F, .z = 1.0F },
    } };
    const std::array<fbe_shape_vec3_t, 4> line{ {
        { .x = 0.0F, .y = 0.0F, .z = 0.0F },
        { .x = 1.0F, .y = 0.0F, .z = 0.0F },
        { .x = 2.0F, .y = 0.0F, .z = 0.0F },
        { .x = 3.0F, .y = 0.0F, .z = 0.0F },
    } };
    const std::array<fbe_shape_vec3_t, 4> plane{ {
        { .x = 0.0F, .y = 0.0F, .z = 0.0F },
        { .x = 1.0F, .y = 0.0F, .z = 0.0F },
        { .x = 0.0F, .y = 0.0F, .z = 1.0F },
        { .x = 1.0F, .y = 0.0F, .z = 1.0F },
    } };
    const std::array<std::uint32_t, 12> faces{ 0, 1, 2, 0, 2, 3, 0, 3, 1, 1, 3, 2 };
    const std::array<std::uint32_t, 6> twice{ 0, 0, 0, 1, 1, 1 };
    for (const auto* points : { &same, &line, &plane }) {
        scene.try_shape(fbe_shape_convex_hull(points->data(), 4), "a degenerate convex hull");
        scene.try_shape(fbe_shape_mesh(points->data(), 4, faces.data(), 12), "a degenerate mesh");
        scene.try_shape(fbe_shape_mesh(points->data(), 4, twice.data(), 6),
                        "a mesh of triangles of one corner");
    }
}

TEST_CASE("a convex hull of thousands of points, and a mesh of thousands of triangles, are made",
          "[stability]") {
    scene_t scene;
    std::vector<fbe_shape_vec3_t> points;
    std::uint32_t seed = 1;
    const auto next = [&seed] {
        seed = (seed * 1664525U) + 1013904223U;
        return (static_cast<float>(seed >> 8U) / static_cast<float>(1U << 24U)) - 0.5F;
    };
    for (int i = 0; i < 20000; ++i) {
        points.push_back({ .x = next(), .y = next(), .z = next() });
    }
    const std::uint64_t hull =
        fbe_shape_convex_hull(points.data(), static_cast<std::int32_t>(points.size()));
    CHECK(hull != 0);
    scene.try_shape(hull, "a convex hull of 20000 points");
    std::vector<std::uint32_t> triangles;
    for (std::uint32_t i = 0; i + 2 < points.size(); ++i) {
        triangles.insert(triangles.end(), { i, i + 1, i + 2 });
    }
    const std::uint64_t mesh =
        fbe_shape_mesh(points.data(), static_cast<std::int32_t>(points.size()), triangles.data(),
                       static_cast<std::int32_t>(triangles.size()));
    CHECK(mesh != 0);
    scene.try_shape(mesh, "a mesh of 20000 triangles");
}

TEST_CASE("compounds of compounds, deeper than a sub shape's id has bits for, are refused, or step "
          "on soundly",
          "[stability]") {
    scene_t scene;
    std::uint64_t shape = scene.shapes[1];
    for (int depth = 0; depth < 64; ++depth) {
        const std::array<fbe_shape_part_t, 2> parts{ {
            { .shape = shape,
              .position = { .x = 0.0F, .y = 0.0F, .z = 0.0F },
              .rotation = { .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F } },
            { .shape = scene.shapes[2],
              .position = { .x = 1.0F, .y = 0.0F, .z = 0.0F },
              .rotation = { .x = 0.0F, .y = 0.0F, .z = 0.0F, .w = 1.0F } },
        } };
        const std::uint64_t deeper = fbe_shape_compound(parts.data(), 2);
        if (deeper == 0) {
            break;
        }
        scene.shapes.push_back(deeper);
        shape = deeper;
    }
    scene.try_shape(shape, "a compound of compounds");
}

TEST_CASE("bodies given the ends of a float are refused, or step on soundly", "[stability]") {
    for (const float a : extremes) {
        const fbe_body_vec3_t v{ .x = a, .y = a, .z = a };
        const std::array<std::pair<const char*, std::function<void(std::uint64_t)>>, 13> calls{ {
            { "put", [&](std::uint64_t b) { fbe_body_set_position(b, &v); } },
            { "sped", [&](std::uint64_t b) { fbe_body_set_velocity(b, &v); } },
            { "spun", [&](std::uint64_t b) { fbe_body_set_angular_velocity(b, &v); } },
            { "pushed", [&](std::uint64_t b) { fbe_body_add_force(b, &v); } },
            { "pushed at", [&](std::uint64_t b) { fbe_body_add_force_at(b, &v, &v); } },
            { "twisted", [&](std::uint64_t b) { fbe_body_add_torque(b, &v); } },
            { "struck", [&](std::uint64_t b) { fbe_body_add_impulse(b, &v); } },
            { "struck at", [&](std::uint64_t b) { fbe_body_add_impulse_at(b, &v, &v); } },
            { "struck turning", [&](std::uint64_t b) { fbe_body_add_angular_impulse(b, &v); } },
            { "weighed", [&](std::uint64_t b) { fbe_body_set_mass(b, a); } },
            { "damped",
              [&](std::uint64_t b) {
                  fbe_body_set_linear_damping(b, a);
                  fbe_body_set_angular_damping(b, a);
              } },
            { "made heavier", [&](std::uint64_t b) { fbe_body_set_gravity_factor(b, a); } },
            { "rubbed",
              [&](std::uint64_t b) {
                  fbe_body_set_friction(b, a);
                  fbe_body_set_restitution(b, a);
              } },
        } };
        for (const auto& [what, call] : calls) {
            scene_t scene;
            for (std::size_t i = 1; i < scene.bodies.size(); ++i) {
                call(scene.bodies[i]);
            }
            scene.step_soundly(std::format("bodies {} by {}", what, a));
        }
        scene_t scene;
        const fbe_physics_vec3_t gravity{ .x = a, .y = a, .z = a };
        fbe_physics_set_gravity(scene.world, &gravity);
        scene.step_soundly(std::format("gravity of {}", a));
        scene_t moved;
        const fbe_body_vec3_t there{ .x = a, .y = a, .z = a };
        const std::uint64_t kinematic =
            moved.add(moved.shapes[1], FBE_BODY_KINEMATIC, { .x = 0.0F, .y = 1.0F, .z = 0.0F });
        fbe_body_move_to(kinematic, &there, &unturned, 1.0F / 60);
        fbe_body_teleport(moved.bodies[1], &there, &unturned);
        moved.step_soundly(std::format("bodies moved to {}", a));
    }
}

TEST_CASE("steps, characters, and joints given the ends of a float are refused, or step on "
          "soundly",
          "[stability]") {
    for (const float a : extremes) {
        scene_t stepped;
        fbe_physics_step(stepped.world, a, 1);
        stepped.step_soundly(std::format("a step of {} seconds", a));
        scene_t walked;
        const fbe_character_vec3_t v{ .x = a, .y = a, .z = a };
        fbe_character_move(walked.character, &v, 1.0F / 60);
        fbe_character_move(walked.character, &v, a);
        fbe_character_set_position(walked.character, &v);
        walked.step_soundly(std::format("a character moved by {}", a));
        scene_t joined;
        const fbe_joint_vec3_t point{ .x = a, .y = a, .z = a };
        const fbe_joint_vec3_t axis{ .x = a, .y = 0.0F, .z = 0.0F };
        const std::uint64_t first = joined.bodies[1];
        const std::uint64_t second = joined.bodies[2];
        for (const std::uint64_t joint :
             { fbe_joint_point(first, second, &point), fbe_joint_hinge(first, 0, &point, &axis),
               fbe_joint_slider(second, 0, &axis),
               fbe_joint_distance(first, second, &point, &point),
               fbe_joint_hinge(joined.bodies[3], 0, &point, &axis),
               fbe_joint_slider(joined.bodies[4], 0, &axis) }) {
            fbe_joint_set_limits(joint, -a, a);
            fbe_joint_set_spring(joint, a, a);
            fbe_joint_set_motor(joint, FBE_JOINT_MOTOR_SPEED, a, a);
        }
        joined.step_soundly(std::format("joints of {}", a));
    }
}

// Each of fbengine_physics' limits takes what's at it and refuses what's just past it, saying so.
TEST_CASE("each limit takes what's at it and refuses what's past it", "[stability]") {
    scene_t scene;
    const auto said = [] { return std::string(fbe_physics_error()); };
    // A shape's size, to 1e5.
    const fbe_shape_vec3_t largest{ .x = 1.0e5F, .y = 1.0F, .z = 1.0F };
    const fbe_shape_vec3_t larger{ .x = 1.01e5F, .y = 1.0F, .z = 1.0F };
    const std::uint64_t box = fbe_shape_box(&largest);
    CHECK(box != 0);
    fbe_shape_free(box);
    CHECK(fbe_shape_box(&larger) == 0);
    CHECK(said() == "a box's width is more than 0 and at most 1e5, not 101000");
    const std::array<fbe_shape_vec3_t, 3> far{ {
        { .x = 0.0F, .y = 0.0F, .z = 0.0F },
        { .x = 1.01e5F, .y = 0.0F, .z = 0.0F },
        { .x = 0.0F, .y = 1.0F, .z = 0.0F },
    } };
    CHECK(fbe_shape_convex_hull(far.data(), 3) == 0);
    CHECK(said() == "a convex hull's points are at most 1e5 from 0 each way");
    const std::array<std::uint32_t, 3> triangle{ 0, 1, 2 };
    CHECK(fbe_shape_mesh(far.data(), 3, triangle.data(), 3) == 0);
    CHECK(said() == "a mesh's vertices are at most 1e5 from 0 each way");
    // A position, to 1e9.
    const std::uint64_t ball = scene.bodies[2];
    const fbe_body_vec3_t furthest{ .x = 1.0e9F, .y = 0.0F, .z = 0.0F };
    const fbe_body_vec3_t further{ .x = 1.01e9F, .y = 0.0F, .z = 0.0F };
    CHECK(fbe_body_set_position(ball, &furthest));
    CHECK_FALSE(fbe_body_set_position(ball, &further));
    CHECK(said() == "a position is at most 1e9 from 0 each way, not (1.01e+09, 0, 0)");
    const fbe_character_vec3_t beyond{ .x = 0.0F, .y = -1.01e9F, .z = 0.0F };
    CHECK_FALSE(fbe_character_set_position(scene.character, &beyond));
    CHECK(said() == "a character's position is at most 1e9 from 0 each way");
    // A push, to 1e12, and what pushes add up to on the body, which a push past is put back from.
    const std::uint64_t box_body = scene.bodies[1];
    const fbe_body_vec3_t most{ .x = 0.0F, .y = 1.0e12F, .z = 0.0F };
    const fbe_body_vec3_t past{ .x = 0.0F, .y = 1.01e12F, .z = 0.0F };
    CHECK_FALSE(fbe_body_add_force(box_body, &past));
    CHECK(said() ==
          "a velocity, force, impulse, or torque is at most 1e12 each way, not (0, 1.01e+12, 0)");
    // The box weighs 1000 kg, so a force of 1e12 changes its velocity by 1e9 a second, and about a
    // thousand of them by 1e12.
    int pushes = 0;
    while (fbe_body_add_force(box_body, &most) && pushes < 2000) {
        ++pushes;
    }
    CHECK(pushes >= 999);
    CHECK(pushes <= 1001);
    CHECK(said() == "that would change how the body moves by more than 1e12 each way");
    scene.step_soundly("a body pushed as far as it goes");
    // A mass, from a milligram.
    CHECK(fbe_body_set_mass(box_body, 1.0e-6F));
    CHECK_FALSE(fbe_body_set_mass(box_body, 0.9e-6F));
    CHECK(said() == "a mass is 1e-6 kg or more, not 9e-07");
    CHECK(fbe_body_set_mass(box_body, 1000.0F));
    // Gravity, and a gravity factor, to 1e6.
    const fbe_physics_vec3_t heaviest{ .x = 0.0F, .y = -1.0e6F, .z = 0.0F };
    const fbe_physics_vec3_t heavier{ .x = 0.0F, .y = -1.01e6F, .z = 0.0F };
    CHECK(fbe_physics_set_gravity(scene.world, &heaviest));
    CHECK_FALSE(fbe_physics_set_gravity(scene.world, &heavier));
    CHECK(said() == "gravity is at most 1e6 each way, not (0, -1010000, 0)");
    CHECK(fbe_body_set_gravity_factor(ball, 1.0e6F));
    CHECK_FALSE(fbe_body_set_gravity_factor(ball, 1.01e6F));
    CHECK(said() == "a gravity factor is at most 1e6 either way, not 1010000");
    // A character's velocity, to 1e6.
    const fbe_character_vec3_t fast{ .x = 1.01e6F, .y = 0.0F, .z = 0.0F };
    CHECK_FALSE(fbe_character_move(scene.character, &fast, 1.0F / 60));
    CHECK(said() == "a character's velocity is at most 1e6 each way");
    // A joint's points, to 1e5 from the bodies they're on, apart for a distance joint, with an
    // axis however short, and a spring of no frequency, or a thousandth or more.
    const fbe_joint_vec3_t away{ .x = 2.0F, .y = 1.01e5F, .z = 0.0F };
    const fbe_joint_vec3_t on{ .x = 2.0F, .y = 2.0F, .z = 0.0F };
    const fbe_joint_vec3_t tiny{ .x = 0.0F, .y = 0.0F, .z = 1.0e-30F };
    CHECK(fbe_joint_point(scene.bodies[3], 0, &away) == 0);
    CHECK(said() == "a joint's points are at most 1e5 from the bodies they're on, each way");
    CHECK(fbe_joint_distance(scene.bodies[3], scene.bodies[4], &on, &on) == 0);
    CHECK(said() == "a distance joint's points are 1e-3 or more apart");
    const std::uint64_t hinge = fbe_joint_hinge(scene.bodies[3], 0, &on, &tiny);
    CHECK(hinge != 0);
    CHECK(fbe_joint_set_spring(hinge, 1.0e-3F, 0.5F));
    CHECK(fbe_joint_set_spring(hinge, 0.0F, 0.5F));
    CHECK_FALSE(fbe_joint_set_spring(hinge, 0.9e-3F, 0.5F));
    CHECK(said() == "a spring's frequency is 0 or 1e-3 or more, and its damping 0 or more, not "
                    "9e-04 and 0.5");
    scene.step_soundly("bodies at every limit");
}
