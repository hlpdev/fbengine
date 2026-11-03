// Spheres swept against triangles' fronts, their edges and corners, and ellipsoids, as
// collisions and picks sweep them.

#include "scene/sweep.hpp"

#include "scene/math.hpp"

#include <cmath>
#include <cstddef>
#include <utility>

namespace fbe::scene {

namespace {

constexpr float tiny = 1e-6F;

/// A vector in double, for the terms that cancel in float.
struct wide_t {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

wide_t wide(vec3_t v) {
    return { .x = v.x, .y = v.y, .z = v.z };
}

wide_t operator-(wide_t a, wide_t b) {
    return { .x = a.x - b.x, .y = a.y - b.y, .z = a.z - b.z };
}

double dot(wide_t a, wide_t b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

/// The smallest root of a t^2 + b t + c from 0 up to `below`, if there's one.
bool lowest_root(double a, double b, double c, double below, double& root) {
    const double discriminant = (b * b) - (4.0 * a * c);
    if (discriminant < 0.0 || std::fabs(a) < 1e-24) {
        return false;
    }
    const double square = std::sqrt(discriminant);
    double first = (-b - square) / (2.0 * a);
    double second = (-b + square) / (2.0 * a);
    if (first > second) {
        std::swap(first, second);
    }
    if (first >= 0.0 && first < below) {
        root = first;
        return true;
    }
    if (second >= 0.0 && second < below) {
        root = second;
        return true;
    }
    return false;
}

} // namespace

bool inside(const triangle_t& triangle, vec3_t p) {
    const auto& c = triangle.corners;
    // A hair past each edge counts, so a point on an edge or a corner its neighbours share is in
    // one of them, whatever rounding does: no cracks between a mesh's triangles.
    const auto within = [&](std::size_t from, std::size_t to) {
        const vec3_t edge = c[to] - c[from];
        return dot(cross(edge, p - c[from]), triangle.normal) >= -1e-5F * dot(edge, edge);
    };
    return within(0, 1) && within(1, 2) && within(2, 0);
}

bool plane_under(const mat4_t& matrix, vec3_t min, vec3_t max,
                 std::array<std::array<vec3_t, 3>, 2>& triangles) {
    mat4_t into;
    if (!inverse_affine(matrix, into)) {
        return false;
    }
    // The box in the plane's own space, and the square of the plane under it, a little wider, so a
    // line straight down has some of the plane under it.
    vec3_t low = transform_point(into, min);
    vec3_t high = low;
    for (unsigned corner = 1; corner < 8; ++corner) {
        const vec3_t p = transform_point(into, box_corner(min, max, corner));
        low = vec3_t(std::fmin(low.x, p.x), std::fmin(low.y, p.y), std::fmin(low.z, p.z));
        high = vec3_t(std::fmax(high.x, p.x), std::fmax(high.y, p.y), std::fmax(high.z, p.z));
    }
    low = low - vec3_t(1.0F, 0.0F, 1.0F);
    high = high + vec3_t(1.0F, 0.0F, 1.0F);
    const vec3_t near_left = transform_point(matrix, vec3_t(low.x, 0.0F, low.z));
    const vec3_t far_left = transform_point(matrix, vec3_t(low.x, 0.0F, high.z));
    const vec3_t far_right = transform_point(matrix, vec3_t(high.x, 0.0F, high.z));
    const vec3_t near_right = transform_point(matrix, vec3_t(high.x, 0.0F, low.z));
    // Counterclockwise seen from +Y.
    triangles = {
        {
            { near_left, far_left, far_right },
            { near_left, far_right, near_right },
        },
    };
    return true;
}

bool make_triangle(std::array<vec3_t, 3> corners, bool mirrored, triangle_t& triangle) {
    if (mirrored) {
        std::swap(corners[1], corners[2]);
    }
    const vec3_t normal = normalize(cross(corners[1] - corners[0], corners[2] - corners[0]));
    if (dot(normal, normal) == 0.0F) {
        return false;
    }
    triangle.corners = corners;
    triangle.normal = normal;
    return true;
}

bool in_face(vec3_t c, float r, const triangle_t& triangle, vec3_t& foot, float& distance) {
    distance = dot(triangle.normal, c - triangle.corners[0]);
    if (distance < 0.0F || distance >= r) {
        return false;
    }
    foot = c - triangle.normal * distance;
    return inside(triangle, foot);
}

void sweep_triangle(vec3_t c, vec3_t v, float r, const triangle_t& triangle, hit_t& hit,
                    bool& found, bool* leaving_face) {
    const vec3_t n = triangle.normal;
    const float distance = dot(n, c - triangle.corners[0]);
    // A mover whose middle is behind a triangle passes out through it, and one going away from
    // its front, further than its radius, doesn't touch it.
    if (distance < 0.0F) {
        return;
    }
    const float approach = dot(n, v);
    float first = 0.0F;
    bool embedded = false;
    // Within its radius of the plane and going away, it can't touch the face, but it can still
    // go sideways onto an edge or a corner.
    bool leaving = false;
    if (std::fabs(approach) < tiny) {
        if (distance >= r) {
            return;
        }
        embedded = true;
    } else if (approach > 0.0F) {
        if (distance >= r) {
            return;
        }
        embedded = true;
        leaving = true;
        if (leaving_face != nullptr) {
            *leaving_face = true;
        }
    } else {
        first = (r - distance) / approach;
        const float last = (-r - distance) / approach;
        if (first > hit.time || last < 0.0F) {
            return;
        }
        first = std::fmax(first, 0.0F);
    }
    if (distance < r && !leaving) {
        // Already touching its plane: its face, if the middle is over it, or else its edges.
        const vec3_t under = c - n * distance;
        if (inside(triangle, under)) {
            if (0.0F <= hit.time) {
                hit = {
                    .time = 0.0F,
                    .point = under,
                    .normal = n,
                    .target = triangle.target,
                    .origin = triangle.origin,
                    .from_mesh = triangle.from_mesh,
                };
                found = true;
            }
            return;
        }
    } else if (!embedded) {
        const vec3_t on_plane = c - n * r + v * first;
        if (inside(triangle, on_plane)) {
            if (first < hit.time || !found) {
                hit = {
                    .time = first,
                    .point = on_plane,
                    .normal = n,
                    .target = triangle.target,
                    .origin = triangle.origin,
                    .from_mesh = triangle.from_mesh,
                };
                found = true;
            }
            return;
        }
    }
    // A line touches a triangle only where it crosses its face: its edges' and corners' tests
    // would find only rounding's roots, where it passes them.
    if (r <= 0.0F) {
        return;
    }
    // Its corners, then its edges, in double: for a sweep long beside a big triangle their terms
    // are far bigger than how near it passes, and a near miss would touch in float.
    const wide_t from = wide(c);
    const wide_t way = wide(v);
    const double speed = dot(way, way);
    const double reach = static_cast<double>(r) * r;
    double best = found ? hit.time : 1.0;
    bool touched = false;
    vec3_t point;
    for (const vec3_t& corner : triangle.corners) {
        const wide_t to_corner = wide(corner) - from;
        const double apart = dot(to_corner, to_corner) - reach;
        const double closing = -2.0 * dot(way, to_corner);
        // Already over a corner, or just at it, it touches it now if it's going further in, else
        // not at all: a root then would be where it leaves, which can't stop it, and at it, a
        // root at 0 (-0 in float) would hold it there whichever way it went. A touch now that's
        // already found stays, so a face's isn't taken for its neighbour's edge, whose normal
        // leans.
        if (apart <= 0.0) {
            if (closing < 0.0 && best > 0.0) {
                best = 0.0;
                point = corner;
                touched = true;
            }
            continue;
        }
        double root = 0.0;
        if (lowest_root(speed, closing, apart, best, root)) {
            best = root;
            point = corner;
            touched = true;
        }
    }
    for (std::size_t k = 0; k < 3; ++k) {
        const vec3_t start = triangle.corners[k];
        const vec3_t edge = triangle.corners[(k + 1) % 3] - start;
        const wide_t across = wide(triangle.corners[(k + 1) % 3]) - wide(start);
        const wide_t to_start = wide(start) - from;
        const double length = dot(across, across);
        const double along_v = dot(across, way);
        const double along_start = dot(across, to_start);
        const double a = (length * -speed) + (along_v * along_v);
        const double b = (length * 2.0 * dot(way, to_start)) - (2.0 * along_v * along_start);
        const double cc =
            (length * (reach - dot(to_start, to_start))) + (along_start * along_start);
        // Already over the edge's line, or just at it: where it's over the edge itself, it
        // touches it now if it's going further in, else not at all; past the edge's ends, its
        // corners say.
        if (cc >= 0.0) {
            const double fraction = -along_start / length;
            if (fraction >= 0.0 && fraction <= 1.0 && b > 0.0 && best > 0.0) {
                best = 0.0;
                point = start + edge * static_cast<float>(fraction);
                touched = true;
            }
            continue;
        }
        double root = 0.0;
        if (lowest_root(a, b, cc, best, root)) {
            const double fraction = ((along_v * root) - along_start) / length;
            if (fraction >= 0.0 && fraction <= 1.0) {
                best = root;
                point = start + edge * static_cast<float>(fraction);
                touched = true;
            }
        }
    }
    if (touched) {
        const auto time = static_cast<float>(best);
        const vec3_t at = c + v * time;
        hit = {
            .time = time,
            .point = point,
            .normal = normalize(at - point),
            .target = triangle.target,
            .origin = triangle.origin,
            .from_mesh = triangle.from_mesh,
        };
        found = true;
    }
}

void sweep_ellipsoid(vec3_t c, vec3_t v, float r, const ellipsoid_t& ellipsoid, hit_t& hit,
                     bool& found) {
    const vec3_t inverse(1.0F / ellipsoid.radii.x, 1.0F / ellipsoid.radii.y,
                         1.0F / ellipsoid.radii.z);
    const vec3_t offset = c - ellipsoid.center;
    const vec3_t q = offset * inverse;
    const vec3_t d = v * inverse;
    const float start = dot(q, q) - 1.0F;
    float time = 0.0F;
    double root = 0.0;
    if (start <= 0.0F) {
        // Inside already, or just at it: it touches only going further in, else a root at 0 would
        // hold it there whichever way it went.
        if (dot(q, d) >= 0.0F) {
            return;
        }
    } else if (!lowest_root(dot(d, d), 2.0F * dot(q, d), start, found ? hit.time : 1.0F + tiny,
                            root) ||
               root > 1.0) {
        return;
    } else {
        time = static_cast<float>(root);
    }
    const vec3_t at = c + v * time;
    const vec3_t from_center = at - ellipsoid.center;
    const vec3_t normal = normalize(from_center * inverse * inverse);
    hit = {
        .time = time,
        .point = at - normal * r,
        .normal = normal,
        .target = ellipsoid.target,
        .origin = triangle_origin_t(),
        .from_mesh = false,
    };
    found = true;
}

} // namespace fbe::scene
