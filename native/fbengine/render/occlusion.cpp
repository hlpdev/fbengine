// Occlusion: what's flagged as an occluder rasterized on the CPU, for each view, into a small
// buffer of how near what's in front is, and each thing's box tested against it, so what's wholly
// behind isn't drawn. On the CPU, so it's the same on any GPU, a frame's never late, and it saves
// what would have gone into drawing it. It only ever errs toward drawing: a pixel keeps the
// farthest of what's in it, along a triangle's outer edges only what's wholly in a pixel fills it,
// and a box's test goes one pixel past it each way.

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
/// The most triangles a view rasterizes, the nearest occluders first.
constexpr std::size_t most_triangles = 16384;

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

/// A corner in the buffer's pixels, and its depth, reversed, as drawing's is.
struct corner_t {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

corner_t corner_of(const clip_t& clip) {
    return {
        .x = ((clip.x / clip.w) + 1.0F) * 0.5F * buffer_width,
        .y = ((clip.y / clip.w) + 1.0F) * 0.5F * buffer_height,
        .z = clip.z / clip.w,
    };
}

/// A geometry's surface's triangles' edges, whether each is shared with another: what isn't is an
/// outer edge. Three to a triangle, kept while the geometry doesn't change.
struct kept_edges_t {
    std::uint64_t version = UINT64_MAX;
    std::vector<std::vector<std::uint8_t>> shared;
};

std::unordered_map<std::uint32_t, kept_edges_t>& kept_edges() {
    static std::unordered_map<std::uint32_t, kept_edges_t> made;
    return made;
}

const std::vector<std::vector<std::uint8_t>>& shared_edges(std::uint32_t geometry) {
    const scene::geometry_t& found = scene::scene().geometries[geometry];
    kept_edges_t& kept = kept_edges()[geometry];
    if (kept.version == found.version) {
        return kept.shared;
    }
    kept.version = found.version;
    kept.shared.clear();
    std::unordered_map<std::uint64_t, std::uint32_t> counts;
    const auto key_of = [](std::uint32_t a, std::uint32_t b) {
        return (std::uint64_t{ std::min(a, b) } << 32U) | std::max(a, b);
    };
    for (const scene::surface_t& surface : found.surfaces) {
        counts.clear();
        const std::vector<std::uint32_t>& indices = surface.indices;
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            ++counts[key_of(indices[i], indices[i + 1])];
            ++counts[key_of(indices[i + 1], indices[i + 2])];
            ++counts[key_of(indices[i + 2], indices[i])];
        }
        std::vector<std::uint8_t> shared(indices.size());
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            shared[i] = counts[key_of(indices[i], indices[i + 1])] > 1 ? 1 : 0;
            shared[i + 1] = counts[key_of(indices[i + 1], indices[i + 2])] > 1 ? 1 : 0;
            shared[i + 2] = counts[key_of(indices[i + 2], indices[i])] > 1 ? 1 : 0;
        }
        kept.shared.push_back(std::move(shared));
    }
    return kept.shared;
}

/// What a view's occluders left: how near what's in front is in each pixel, 0 for nothing, and in
/// each block of them the farthest.
struct buffer_t {
    mat4_t to_clip;
    std::vector<float> depth;
    std::array<float, static_cast<std::size_t>(blocks_across* blocks_down)> farthest{};
};

buffer_t& buffer() {
    static buffer_t made;
    return made;
}

/// Fills what a triangle wholly covers, of pixels whose depth it's nearer in, at the farthest of
/// its depth in each. An edge that isn't shared has to have the whole pixel inside it.
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
    // Each edge as a line: how far inside it a point is, positive inside.
    const float past_a = ((a.x * b.y) - (a.y * b.x)) * sign;
    const float past_b = ((b.x * c.y) - (b.y * c.x)) * sign;
    const float past_c = ((c.x * a.y) - (c.y * a.x)) * sign;
    const std::array<float, 3> line_a{ (a.y - b.y) * sign, (b.x - a.x) * sign, past_a };
    const std::array<float, 3> line_b{ (b.y - c.y) * sign, (c.x - b.x) * sign, past_b };
    const std::array<float, 3> line_c{ (c.y - a.y) * sign, (a.x - c.x) * sign, past_c };
    const std::array lines{ line_a, line_b, line_c };
    std::array<float, 3> inset{};
    for (std::size_t edge = 0; edge < 3; ++edge) {
        inset[edge] =
            shared[edge] ? 0.0F : (std::fabs(lines[edge][0]) + std::fabs(lines[edge][1])) * 0.5F;
    }
    // Its depth's a plane across the buffer; a pixel keeps the farthest of it, half a pixel off.
    const float per_x = (((b.z - a.z) * (c.y - a.y)) - ((c.z - a.z) * (b.y - a.y))) / area;
    const float per_y = (((c.z - a.z) * (b.x - a.x)) - ((b.z - a.z) * (c.x - a.x))) / area;
    const float slack = (std::fabs(per_x) + std::fabs(per_y)) * 0.5F;
    for (int y = top; y < bottom; ++y) {
        const float py = static_cast<float>(y) + 0.5F;
        for (int x = left; x < right; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            bool inside = true;
            for (std::size_t edge = 0; edge < 3 && inside; ++edge) {
                const std::array<float, 3>& line = lines[edge];
                inside = (line[0] * px) + (line[1] * py) + line[2] >= inset[edge];
            }
            if (!inside) {
                continue;
            }
            const float depth = a.z + ((px - a.x) * per_x) + ((py - a.y) * per_y) - slack;
            float& kept = into.depth[(static_cast<std::size_t>(y) * buffer_width) +
                                     static_cast<std::size_t>(x)];
            kept = std::max(kept, depth);
        }
    }
}

/// A triangle, clipped to what's past the near plane, as a fan of what's left.
void add_triangle(buffer_t& into, const clip_t& a, const clip_t& b, const clip_t& c,
                  const std::array<bool, 3>& shared) {
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
    const corner_t first = corner_of(clipped[0]);
    for (std::size_t i = 1; i + 1 < count; ++i) {
        fill_triangle(
            into, first, corner_of(clipped[i]), corner_of(clipped[i + 1]),
            { i == 1 ? edges[0] : true, edges[i], i + 2 == count ? edges[count - 1] : true });
    }
}

/// An occluder's triangles, those that face the view. False once they're past what's left.
void add_surface(buffer_t& into, const scene::surface_t& surface,
                 const std::vector<std::uint8_t>& shared, const mat4_t& to_clip, bool both_sides,
                 bool mirrored) {
    const std::vector<std::uint32_t>& indices = surface.indices;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const fbe_vertex_t& one = surface.vertices[indices[i]];
        const fbe_vertex_t& two = surface.vertices[indices[i + 1]];
        const fbe_vertex_t& three = surface.vertices[indices[i + 2]];
        const clip_t a = clip_of(to_clip, vec3_t(one.position.x, one.position.y, one.position.z));
        const clip_t b = clip_of(to_clip, vec3_t(two.position.x, two.position.y, two.position.z));
        const clip_t c =
            clip_of(to_clip, vec3_t(three.position.x, three.position.y, three.position.z));
        if (!both_sides) {
            // As the pipeline's front faces are: with y down, what goes around clockwise on the
            // buffer's numbers. In homogeneous terms, so it holds across the near plane.
            const float across = (a.x * ((b.y * c.w) - (c.y * b.w))) -
                                 (a.y * ((b.x * c.w) - (c.x * b.w))) +
                                 (a.w * ((b.x * c.y) - (c.x * b.y)));
            if ((across > 0.0F) != mirrored) {
                continue;
            }
        }
        add_triangle(into, a, b, c, { shared[i] != 0, shared[i + 1] != 0, shared[i + 2] != 0 });
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
        mat4_t matrix;
    };
    std::vector<occluder_t> occluders;
    float half_width = 0.0F;
    float half_height = 0.0F;
    scene::half_extents(camera, view, half_width, half_height);
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
        vec3_t low;
        vec3_t high;
        scene::geometry_bounds(mesh.geometry, low, high);
        const vec3_t both = low + high;
        const vec3_t center = scene::transform_point(matrix, both * 0.5F);
        const float scale =
            std::fmax(std::fmax(scene::length(matrix.axis(0)), scene::length(matrix.axis(1))),
                      scene::length(matrix.axis(2)));
        const float radius = scene::length(high - low) * 0.5F * scale;
        const std::uint32_t drawn =
            scene::lod_for(mesh.geometry, center, radius, camera, frustum.eye, half_height);
        if (drawn == none) {
            continue;
        }
        occluders.push_back({
            .mesh = at,
            .drawn = drawn,
            .apart = scene::length(center - frustum.eye) - radius,
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
    std::size_t triangles = 0;
    const scene::material_t standard;
    for (const occluder_t& occluder : occluders) {
        const scene::mesh_t& mesh = s.meshes[occluder.mesh];
        const scene::geometry_t& geometry = s.geometries[occluder.drawn];
        const std::vector<std::vector<std::uint8_t>>& shared = shared_edges(occluder.drawn);
        const mat4_t to_clip = into.to_clip * occluder.matrix;
        const bool mirrored = scene::mirrors(occluder.matrix);
        for (std::size_t at = 0; at < geometry.surfaces.size(); ++at) {
            const scene::surface_t& surface = geometry.surfaces[at];
            const std::uint32_t slot = mesh.material != none ? mesh.material : surface.material;
            const scene::material_t& material = slot != none ? s.materials[slot] : standard;
            // Only what draws solid, and where it's been said to: a skinned one's vertices aren't.
            if (material.order != 0 || material.alpha_cutoff > 0.0F || !surface.joints.empty() ||
                scene::drawn_blend(slot, mesh.tint[3]) != FBE_BLEND_OPAQUE ||
                triangles + (surface.indices.size() / 3) > most_triangles) {
                continue;
            }
            triangles += surface.indices.size() / 3;
            add_surface(into, surface, shared[at], to_clip, material.two_sided, mirrored);
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
    for (std::size_t i = 0; i < count; ++i) {
        const clip_t clip = clip_of(from.to_clip, points[i]);
        // Past the near plane, it's around the eye: nothing's in front of it all.
        if (clip.w <= 0.0F || clip.z > clip.w) {
            return false;
        }
        const corner_t corner = corner_of(clip);
        left = std::min(left, corner.x);
        right = std::max(right, corner.x);
        top = std::min(top, corner.y);
        bottom = std::max(bottom, corner.y);
        nearest = std::max(nearest, corner.z);
    }
    const int from_x = std::max(static_cast<int>(std::floor(left)) - 1, 0);
    const int to_x = std::min(static_cast<int>(std::ceil(right)) + 1, buffer_width);
    const int from_y = std::max(static_cast<int>(std::floor(top)) - 1, 0);
    const int to_y = std::min(static_cast<int>(std::ceil(bottom)) + 1, buffer_height);
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

} // namespace fbe::render
