#pragma once

// A tree of boxes over a geometry's triangles, in the geometry's own space, which line of sight,
// picks, and collisions search instead of every triangle: each box holds the boxes or the few
// triangles under it, split at the median of their centers along their longest side.

#include "scene/math.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace fbe::scene {

struct surface_t;

/// A vector's x, y, or z, by 0, 1, or 2.
inline float component(vec3_t v, int axis) {
    switch (axis) {
    case 0:
        return v.x;
    case 1:
        return v.y;
    default:
        return v.z;
    }
}

/// A segment from a point along a vector, with the vector's inverse worked out once, for testing it
/// against many boxes.
struct segment_t {
    vec3_t from;
    vec3_t inverse;

    segment_t(vec3_t a, vec3_t delta)
        : from(a), inverse(1.0F / delta.x, 1.0F / delta.y, 1.0F / delta.z) {}
};

/// Narrows the part of a segment, from `enter` to `leave`, to what's within one axis's slab of a
/// box, and whether any is left. Not moving along the axis, its inverse is infinite, so it's in
/// the slab everywhere or nowhere; on the slab's side, 0 times infinity leaves the part as it was.
inline bool within_slab(float from, float inverse, float low, float high, float& enter,
                        float& leave) {
    float t0 = (low - from) * inverse;
    float t1 = (high - from) * inverse;
    if (t0 > t1) {
        std::swap(t0, t1);
    }
    enter = t0 > enter ? t0 : enter;
    leave = t1 < leave ? t1 : leave;
    return enter <= leave;
}

/// Whether a segment passes through the box anywhere from 0 to `until` along it, and how far
/// along, `enter`, it goes in.
inline bool crosses_box(const segment_t& segment, vec3_t min, vec3_t max, float until,
                        float& enter) {
    enter = 0.0F;
    float leave = until;
    return within_slab(segment.from.x, segment.inverse.x, min.x, max.x, enter, leave) &&
           within_slab(segment.from.y, segment.inverse.y, min.y, max.y, enter, leave) &&
           within_slab(segment.from.z, segment.inverse.z, min.z, max.z, enter, leave);
}

/// Whether a segment passes through the box anywhere from 0 to `until` along it.
inline bool crosses_box(const segment_t& segment, vec3_t min, vec3_t max, float until) {
    float enter = 0.0F;
    return crosses_box(segment, min, max, until, enter);
}

/// Where along the segment from `a` along `delta`, 0 to 1, it crosses a triangle's three
/// `corners`, Moller and Trumbore's way, or a negative for nowhere; `front` says it met the
/// triangle's front.
float crosses_triangle(vec3_t a, vec3_t delta, const vec3_t* corners, bool& front);

/// Where a triangle came from: its surface and its index there.
struct triangle_origin_t {
    std::uint32_t surface = 0;
    std::uint32_t triangle = 0;
};

/// A hit along a segment: how far along it, 0 to 1, the triangle's normal, facing the segment's
/// start, and the triangle.
struct segment_hit_t {
    float time = std::numeric_limits<float>::infinity();
    vec3_t normal;
    triangle_origin_t origin;
};

class bvh_t {
public:
    /// Builds the tree over the surfaces' triangles.
    void build(const std::vector<surface_t>& surfaces);

    /// Fits the tree's boxes to the surfaces' triangles again, which are the ones it was built
    /// over, moved: far quicker than building it again.
    void refit(const std::vector<surface_t>& surfaces);

    /// Whether anything crosses the segment from `a` to `b`, from either side.
    [[nodiscard]] bool blocks(vec3_t a, vec3_t b) const;

    /// The nearest triangle the segment from `a` to `b` crosses, from either side, or, with
    /// `front_only`, only those it meets from their front; false for none.
    bool nearest(vec3_t a, vec3_t b, bool front_only, segment_hit_t& hit) const;

    /// Calls `visit` with each triangle's index whose box touches the box from `min` to `max`.
    template <typename Visit>
    void overlapping(vec3_t min, vec3_t max, Visit&& visit) const;

    /// Calls `visit` with each triangle's index in a leaf whose box the segment from `a` to `b`
    /// passes within `grow` of, along each axis, before the part of it `until()` gives, 0 to 1,
    /// which `visit` may lower as it finds hits; and how far along, 0 to 1, it comes within
    /// `grow` of the leaf.
    template <typename Until, typename Visit>
    void along(vec3_t a, vec3_t b, vec3_t grow, Until&& until, Visit&& visit) const;

    /// A triangle's three corners.
    [[nodiscard]] const vec3_t* corners(std::uint32_t triangle) const {
        return &m_corners[static_cast<std::size_t>(triangle) * 3];
    }
    [[nodiscard]] triangle_origin_t origin(std::uint32_t triangle) const {
        return m_origins[triangle];
    }
    [[nodiscard]] std::uint32_t triangle_count() const {
        return static_cast<std::uint32_t>(m_origins.size());
    }

private:
    struct node_t {
        vec3_t min;
        vec3_t max;
        /// A leaf's first triangle in m_order, and how many; an inner node has 0, its first child
        /// right after it and its second at `second`.
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        std::uint32_t second = 0;
    };

    std::uint32_t split(std::uint32_t first, std::uint32_t count, std::vector<vec3_t>& centers);

    std::vector<vec3_t> m_corners;
    std::vector<triangle_origin_t> m_origins;
    std::vector<std::uint32_t> m_order;
    std::vector<node_t> m_nodes;
};

template <typename Visit>
void bvh_t::overlapping(vec3_t min, vec3_t max, Visit&& visit) const {
    if (m_nodes.empty()) {
        return;
    }
    // Kept for the next search, so a search doesn't allocate.
    thread_local std::vector<std::uint32_t> stack;
    stack.assign(1, 0);
    while (!stack.empty()) {
        const node_t& node = m_nodes[stack.back()];
        const std::uint32_t at = stack.back();
        stack.pop_back();
        if (node.max.x < min.x || node.min.x > max.x || node.max.y < min.y || node.min.y > max.y ||
            node.max.z < min.z || node.min.z > max.z) {
            continue;
        }
        if (node.count > 0) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                // Each of the leaf's own, by its corners' box.
                const vec3_t* c = corners(m_order[i]);
                if (std::fmax(std::fmax(c[0].x, c[1].x), c[2].x) < min.x ||
                    std::fmin(std::fmin(c[0].x, c[1].x), c[2].x) > max.x ||
                    std::fmax(std::fmax(c[0].y, c[1].y), c[2].y) < min.y ||
                    std::fmin(std::fmin(c[0].y, c[1].y), c[2].y) > max.y ||
                    std::fmax(std::fmax(c[0].z, c[1].z), c[2].z) < min.z ||
                    std::fmin(std::fmin(c[0].z, c[1].z), c[2].z) > max.z) {
                    continue;
                }
                visit(m_order[i]);
            }
        } else {
            stack.push_back(node.second);
            stack.push_back(at + 1);
        }
    }
}

template <typename Until, typename Visit>
void bvh_t::along(vec3_t a, vec3_t b, vec3_t grow, Until&& until, Visit&& visit) const {
    if (m_nodes.empty()) {
        return;
    }
    const segment_t segment(a, b - a);
    std::vector<std::uint32_t> stack{ 0 };
    while (!stack.empty()) {
        const std::uint32_t at = stack.back();
        stack.pop_back();
        const node_t& node = m_nodes[at];
        float enter = 0.0F;
        if (!crosses_box(segment, node.min - grow, node.max + grow, until(), enter)) {
            continue;
        }
        if (node.count > 0) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                visit(m_order[i], enter);
            }
        } else {
            stack.push_back(node.second);
            stack.push_back(at + 1);
        }
    }
}

} // namespace fbe::scene
