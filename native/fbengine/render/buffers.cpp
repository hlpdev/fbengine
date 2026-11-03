// What drawing keeps on the GPU from frame to frame: each drawn geometry's surfaces, sent again
// when the geometry changes and let go of when it's freed, and the quad every sprite draws; and a
// terrain's pieces as a view of it sees them, nearer ones finer, in the frame's ring.

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/buffer.hpp"
#include "gpu/vulkan.hpp"
#include "render/render.hpp"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>
#include <vector>

namespace fbe::render {

namespace {

using scene::mat4_t;
using scene::vec3_t;

/// A geometry's surfaces on the GPU, the version of it they're of, and the frame they were sent in.
struct geometry_buffers_t {
    std::uint64_t version = UINT64_MAX;
    std::int64_t frame = -1;
    std::vector<surface_buffers_t> surfaces;
};

std::unordered_map<std::uint64_t, geometry_buffers_t>& kept() {
    static std::unordered_map<std::uint64_t, geometry_buffers_t> made;
    return made;
}

surface_buffers_t quad;
bool registered = false;

void free_surface(surface_buffers_t& surface) {
    gpu::free_buffer(surface.vertices);
    gpu::free_buffer(surface.indices);
    gpu::free_buffer(surface.joints);
    gpu::free_buffer(surface.tangents);
    surface.index_count = 0;
}

/// Everything kept goes as the GPU stops.
void stop_buffers() {
    for (auto& [handle, geometry] : kept()) {
        for (surface_buffers_t& surface : geometry.surfaces) {
            free_surface(surface);
        }
    }
    kept().clear();
    free_surface(quad);
}

/// Bytes into a buffer, made anew unless it's their size: false, with fbe_error's why, if it can't.
bool send(gpu::buffer_t& buffer, std::span<const std::byte> bytes, VkBufferUsageFlags usage) {
    if (buffer.buffer == VK_NULL_HANDLE || buffer.size != bytes.size()) {
        gpu::free_buffer(buffer);
        buffer = gpu::make_buffer(bytes.size(), usage);
        if (buffer.buffer == VK_NULL_HANDLE) {
            return false;
        }
    }
    return gpu::write_buffer(buffer, 0, bytes);
}

bool send_surface(surface_buffers_t& out, std::span<const fbe_vertex_t> vertices,
                  std::span<const std::uint32_t> indices) {
    if (!registered) {
        gpu::at_stop(stop_buffers);
        registered = true;
    }
    out.index_count = 0;
    if (!send(out.vertices, std::as_bytes(vertices),
              gpu::flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) ||
        !send(out.indices, std::as_bytes(indices),
              gpu::flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_INDEX_BUFFER_BIT))) {
        return false;
    }
    out.index_count = static_cast<std::uint32_t>(indices.size());
    return true;
}

/// How many squares a side a terrain's piece has at most, however far it reaches.
constexpr std::int32_t piece_cells = 16;
/// A piece this many times as far from the eye as it's wide is left whole, its squares then about
/// this share of a piece's width of the view.
constexpr float whole_from = 1.5F;

struct piece_area_t {
    std::int32_t x = 0;
    std::int32_t z = 0;
    std::int32_t wide = 0;
    std::int32_t deep = 0;
};

float height_of(const scene::terrain_t& terrain, std::int32_t x, std::int32_t z) {
    const auto row = static_cast<std::size_t>(terrain.size) + 1;
    return terrain.heights[(static_cast<std::size_t>(z) * row) + static_cast<std::size_t>(x)];
}

/// The corner of a piece's grid along one side, its squares as even as whole squares go.
std::int32_t grid_at(std::int32_t from, std::int32_t across, std::int32_t cells, std::int32_t at) {
    return from + static_cast<std::int32_t>(static_cast<std::int64_t>(across) * at / cells);
}

/// The indices of a piece of `cells` by `cells`: the grid, then a skirt down from each edge, whose
/// vertices follow the grid's, an edge's worth each, so where a finer piece meets a coarser one
/// what they don't share of an edge shows no gap.
std::vector<std::uint32_t> piece_indices(std::uint32_t cells) {
    std::vector<std::uint32_t> indices;
    indices.reserve((std::size_t{ cells } * cells * 6) + (std::size_t{ cells } * 24));
    const std::uint32_t row = cells + 1;
    for (std::uint32_t z = 0; z < cells; ++z) {
        for (std::uint32_t x = 0; x < cells; ++x) {
            const std::uint32_t a = (z * row) + x;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + row;
            const std::uint32_t d = c + 1;
            indices.insert(indices.end(), { a, c, d, a, d, b });
        }
    }
    const std::uint32_t skirts = row * row;
    const auto add_skirt = [&indices](std::uint32_t a, std::uint32_t b, std::uint32_t c,
                                      std::uint32_t d) {
        indices.insert(indices.end(), { a, b, c, a, c, d });
    };
    for (std::uint32_t i = 0; i < cells; ++i) {
        const std::uint32_t low_z = i;
        const std::uint32_t high_z = (cells * row) + i;
        const std::uint32_t low_x = i * row;
        const std::uint32_t high_x = (i * row) + cells;
        const std::uint32_t skirt_low_z = skirts + i;
        const std::uint32_t skirt_high_z = skirts + row + i;
        const std::uint32_t skirt_low_x = skirts + (2 * row) + i;
        const std::uint32_t skirt_high_x = skirts + (3 * row) + i;
        // Each faces out of the piece: -Z, +Z, -X, and +X.
        add_skirt(low_z, low_z + 1, skirt_low_z + 1, skirt_low_z);
        add_skirt(high_z + 1, high_z, skirt_high_z, skirt_high_z + 1);
        add_skirt(low_x + row, low_x, skirt_low_x, skirt_low_x + 1);
        add_skirt(high_x, high_x + row, skirt_high_x + 1, skirt_high_x);
    }
    return indices;
}

/// A piece's vertices in the frame's ring.
bool piece_vertices(const scene::terrain_t& terrain, const piece_area_t& area, std::int32_t cells,
                    float drop, std::vector<fbe_vertex_t>& vertices, gpu::slice_t& slice) {
    vertices.clear();
    const auto make = [&](std::int32_t x, std::int32_t z, float lowered) {
        const std::int32_t size = terrain.size;
        const float across = height_of(terrain, std::max(x - 1, 0), z) -
                             height_of(terrain, std::min(x + 1, size), z);
        const float along = height_of(terrain, x, std::max(z - 1, 0)) -
                            height_of(terrain, x, std::min(z + 1, size));
        const vec3_t normal = scene::normalize(vec3_t(across, 2.0F, along));
        const auto fx = static_cast<float>(x);
        const auto fz = static_cast<float>(z);
        const float v = static_cast<float>(size) - fz;
        vertices.push_back({
            .position = { .x = fx, .y = height_of(terrain, x, z) - lowered, .z = fz },
            .normal = { .x = normal.x, .y = normal.y, .z = normal.z },
            .uv0 = { .x = fx, .y = v },
            .uv1 = { .x = fx, .y = v },
            .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
        });
    };
    const auto x_at = [&](std::int32_t i) { return grid_at(area.x, area.wide, cells, i); };
    const auto z_at = [&](std::int32_t i) { return grid_at(area.z, area.deep, cells, i); };
    for (std::int32_t z = 0; z <= cells; ++z) {
        for (std::int32_t x = 0; x <= cells; ++x) {
            make(x_at(x), z_at(z), 0.0F);
        }
    }
    // On the terrain's own edge, a skirt has no height: nothing's beyond it to meet.
    const std::int32_t size = terrain.size;
    const float low_z = area.z > 0 ? drop : 0.0F;
    const float high_z = area.z + area.deep < size ? drop : 0.0F;
    const float low_x = area.x > 0 ? drop : 0.0F;
    const float high_x = area.x + area.wide < size ? drop : 0.0F;
    for (std::int32_t i = 0; i <= cells; ++i) {
        make(x_at(i), z_at(0), low_z);
    }
    for (std::int32_t i = 0; i <= cells; ++i) {
        make(x_at(i), z_at(cells), high_z);
    }
    for (std::int32_t i = 0; i <= cells; ++i) {
        make(x_at(0), z_at(i), low_x);
    }
    for (std::int32_t i = 0; i <= cells; ++i) {
        make(x_at(cells), z_at(i), high_x);
    }
    const auto bytes = static_cast<VkDeviceSize>(vertices.size() * sizeof(fbe_vertex_t));
    slice = gpu::frame_memory(bytes, 16);
    if (slice.data == nullptr) {
        return false;
    }
    std::memcpy(slice.data, vertices.data(), bytes);
    return true;
}

} // namespace

const surface_buffers_t* surface_buffers(std::uint64_t geometry, std::size_t surface) {
    const scene::scene_t& s = scene::scene();
    const auto slot = s.geometry_slots.find(geometry);
    if (!slot) {
        return nullptr;
    }
    const scene::geometry_t& found = s.geometries[*slot];
    geometry_buffers_t& buffers = kept()[geometry];
    // Once a frame: what's written goes to every draw of the frame, those recorded before too, so
    // an edit between two cameras would pair new indices with old vertices. It's the next's.
    const std::int64_t frame = fbe_frame_count();
    if (buffers.version != found.version && buffers.frame != frame) {
        buffers.frame = frame;
        // Surfaces gone go; each that's left is sent again.
        for (std::size_t i = found.surfaces.size(); i < buffers.surfaces.size(); ++i) {
            free_surface(buffers.surfaces[i]);
        }
        buffers.surfaces.resize(found.surfaces.size());
        for (std::size_t i = 0; i < found.surfaces.size(); ++i) {
            const scene::surface_t& from = found.surfaces[i];
            if (from.vertices.empty() || from.indices.empty()) {
                free_surface(buffers.surfaces[i]);
            } else if (!send_surface(buffers.surfaces[i], from.vertices, from.indices)) {
                return nullptr;
            }
            // Only those that are still one for each vertex.
            if (from.joints.size() != from.vertices.size() || from.indices.empty()) {
                gpu::free_buffer(buffers.surfaces[i].joints);
            } else if (!send(buffers.surfaces[i].joints, std::as_bytes(std::span(from.joints)),
                             gpu::flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))) {
                return nullptr;
            }
            if (from.tangents.size() != from.vertices.size() || from.indices.empty()) {
                gpu::free_buffer(buffers.surfaces[i].tangents);
            } else if (!send(buffers.surfaces[i].tangents, std::as_bytes(std::span(from.tangents)),
                             gpu::flag<VkBufferUsageFlags>(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))) {
                return nullptr;
            }
        }
        buffers.version = found.version;
    }
    // One added since, in the frame it's waiting for.
    static const surface_buffers_t none_yet;
    return surface < buffers.surfaces.size() ? &buffers.surfaces[surface] : &none_yet;
}

const surface_buffers_t* sprite_quad() {
    if (quad.vertices.buffer == VK_NULL_HANDLE) {
        // -1 to 1 in x and y, UV 0, 0 at its top left; no normal, so only ambient light lights it.
        const auto corner = [](float x, float y) {
            return fbe_vertex_t{
                .position = { .x = x, .y = y, .z = 0.0F },
                .normal = { .x = 0.0F, .y = 0.0F, .z = 0.0F },
                .uv0 = { .x = (x + 1.0F) * 0.5F, .y = (1.0F - y) * 0.5F },
                .uv1 = { .x = (x + 1.0F) * 0.5F, .y = (1.0F - y) * 0.5F },
                .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
            };
        };
        const fbe_vertex_t low_left = corner(-1.0F, -1.0F);
        const fbe_vertex_t low_right = corner(1.0F, -1.0F);
        const fbe_vertex_t high_right = corner(1.0F, 1.0F);
        const fbe_vertex_t high_left = corner(-1.0F, 1.0F);
        const std::array vertices{ low_left, low_right, high_right, high_left };
        const std::array<std::uint32_t, 6> indices{ 0, 1, 2, 0, 2, 3 };
        if (!send_surface(quad, vertices, indices)) {
            return nullptr;
        }
    }
    return &quad;
}

void forget_freed_geometries() {
    const scene::scene_t& s = scene::scene();
    for (auto it = kept().begin(); it != kept().end();) {
        if (!s.geometry_slots.find(it->first)) {
            for (surface_buffers_t& surface : it->second.surfaces) {
                free_surface(surface);
            }
            it = kept().erase(it);
        } else {
            ++it;
        }
    }
}

bool terrain_pieces(const scene::camera_t& camera, const scene::view_t& view, const item_t& item,
                    const scene::terrain_t& terrain, std::vector<terrain_piece_t>& pieces) {
    mat4_t world;
    std::ranges::copy(item.draw.matrix, world.m.begin());
    vec3_t low;
    vec3_t high;
    scene::terrain_bounds(terrain, low, high);
    // A skirt reaches as far as its terrain's heights do, so it's below any piece it can meet.
    const float drop = (high.y - low.y) + 1.0F;
    const float world_per_cell =
        std::fmax(scene::length(world.axis(0)), scene::length(world.axis(2)));
    // The grids' indices, a list for each size a piece has.
    std::array<gpu::slice_t, piece_cells + 1> grids{};
    std::array<std::uint32_t, piece_cells + 1> grid_counts{};
    std::vector<fbe_vertex_t> vertices;
    std::vector<piece_area_t> stack;
    stack.push_back({ .x = 0, .z = 0, .wide = terrain.size, .deep = terrain.size });
    while (!stack.empty()) {
        const piece_area_t area = stack.back();
        stack.pop_back();
        const auto fx = static_cast<float>(area.x);
        const auto fz = static_cast<float>(area.z);
        std::array<vec3_t, 8> corners;
        for (unsigned corner = 0; corner < 8; ++corner) {
            corners[corner] = scene::transform_point(
                world, vec3_t((corner & 1U) != 0 ? fx + static_cast<float>(area.wide) : fx,
                              (corner & 2U) != 0 ? high.y : low.y,
                              (corner & 4U) != 0 ? fz + static_cast<float>(area.deep) : fz));
        }
        if (!scene::in_view(camera, view, corners.data(), corners.size())) {
            continue;
        }
        vec3_t nearest_low = corners[0];
        vec3_t nearest_high = corners[0];
        for (const vec3_t& corner : corners) {
            nearest_low =
                vec3_t(std::fmin(nearest_low.x, corner.x), std::fmin(nearest_low.y, corner.y),
                       std::fmin(nearest_low.z, corner.z));
            nearest_high =
                vec3_t(std::fmax(nearest_high.x, corner.x), std::fmax(nearest_high.y, corner.y),
                       std::fmax(nearest_high.z, corner.z));
        }
        const vec3_t closest(std::clamp(view.position.x, nearest_low.x, nearest_high.x),
                             std::clamp(view.position.y, nearest_low.y, nearest_high.y),
                             std::clamp(view.position.z, nearest_low.z, nearest_high.z));
        const float apart = scene::length(view.position - closest);
        const std::int32_t reach = std::max(area.wide, area.deep);
        if (reach > piece_cells &&
            apart < static_cast<float>(reach) * world_per_cell * whole_from) {
            const std::int32_t half_wide = area.wide / 2;
            const std::int32_t half_deep = area.deep / 2;
            const std::int32_t rest_wide = area.wide - half_wide;
            const std::int32_t rest_deep = area.deep - half_deep;
            const std::int32_t mid_x = area.x + half_wide;
            const std::int32_t mid_z = area.z + half_deep;
            stack.push_back({ .x = area.x, .z = area.z, .wide = half_wide, .deep = half_deep });
            stack.push_back({ .x = mid_x, .z = area.z, .wide = rest_wide, .deep = half_deep });
            stack.push_back({ .x = area.x, .z = mid_z, .wide = half_wide, .deep = rest_deep });
            stack.push_back({ .x = mid_x, .z = mid_z, .wide = rest_wide, .deep = rest_deep });
            continue;
        }
        const std::int32_t cells = std::min(reach, piece_cells);
        auto& grid = grids[static_cast<std::size_t>(cells)];
        if (grid.data == nullptr) {
            const std::vector<std::uint32_t> indices =
                piece_indices(static_cast<std::uint32_t>(cells));
            const auto bytes = static_cast<VkDeviceSize>(indices.size() * sizeof(std::uint32_t));
            grid = gpu::frame_memory(bytes, 16);
            if (grid.data == nullptr) {
                return false;
            }
            std::memcpy(grid.data, indices.data(), bytes);
            grid_counts[static_cast<std::size_t>(cells)] =
                static_cast<std::uint32_t>(indices.size());
        }
        gpu::slice_t slice;
        if (!piece_vertices(terrain, area, cells, drop, vertices, slice)) {
            return false;
        }
        pieces.push_back({
            .vertices = slice.address,
            .indices = grid.buffer,
            .index_offset = grid.offset,
            .index_count = grid_counts[static_cast<std::size_t>(cells)],
        });
    }
    return true;
}

} // namespace fbe::render
