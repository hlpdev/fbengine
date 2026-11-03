// Terrains: a grid of heights, made in code and changed in place, its surface's height and normal
// at a point, and the triangles of it under a box, which collisions and picks sweep.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/bvh.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"
#include "scene/sweep.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

namespace fbe::scene {

namespace {

constexpr std::int32_t most_size = 4096;

/// A grid corner's height.
float height_at(const terrain_t& terrain, std::int32_t x, std::int32_t z) {
    const auto row = static_cast<std::size_t>(terrain.size) + 1;
    return terrain.heights[(static_cast<std::size_t>(z) * row) + static_cast<std::size_t>(x)];
}

/// The two triangles of a square, (x, z) its least corner, counterclockwise seen from above.
std::array<std::array<vec3_t, 3>, 2> square(const terrain_t& terrain, std::int32_t x,
                                            std::int32_t z) {
    const auto fx = static_cast<float>(x);
    const auto fz = static_cast<float>(z);
    const vec3_t a(fx, height_at(terrain, x, z), fz);
    const vec3_t b(fx + 1.0F, height_at(terrain, x + 1, z), fz);
    const vec3_t c(fx, height_at(terrain, x, z + 1), fz + 1.0F);
    const vec3_t d(fx + 1.0F, height_at(terrain, x + 1, z + 1), fz + 1.0F);
    return { { { a, c, d }, { a, d, b } } };
}

} // namespace

vec3_t terrain_corner_normal(const terrain_t& terrain, std::int32_t x, std::int32_t z) {
    const std::int32_t size = terrain.size;
    const float across =
        height_at(terrain, std::max(x - 1, 0), z) - height_at(terrain, std::min(x + 1, size), z);
    const float along =
        height_at(terrain, x, std::max(z - 1, 0)) - height_at(terrain, x, std::min(z + 1, size));
    return normalize(vec3_t(across, 2.0F, along));
}

float terrain_coarse_rise(const terrain_t& terrain, vec3_t min, vec3_t max, float reach_scale) {
    const auto size = static_cast<float>(terrain.size);
    if (max.x < 0.0F || max.z < 0.0F || min.x > size || min.z > size) {
        return 0.0F;
    }
    const auto from_x = static_cast<std::int32_t>(std::floor(std::fmax(min.x, 0.0F)));
    const auto from_z = static_cast<std::int32_t>(std::floor(std::fmax(min.z, 0.0F)));
    const auto to_x = static_cast<std::int32_t>(std::ceil(std::fmin(max.x, size)));
    const auto to_z = static_cast<std::int32_t>(std::ceil(std::fmin(max.z, size)));
    float most = 0.0F;
    for (std::int32_t reach = terrain.size; reach > terrain_piece_cells; reach /= 2) {
        const std::int32_t step = std::max(reach / terrain_piece_cells, 1);
        float rise = 0.0F;
        for (std::int32_t z = from_z; z <= to_z; ++z) {
            for (std::int32_t x = from_x; x <= to_x; ++x) {
                // Where it is on a coarse square, by the same two triangles.
                const std::int32_t low_x = std::min(x / step * step, terrain.size - 1);
                const std::int32_t low_z = std::min(z / step * step, terrain.size - 1);
                const std::int32_t high_x = std::min(low_x + step, terrain.size);
                const std::int32_t high_z = std::min(low_z + step, terrain.size);
                const float u = static_cast<float>(x - low_x) / static_cast<float>(high_x - low_x);
                const float v = static_cast<float>(z - low_z) / static_cast<float>(high_z - low_z);
                const float a = height_at(terrain, low_x, low_z);
                const float b = height_at(terrain, high_x, low_z);
                const float c = height_at(terrain, low_x, high_z);
                const float d = height_at(terrain, high_x, high_z);
                const float coarse =
                    v > u ? a + (v * (c - a)) + (u * (d - c)) : a + (u * (b - a)) + (v * (d - b));
                rise = std::fmax(rise, coarse - height_at(terrain, x, z));
            }
        }
        // It's only drawn this coarse from this far.
        most =
            std::fmax(most, rise / (terrain_whole_from * static_cast<float>(reach) * reach_scale));
    }
    return most;
}

terrain_t* terrain_of(std::uint64_t handle) {
    const std::uint32_t entity = entity_slot(handle);
    if (entity == none) {
        return nullptr;
    }
    if (scene().entities.kind[entity] != FBE_ENTITY_TERRAIN) {
        fail("the entity isn't a terrain");
        return nullptr;
    }
    return &scene().terrains[scene().entities.component[entity]];
}

void add_terrain(std::uint32_t entity) {
    scene_t& s = scene();
    s.entities.component[entity] = static_cast<std::uint32_t>(s.terrains.size());
    terrain_t terrain{ .entity = entity };
    terrain.heights.assign(4, 0.0F);
    s.terrains.push_back(std::move(terrain));
}

void remove_terrain(std::uint32_t entity) {
    scene_t& s = scene();
    const std::uint32_t index = s.entities.component[entity];
    release_material(s.terrains[index].material);
    if (index != s.terrains.size() - 1) {
        s.terrains[index] = std::move(s.terrains.back());
        s.entities.component[s.terrains[index].entity] = index;
    }
    s.terrains.pop_back();
}

void terrain_bounds(const terrain_t& terrain, vec3_t& min, vec3_t& max) {
    if (terrain.range_stale) {
        const auto [low, high] = std::ranges::minmax(terrain.heights);
        terrain.low = low;
        terrain.high = high;
        terrain.range_stale = false;
    }
    const auto size = static_cast<float>(terrain.size);
    min = vec3_t(0.0F, terrain.low, 0.0F);
    max = vec3_t(size, terrain.high, size);
}

bool terrain_blocks(const terrain_t& terrain, vec3_t a, vec3_t b) {
    // In pieces of a few squares each, as picks go, so a long line tries only the squares near it.
    const vec3_t across = b - a;
    const float span = std::fmax(std::fabs(across.x), std::fabs(across.z));
    const int pieces = terrain_line_pieces(span);
    std::vector<std::array<vec3_t, 3>> under;
    for (int piece = 0; piece < pieces; ++piece) {
        const vec3_t p0 = a + across * (static_cast<float>(piece) / static_cast<float>(pieces));
        const vec3_t p1 = a + across * (static_cast<float>(piece + 1) / static_cast<float>(pieces));
        under.clear();
        terrain_under(
            terrain, vec3_t(std::fmin(p0.x, p1.x), std::fmin(p0.y, p1.y), std::fmin(p0.z, p1.z)),
            vec3_t(std::fmax(p0.x, p1.x), std::fmax(p0.y, p1.y), std::fmax(p0.z, p1.z)), under);
        for (const auto& corners : under) {
            bool front = false;
            if (crosses_triangle(a, across, corners.data(), front) >= 0.0F) {
                return true;
            }
        }
    }
    return false;
}

void terrain_under(const terrain_t& terrain, vec3_t min, vec3_t max,
                   std::vector<std::array<vec3_t, 3>>& triangles) {
    // The squares the box's footprint is on, in the grid's own space: a point's own square too,
    // and one on the grid's far edge, its last.
    const auto square_at = [](float at, std::int32_t past, std::int32_t most) {
        // In float first, since a box can be further out than an int counts.
        return static_cast<std::int32_t>(std::fmin(
            std::fmax(std::floor(at) + static_cast<float>(past), 0.0F), static_cast<float>(most)));
    };
    const std::int32_t first_x = square_at(min.x, 0, terrain.size - 1);
    const std::int32_t first_z = square_at(min.z, 0, terrain.size - 1);
    const std::int32_t last_x = square_at(max.x, 1, terrain.size);
    const std::int32_t last_z = square_at(max.z, 1, terrain.size);
    for (std::int32_t z = first_z; z < last_z; ++z) {
        for (std::int32_t x = first_x; x < last_x; ++x) {
            for (const auto& corners : square(terrain, x, z)) {
                triangles.push_back(corners);
            }
        }
    }
}

bool terrain_surface(const terrain_t& terrain, vec3_t local, vec3_t& point, vec3_t& normal) {
    const auto size = static_cast<float>(terrain.size);
    if (std::isnan(local.x) || std::isnan(local.z) || local.x < 0.0F || local.x > size ||
        local.z < 0.0F || local.z > size) {
        return false;
    }
    const auto x = std::min(static_cast<std::int32_t>(local.x), terrain.size - 1);
    const auto z = std::min(static_cast<std::int32_t>(local.z), terrain.size - 1);
    const auto triangles = square(terrain, x, z);
    // Over the square's diagonal from its least corner to its greatest, or under it.
    const float across = local.x - static_cast<float>(x);
    const float along = local.z - static_cast<float>(z);
    const auto& t = along >= across ? triangles[0] : triangles[1];
    normal = normalize(cross(t[1] - t[0], t[2] - t[0]));
    // Up the plane of that triangle to the point above `local`.
    const float height =
        t[0].y - (((normal.x * (local.x - t[0].x)) + (normal.z * (local.z - t[0].z))) / normal.y);
    point = vec3_t(local.x, height, local.z);
    return true;
}

} // namespace fbe::scene

using fbe::scene::entity_slot;
using fbe::scene::none;
using fbe::scene::scene;
using fbe::scene::terrain_t;
using fbe::scene::vec3_t;

extern "C" {

FBE_API bool fbe_terrain_set_size(uint64_t terrain, int32_t size) {
    terrain_t* found = fbe::scene::terrain_of(terrain);
    if (found == nullptr) {
        return false;
    }
    if (size < 1 || size > fbe::scene::most_size) {
        return fbe::fail(std::format("a terrain is 1 to {} squares a side, not {}",
                                     fbe::scene::most_size, size));
    }
    found->size = size;
    const auto row = static_cast<std::size_t>(size) + 1;
    found->heights.assign(row * row, 0.0F);
    found->low = 0.0F;
    found->high = 0.0F;
    found->range_stale = false;
    ++found->version;
    fbe::scene::pickable_moved(found->entity);
    return true;
}

FBE_API int32_t fbe_terrain_size(uint64_t terrain) {
    const terrain_t* found = fbe::scene::terrain_of(terrain);
    return found != nullptr ? found->size : -1;
}

FBE_API bool fbe_terrain_set_height(uint64_t terrain, int32_t x, int32_t z, float height) {
    terrain_t* found = fbe::scene::terrain_of(terrain);
    if (found == nullptr) {
        return false;
    }
    if (x < 0 || x > found->size || z < 0 || z > found->size) {
        return fbe::fail(std::format(
            "a terrain's corners are 0 to {} across and along, not ({}, {})", found->size, x, z));
    }
    if (!std::isfinite(height)) {
        return fbe::fail(std::format("a terrain's height is finite, not {}", height));
    }
    const auto row = static_cast<std::size_t>(found->size) + 1;
    float& corner =
        found->heights[(static_cast<std::size_t>(z) * row) + static_cast<std::size_t>(x)];
    const float was = corner;
    corner = height;
    // Past the range, it widens it; the highest moved down or the lowest up, it may narrow.
    found->range_stale = found->range_stale || (was == found->high && height < was) ||
                         (was == found->low && height > was);
    found->low = std::fmin(found->low, height);
    found->high = std::fmax(found->high, height);
    ++found->version;
    fbe::scene::pickable_moved(found->entity);
    return true;
}

FBE_API float fbe_terrain_height(uint64_t terrain, int32_t x, int32_t z) {
    const terrain_t* found = fbe::scene::terrain_of(terrain);
    if (found == nullptr || x < 0 || x > found->size || z < 0 || z > found->size) {
        return NAN;
    }
    return fbe::scene::height_at(*found, x, z);
}

FBE_API bool fbe_terrain_set_heights(uint64_t terrain, const float* heights, int32_t count) {
    terrain_t* found = fbe::scene::terrain_of(terrain);
    if (found == nullptr) {
        return false;
    }
    if (static_cast<std::size_t>(std::max(count, 0)) != found->heights.size()) {
        return fbe::fail(std::format("a terrain {} squares a side has {} heights, not {}",
                                     found->size, found->heights.size(), count));
    }
    for (std::int32_t i = 0; i < count; ++i) {
        if (!std::isfinite(heights[i])) {
            return fbe::fail(std::format("a terrain's height is finite, not {}", heights[i]));
        }
    }
    std::copy(heights, heights + count, found->heights.begin());
    const auto [low, high] = std::ranges::minmax(found->heights);
    found->low = low;
    found->high = high;
    found->range_stale = false;
    ++found->version;
    fbe::scene::pickable_moved(found->entity);
    return true;
}

FBE_API int32_t fbe_terrain_surface(uint64_t terrain, float x, float y, float z, fbe_vec3_t* point,
                                    fbe_vec3_t* normal) {
    const std::uint32_t slot = entity_slot(terrain);
    const terrain_t* found = slot != none ? fbe::scene::terrain_of(terrain) : nullptr;
    if (found == nullptr) {
        return -1;
    }
    fbe::scene::update_world(slot);
    const fbe::scene::mat4_t& matrix = scene().entities.world_matrix[slot];
    fbe::scene::mat4_t into;
    if (!fbe::scene::inverse_affine(matrix, into)) {
        return 0;
    }
    vec3_t local_point;
    vec3_t local_normal;
    if (!fbe::scene::terrain_surface(*found, fbe::scene::transform_point(into, vec3_t(x, y, z)),
                                     local_point, local_normal)) {
        return 0;
    }
    const vec3_t world_point = fbe::scene::transform_point(matrix, local_point);
    const vec3_t world_normal = fbe::scene::transform_normal(matrix, local_normal);
    *point = { .x = world_point.x, .y = world_point.y, .z = world_point.z };
    *normal = { .x = world_normal.x, .y = world_normal.y, .z = world_normal.z };
    return 1;
}

} // extern "C"
