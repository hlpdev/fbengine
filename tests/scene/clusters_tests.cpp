// What lights what a camera draws, through the C API: its World's lights in its view, in order and
// in its view's space, the clusters each point and spot light reaches, against every cluster's box
// and part of the view worked out here from fbe_render.h's description, and the lights of what's
// lit per vertex.

#include "fbengine.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <utility>
#include <vector>

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
    return std::fabs(a - b) < 0.001F;
}

/// What lights a camera's view of 1600 by 900, all of it.
struct lighting_t {
    std::vector<fbe_draw_light_t> lights;
    std::vector<fbe_cluster_t> clusters;
    std::vector<std::uint32_t> indices;
};

lighting_t lighting(std::uint64_t camera, float tween = 1.0F) {
    std::int32_t light_count = 0;
    std::int32_t index_count = 0;
    REQUIRE(fbe_render_lights(camera, 1600.0F, 900.0F, tween, nullptr, 0, &light_count, nullptr,
                              nullptr, 0, &index_count));
    lighting_t found;
    found.lights.resize(static_cast<std::size_t>(light_count));
    found.clusters.resize(FBE_CLUSTERS);
    found.indices.resize(static_cast<std::size_t>(index_count));
    REQUIRE(fbe_render_lights(camera, 1600.0F, 900.0F, tween, found.lights.data(), light_count,
                              &light_count, found.clusters.data(), found.indices.data(),
                              index_count, &index_count));
    return found;
}

std::uint64_t light(std::uint64_t world, std::int32_t type, float x, float y, float z,
                    float range) {
    const std::uint64_t made = fbe_entity_create(world, 0, FBE_ENTITY_LIGHT);
    fbe_light_set_type(made, type);
    fbe_light_set_range(made, range);
    fbe_entity_set_position(made, x, y, z, FBE_SPACE_WORLD);
    return made;
}

} // namespace

TEST_CASE("a camera's lights are its World's showing ones that reach its view, in order",
          "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    fbe_camera_set_range(camera, 0.5F, 100.0F);
    // Turned a quarter left: its view's -z is the world's -x.
    fbe_entity_set_rotation(camera, 0.0F, 0.70710678F, 0.0F, 0.70710678F, FBE_SPACE_WORLD);
    const std::uint64_t ahead = light(world.handle, FBE_LIGHT_POINT, -10.0F, 0.0F, 2.0F, 3.0F);
    fbe_light_set_color(ahead, 1.0F, 0.5F, 0.25F);
    fbe_light_set_intensity(ahead, 2.0F);
    const std::uint64_t sun = light(world.handle, FBE_LIGHT_DIRECTIONAL, 0.0F, 50.0F, 0.0F, 1.0F);
    const std::uint64_t spot = light(world.handle, FBE_LIGHT_SPOT, -20.0F, 1.0F, 0.0F, 5.0F);
    fbe_light_set_cone(spot, 0.25F, 0.5F);
    // Behind, out of reach of its view; hidden; with no intensity; in another World.
    light(world.handle, FBE_LIGHT_POINT, 10.0F, 0.0F, 0.0F, 5.0F);
    const std::uint64_t hidden = light(world.handle, FBE_LIGHT_POINT, -5.0F, 0.0F, 0.0F, 5.0F);
    fbe_entity_set_hidden(hidden, true);
    const std::uint64_t dark = light(world.handle, FBE_LIGHT_POINT, -5.0F, 0.0F, 0.0F, 5.0F);
    fbe_light_set_intensity(dark, 0.0F);
    const test_world_t other;
    light(other.handle, FBE_LIGHT_POINT, -5.0F, 0.0F, 0.0F, 5.0F);
    // Just behind, but reaching past the near plane.
    const std::uint64_t reaching = light(world.handle, FBE_LIGHT_POINT, 1.0F, 0.0F, 0.0F, 2.0F);
    const lighting_t found = lighting(camera);
    REQUIRE(found.lights.size() == 4);
    CHECK(found.lights[0].light == sun);
    CHECK(found.lights[1].light == ahead);
    CHECK(found.lights[2].light == spot);
    CHECK(found.lights[3].light == reaching);
    // In the view: 10 ahead, 2 to the left, the world's +z; its color times its intensity.
    CHECK((near(found.lights[1].position[0], -2.0F) && near(found.lights[1].position[1], 0.0F) &&
           near(found.lights[1].position[2], -10.0F)));
    CHECK((near(found.lights[1].color[0], 2.0F) && near(found.lights[1].color[2], 0.5F)));
    CHECK(found.lights[1].type == FBE_LIGHT_POINT);
    CHECK(near(found.lights[1].range, 3.0F));
    // The sun shines along its -z, the world's, which is the view's +x.
    CHECK((near(found.lights[0].direction[0], 1.0F) && near(found.lights[0].direction[2], 0.0F)));
    CHECK((near(found.lights[2].inner_cosine, std::cos(0.25F)) &&
           near(found.lights[2].outer_cosine, std::cos(0.5F))));
    // Less room: the counts still, and what fits.
    std::int32_t light_count = 0;
    std::int32_t index_count = 0;
    std::array<fbe_draw_light_t, 2> some{};
    REQUIRE(fbe_render_lights(camera, 1600.0F, 900.0F, 1.0F, some.data(), 2, &light_count, nullptr,
                              nullptr, 0, &index_count));
    CHECK(light_count == 4);
    CHECK(std::cmp_equal(index_count, found.indices.size()));
    CHECK(some[1].light == ahead);
    // A hidden camera, or one with no viewport, sees none; a bad tween or another entity fails.
    fbe_entity_set_hidden(camera, true);
    CHECK(lighting(camera).lights.empty());
    fbe_entity_set_hidden(camera, false);
    CHECK_FALSE(fbe_render_lights(camera, 1600.0F, 900.0F, 2.0F, nullptr, 0, &light_count, nullptr,
                                  nullptr, 0, &index_count));
    CHECK_FALSE(fbe_render_lights(sun, 1600.0F, 900.0F, 1.0F, nullptr, 0, &light_count, nullptr,
                                  nullptr, 0, &index_count));
}

TEST_CASE("a light between the steps is lit from where it is then", "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t lamp = light(world.handle, FBE_LIGHT_POINT, 0.0F, 0.0F, -10.0F, 4.0F);
    REQUIRE(fbe_world_capture(world.handle));
    fbe_entity_translate(lamp, 4.0F, 0.0F, 0.0F, FBE_SPACE_WORLD);
    fbe_entity_translate(camera, 0.0F, 0.0F, -2.0F, FBE_SPACE_WORLD);
    const lighting_t found = lighting(camera, 0.5F);
    REQUIRE(found.lights.size() == 1);
    CHECK((near(found.lights[0].position[0], 2.0F) && near(found.lights[0].position[2], -9.0F)));
}

TEST_CASE("each cluster lists the lights whose range reaches its part of the view", "[render]") {
    // For each seed, points in each light's sphere that are in the view, into the cluster shading
    // would find them in: it has to list that light, whichever cluster that is.
    const double nearest = 0.5;
    const double farthest = 60.0;
    for (const std::int32_t projection :
         { FBE_PROJECTION_PERSPECTIVE, FBE_PROJECTION_ORTHOGRAPHIC }) {
        const bool perspective = projection == FBE_PROJECTION_PERSPECTIVE;
        const double half_height = perspective ? std::tan(1.0471976 / 2.0) : 10.0;
        const double half_width = half_height * 1600.0 / 900.0;
        std::int64_t tried = 0;
        std::int64_t wrong = 0;
        for (unsigned seed = 1; seed <= 100; ++seed) {
            std::mt19937 random(seed);
            std::uniform_real_distribution<float> unit(-1.0F, 1.0F);
            std::uniform_real_distribution<float> reach(0.2F, 6.0F);
            const test_world_t world;
            const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
            fbe_camera_set_projection(camera, projection);
            fbe_camera_set_range(camera, 0.5F, 60.0F);
            fbe_camera_set_orthographic_height(camera, 20.0F);
            for (int i = 0; i < 30; ++i) {
                const std::int32_t type = i % 3 == 0 ? FBE_LIGHT_SPOT : FBE_LIGHT_POINT;
                light(world.handle, type, unit(random) * 40.0F, unit(random) * 25.0F,
                      -((unit(random) + 1.0F) * 35.0F) + 4.0F, reach(random));
            }
            const lighting_t found = lighting(camera);
            for (std::uint32_t index = 0; index < found.lights.size(); ++index) {
                const fbe_draw_light_t& lit = found.lights[index];
                for (int point = 0; point < 200; ++point) {
                    // Somewhere in the sphere, the most of them near its skin.
                    const double range = lit.range;
                    const double out = 0.999 * std::sqrt(std::fabs(unit(random)));
                    const std::array<double, 3> way{ unit(random), unit(random), unit(random) };
                    const double length =
                        std::sqrt((way[0] * way[0]) + (way[1] * way[1]) + (way[2] * way[2]));
                    if (length < 0.001 || length > 1.0) {
                        continue;
                    }
                    const double x = lit.position[0] + (way[0] / length * out * range);
                    const double y = lit.position[1] + (way[1] / length * out * range);
                    const double depth = -(lit.position[2] + (way[2] / length * out * range));
                    if (depth < nearest || depth >= farthest) {
                        continue;
                    }
                    const double scale = perspective ? depth : 1.0;
                    const double across = x / (scale * half_width);
                    const double up = y / (scale * half_height);
                    if (std::fabs(across) >= 1.0 || std::fabs(up) >= 1.0) {
                        continue;
                    }
                    const double part =
                        perspective ? std::log(depth / nearest) / std::log(farthest / nearest)
                                    : (depth - nearest) / (farthest - nearest);
                    const auto column =
                        static_cast<std::size_t>((across + 1.0) * 0.5 * FBE_CLUSTERS_ACROSS);
                    const auto row = static_cast<std::size_t>((1.0 - up) * 0.5 * FBE_CLUSTERS_UP);
                    const auto slice = static_cast<std::size_t>(part * FBE_CLUSTERS_DEEP);
                    const std::size_t cluster =
                        column + (std::size_t{ FBE_CLUSTERS_ACROSS } *
                                  (row + (std::size_t{ FBE_CLUSTERS_UP } * slice)));
                    const fbe_cluster_t& lists = found.clusters[cluster];
                    const auto first = found.indices.begin() + lists.first;
                    const auto last = first + lists.count;
                    ++tried;
                    wrong += std::find(first, last, index) == last ? 1 : 0;
                }
            }
        }
        CAPTURE(projection);
        CHECK(tried > 20000);
        CHECK(wrong == 0);
    }
}

TEST_CASE("each cluster lists only lights near its box, and each listed light is in some cluster",
          "[render]") {
    std::mt19937 random(13); // NOLINT(bugprone-random-generator-seed)
    std::uniform_real_distribution<float> unit(-1.0F, 1.0F);
    std::uniform_real_distribution<float> reach(0.2F, 6.0F);
    for (const std::int32_t projection :
         { FBE_PROJECTION_PERSPECTIVE, FBE_PROJECTION_ORTHOGRAPHIC }) {
        const test_world_t world;
        const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
        fbe_camera_set_projection(camera, projection);
        fbe_camera_set_range(camera, 0.5F, 60.0F);
        fbe_camera_set_orthographic_height(camera, 20.0F);
        // Lights of each kind all over, near and far, inside and outside the view; the camera
        // left where it was made, so the view's space is the world's.
        for (int i = 0; i < 120; ++i) {
            const std::int32_t type = i % 3 == 0 ? FBE_LIGHT_SPOT : FBE_LIGHT_POINT;
            light(world.handle, type, unit(random) * 40.0F, unit(random) * 25.0F,
                  -((unit(random) + 1.0F) * 35.0F) + 4.0F, reach(random));
        }
        light(world.handle, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
        const lighting_t found = lighting(camera);
        CHECK(found.lights[0].type == FBE_LIGHT_DIRECTIONAL);
        // Each cluster's box, as fbe_render.h has them, against each light's sphere.
        const bool perspective = projection == FBE_PROJECTION_PERSPECTIVE;
        const double nearest = 0.5;
        const double farthest = 60.0;
        const double half_height = perspective ? std::tan(1.0471976 / 2.0) : 10.0;
        const double half_width = half_height * 1600.0 / 900.0;
        const auto start = [&](int slice) {
            const double part = static_cast<double>(slice) / FBE_CLUSTERS_DEEP;
            return perspective ? nearest * std::pow(farthest / nearest, part)
                               : nearest + ((farthest - nearest) * part);
        };
        int listed = 0;
        int wrong = 0;
        for (int slice = 0; slice < FBE_CLUSTERS_DEEP; ++slice) {
            const double from_depth = start(slice);
            const double to_depth = start(slice + 1);
            for (int row = 0; row < FBE_CLUSTERS_UP; ++row) {
                for (int column = 0; column < FBE_CLUSTERS_ACROSS; ++column) {
                    const double left = -1.0 + (2.0 * column / FBE_CLUSTERS_ACROSS);
                    const double right = -1.0 + (2.0 * (column + 1) / FBE_CLUSTERS_ACROSS);
                    const double top = 1.0 - (2.0 * row / FBE_CLUSTERS_UP);
                    const double bottom = 1.0 - (2.0 * (row + 1) / FBE_CLUSTERS_UP);
                    const double near_scale = perspective ? from_depth : 1.0;
                    const double far_scale = perspective ? to_depth : 1.0;
                    const std::array<double, 3> low{
                        std::fmin(left * near_scale, left * far_scale) * half_width,
                        std::fmin(bottom * near_scale, bottom * far_scale) * half_height,
                        -to_depth,
                    };
                    const std::array<double, 3> high{
                        std::fmax(right * near_scale, right * far_scale) * half_width,
                        std::fmax(top * near_scale, top * far_scale) * half_height,
                        -from_depth,
                    };
                    const std::size_t cluster =
                        static_cast<std::size_t>(column) +
                        (std::size_t{ FBE_CLUSTERS_ACROSS } *
                         (static_cast<std::size_t>(row) +
                          (std::size_t{ FBE_CLUSTERS_UP } * static_cast<std::size_t>(slice))));
                    const fbe_cluster_t& lists = found.clusters[cluster];
                    for (std::uint32_t i = 0; i < lists.count; ++i) {
                        const fbe_draw_light_t& lit = found.lights[found.indices[lists.first + i]];
                        double apart = 0.0;
                        for (std::size_t axis = 0; axis < 3; ++axis) {
                            const double p = lit.position[axis];
                            const double q = std::clamp(p, low[axis], high[axis]);
                            apart += (p - q) * (p - q);
                        }
                        const double range = lit.range;
                        ++listed;
                        wrong += apart > (range * range * 1.0001) + 1e-6 ? 1 : 0;
                    }
                }
            }
        }
        CAPTURE(projection);
        CHECK(listed > 200);
        CHECK(wrong == 0);
        // Every point and spot light listed reaches some cluster.
        for (std::size_t index = 1; index < found.lights.size(); ++index) {
            bool somewhere = false;
            for (const std::uint32_t i : found.indices) {
                somewhere = somewhere || i == index;
            }
            CHECK(somewhere);
        }
    }
}

TEST_CASE("a light of a huge range is in every cluster", "[render]") {
    for (const std::int32_t projection :
         { FBE_PROJECTION_PERSPECTIVE, FBE_PROJECTION_ORTHOGRAPHIC }) {
        for (const float range : { 3e7F, 1e10F }) {
            const test_world_t world;
            const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
            fbe_camera_set_projection(camera, projection);
            light(world.handle, FBE_LIGHT_POINT, 0.0F, 0.0F, -5.0F, range);
            const lighting_t found = lighting(camera);
            CAPTURE(projection, range);
            REQUIRE(found.lights.size() == 1);
            CHECK(found.indices.size() == FBE_CLUSTERS);
            for (const fbe_cluster_t& cluster : found.clusters) {
                CHECK(cluster.count == 1);
            }
        }
    }
}

namespace {

/// A VERTEX material, and what a camera's draws are lit by per vertex, each draw's lights.
struct per_vertex_t {
    lighting_t found;
    std::vector<fbe_draw_t> draws;
    std::vector<std::uint32_t> draw_lights;

    /// The lights a draw's lit by, in order.
    [[nodiscard]] std::vector<std::uint64_t> of(std::size_t draw) const {
        std::vector<std::uint64_t> handles;
        handles.reserve(draws[draw].light_count);
        for (std::uint32_t i = 0; i < draws[draw].light_count; ++i) {
            handles.push_back(found.lights[draw_lights[draws[draw].light_first + i]].light);
        }
        return handles;
    }
};

per_vertex_t per_vertex(std::uint64_t camera) {
    per_vertex_t got;
    got.found = lighting(camera);
    const std::int32_t draws = fbe_render_draws(camera, 1600.0F, 900.0F, 1.0F, nullptr, 0);
    REQUIRE(draws >= 0);
    got.draws.resize(static_cast<std::size_t>(draws));
    fbe_render_draws(camera, 1600.0F, 900.0F, 1.0F, got.draws.data(), draws);
    const std::int32_t count = fbe_render_draw_lights(camera, 1600.0F, 900.0F, 1.0F, nullptr, 0);
    REQUIRE(count >= 0);
    got.draw_lights.resize(static_cast<std::size_t>(count));
    fbe_render_draw_lights(camera, 1600.0F, 900.0F, 1.0F, got.draw_lights.data(), count);
    return got;
}

std::uint64_t mesh_in(std::uint64_t world, std::uint64_t material) {
    const std::uint64_t mesh = fbe_entity_create(world, 0, FBE_ENTITY_MESH);
    const std::uint64_t geometry = fbe_geometry_cube();
    fbe_mesh_set_geometry(mesh, geometry);
    fbe_geometry_release(geometry);
    fbe_mesh_set_material(mesh, material);
    return mesh;
}

} // namespace

TEST_CASE("a VERTEX draw has the lights that reach it, in view or not", "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t retro = fbe_material_create();
    fbe_material_set_shading(retro, FBE_SHADING_VERTEX);
    // From x -100 to 100, y -1.1 to -0.9, z -20 to 0, and a lamp by its corner off to the side.
    const std::uint64_t floor = mesh_in(world.handle, retro);
    fbe_entity_set_position(floor, 0.0F, -1.0F, -10.0F, FBE_SPACE_WORLD);
    fbe_entity_set_scale(floor, 100.0F, 0.1F, 10.0F, FBE_SPACE_WORLD);
    const std::uint64_t lamp = light(world.handle, FBE_LIGHT_POINT, 100.0F, -0.5F, -20.0F, 5.0F);
    // Out of its reach; hidden; with no intensity: none of them light it.
    light(world.handle, FBE_LIGHT_POINT, 100.0F, 20.0F, -20.0F, 5.0F);
    const std::uint64_t hidden = light(world.handle, FBE_LIGHT_POINT, 0.0F, 0.0F, -5.0F, 5.0F);
    fbe_entity_set_hidden(hidden, true);
    const std::uint64_t dark = light(world.handle, FBE_LIGHT_POINT, 0.0F, 0.0F, -5.0F, 5.0F);
    fbe_light_set_intensity(dark, 0.0F);
    const per_vertex_t got = per_vertex(camera);
    REQUIRE(got.draws.size() == 1);
    CHECK(got.of(0) == std::vector<std::uint64_t>{ lamp });
    // It's in the lights for that, though no cluster has it.
    REQUIRE(got.found.lights.size() == 1);
    CHECK(got.found.indices.empty());
    fbe_material_release(retro);
}

TEST_CASE("only VERTEX draws have lights of their own", "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t retro = fbe_material_create();
    fbe_material_set_shading(retro, FBE_SHADING_VERTEX);
    const std::uint64_t pixel = mesh_in(world.handle, 0);
    fbe_entity_set_position(pixel, -3.0F, 0.0F, -6.0F, FBE_SPACE_WORLD);
    const std::uint64_t per_vertex_mesh = mesh_in(world.handle, retro);
    fbe_entity_set_position(per_vertex_mesh, 3.0F, 0.0F, -6.0F, FBE_SPACE_WORLD);
    const std::uint64_t sun = light(world.handle, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F);
    // By the VERTEX mesh, and out of reach of the other.
    const std::uint64_t lamp = light(world.handle, FBE_LIGHT_POINT, 3.0F, 0.0F, -3.0F, 2.5F);
    const per_vertex_t got = per_vertex(camera);
    REQUIRE(got.draws.size() == 2);
    for (std::size_t draw = 0; draw < got.draws.size(); ++draw) {
        if (got.draws[draw].entity == pixel) {
            CHECK(got.of(draw).empty());
        } else {
            CHECK(got.of(draw) == std::vector<std::uint64_t>{ sun, lamp });
        }
    }
    fbe_material_release(retro);
}

TEST_CASE("a VERTEX draw's lights are the first eight that reach it, in the tree's order",
          "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t retro = fbe_material_create();
    fbe_material_set_shading(retro, FBE_SHADING_VERTEX);
    const std::uint64_t mesh = mesh_in(world.handle, retro);
    fbe_entity_set_position(mesh, 0.0F, 0.0F, -6.0F, FBE_SPACE_WORLD);
    // Nine that reach it: made first, a pivot, whose child is made last, so the child's first in
    // the tree's order and last as they were made; a directional one among the roots, not ahead
    // of them as it is in the lights; and one out of reach, which takes no place.
    const std::uint64_t pivot = fbe_entity_create(world.handle, 0, FBE_ENTITY_PIVOT);
    std::vector<std::uint64_t> roots;
    roots.reserve(7);
    light(world.handle, FBE_LIGHT_POINT, 50.0F, 0.0F, -6.0F, 2.0F);
    for (int i = 0; i < 7; ++i) {
        roots.push_back(i == 2 ? light(world.handle, FBE_LIGHT_DIRECTIONAL, 0.0F, 0.0F, 0.0F, 1.0F)
                               : light(world.handle, FBE_LIGHT_POINT, 0.25F * static_cast<float>(i),
                                       1.0F, -4.0F, 4.0F));
    }
    const std::uint64_t ninth = light(world.handle, FBE_LIGHT_POINT, 0.0F, 0.0F, -4.0F, 4.0F);
    const std::uint64_t child = fbe_entity_create(world.handle, pivot, FBE_ENTITY_LIGHT);
    fbe_light_set_range(child, 4.0F);
    fbe_entity_set_position(child, 0.0F, 0.0F, -4.0F, FBE_SPACE_WORLD);
    const per_vertex_t got = per_vertex(camera);
    REQUIRE(got.draws.size() == 1);
    std::vector<std::uint64_t> expected{ child };
    expected.insert(expected.end(), roots.begin(), roots.end());
    CHECK(got.of(0) == expected);
    CHECK(got.draws[0].light_count == FBE_DRAW_LIGHTS);
    // Each is in the lights as the lights have them, with what no draw wants and no cluster has
    // left out: the directional one first, the rest as they were made.
    CHECK(got.found.lights[0].type == FBE_LIGHT_DIRECTIONAL);
    CHECK(std::ranges::none_of(
        got.found.lights, [&](const fbe_draw_light_t& lit) { return lit.position[0] > 40.0F; }));
    CHECK(std::ranges::any_of(got.found.lights,
                              [&](const fbe_draw_light_t& lit) { return lit.light == ninth; }));
    fbe_material_release(retro);
}

TEST_CASE("a VERTEX ground plane has the lights that reach its plane", "[render]") {
    const test_world_t world;
    const std::uint64_t camera = fbe_entity_create(world.handle, 0, FBE_ENTITY_CAMERA);
    const std::uint64_t retro = fbe_material_create();
    fbe_material_set_shading(retro, FBE_SHADING_VERTEX);
    const std::uint64_t ground = fbe_entity_create(world.handle, 0, FBE_ENTITY_PLANE);
    fbe_plane_set_material(ground, retro);
    fbe_entity_set_position(ground, 0.0F, -2.0F, 0.0F, FBE_SPACE_WORLD);
    // Far off along the plane, by it; and as far off, too high above it.
    const std::uint64_t by_it = light(world.handle, FBE_LIGHT_POINT, 900.0F, -1.0F, 0.0F, 2.0F);
    light(world.handle, FBE_LIGHT_POINT, 900.0F, 5.0F, 0.0F, 2.0F);
    const per_vertex_t got = per_vertex(camera);
    REQUIRE(got.draws.size() == 1);
    CHECK(got.of(0) == std::vector<std::uint64_t>{ by_it });
    fbe_material_release(retro);
}
