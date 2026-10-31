// The tree of boxes over a geometry's triangles, and segments through it.

#include "scene/bvh.hpp"

#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace fbe::scene {

namespace {

/// At most this many triangles to a leaf.
constexpr std::uint32_t leaf_size = 8;

vec3_t vec(const fbe_vec3_t& v) {
    return { v.x, v.y, v.z };
}

vec3_t least(vec3_t a, vec3_t b) {
    return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) };
}

vec3_t greatest(vec3_t a, vec3_t b) {
    return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) };
}

/// How far past a triangle's edge, as a part of the triangle, a crossing still counts: so a segment
/// through an edge two triangles share crosses one of them, however it rounds.
constexpr float edge_slack = 1e-5F;

} // namespace

float crosses_triangle(vec3_t a, vec3_t delta, const vec3_t* corners, bool& front) {
    const vec3_t edge1 = corners[1] - corners[0];
    const vec3_t edge2 = corners[2] - corners[0];
    const vec3_t across = cross(delta, edge2);
    const float determinant = dot(edge1, across);
    if (std::fabs(determinant) < 1e-12F) {
        return -1.0F;
    }
    const float inverse = 1.0F / determinant;
    const vec3_t offset = a - corners[0];
    const float u = dot(offset, across) * inverse;
    if (u < -edge_slack || u > 1.0F + edge_slack) {
        return -1.0F;
    }
    const vec3_t up = cross(offset, edge1);
    const float v = dot(delta, up) * inverse;
    if (v < -edge_slack || u + v > 1.0F + edge_slack) {
        return -1.0F;
    }
    const float time = dot(edge2, up) * inverse;
    if (time < 0.0F || time > 1.0F) {
        return -1.0F;
    }
    // A segment meeting a counterclockwise triangle's front goes against its normal.
    front = determinant > 0.0F;
    return time;
}

void bvh_t::build(const std::vector<surface_t>& surfaces) {
    m_corners.clear();
    m_origins.clear();
    m_order.clear();
    m_nodes.clear();
    for (std::size_t s = 0; s < surfaces.size(); ++s) {
        const surface_t& surface = surfaces[s];
        for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
            for (std::size_t k = 0; k < 3; ++k) {
                m_corners.push_back(vec(surface.vertices[surface.indices[i + k]].position));
            }
            const auto surface_index = static_cast<std::uint32_t>(s);
            const auto triangle_index = static_cast<std::uint32_t>(i / 3);
            m_origins.push_back({ .surface = surface_index, .triangle = triangle_index });
        }
    }
    const auto count = static_cast<std::uint32_t>(m_origins.size());
    if (count == 0) {
        return;
    }
    std::vector<vec3_t> centers(count);
    m_order.resize(count);
    for (std::uint32_t t = 0; t < count; ++t) {
        const vec3_t* at = corners(t);
        const vec3_t sum = at[0] + at[1] + at[2];
        centers[t] = sum * (1.0F / 3.0F);
        m_order[t] = t;
    }
    m_nodes.reserve(((static_cast<std::size_t>(count) / leaf_size) + 1) * 2);
    split(0, count, centers);
}

std::uint32_t bvh_t::split(std::uint32_t first, std::uint32_t count, std::vector<vec3_t>& centers) {
    const auto at = static_cast<std::uint32_t>(m_nodes.size());
    m_nodes.emplace_back();
    vec3_t min = corners(m_order[first])[0];
    vec3_t max = min;
    vec3_t center_min = centers[m_order[first]];
    vec3_t center_max = center_min;
    for (std::uint32_t i = first; i < first + count; ++i) {
        const vec3_t* triangle = corners(m_order[i]);
        for (std::size_t k = 0; k < 3; ++k) {
            min = least(min, triangle[k]);
            max = greatest(max, triangle[k]);
        }
        center_min = least(center_min, centers[m_order[i]]);
        center_max = greatest(center_max, centers[m_order[i]]);
    }
    m_nodes[at].min = min;
    m_nodes[at].max = max;
    if (count <= leaf_size) {
        m_nodes[at].first = first;
        m_nodes[at].count = count;
        return at;
    }
    const vec3_t size = center_max - center_min;
    int axis = 2;
    if (size.x >= size.y && size.x >= size.z) {
        axis = 0;
    } else if (size.y >= size.z) {
        axis = 1;
    }
    const std::uint32_t half = count / 2;
    const auto begin = m_order.begin() + first;
    std::nth_element(begin, begin + half, begin + count, [&](std::uint32_t a, std::uint32_t b) {
        return component(centers[a], axis) < component(centers[b], axis);
    });
    split(first, half, centers);
    const std::uint32_t second = split(first + half, count - half, centers);
    m_nodes[at].second = second;
    return at;
}

void bvh_t::refit(const std::vector<surface_t>& surfaces) {
    for (std::size_t t = 0; t < m_origins.size(); ++t) {
        const surface_t& surface = surfaces[m_origins[t].surface];
        const std::size_t first = static_cast<std::size_t>(m_origins[t].triangle) * 3;
        for (std::size_t k = 0; k < 3; ++k) {
            m_corners[(t * 3) + k] = vec(surface.vertices[surface.indices[first + k]].position);
        }
    }
    // A node's children come after it, so going back fits every child before its parent.
    for (std::size_t at = m_nodes.size(); at-- > 0;) {
        node_t& node = m_nodes[at];
        if (node.count > 0) {
            node.min = corners(m_order[node.first])[0];
            node.max = node.min;
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                const vec3_t* triangle = corners(m_order[i]);
                for (std::size_t k = 0; k < 3; ++k) {
                    node.min = least(node.min, triangle[k]);
                    node.max = greatest(node.max, triangle[k]);
                }
            }
        } else {
            const node_t& first = m_nodes[at + 1];
            const node_t& second = m_nodes[node.second];
            node.min = least(first.min, second.min);
            node.max = greatest(first.max, second.max);
        }
    }
}

bool bvh_t::blocks(vec3_t a, vec3_t b) const {
    segment_hit_t hit;
    return nearest(a, b, false, hit);
}

bool bvh_t::nearest(vec3_t a, vec3_t b, bool front_only, segment_hit_t& hit) const {
    if (m_nodes.empty()) {
        return false;
    }
    const vec3_t delta = b - a;
    const segment_t segment(a, delta);
    bool found = false;
    std::vector<std::uint32_t> stack{ 0 };
    while (!stack.empty()) {
        const std::uint32_t at = stack.back();
        stack.pop_back();
        const node_t& node = m_nodes[at];
        if (!crosses_box(segment, node.min, node.max, found ? hit.time : 1.0F)) {
            continue;
        }
        if (node.count == 0) {
            stack.push_back(node.second);
            stack.push_back(at + 1);
            continue;
        }
        for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
            const std::uint32_t triangle = m_order[i];
            bool front = false;
            const float time = crosses_triangle(a, delta, corners(triangle), front);
            if (time < 0.0F || (front_only && !front) || (found && time >= hit.time)) {
                continue;
            }
            const vec3_t* at_corners = corners(triangle);
            const vec3_t normal =
                normalize(cross(at_corners[1] - at_corners[0], at_corners[2] - at_corners[0]));
            hit.time = time;
            hit.normal = front ? normal : -normal;
            hit.origin = m_origins[triangle];
            found = true;
        }
    }
    return found;
}

} // namespace fbe::scene
