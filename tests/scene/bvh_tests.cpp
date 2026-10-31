// The tree over a geometry's triangles, built into the test from its source: what it finds along
// segments and in boxes, against every triangle tried one by one.

#include "scene/bvh.hpp"
#include "scene/scene.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

namespace {

using fbe::scene::bvh_t;
using fbe::scene::segment_hit_t;
using fbe::scene::surface_t;
using fbe::scene::vec3_t;

/// `count` random triangles in a cube 20 across, in two surfaces.
std::vector<surface_t> soup(std::uint32_t count, std::mt19937& random) {
    std::uniform_real_distribution<float> place(-10.0F, 10.0F);
    std::uniform_real_distribution<float> size(-1.0F, 1.0F);
    std::vector<surface_t> surfaces(2);
    for (std::uint32_t i = 0; i < count; ++i) {
        surface_t& surface = surfaces[i % 2];
        const vec3_t center(place(random), place(random), place(random));
        for (int k = 0; k < 3; ++k) {
            fbe_vertex_t vertex{};
            const float x = center.x + size(random);
            const float y = center.y + size(random);
            const float z = center.z + size(random);
            vertex.position = { .x = x, .y = y, .z = z };
            surface.indices.push_back(static_cast<std::uint32_t>(surface.vertices.size()));
            surface.vertices.push_back(vertex);
        }
    }
    return surfaces;
}

/// The nearest crossing along the segment, trying each triangle, as the tree should find it.
float brute_force(const bvh_t& tree, vec3_t a, vec3_t b, bool front_only) {
    float best = INFINITY;
    for (std::uint32_t t = 0; t < tree.triangle_count(); ++t) {
        const vec3_t* corners = tree.corners(t);
        const vec3_t normal = cross(corners[1] - corners[0], corners[2] - corners[0]);
        const vec3_t delta = b - a;
        const float facing = dot(normal, delta);
        if (facing == 0.0F || (front_only && facing > 0.0F)) {
            continue;
        }
        const float time = dot(normal, corners[0] - a) / facing;
        if (time < 0.0F || time > 1.0F || time >= best) {
            continue;
        }
        const vec3_t p = a + delta * time;
        // Inside when the point is on the inner side of each edge.
        const bool inside = dot(cross(corners[1] - corners[0], p - corners[0]), normal) >= 0.0F &&
                            dot(cross(corners[2] - corners[1], p - corners[1]), normal) >= 0.0F &&
                            dot(cross(corners[0] - corners[2], p - corners[2]), normal) >= 0.0F;
        if (inside) {
            best = time;
        }
    }
    return best;
}

} // namespace

TEST_CASE("the tree finds the nearest triangle along a segment as trying each does", "[scene]") {
    // A fixed seed, so a failure comes back the same.
    std::mt19937 random(7); // NOLINT(bugprone-random-generator-seed)
    bvh_t tree;
    tree.build(soup(2000, random));
    REQUIRE(tree.triangle_count() == 2000);
    std::uniform_real_distribution<float> place(-12.0F, 12.0F);
    int hits = 0;
    for (int i = 0; i < 500; ++i) {
        const vec3_t a(place(random), place(random), place(random));
        const vec3_t b(place(random), place(random), place(random));
        for (const bool front_only : { false, true }) {
            segment_hit_t hit;
            const bool found = tree.nearest(a, b, front_only, hit);
            const float expected = brute_force(tree, a, b, front_only);
            CHECK(found == std::isfinite(expected));
            if (found && std::isfinite(expected)) {
                CHECK(std::fabs(hit.time - expected) < 1e-4F);
                ++hits;
            }
        }
        CHECK(tree.blocks(a, b) == std::isfinite(brute_force(tree, a, b, false)));
    }
    CHECK(hits > 100);
}

TEST_CASE("the tree gives every triangle whose box a box touches", "[scene]") {
    std::mt19937 random(11); // NOLINT(bugprone-random-generator-seed)
    bvh_t tree;
    tree.build(soup(1000, random));
    const vec3_t min(-2.0F, -2.0F, -2.0F);
    const vec3_t max(3.0F, 1.0F, 2.0F);
    std::set<std::uint32_t> found;
    tree.overlapping(min, max, [&found](std::uint32_t triangle) { found.insert(triangle); });
    for (std::uint32_t t = 0; t < tree.triangle_count(); ++t) {
        const vec3_t* corners = tree.corners(t);
        bool touches = true;
        for (int axis = 0; axis < 3; ++axis) {
            const float a = fbe::scene::component(corners[0], axis);
            const float b = fbe::scene::component(corners[1], axis);
            const float c = fbe::scene::component(corners[2], axis);
            const float low = std::fmin(a, std::fmin(b, c));
            const float high = std::fmax(a, std::fmax(b, c));
            touches = touches && high >= fbe::scene::component(min, axis) &&
                      low <= fbe::scene::component(max, axis);
        }
        if (touches) {
            CHECK(found.contains(t));
        }
    }
}

TEST_CASE("a hit's normal faces the segment's start, and its origin is its triangle", "[scene]") {
    std::vector<surface_t> surfaces(1);
    for (const vec3_t& p :
         { vec3_t(-1.0F, -1.0F, 0.0F), vec3_t(1.0F, -1.0F, 0.0F), vec3_t(0.0F, 1.0F, 0.0F) }) {
        fbe_vertex_t vertex{};
        vertex.position = { .x = p.x, .y = p.y, .z = p.z };
        surfaces[0].vertices.push_back(vertex);
    }
    surfaces[0].indices = { 0, 1, 2 };
    bvh_t tree;
    tree.build(surfaces);
    segment_hit_t hit;
    REQUIRE(tree.nearest({ 0.0F, 0.0F, 5.0F }, { 0.0F, 0.0F, -5.0F }, true, hit));
    CHECK(hit.time == 0.5F);
    CHECK(hit.normal.z == 1.0F);
    CHECK(hit.origin.surface == 0);
    CHECK(hit.origin.triangle == 0);
    CHECK_FALSE(tree.nearest({ 0.0F, 0.0F, -5.0F }, { 0.0F, 0.0F, 5.0F }, true, hit));
    REQUIRE(tree.nearest({ 0.0F, 0.0F, -5.0F }, { 0.0F, 0.0F, 5.0F }, false, hit));
    CHECK(hit.normal.z == -1.0F);
    bvh_t empty;
    empty.build({});
    CHECK_FALSE(empty.blocks({}, { 1.0F, 1.0F, 1.0F }));
}

TEST_CASE("a refitted tree finds what a tree built again does", "[scene]") {
    std::mt19937 random(5); // NOLINT(bugprone-random-generator-seed)
    std::vector<surface_t> surfaces = soup(1500, random);
    bvh_t refitted;
    refitted.build(surfaces);
    // Every vertex moved, as an animated or edited mesh's are; the triangles stay.
    std::uniform_real_distribution<float> nudge(-2.0F, 2.0F);
    for (surface_t& surface : surfaces) {
        for (fbe_vertex_t& vertex : surface.vertices) {
            vertex.position.x += nudge(random);
            vertex.position.y += nudge(random);
        }
    }
    refitted.refit(surfaces);
    bvh_t built;
    built.build(surfaces);
    std::uniform_real_distribution<float> place(-12.0F, 12.0F);
    for (int i = 0; i < 300; ++i) {
        const vec3_t a(place(random), place(random), place(random));
        const vec3_t b(place(random), place(random), place(random));
        segment_hit_t first;
        segment_hit_t second;
        const bool found = refitted.nearest(a, b, false, first);
        CHECK(found == built.nearest(a, b, false, second));
        if (found) {
            CHECK(first.time == second.time);
            CHECK(first.origin.triangle == second.origin.triangle);
        }
    }
}
