// Occlusion: what's flagged as an occluder rasterized on the CPU, for each view, into a small
// buffer of how near what's in front is, and each thing's box tested against it, so what's wholly
// behind isn't drawn. On the CPU, so it's the same on any GPU, a frame's never late, and it saves
// what would have gone into drawing it. It errs toward drawing: a pixel keeps the farthest of
// what's in it, by what rounding can put that out by too; along a triangle's outer edges, and where
// what's next to it isn't drawn, only what's wholly in a pixel fills it; and a box's test goes a
// pixel past it each way, and its depth nearer by what rounding can put that out by.

#include "fbengine.h"
#include "render/render.hpp"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbe::render {

namespace {

using scene::mat4_t;
using scene::none;
using scene::vec3_t;

constexpr int buffer_width = 256;
constexpr int buffer_height = 144;
constexpr int block_side = 8;
constexpr int blocks_across = buffer_width / block_side;
constexpr int blocks_down = buffer_height / block_side;
/// The most triangles a view rasterizes, the nearest occluders first, and the most pixels it looks
/// at for them: past either, what's left doesn't occlude.
constexpr std::size_t most_triangles = 16384;
constexpr std::int64_t most_pixels = 2097152;
/// How many of a float's last places a place's way through its matrices can round off by, and what
/// a depth's kept off by at the least, as a part of it.
constexpr float rounded_places = 16.0F * std::numeric_limits<float>::epsilon();
constexpr float least_depth_part = 1e-4F;
constexpr std::uint32_t no_neighbor = UINT32_MAX;

/// A corner in clip space.
struct clip_t {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;
};

clip_t clip_of(const mat4_t& m, vec3_t p) {
    return {
        .x = (m.at(0, 0) * p.x) + (m.at(1, 0) * p.y) + (m.at(2, 0) * p.z) + m.at(3, 0),
        .y = (m.at(0, 1) * p.x) + (m.at(1, 1) * p.y) + (m.at(2, 1) * p.z) + m.at(3, 1),
        .z = (m.at(0, 2) * p.x) + (m.at(1, 2) * p.y) + (m.at(2, 2) * p.z) + m.at(3, 2),
        .w = (m.at(0, 3) * p.x) + (m.at(1, 3) * p.y) + (m.at(2, 3) * p.z) + m.at(3, 3),
    };
}

/// What rounding can put a place's clip space out by, of how far it reaches from the origin and
/// the eye: across and up, the larger, in depth, and in w.
struct rounding_t {
    float across = 0.0F;
    float z = 0.0F;
    float w = 0.0F;
};

/// A corner in the buffer's pixels, and its depth, reversed, as drawing's is, and how many pixels
/// and how much depth rounding can put it out by.
struct corner_t {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float off_pixels = 0.0F;
    float off_depth = 0.0F;
};

corner_t corner_of(const clip_t& clip, const rounding_t& rounding) {
    const float through = std::fabs(clip.w);
    const float x = clip.x / clip.w;
    const float y = clip.y / clip.w;
    const float z = clip.z / clip.w;
    const float across = std::fmax(std::fabs(x), std::fabs(y));
    return {
        .x = (x + 1.0F) * 0.5F * buffer_width,
        .y = (y + 1.0F) * 0.5F * buffer_height,
        .z = z,
        .off_pixels = (rounding.across + (across * rounding.w)) / through * 0.5F * buffer_width,
        .off_depth = ((rounding.z + (std::fabs(z) * rounding.w)) / through) +
                     (std::fabs(z) * least_depth_part),
    };
}

/// A geometry's surface's triangles' neighbors, three to a triangle: which one shares each edge,
/// no_neighbor for none, or for more than one. Kept while its triangles are, by its handle.
struct kept_edges_t {
    std::uint64_t shape_version = UINT64_MAX;
    std::vector<std::vector<std::uint32_t>> neighbors;
};

std::uint64_t& edges_worked_out() {
    static std::uint64_t count = 0;
    return count;
}

std::unordered_map<std::uint64_t, kept_edges_t>& kept_edges() {
    static std::unordered_map<std::uint64_t, kept_edges_t> made;
    return made;
}

const std::vector<std::vector<std::uint32_t>>& neighbors_of(std::uint32_t geometry) {
    const scene::scene_t& s = scene::scene();
    const scene::geometry_t& found = s.geometries[geometry];
    kept_edges_t& kept = kept_edges()[s.geometry_slots.handle(geometry)];
    if (kept.shape_version == found.shape_version &&
        kept.neighbors.size() == found.surfaces.size()) {
        return kept.neighbors;
    }
    ++edges_worked_out();
    kept.shape_version = found.shape_version;
    kept.neighbors.clear();
    struct edge_t {
        std::uint32_t first = no_neighbor;
        std::uint32_t second = no_neighbor;
        std::uint32_t count = 0;
    };
    std::unordered_map<std::uint64_t, edge_t> edges;
    const auto key_of = [](std::uint32_t a, std::uint32_t b) {
        return (std::uint64_t{ std::min(a, b) } << 32U) | std::max(a, b);
    };
    for (const scene::surface_t& surface : found.surfaces) {
        edges.clear();
        const std::vector<std::uint32_t>& indices = surface.indices;
        const std::size_t triangles = indices.size() / 3;
        for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
            for (std::size_t corner = 0; corner < 3; ++corner) {
                edge_t& edge = edges[key_of(indices[(triangle * 3) + corner],
                                            indices[(triangle * 3) + ((corner + 1) % 3)])];
                (edge.count == 0 ? edge.first : edge.second) = static_cast<std::uint32_t>(triangle);
                ++edge.count;
            }
        }
        std::vector<std::uint32_t> neighbors(triangles * 3, no_neighbor);
        for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const edge_t& edge = edges[key_of(indices[(triangle * 3) + corner],
                                                  indices[(triangle * 3) + ((corner + 1) % 3)])];
                if (edge.count == 2) {
                    neighbors[(triangle * 3) + corner] =
                        edge.first == triangle ? edge.second : edge.first;
                }
            }
        }
        kept.neighbors.push_back(std::move(neighbors));
    }
    return kept.neighbors;
}

/// What a view's occluders left: how near what's in front is in each pixel, 0 for nothing, and in
/// each block of them the farthest; what rounding goes by; and how many pixels it's looked at.
struct buffer_t {
    mat4_t to_clip;
    std::vector<float> depth;
    std::array<float, static_cast<std::size_t>(blocks_across* blocks_down)> farthest{};
    /// How long to_clip's rows are, of a place's reach; and its depth's own, which is any place's.
    float across_per = 0.0F;
    float z_per = 0.0F;
    float w_per = 0.0F;
    float z_alone = 0.0F;
    float eye_reach = 0.0F;
    std::int64_t pixels = 0;
    std::vector<clip_t> clips;
    std::vector<std::uint8_t> drawn;
};

buffer_t& buffer() {
    static buffer_t made;
    return made;
}

rounding_t rounding_of(const buffer_t& from, float reach) {
    const float off = rounded_places * (reach + from.eye_reach);
    return {
        .across = off * from.across_per,
        .z = (off * from.z_per) + (rounded_places * from.z_alone),
        .w = off * from.w_per,
    };
}

/// Fills what a triangle wholly covers, of pixels whose depth it's nearer in, at the farthest of
/// its depth in each. An edge that isn't shared with what's drawn has to have the whole pixel
/// inside it.
void fill_triangle(buffer_t& into, corner_t a, corner_t b, corner_t c,
                   const std::array<bool, 3>& shared) {
    const float area = ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
    if (area == 0.0F || !std::isfinite(area)) {
        return;
    }
    const float sign = area > 0.0F ? 1.0F : -1.0F;
    const float from_x = std::floor(std::min({ a.x, b.x, c.x }));
    const float to_x = std::ceil(std::max({ a.x, b.x, c.x }));
    const float from_y = std::floor(std::min({ a.y, b.y, c.y }));
    const float to_y = std::ceil(std::max({ a.y, b.y, c.y }));
    const int left = static_cast<int>(std::clamp(from_x, 0.0F, float{ buffer_width }));
    const int right = static_cast<int>(std::clamp(to_x, 0.0F, float{ buffer_width }));
    const int top = static_cast<int>(std::clamp(from_y, 0.0F, float{ buffer_height }));
    const int bottom = static_cast<int>(std::clamp(to_y, 0.0F, float{ buffer_height }));
    into.pixels += std::int64_t{ right - left } * (bottom - top);
    // Each edge as a line: how far inside it a point is, positive inside.
    const float past_a = ((a.x * b.y) - (a.y * b.x)) * sign;
    const float past_b = ((b.x * c.y) - (b.y * c.x)) * sign;
    const float past_c = ((c.x * a.y) - (c.y * a.x)) * sign;
    const std::array<float, 3> line_a{ (a.y - b.y) * sign, (b.x - a.x) * sign, past_a };
    const std::array<float, 3> line_b{ (b.y - c.y) * sign, (c.x - b.x) * sign, past_b };
    const std::array<float, 3> line_c{ (c.y - a.y) * sign, (a.x - c.x) * sign, past_c };
    const std::array lines{ line_a, line_b, line_c };
    std::array<float, 3> across{};
    for (std::size_t edge = 0; edge < 3; ++edge) {
        across[edge] = shared[edge] ? 0.0F : std::fabs(lines[edge][0]) + std::fabs(lines[edge][1]);
    }
    const auto plane_x = [&](float at_a, float at_b, float at_c) {
        return (((at_b - at_a) * (c.y - a.y)) - ((at_c - at_a) * (b.y - a.y))) / area;
    };
    const auto plane_y = [&](float at_a, float at_b, float at_c) {
        return (((at_c - at_a) * (b.x - a.x)) - ((at_b - at_a) * (c.x - a.x))) / area;
    };
    // Its depth's a plane across the buffer; a pixel keeps the farthest of it, half a pixel off.
    const float per_x = plane_x(a.z, b.z, c.z);
    const float per_y = plane_y(a.z, b.z, c.z);
    const float rising = std::fabs(per_x) + std::fabs(per_y);
    // And past what rounding can have put its corners out by, which is as far between them as they
    // are, so what's at a corner far off the view's only there; and in working this out at all.
    const float off_at_a = a.off_pixels;
    const float off_per_x = plane_x(a.off_pixels, b.off_pixels, c.off_pixels);
    const float off_per_y = plane_y(a.off_pixels, b.off_pixels, c.off_pixels);
    const float depth_off_per_x = plane_x(a.off_depth, b.off_depth, c.off_depth);
    const float depth_off_per_y = plane_y(a.off_depth, b.off_depth, c.off_depth);
    const float largest = std::fmax(std::fmax(std::fmax(std::fabs(a.x), std::fabs(a.y)),
                                              std::fmax(std::fabs(b.x), std::fabs(b.y))),
                                    std::fmax(std::fabs(c.x), std::fabs(c.y)));
    const float widest_x = std::fmax(std::fabs(a.x), std::fabs(a.x - float{ buffer_width }));
    const float widest_y = std::fmax(std::fabs(a.y), std::fabs(a.y - float{ buffer_height }));
    const float summing = rounded_places * (std::fabs(a.z) + (std::fabs(per_x) * widest_x) +
                                            (std::fabs(per_y) * widest_y));
    const float nearest = std::max({ a.z, b.z, c.z });
    for (int y = top; y < bottom; ++y) {
        const float py = static_cast<float>(y) + 0.5F;
        for (int x = left; x < right; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            const float apart_x = px - a.x;
            const float apart_y = py - a.y;
            const float off =
                0.5F + (rounded_places * largest) +
                std::fmax(off_at_a + (apart_x * off_per_x) + (apart_y * off_per_y), 0.0F);
            bool inside = true;
            for (std::size_t edge = 0; edge < 3 && inside; ++edge) {
                const std::array<float, 3>& line = lines[edge];
                inside = (line[0] * px) + (line[1] * py) + line[2] >= across[edge] * off;
            }
            if (!inside) {
                continue;
            }
            const float depth_off = std::fmax(
                a.off_depth + (apart_x * depth_off_per_x) + (apart_y * depth_off_per_y), 0.0F);
            const float depth = std::min(a.z + (apart_x * per_x) + (apart_y * per_y) -
                                             (rising * off) - depth_off - summing,
                                         nearest);
            float& kept = into.depth[(static_cast<std::size_t>(y) * buffer_width) +
                                     static_cast<std::size_t>(x)];
            kept = std::max(kept, depth);
        }
    }
}

/// A triangle, clipped to what's past the near plane, as a fan of what's left.
void add_triangle(buffer_t& into, const clip_t& a, const clip_t& b, const clip_t& c,
                  const std::array<bool, 3>& shared, const rounding_t& rounding) {
    std::array<clip_t, 4> clipped;
    std::array<bool, 4> edges{};
    std::size_t count = 0;
    const std::array<clip_t, 3> corners{ a, b, c };
    for (std::size_t i = 0; i < 3; ++i) {
        const clip_t& from = corners[i];
        const clip_t& to = corners[(i + 1) % 3];
        const float from_in = from.w - from.z;
        const float to_in = to.w - to.z;
        if (from_in >= 0.0F) {
            clipped[count] = from;
            edges[count] = shared[i];
            ++count;
        }
        if ((from_in >= 0.0F) != (to_in >= 0.0F)) {
            const float part = from_in / (from_in - to_in);
            clipped[count] = {
                .x = from.x + ((to.x - from.x) * part),
                .y = from.y + ((to.y - from.y) * part),
                .z = from.z + ((to.z - from.z) * part),
                .w = from.w + ((to.w - from.w) * part),
            };
            // Going out, what follows runs along the near plane, off where anything's seen.
            edges[count] = from_in >= 0.0F ? true : shared[i];
            ++count;
        }
    }
    if (count < 3 ||
        std::ranges::any_of(
            std::span(clipped).first(count), [](const clip_t& one) { return one.w <= 0.0F; })) {
        return;
    }
    const corner_t first = corner_of(clipped[0], rounding);
    for (std::size_t i = 1; i + 1 < count; ++i) {
        fill_triangle(
            into, first, corner_of(clipped[i], rounding), corner_of(clipped[i + 1], rounding),
            { i == 1 ? edges[0] : true, edges[i], i + 2 == count ? edges[count - 1] : true });
    }
}

/// An occluder's triangles, those that face the view, an edge shared only with what's drawn too.
void add_surface(buffer_t& into, const scene::surface_t& surface,
                 const std::vector<std::uint32_t>& neighbors, const mat4_t& to_clip,
                 bool both_sides, bool mirrored, const rounding_t& rounding) {
    const std::vector<std::uint32_t>& indices = surface.indices;
    const std::size_t triangles = indices.size() / 3;
    into.clips.resize(surface.vertices.size());
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        const fbe_vec3_t& at = surface.vertices[i].position;
        into.clips[i] = clip_of(to_clip, vec3_t(at.x, at.y, at.z));
    }
    into.drawn.assign(triangles, 0);
    for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
        const clip_t& a = into.clips[indices[triangle * 3]];
        const clip_t& b = into.clips[indices[(triangle * 3) + 1]];
        const clip_t& c = into.clips[indices[(triangle * 3) + 2]];
        // As the pipeline's front faces are: with y down, what goes around clockwise on the
        // buffer's numbers. In homogeneous terms, so it holds across the near plane.
        const float across = (a.x * ((b.y * c.w) - (c.y * b.w))) -
                             (a.y * ((b.x * c.w) - (c.x * b.w))) +
                             (a.w * ((b.x * c.y) - (c.x * b.y)));
        into.drawn[triangle] =
            across != 0.0F && (both_sides || (across > 0.0F) == mirrored) ? 1 : 0;
    }
    const auto drawn_beside = [&](std::size_t edge) {
        return neighbors[edge] != no_neighbor && into.drawn[neighbors[edge]] != 0;
    };
    for (std::size_t triangle = 0; triangle < triangles && into.pixels <= most_pixels; ++triangle) {
        if (into.drawn[triangle] == 0) {
            continue;
        }
        const std::size_t first = triangle * 3;
        add_triangle(into, into.clips[indices[first]], into.clips[indices[first + 1]],
                     into.clips[indices[first + 2]],
                     { drawn_beside(first), drawn_beside(first + 1), drawn_beside(first + 2) },
                     rounding);
    }
}

} // namespace

bool fill_occlusion(const scene::camera_t& camera, const scene::view_t& view,
                    const scene::tweened_t& tweened, const scene::frustum_t& frustum) {
    const scene::scene_t& s = scene::scene();
    const std::uint32_t world = s.entities.world[camera.entity];
    struct occluder_t {
        std::uint32_t mesh = 0;
        std::uint32_t drawn = none;
        float apart = 0.0F;
        float reach = 0.0F;
        mat4_t matrix;
    };
    std::vector<occluder_t> occluders;
    for (std::uint32_t at = 0; at < s.meshes.size(); ++at) {
        const scene::mesh_t& mesh = s.meshes[at];
        // A skin or a morph moves it from what its vertices say, so it can't vouch for them.
        if (!s.entities.occluder[mesh.entity] || mesh.geometry == none ||
            s.entities.world[mesh.entity] != world || !scene::showing(mesh.entity) ||
            mesh.tint[3] < 1.0F || s.entities.fade_far[mesh.entity] > 0.0F ||
            !mesh.weights.empty()) {
            continue;
        }
        const mat4_t matrix = tweened.pose(mesh.entity).matrix;
        const std::uint32_t drawn = mesh_lod(mesh, matrix, tweened, camera, view);
        if (drawn == none) {
            continue;
        }
        vec3_t low;
        vec3_t high;
        scene::geometry_bounds(drawn, low, high);
        // Nobody sees what's out of view, so it spends none of what the view has.
        std::array<vec3_t, 8> corners;
        for (unsigned corner = 0; corner < 8; ++corner) {
            corners[corner] =
                scene::transform_point(matrix, vec3_t((corner & 1U) != 0 ? high.x : low.x,
                                                      (corner & 2U) != 0 ? high.y : low.y,
                                                      (corner & 4U) != 0 ? high.z : low.z));
        }
        if (!scene::in_frustum(frustum, corners.data(), corners.size())) {
            continue;
        }
        const vec3_t both = low + high;
        const vec3_t center = scene::transform_point(matrix, both * 0.5F);
        const float scale =
            std::fmax(std::fmax(scene::length(matrix.axis(0)), scene::length(matrix.axis(1))),
                      scene::length(matrix.axis(2)));
        const float radius = scene::length(high - low) * 0.5F * scale;
        occluders.push_back({
            .mesh = at,
            .drawn = drawn,
            .apart = scene::length(center - frustum.eye) - radius,
            .reach = scene::length(center) + radius,
            .matrix = matrix,
        });
    }
    if (occluders.empty()) {
        return false;
    }
    std::ranges::sort(occluders, {}, &occluder_t::apart);
    buffer_t& into = buffer();
    into.depth.assign(static_cast<std::size_t>(buffer_width) * buffer_height, 0.0F);
    const scene::quat_t back = scene::inverse(view.rotation);
    const mat4_t view_matrix =
        scene::compose(back * -view.position, back, vec3_t(1.0F, 1.0F, 1.0F));
    into.to_clip = scene::draw_projection(camera, view) * view_matrix;
    const mat4_t& m = into.to_clip;
    const auto row_length = [&m](int row) {
        return scene::length(vec3_t(m.at(0, row), m.at(1, row), m.at(2, row)));
    };
    into.across_per = std::fmax(row_length(0), row_length(1));
    into.z_per = row_length(2);
    into.w_per = row_length(3);
    into.z_alone = std::fabs(m.at(3, 2));
    into.eye_reach = scene::length(frustum.eye);
    into.pixels = 0;
    std::size_t triangles = 0;
    const scene::material_t standard;
    for (const occluder_t& occluder : occluders) {
        const scene::mesh_t& mesh = s.meshes[occluder.mesh];
        const scene::geometry_t& geometry = s.geometries[occluder.drawn];
        const std::vector<std::vector<std::uint32_t>>& neighbors = neighbors_of(occluder.drawn);
        const rounding_t rounding = rounding_of(into, occluder.reach);
        const mat4_t to_clip = into.to_clip * occluder.matrix;
        const bool mirrored = scene::mirrors(occluder.matrix);
        for (std::size_t at = 0; at < geometry.surfaces.size(); ++at) {
            const scene::surface_t& surface = geometry.surfaces[at];
            const std::uint32_t slot = mesh.material != none ? mesh.material : surface.material;
            const scene::material_t& material = slot != none ? s.materials[slot] : standard;
            // Only what draws solid, and where it's been said to: a skinned one's vertices aren't.
            if (material.order != 0 || material.alpha_cutoff > 0.0F || material.wireframe ||
                !surface.joints.empty() ||
                scene::drawn_blend(slot, mesh.tint[3]) != FBE_BLEND_OPAQUE ||
                triangles + (surface.indices.size() / 3) > most_triangles) {
                continue;
            }
            triangles += surface.indices.size() / 3;
            add_surface(into, surface, neighbors[at], to_clip, material.two_sided, mirrored,
                        rounding);
        }
    }
    for (int block_y = 0; block_y < blocks_down; ++block_y) {
        for (int block_x = 0; block_x < blocks_across; ++block_x) {
            float farthest = 1.0F;
            for (int y = block_y * block_side; y < (block_y + 1) * block_side; ++y) {
                const auto row = static_cast<std::size_t>(y) * buffer_width;
                for (int x = block_x * block_side; x < (block_x + 1) * block_side; ++x) {
                    farthest = std::min(farthest, into.depth[row + static_cast<std::size_t>(x)]);
                }
            }
            into.farthest[(static_cast<std::size_t>(block_y) * blocks_across) +
                          static_cast<std::size_t>(block_x)] = farthest;
        }
    }
    return true;
}

bool occluded(const vec3_t* points, std::size_t count) {
    const buffer_t& from = buffer();
    auto left = float{ buffer_width };
    float right = 0.0F;
    auto top = float{ buffer_height };
    float bottom = 0.0F;
    float nearest = 0.0F;
    float off_pixels = 0.0F;
    for (std::size_t i = 0; i < count; ++i) {
        const clip_t clip = clip_of(from.to_clip, points[i]);
        // Past the near plane, it's around the eye: nothing's in front of it all. Behind the eye's
        // nearer than that in reversed depth, whatever its w.
        if (clip.z > clip.w) {
            return false;
        }
        const float reach =
            std::fabs(points[i].x) + std::fabs(points[i].y) + std::fabs(points[i].z);
        const corner_t corner = corner_of(clip, rounding_of(from, reach));
        // What's no number, of what's no number or a w of 0, says nothing.
        if (std::isnan(corner.x) || std::isnan(corner.y) || std::isnan(corner.z)) {
            return false;
        }
        left = std::min(left, corner.x);
        right = std::max(right, corner.x);
        top = std::min(top, corner.y);
        bottom = std::max(bottom, corner.y);
        nearest = std::max(nearest, corner.z + corner.off_depth);
        off_pixels = std::max(off_pixels, corner.off_pixels);
    }
    // In float first: a box wide enough goes past what an int counts.
    const float past = 1.0F + std::ceil(off_pixels);
    const auto wide = float{ buffer_width };
    const auto high = float{ buffer_height };
    const int from_x = static_cast<int>(std::clamp(std::floor(left) - past, 0.0F, wide));
    const int to_x = static_cast<int>(std::clamp(std::ceil(right) + past, 0.0F, wide));
    const int from_y = static_cast<int>(std::clamp(std::floor(top) - past, 0.0F, high));
    const int to_y = static_cast<int>(std::clamp(std::ceil(bottom) + past, 0.0F, high));
    for (int y = from_y; y < to_y;) {
        const int block_y = y / block_side;
        const int end_y = std::min((block_y + 1) * block_side, to_y);
        for (int x = from_x; x < to_x;) {
            const int block_x = x / block_side;
            const int end_x = std::min((block_x + 1) * block_side, to_x);
            // The block's farthest in front of it: all of it is.
            if (from.farthest[(static_cast<std::size_t>(block_y) * blocks_across) +
                              static_cast<std::size_t>(block_x)] <= nearest) {
                for (int row = y; row < end_y; ++row) {
                    const auto first = static_cast<std::size_t>(row) * buffer_width;
                    for (int column = x; column < end_x; ++column) {
                        if (from.depth[first + static_cast<std::size_t>(column)] <= nearest) {
                            return false;
                        }
                    }
                }
            }
            x = end_x;
        }
        y = end_y;
    }
    return true;
}

void forget_freed_occluders() {
    const scene::scene_t& s = scene::scene();
    for (auto it = kept_edges().begin(); it != kept_edges().end();) {
        it = s.geometry_slots.find(it->first) ? std::next(it) : kept_edges().erase(it);
    }
}

std::uint64_t occluder_edges_worked_out() {
    return edges_worked_out();
}

std::size_t occluder_edges_kept() {
    return kept_edges().size();
}

std::int64_t occluder_pixels() {
    return buffer().pixels;
}

} // namespace fbe::render
