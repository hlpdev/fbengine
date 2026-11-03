// Shadows: before a camera draws, every shadowed light of its World gets tiles of one atlas, and
// what casts is drawn into them from the light. A pass of R32F, each texel the depth of the nearest
// thing there, as the bindless set has only color images to sample: PBR's fragment shader compares
// with them itself. A directional light has a cascade a tile, each around its slice of the camera's
// view, on a texel grid that doesn't move as the camera does; a spot light a tile; a point light
// six, along the world's axes.

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/graph.hpp"
#include "gpu/vulkan.hpp"
#include "render/drawing.hpp"
#include "render/render.hpp"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"
#include "skin/skin.hpp"
#include "texture/texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

namespace fbe::render {

namespace {

using scene::mat4_t;
using scene::none;
using scene::quat_t;
using scene::vec3_t;

/// A cascade's slices: this much of the way from even slices to ever longer ones.
constexpr float slice_lean = 0.7F;
/// How far past a point light's face its tile looks, so a sample at a face's edge has its
/// neighbors.
constexpr float face_angle = (std::numbers::pi_v<float> / 2.0F) * 1.02F;
/// The smallest tile, and the most lights' worth of a side of the atlas.
constexpr std::uint32_t least_tile = 16;

/// The atlas, kept while the GPU's up, and let go of once it's the wrong size.
std::uint64_t atlas = 0;
std::uint32_t atlas_size = 0;
bool registered = false;

void forget_atlas() {
    // Gone with the GPU, which frees every texture.
    atlas = 0;
    atlas_size = 0;
}

/// A clip space, Vulkan's, y down, depth reversed, looking down -z.
mat4_t perspective(float field, float nearest, float farthest) {
    const float focal = 1.0F / std::tan(field * 0.5F);
    mat4_t made;
    made.at(0, 0) = focal;
    made.at(1, 1) = -focal;
    made.at(2, 2) = nearest / (farthest - nearest);
    made.at(2, 3) = -1.0F;
    made.at(3, 2) = farthest * nearest / (farthest - nearest);
    made.at(3, 3) = 0.0F;
    return made;
}

mat4_t orthographic(float half, float farthest) {
    mat4_t made;
    made.at(0, 0) = 1.0F / half;
    made.at(1, 1) = -1.0F / half;
    made.at(2, 2) = 1.0F / farthest;
    made.at(3, 2) = 1.0F;
    return made;
}

/// The matrix to a light's view, from where it is and how it's turned.
mat4_t view_of(vec3_t place, quat_t turn) {
    const quat_t back = scene::inverse(turn);
    return scene::compose(back * -place, back, vec3_t(1.0F, 1.0F, 1.0F));
}

/// A plane's sides of a matrix's frustum: Gribb and Hartmann's, for clip z 0 to w.
std::array<std::array<float, 4>, 6> planes_of(const mat4_t& m) {
    const auto row = [&m](int at) {
        return std::array<float, 4>{ m.at(0, at), m.at(1, at), m.at(2, at), m.at(3, at) };
    };
    const std::array<float, 4> x = row(0);
    const std::array<float, 4> y = row(1);
    const std::array<float, 4> z = row(2);
    const std::array<float, 4> w = row(3);
    std::array<std::array<float, 4>, 6> planes{};
    for (std::size_t i = 0; i < 4; ++i) {
        planes[0][i] = w[i] + x[i];
        planes[1][i] = w[i] - x[i];
        planes[2][i] = w[i] + y[i];
        planes[3][i] = w[i] - y[i];
        // Reversed depth: what's past the far plane is below 0; nearer than the near one is kept,
        // as the shader flattens it onto the near plane, and it still casts.
        planes[4][i] = z[i];
        planes[5][i] = w[i] * 1000000.0F;
    }
    for (std::array<float, 4>& plane : planes) {
        const float length =
            std::sqrt((plane[0] * plane[0]) + (plane[1] * plane[1]) + (plane[2] * plane[2]));
        if (length > 0.0F) {
            for (float& part : plane) {
                part /= length;
            }
        }
    }
    return planes;
}

/// A tile's place in the atlas.
struct place_t {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t size = 0;
};

/// Where tiles go, as shelves, the sides given biggest first, each shelf as tall as its first:
/// false if they don't fit a side of `side`.
bool pack(const std::vector<std::uint32_t>& sizes, std::uint32_t side,
          std::vector<place_t>& places) {
    std::vector<std::uint32_t> order(sizes.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<std::uint32_t>(i);
    }
    std::ranges::stable_sort(order, {}, [&sizes](std::uint32_t i) { return ~sizes[i]; });
    places.assign(sizes.size(), {});
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t shelf = 0;
    for (const std::uint32_t i : order) {
        const std::uint32_t size = sizes[i];
        if (x + size > side) {
            x = 0;
            y += shelf;
            shelf = 0;
        }
        if (size > side || y + size > side) {
            return false;
        }
        places[i] = { .x = x, .y = y, .size = size };
        x += size;
        shelf = std::max(shelf, size);
    }
    return true;
}

/// What casts: a draw of the shadow pass's list, and the sphere it's within, in the world.
struct caster_t {
    draw_data_t data{};
    std::uint64_t key = 0;
    VkBuffer indices = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    std::uint32_t index_count = 0;
    vec3_t center;
    float radius = 0.0F;
};

/// The key of a caster's pipeline, as fbe_render.h lays its bits out: only what the shadow pass
/// goes by: an alpha cutoff, its sides, whether it mirrors, and its first layer's texture.
std::uint64_t caster_key(const scene::material_t& material, bool mirrored) {
    std::uint64_t layers = 0;
    std::uint64_t layer_count = 0;
    if (material.alpha_cutoff > 0.0F) {
        for (const scene::layer_t& layer : material.layers) {
            if (layer.texture != 0) {
                const auto uv_set = static_cast<std::uint64_t>(layer.uv_set);
                layers = uv_set << 3U;
                layer_count = 1;
                break;
            }
        }
    }
    return (static_cast<std::uint64_t>(FBE_BLEND_OPAQUE) << 2U) |
           (static_cast<std::uint64_t>(material.alpha_cutoff > 0.0F ? 1 : 0) << 5U) |
           (static_cast<std::uint64_t>(material.two_sided ? 1 : 0) << 7U) |
           (static_cast<std::uint64_t>(FBE_DEPTH_TEST_AND_WRITE) << 10U) |
           (static_cast<std::uint64_t>(mirrored ? 1 : 0) << 12U) | (layer_count << 13U) |
           (layers << 17U);
}

/// Every mesh surface and terrain of a world that casts, sorted so what can go together does.
bool gather_casters(std::uint32_t world, const scene::tweened_t& tweened, std::uint64_t target,
                    std::vector<caster_t>& casters, std::vector<material_data_t>& materials,
                    std::vector<std::uint64_t>& sampled) {
    const scene::scene_t& s = scene::scene();
    std::unordered_map<std::uint32_t, std::uint32_t> material_index;
    std::unordered_map<std::uint32_t, std::pair<VkDeviceAddress, std::uint32_t>> palettes;
    const scene::material_t standard;
    const auto index_of = [&](std::uint32_t material) {
        const auto [found, added] =
            material_index.try_emplace(material, static_cast<std::uint32_t>(materials.size()));
        if (added) {
            materials.push_back(material_data(material, target, sampled));
        }
        return found->second;
    };
    const auto casts = [&](std::uint32_t entity) {
        return s.entities.world[entity] == world && s.entities.casts_shadows[entity] &&
               scene::showing(entity);
    };
    for (const scene::mesh_t& mesh : s.meshes) {
        if (mesh.geometry == none || !casts(mesh.entity) || mesh.tint[3] < 1.0F) {
            continue;
        }
        const mat4_t matrix = tweened.pose(mesh.entity).matrix;
        const scene::geometry_t& geometry = s.geometries[mesh.geometry];
        vec3_t low;
        vec3_t high;
        scene::geometry_bounds(mesh.geometry, low, high);
        const vec3_t both = low + high;
        const vec3_t middle = scene::transform_point(matrix, both * 0.5F);
        const float scale =
            std::fmax(std::fmax(scene::length(matrix.axis(0)), scene::length(matrix.axis(1))),
                      scene::length(matrix.axis(2)));
        float radius = scene::length(high - low) * 0.5F * scale;
        // Skinned, it's about where its joints are, its bind pose's whole size around them.
        vec3_t joints_low;
        vec3_t joints_high;
        vec3_t center = middle;
        if (anim::skin_joints_box(mesh.entity, tweened, joints_low, joints_high)) {
            const vec3_t both_joints = joints_low + joints_high;
            center = both_joints * 0.5F;
            radius += (scene::length(joints_high - joints_low) * 0.5F) + radius;
        }
        for (std::size_t surface = 0; surface < geometry.surfaces.size(); ++surface) {
            if (geometry.surfaces[surface].indices.empty()) {
                continue;
            }
            const std::uint32_t slot =
                mesh.material != none ? mesh.material : geometry.surfaces[surface].material;
            const scene::material_t& material = slot != none ? s.materials[slot] : standard;
            const std::int32_t blend = scene::drawn_blend(slot, mesh.tint[3]);
            if (material.order != 0 || blend != FBE_BLEND_OPAQUE) {
                continue;
            }
            const surface_buffers_t* buffers =
                surface_buffers(s.geometry_slots.handle(mesh.geometry), surface);
            if (buffers == nullptr) {
                return false;
            }
            if (buffers->index_count == 0) {
                continue;
            }
            caster_t caster;
            caster.data.to_view = matrix.m;
            caster.data.color = { 1.0F, 1.0F, 1.0F, mesh.tint[3] };
            caster.data.vertices = buffers->vertices.address;
            caster.data.material = index_of(slot);
            if (buffers->joints.buffer != VK_NULL_HANDLE) {
                const auto [palette, fresh] =
                    palettes.try_emplace(mesh.entity, std::pair{ VkDeviceAddress{ 0 }, 0U });
                if (fresh) {
                    const std::span<const mat4_t> poses = anim::skin_palette(mesh.entity, tweened);
                    if (!poses.empty()) {
                        bool no_room = false;
                        palette->second = { in_ring<mat4_t>(poses, no_room).address,
                                            static_cast<std::uint32_t>(poses.size()) };
                        if (no_room) {
                            return false;
                        }
                    }
                }
                if (palette->second.first != 0) {
                    caster.data.joints = buffers->joints.address;
                    caster.data.palette = palette->second.first;
                    caster.data.joint_count = palette->second.second;
                }
            }
            caster.key = caster_key(material, scene::mirrors(matrix));
            caster.indices = buffers->indices.buffer;
            caster.index_count = buffers->index_count;
            caster.center = center;
            caster.radius = radius;
            casters.push_back(caster);
        }
    }
    for (const scene::terrain_t& terrain : s.terrains) {
        if (!casts(terrain.entity) || terrain.tint[3] < 1.0F) {
            continue;
        }
        const scene::material_t& material =
            terrain.material != none ? s.materials[terrain.material] : standard;
        if (material.order != 0 ||
            scene::drawn_blend(terrain.material, terrain.tint[3]) != FBE_BLEND_OPAQUE) {
            continue;
        }
        const mat4_t matrix = tweened.pose(terrain.entity).matrix;
        std::vector<terrain_piece_t> pieces;
        if (!terrain_coarse_pieces(matrix, terrain, pieces)) {
            return false;
        }
        const std::uint32_t index = index_of(terrain.material);
        for (const terrain_piece_t& piece : pieces) {
            caster_t caster;
            caster.data.to_view = matrix.m;
            caster.data.color = { 1.0F, 1.0F, 1.0F, 1.0F };
            caster.data.vertices = piece.vertices;
            caster.data.material = index;
            caster.key = caster_key(material, scene::mirrors(matrix));
            caster.indices = piece.indices;
            caster.index_offset = piece.index_offset;
            caster.index_count = piece.index_count;
            caster.center = piece.center;
            caster.radius = piece.radius;
            casters.push_back(caster);
        }
    }
    std::ranges::stable_sort(casters, [](const caster_t& a, const caster_t& b) {
        if (a.key != b.key) {
            return a.key < b.key;
        }
        if (a.indices != b.indices) {
            return a.indices < b.indices;
        }
        return a.index_offset < b.index_offset;
    });
    return true;
}

/// A tile as the frame's being worked out: its light's, what's seen from it, and how a texel's size
/// goes with distance.
struct tile_t {
    std::uint32_t size = 0;
    mat4_t to_clip;
    float depth_per = 0.0F;
    float texel = 0.0F;
    float split = 0.0F;
    bool level = false;
    float bias = 0.0F;
    float normal_bias = 0.0F;
};

/// Each cascade of a directional light, around its slice of the view.
void add_cascades(const scene::camera_t& camera, const scene::view_t& view,
                  const scene::light_t& light, quat_t turn, std::vector<tile_t>& tiles) {
    float half_width = 0.0F;
    float half_height = 0.0F;
    scene::half_extents(camera, view, half_width, half_height);
    const bool looks_in = camera.projection == FBE_PROJECTION_PERSPECTIVE;
    const float start = camera.nearest;
    const float finish = std::fmin(light.shadow_distance, camera.farthest);
    const vec3_t forward = view.rotation * vec3_t(0.0F, 0.0F, -1.0F);
    const float wide_squared = (half_width * half_width) + (half_height * half_height);
    float from = start;
    for (std::int32_t cascade = 0; cascade < light.cascades; ++cascade) {
        const float part = static_cast<float>(cascade + 1) / static_cast<float>(light.cascades);
        const float even = start + ((finish - start) * part);
        const float longer = start * std::pow(finish / start, part);
        const float to =
            cascade + 1 == light.cascades ? finish : even + ((longer - even) * slice_lean);
        // The sphere around the slice, whatever way the camera's turned, so its size never changes.
        float middle = (from + to) * 0.5F;
        float radius = 0.0F;
        if (looks_in) {
            middle *= 1.0F + wide_squared;
            middle = std::fmin(middle, to);
            const float across_squared = to * to * wide_squared;
            radius = std::sqrt(((to - middle) * (to - middle)) + across_squared);
        } else {
            const float half_length = (to - from) * 0.5F;
            radius = std::sqrt((half_length * half_length) + wide_squared);
        }
        // It only moves by whole texels, so a shadow's edge doesn't crawl as the camera does.
        const float texel = 2.0F * radius / static_cast<float>(light.shadow_size);
        const vec3_t center = view.position + forward * middle;
        const quat_t back = scene::inverse(turn);
        vec3_t in_light = back * center;
        in_light.x = std::round(in_light.x / texel) * texel;
        in_light.y = std::round(in_light.y / texel) * texel;
        const vec3_t snapped = turn * in_light;
        const vec3_t behind = turn * vec3_t(0.0F, 0.0F, 1.0F);
        const vec3_t eye = snapped + behind * radius;
        tile_t tile;
        tile.size = static_cast<std::uint32_t>(light.shadow_size);
        tile.to_clip = orthographic(radius, radius * 2.0F) * view_of(eye, turn);
        tile.depth_per = 1.0F / (radius * 2.0F);
        tile.texel = texel;
        tile.split = to;
        tile.level = true;
        tiles.push_back(tile);
        from = to;
    }
}

/// A perspective tile: a spot light's, or one face of a point light's.
void add_look(const scene::light_t& light, vec3_t place, quat_t turn, float field,
              std::vector<tile_t>& tiles) {
    const float nearest = std::fmax(light.range * 0.002F, 0.01F);
    tile_t tile;
    tile.size = static_cast<std::uint32_t>(light.shadow_size);
    tile.to_clip = perspective(field, nearest, light.range) * view_of(place, turn);
    tile.depth_per = nearest * light.range / (light.range - nearest);
    tile.texel = 2.0F * std::tan(field * 0.5F) / static_cast<float>(light.shadow_size);
    tiles.push_back(tile);
}

} // namespace

bool draw_shadows(const scene::camera_t& camera, const scene::view_t& view,
                  const scene::tweened_t& tweened, shadow_frame_t& frame) {
    const scene::scene_t& s = scene::scene();
    const std::uint32_t world = s.entities.world[camera.entity];
    // Each shadowed light's tiles, in the tree's order, so which keep theirs doesn't change.
    std::vector<std::uint32_t> found;
    scene::lights_in(s.worlds[world].roots, found);
    std::vector<tile_t> tiles;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> firsts;
    std::vector<std::uint32_t> ends;
    for (const std::uint32_t entity : found) {
        const scene::light_t& light = s.lights[s.entities.component[entity]];
        if (!light.shadows || !scene::showing(entity) || light.intensity <= 0.0F) {
            continue;
        }
        const scene::pose_t pose = tweened.pose(entity);
        const vec3_t place = pose.matrix.axis(3);
        firsts.emplace_back(s.entities.slots.handle(entity),
                            static_cast<std::uint32_t>(tiles.size()));
        const std::size_t before = tiles.size();
        if (light.type == FBE_LIGHT_DIRECTIONAL) {
            add_cascades(camera, view, light, pose.rotation, tiles);
        } else if (light.type == FBE_LIGHT_SPOT) {
            const float field = std::fmin(light.outer_cone * 2.04F, 3.0F);
            add_look(light, place, pose.rotation, field, tiles);
        } else {
            // +X, -X, +Y, -Y, +Z, -Z, as the shader finds them by a way's biggest part.
            const float half_pi = std::numbers::pi_v<float> * 0.5F;
            add_look(light, place, scene::axis_angle(vec3_t(0.0F, 1.0F, 0.0F), -half_pi),
                     face_angle, tiles);
            add_look(light, place, scene::axis_angle(vec3_t(0.0F, 1.0F, 0.0F), half_pi), face_angle,
                     tiles);
            add_look(light, place, scene::axis_angle(vec3_t(1.0F, 0.0F, 0.0F), half_pi), face_angle,
                     tiles);
            add_look(light, place, scene::axis_angle(vec3_t(1.0F, 0.0F, 0.0F), -half_pi),
                     face_angle, tiles);
            add_look(light, place,
                     scene::axis_angle(vec3_t(0.0F, 1.0F, 0.0F), std::numbers::pi_v<float>),
                     face_angle, tiles);
            add_look(light, place, quat_t(), face_angle, tiles);
        }
        for (std::size_t i = before; i < tiles.size(); ++i) {
            tiles[i].bias = light.shadow_bias;
            tiles[i].normal_bias = light.shadow_normal_bias;
        }
        ends.push_back(static_cast<std::uint32_t>(tiles.size()));
    }
    if (tiles.empty()) {
        return true;
    }
    // The atlas as big as they need, as far as the device goes; whose don't fit have no shadow,
    // the last lights first, their tiles all or none.
    const std::uint32_t most = gpu::device()->properties.limits.maxFramebufferWidth;
    const std::uint32_t largest = std::min(most, std::uint32_t{ 8192 });
    std::vector<place_t> places;
    std::uint32_t side = std::max(least_tile, std::uint32_t{ 256 });
    while (true) {
        std::vector<std::uint32_t> sizes;
        sizes.reserve(tiles.size());
        for (const tile_t& tile : tiles) {
            sizes.push_back(std::min(tile.size, largest));
        }
        if (pack(sizes, side, places)) {
            break;
        }
        if (side < largest) {
            side = std::min(side * 2, largest);
            continue;
        }
        tiles.resize(ends.size() >= 2 ? ends[ends.size() - 2] : 0);
        ends.pop_back();
        firsts.pop_back();
        if (tiles.empty()) {
            return true;
        }
    }
    if (atlas == 0 || atlas_size != side) {
        if (!registered) {
            gpu::at_stop(forget_atlas);
            registered = true;
        }
        if (atlas != 0) {
            fbe_texture_release(atlas);
        }
        atlas = texture::create_engine_target(side, side, FBE_TEXTURE_R32F, VK_SAMPLE_COUNT_1_BIT,
                                              true);
        atlas_size = side;
        if (atlas == 0) {
            atlas_size = 0;
            return false;
        }
    }
    // What casts, shared by every tile; the pass's own textures are its cut-outs'.
    std::vector<caster_t> casters;
    std::vector<material_data_t> materials;
    std::vector<std::uint64_t> sampled;
    if (!gather_casters(world, tweened, atlas, casters, materials, sampled)) {
        return false;
    }
    std::vector<draw_data_t> draws;
    draws.reserve(casters.size());
    for (const caster_t& caster : casters) {
        draws.push_back(caster.data);
    }
    bool failed = false;
    const VkDeviceAddress draws_address = in_ring<draw_data_t>(draws, failed).address;
    const VkDeviceAddress materials_address = in_ring<material_data_t>(materials, failed).address;
    auto pass = std::make_shared<recorded_pass_t>();
    pass->format = VK_FORMAT_R32_SFLOAT;
    pass->samples = VK_SAMPLE_COUNT_1_BIT;
    pass->shadows = true;
    const auto whole = static_cast<float>(side);
    for (std::size_t at = 0; at < tiles.size(); ++at) {
        const tile_t& tile = tiles[at];
        const place_t& where = places[at];
        camera_data_t data{};
        data.projection = tile.to_clip.m;
        data.draws = draws_address;
        data.materials = materials_address;
        const std::array one{ data };
        view_batches_t view_batches;
        view_batches.camera = in_ring<camera_data_t>(one, failed).address;
        view_batches.has_viewport = true;
        view_batches.viewport = {
            .x = static_cast<float>(where.x),
            .y = static_cast<float>(where.y),
            .width = static_cast<float>(where.size),
            .height = static_cast<float>(where.size),
            .minDepth = 0.0F,
            .maxDepth = 1.0F,
        };
        const auto tile_x = static_cast<std::int32_t>(where.x);
        const auto tile_y = static_cast<std::int32_t>(where.y);
        view_batches.scissor = {
            .offset = { .x = tile_x, .y = tile_y },
            .extent = { .width = where.size, .height = where.size },
        };
        const std::array<std::array<float, 4>, 6> planes = planes_of(tile.to_clip);
        for (std::size_t i = 0; i < casters.size(); ++i) {
            const caster_t& caster = casters[i];
            const bool seen = std::ranges::all_of(planes, [&caster](const std::array<float, 4>& p) {
                return (p[0] * caster.center.x) + (p[1] * caster.center.y) +
                           (p[2] * caster.center.z) + p[3] >=
                       -caster.radius;
            });
            if (!seen) {
                continue;
            }
            if (!view_batches.batches.empty()) {
                batch_t& last = view_batches.batches.back();
                if (last.key == caster.key && last.indices == caster.indices &&
                    last.index_offset == caster.index_offset &&
                    last.index_count == caster.index_count && last.first_draw + last.draws == i) {
                    ++last.draws;
                    continue;
                }
            }
            batch_t batch;
            batch.key = caster.key;
            batch.indices = caster.indices;
            batch.index_offset = caster.index_offset;
            batch.index_count = caster.index_count;
            batch.first_draw = static_cast<std::uint32_t>(i);
            view_batches.batches.push_back(batch);
        }
        pass->views.push_back(std::move(view_batches));
        frame.world_to_clip.push_back(tile.to_clip);
        frame.records.push_back({
            .rect = { static_cast<float>(where.x), static_cast<float>(where.y),
                      static_cast<float>(where.x + where.size - 1),
                      static_cast<float>(where.y + where.size - 1), },
            .bias = tile.bias,
            .normal_bias = tile.normal_bias * tile.texel,
            .depth_per = tile.depth_per,
            .split = tile.split,
            .level = tile.level ? 1U : 0U,
            .inverse_side = 1.0F / whole,
        });
    }
    if (failed) {
        return false;
    }
    std::ranges::sort(sampled);
    const auto [last, end] = std::ranges::unique(sampled);
    sampled.erase(last, end);
    std::vector<std::pair<gpu::resource_t, gpu::use_t>> uses;
    uses.reserve(sampled.size());
    for (const std::uint64_t texture : sampled) {
        const gpu::resource_t resource = gpu::texture_resource(texture);
        if (resource != gpu::no_resource) {
            uses.emplace_back(resource, gpu::use_t::SAMPLED);
        }
    }
    const gpu::resource_t into = gpu::texture_resource(atlas);
    if (into == gpu::no_resource) {
        return false;
    }
    frame.atlas = atlas;
    for (const auto& [light, first] : firsts) {
        frame.firsts.emplace(light, first);
    }
    return gpu::add_pass({
        .name = "shadows",
        .colors = { {
            .target = into,
            .clear = true,
        }, },
        .depth = into,
        .clear_depth = true,
        .uses = std::move(uses),
        .record = [pass](VkCommandBuffer commands) { record_pass(commands, *pass); },
    });
}

} // namespace fbe::render
