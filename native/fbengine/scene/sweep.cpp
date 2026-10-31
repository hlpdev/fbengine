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

/// The smallest root of a t^2 + b t + c from 0 up to `below`, if there's one.
bool lowest_root(float a, float b, float c, float below, float& root) {
    const float discriminant = (b * b) - (4.0F * a * c);
    if (discriminant < 0.0F || std::fabs(a) < 1e-12F) {
        return false;
    }
    const float square = std::sqrt(discriminant);
    float first = (-b - square) / (2.0F * a);
    float second = (-b + square) / (2.0F * a);
    if (first > second) {
        std::swap(first, second);
    }
    if (first >= 0.0F && first < below) {
        root = first;
        return true;
    }
    if (second >= 0.0F && second < below) {
        root = second;
        return true;
    }
    return false;
}

/// Whether a point on a triangle's plane is inside it.
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

} // namespace

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
                    bool& found) {
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
    // Its corners, then its edges.
    const float speed = dot(v, v);
    float best = found ? hit.time : 1.0F;
    bool touched = false;
    vec3_t point;
    for (const vec3_t& corner : triangle.corners) {
        const float apart = dot(corner - c, corner - c) - (r * r);
        const float closing = 2.0F * dot(v, c - corner);
        // Already over a corner, it touches it now if it's going further in, else not at all: a
        // root then would be where it leaves, which can't stop it. A touch now that's already
        // found stays, so a face's isn't taken for its neighbour's edge, whose normal leans.
        if (apart < 0.0F) {
            if (closing < 0.0F && best > 0.0F) {
                best = 0.0F;
                point = corner;
                touched = true;
            }
            continue;
        }
        float root = 0.0F;
        if (lowest_root(speed, closing, apart, best, root)) {
            best = root;
            point = corner;
            touched = true;
        }
    }
    for (std::size_t k = 0; k < 3; ++k) {
        const vec3_t start = triangle.corners[k];
        const vec3_t edge = triangle.corners[(k + 1) % 3] - start;
        const vec3_t to_start = start - c;
        const float length = dot(edge, edge);
        const float along_v = dot(edge, v);
        const float along_start = dot(edge, to_start);
        const float a = (length * -speed) + (along_v * along_v);
        const float b = (length * 2.0F * dot(v, to_start)) - (2.0F * along_v * along_start);
        const float cc =
            (length * ((r * r) - dot(to_start, to_start))) + (along_start * along_start);
        // Already over the edge's line: where it's over the edge itself, it touches it now if
        // it's going further in, else not at all; past the edge's ends, its corners say.
        if (cc > 0.0F) {
            const float fraction = -along_start / length;
            if (fraction >= 0.0F && fraction <= 1.0F && b > 0.0F && best > 0.0F) {
                best = 0.0F;
                point = start + edge * fraction;
                touched = true;
            }
            continue;
        }
        float root = 0.0F;
        if (lowest_root(a, b, cc, best, root)) {
            const float fraction = ((along_v * root) - along_start) / length;
            if (fraction >= 0.0F && fraction <= 1.0F) {
                best = root;
                point = start + edge * fraction;
                touched = true;
            }
        }
    }
    if (touched) {
        const vec3_t at = c + v * best;
        hit = {
            .time = best,
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
    if (start < 0.0F) {
        // Inside already: it touches only going further in.
        if (dot(q, d) >= 0.0F) {
            return;
        }
    } else if (!lowest_root(dot(d, d), 2.0F * dot(q, d), start, found ? hit.time : 1.0F + tiny,
                            time) ||
               time > 1.0F) {
        return;
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
