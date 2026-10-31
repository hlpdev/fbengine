#pragma once

// Spheres swept against triangles and ellipsoids, which collisions and picks share: a sphere of
// radius r from c along v, touching triangles only from their fronts, and its hit's time the part
// of v it got through, 0 to 1.

#include "scene/bvh.hpp"
#include "scene/math.hpp"

#include <array>
#include <cstdint>

namespace fbe::scene {

/// A triangle to sweep against, its front counterclockwise, and what it's of, the caller's
/// `target`.
struct triangle_t {
    std::array<vec3_t, 3> corners;
    vec3_t normal;
    std::uint32_t target = 0;
    triangle_origin_t origin;
    bool from_mesh = false;
};

/// An ellipsoid grown by the swept sphere's radius, so the sphere is a point to it.
struct ellipsoid_t {
    vec3_t center;
    vec3_t radii;
    std::uint32_t target = 0;
};

/// The nearest touch along a sweep: when, where on what it touched, that's normal there, and what.
struct hit_t {
    float time = 1.0F;
    vec3_t point;
    vec3_t normal;
    std::uint32_t target = 0;
    triangle_origin_t origin;
    bool from_mesh = false;
};

/// A box's six faces, two triangles each, facing out, by box_corner's corners.
inline constexpr std::array<std::array<unsigned, 3>, 12> box_triangles{
    {
        { 0, 4, 6 },
        { 0, 6, 2 },
        { 1, 3, 7 },
        { 1, 7, 5 },
        { 0, 1, 5 },
        { 0, 5, 4 },
        { 2, 6, 7 },
        { 2, 7, 3 },
        { 0, 2, 3 },
        { 0, 3, 1 },
        { 4, 5, 7 },
        { 4, 7, 6 },
    },
};

/// A box's corner, 0 to 7: bit 0 for its greatest x, 1 for y, 2 for z.
inline vec3_t box_corner(vec3_t min, vec3_t max, unsigned corner) {
    return { (corner & 1U) != 0 ? max.x : min.x, (corner & 2U) != 0 ? max.y : min.y,
             (corner & 4U) != 0 ? max.z : min.z };
}

/// Makes a triangle of three corners, turned when what it's of mirrors, so its front stays out;
/// false for one too thin to have a front.
bool make_triangle(std::array<vec3_t, 3> corners, bool mirrored, triangle_t& triangle);

/// Sweeps a sphere of radius `r` from `c` along `v` against a triangle's front, and keeps the hit
/// if it's sooner than `hit`'s, or `found` is false.
void sweep_triangle(vec3_t c, vec3_t v, float r, const triangle_t& triangle, hit_t& hit,
                    bool& found);

/// Sweeps a point from `c` along `v` against an ellipsoid grown by the sphere's radius `r`, and
/// keeps the hit if it's sooner than `hit`'s, or `found` is false.
void sweep_ellipsoid(vec3_t c, vec3_t v, float r, const ellipsoid_t& ellipsoid, hit_t& hit,
                     bool& found);

} // namespace fbe::scene
