// Whether two meshes' triangles cross, in the world: one's triangles into the other's space, that
// one's tree's leaves near each, and a triangle against triangle test.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/bvh.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fbe::scene {

namespace {

constexpr float tiny = 1e-7F;

vec3_t least(vec3_t a, vec3_t b) {
    return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
}

vec3_t greatest(vec3_t a, vec3_t b) {
    return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
}

/// Whether a triangle has corners strictly on both sides of a plane, by their signed distances
/// times the plane's normal's length, within a tolerance of that length: one resting on the plane,
/// or touching it at a corner or an edge, doesn't cross it.
bool straddles(const std::array<float, 3>& distances, float normal) {
    const float tolerance = 1e-5F * normal;
    const bool above =
        distances[0] > tolerance || distances[1] > tolerance || distances[2] > tolerance;
    const bool below =
        distances[0] < -tolerance || distances[1] < -tolerance || distances[2] < -tolerance;
    return above && below;
}

/// The part of the line where two planes meet that a triangle covers, as times along `axis` from
/// the origin of the line's projection: the triangle's corners projected, and its signed distances
/// to the other's plane. Gives the interval's ends.
void interval(const std::array<vec3_t, 3>& t, const std::array<float, 3>& d, vec3_t axis,
              float& low, float& high) {
    std::array<float, 3> p{ dot(t[0], axis), dot(t[1], axis), dot(t[2], axis) };
    // The corner alone on its side is the one the others cross toward.
    std::size_t alone = 0;
    if ((d[0] > 0.0F) == (d[1] > 0.0F)) {
        alone = 2;
    } else if ((d[0] > 0.0F) == (d[2] > 0.0F)) {
        alone = 1;
    }
    const std::size_t a = (alone + 1) % 3;
    const std::size_t b = (alone + 2) % 3;
    const auto cross_at = [&](std::size_t other) {
        const float span = d[alone] - d[other];
        return std::fabs(span) > tiny ? p[alone] + ((p[other] - p[alone]) * (d[alone] / span))
                                      : p[alone];
    };
    low = cross_at(a);
    high = cross_at(b);
    if (low > high) {
        std::swap(low, high);
    }
}

/// Whether two triangles cross, Moller's way: each must straddle the other's plane, and the
/// intervals where they meet the line between the planes must overlap. Ones in one plane are
/// taken not to cross: a mesh resting flat on another touches, it isn't in it.
bool triangles_cross(const std::array<vec3_t, 3>& u, const std::array<vec3_t, 3>& v) {
    const vec3_t nu = cross(u[1] - u[0], u[2] - u[0]);
    const std::array<float, 3> dv{
        dot(nu, v[0] - u[0]),
        dot(nu, v[1] - u[0]),
        dot(nu, v[2] - u[0]),
    };
    if (!straddles(dv, length(nu))) {
        return false;
    }
    const vec3_t nv = cross(v[1] - v[0], v[2] - v[0]);
    const std::array<float, 3> du{
        dot(nv, u[0] - v[0]),
        dot(nv, u[1] - v[0]),
        dot(nv, u[2] - v[0]),
    };
    if (!straddles(du, length(nv))) {
        return false;
    }
    const vec3_t line = cross(nu, nv);
    if (dot(line, line) <= tiny * tiny) {
        return false;
    }
    float u_low = 0.0F;
    float u_high = 0.0F;
    float v_low = 0.0F;
    float v_high = 0.0F;
    interval(u, du, line, u_low, u_high);
    interval(v, dv, line, v_low, v_high);
    return u_low <= v_high && v_low <= u_high;
}

} // namespace

bool meshes_cross(std::uint32_t first, std::uint32_t second) {
    const scene_t& s = scene();
    const std::uint32_t a = s.meshes[s.entities.component[first]].geometry;
    const std::uint32_t b = s.meshes[s.entities.component[second]].geometry;
    if (a == none || b == none) {
        return false;
    }
    // Their boxes in the world first: apart, nothing crosses.
    std::array<vec3_t, 4> boxes;
    for (std::size_t i = 0; i < 2; ++i) {
        const std::uint32_t entity = i == 0 ? first : second;
        vec3_t min;
        vec3_t max;
        geometry_bounds(i == 0 ? a : b, min, max);
        world_box(entity, min, max, boxes[i * 2], boxes[(i * 2) + 1]);
    }
    if (boxes[1].x < boxes[2].x || boxes[3].x < boxes[0].x || boxes[1].y < boxes[2].y ||
        boxes[3].y < boxes[0].y || boxes[1].z < boxes[2].z || boxes[3].z < boxes[0].z) {
        return false;
    }
    mat4_t into;
    if (!inverse_affine(s.entities.world_matrix[first], into)) {
        return false;
    }
    // The second's triangles in the first's space, where the first's tree is.
    const mat4_t across = into * s.entities.world_matrix[second];
    const bvh_t& here = geometry_tree(a);
    const bvh_t& there = geometry_tree(b);
    for (std::uint32_t t = 0; t < there.triangle_count(); ++t) {
        const vec3_t* corners = there.corners(t);
        const std::array<vec3_t, 3> moved{
            transform_point(across, corners[0]),
            transform_point(across, corners[1]),
            transform_point(across, corners[2]),
        };
        const vec3_t min = least(least(moved[0], moved[1]), moved[2]);
        const vec3_t max = greatest(greatest(moved[0], moved[1]), moved[2]);
        bool found = false;
        here.overlapping(min, max, [&](std::uint32_t other) {
            if (found) {
                return;
            }
            const vec3_t* mine = here.corners(other);
            found = triangles_cross({ mine[0], mine[1], mine[2] }, moved);
        });
        if (found) {
            return true;
        }
    }
    return false;
}

} // namespace fbe::scene
