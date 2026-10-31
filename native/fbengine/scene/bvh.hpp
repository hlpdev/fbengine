#pragma once

// A tree of boxes over a geometry's triangles, in the geometry's own space, which line of sight,
// picks, and collisions search instead of every triangle: each box holds the boxes or the few
// triangles under it, split at the median of their centers along their longest side.

#include "scene/math.hpp"

#include <cstdint>
#include <limits>
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

    /// Whether anything crosses the segment from `a` to `b`, from either side.
    [[nodiscard]] bool blocks(vec3_t a, vec3_t b) const;

    /// The nearest triangle the segment from `a` to `b` crosses, from either side, or, with
    /// `front_only`, only those it meets from their front; false for none.
    bool nearest(vec3_t a, vec3_t b, bool front_only, segment_hit_t& hit) const;

    /// Calls `visit` with each triangle's index whose box touches the box from `min` to `max`.
    template <typename Visit>
    void overlapping(vec3_t min, vec3_t max, Visit&& visit) const;

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
    std::vector<std::uint32_t> stack{ 0 };
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
                visit(m_order[i]);
            }
        } else {
            stack.push_back(node.second);
            stack.push_back(at + 1);
        }
    }
}

} // namespace fbe::scene
