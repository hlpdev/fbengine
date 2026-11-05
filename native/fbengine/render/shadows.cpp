// Shadows: before a camera draws, every shadowed light that reaches what it sees gets tiles of one
// atlas, the nearest first, and what casts is drawn into them from the light. A pass of R32F, each
// texel the depth of the nearest thing there, as the bindless set has only color images to sample:
// PBR's fragment shader compares with them itself. A directional light has a cascade a tile, each
// around its slice of a view, on a texel grid that doesn't move as the camera does, and a mirror's
// reflection has its own; a spot light a tile; a point light six, along the world's axes.

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/graph.hpp"
#include "gpu/vulkan.hpp"
#include "render/drawing.hpp"
#include "render/render.hpp"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"
#include "shader/shader.hpp"
#include "skin/skin.hpp"
#include "texture/texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <unordered_map>
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
/// How many texels past a point light's face its tile looks, so what compares at a face's edge,
/// moved off it along its normal, has all of the filter's texels.
constexpr float face_margin = 4.0F;
/// The widest a spot light's cone is for one tile to see, as a tile can't look all the way to its
/// side: past it, it has six, as a point light has.
constexpr float widest_cone = 1.2F;
/// The smallest a tile gets, and the most a light's tiles shrink to have a place at all.
constexpr std::uint32_t least_tile = 16;
constexpr std::uint32_t most_shrink = 4;
/// The atlas's sides: the smallest, and how many frames it stays too big, or unused.
constexpr std::uint32_t least_atlas = 256;
constexpr std::uint64_t atlas_patience = 60;

/// The atlas, kept while the GPU's up; how big it's been wanted lately; the most it can be; and the
/// side the GPU had no memory for, if it's been refused.
std::uint64_t atlas = 0;
std::uint32_t atlas_size = 0;
std::uint32_t atlas_peak = 0;
std::uint64_t atlas_checked = 0;
std::uint32_t atlas_limit = 4096;
std::uint32_t atlas_refused = std::numeric_limits<std::uint32_t>::max();
bool registered = false;

void forget_atlas() {
    // Gone with the GPU, which frees every texture.
    atlas = 0;
    atlas_size = 0;
    atlas_peak = 0;
    atlas_refused = std::numeric_limits<std::uint32_t>::max();
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
        // as the pipeline holds it at the near plane, and it still casts.
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

/// What casts: a draw of the shadow pass's list, which those of an Instances' clusters share, the
/// sphere it's within, in the world, and whether a camera can fade it out.
struct caster_t {
    draw_data_t data{};
    std::uint32_t draw = 0;
    std::uint64_t key = 0;
    VkBuffer indices = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    std::uint32_t index_count = 0;
    vec3_t center;
    float radius = 0.0F;
    std::uint32_t entity = none;
    bool fades = false;
    std::uint32_t instance_first = 0;
    std::uint32_t instance_count = 0;
};

/// The key of a caster's pipeline, as fbe_render.h lays its bits out: only what the shadow pass
/// goes by, which is its shader of its own, as that can move its vertices, and a cut-out's alpha:
/// its cutoff, vertex colors, and layers, as it draws by them.
std::uint64_t caster_key(const scene::material_t& material) {
    std::uint64_t key = (static_cast<std::uint64_t>(FBE_BLEND_OPAQUE) << 2U) |
                        (static_cast<std::uint64_t>(FBE_DEPTH_TEST_AND_WRITE) << 10U) |
                        (static_cast<std::uint64_t>(shaders_own::code_of(material.shader)) << 52U);
    if (material.alpha_cutoff <= 0.0F) {
        return key;
    }
    std::uint64_t layers = 0;
    std::uint64_t layer_count = 0;
    for (const scene::layer_t& layer : material.layers) {
        if (layer.texture != 0) {
            const auto blend = static_cast<std::uint64_t>(layer.blend);
            const auto uv_set = static_cast<std::uint64_t>(layer.uv_set);
            layers |= (blend | (uv_set << 3U)) << (layer_count * 4U);
            ++layer_count;
        }
    }
    key |= (std::uint64_t{ 1 } << 5U) |
           (static_cast<std::uint64_t>(material.vertex_colors ? 1 : 0) << 6U) |
           (layer_count << 13U) | (layers << 17U);
    return key;
}

/// What casts in a world at a tween, in the frame's ring, kept for the cameras of one
/// fbe_render_world, between which nothing can change it.
struct gathered_t {
    bool keep = false;
    bool have = false;
    std::uint32_t world = none;
    float tween = 0.0F;
    std::vector<caster_t> casters;
    std::vector<std::uint64_t> sampled;
    VkDeviceAddress draws = 0;
    VkDeviceAddress materials = 0;
};

gathered_t& gathered() {
    static gathered_t made;
    return made;
}

/// Every mesh surface and terrain of a world that casts, sorted so what can go together does. A
/// material's only there for a cut-out or a shader of its own, which is all that reads one. A mesh
/// with levels of detail casts what the camera draws of it, so those aren't kept between cameras.
bool gather_casters(const scene::camera_t& camera, const scene::view_t& view, std::uint32_t world,
                    const scene::tweened_t& tweened) {
    gathered_t& found = gathered();
    if (found.have && found.world == world && found.tween == tweened.tween()) {
        return true;
    }
    std::vector<caster_t>& casters = found.casters;
    casters.clear();
    found.sampled.clear();
    const scene::scene_t& s = scene::scene();
    float half_width = 0.0F;
    float half_height = 0.0F;
    scene::half_extents(camera, view, half_width, half_height);
    bool any_lods = false;
    std::vector<material_data_t> materials;
    std::unordered_map<std::uint32_t, std::uint32_t> material_index;
    std::unordered_map<std::uint32_t, std::pair<VkDeviceAddress, std::uint32_t>> palettes;
    const scene::material_t standard;
    const auto index_of = [&](std::uint32_t material, const scene::material_t& own) {
        if (own.alpha_cutoff <= 0.0F && own.shader == 0) {
            return 0U;
        }
        const auto [at, added] =
            material_index.try_emplace(material, static_cast<std::uint32_t>(materials.size()));
        if (added) {
            materials.push_back(material_data(material, 0, found.sampled));
        }
        return at->second;
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
        any_lods = any_lods || !s.geometries[mesh.geometry].lods.empty();
        const std::uint32_t drawn = mesh_lod(mesh, matrix, tweened, camera, view);
        if (drawn == none) {
            continue;
        }
        const scene::geometry_t& whole = s.geometries[mesh.geometry];
        // Its cull box, if its geometry has one, else its bounds, as what's drawn goes by.
        vec3_t low = whole.cull_min;
        vec3_t high = whole.cull_max;
        const std::span<const float> weights = anim::morph_weights(mesh.entity, tweened.tween());
        if (!whole.has_cull_box) {
            scene::geometry_bounds(mesh.geometry, low, high);
            anim::grow_by_targets(mesh.geometry, weights, low, high);
        }
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
        if (!whole.has_cull_box &&
            anim::skin_joints_box(mesh.entity, tweened, joints_low, joints_high)) {
            const vec3_t both_joints = joints_low + joints_high;
            center = both_joints * 0.5F;
            radius += (scene::length(joints_high - joints_low) * 0.5F) + radius;
        }
        const scene::geometry_t& geometry = s.geometries[drawn];
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
                surface_buffers(s.geometry_slots.handle(drawn), surface);
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
            caster.data.material = index_of(slot, material);
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
            if (!buffers->target_moves.empty()) {
                std::array<anim::drawn_target_t, anim::drawn_targets> strongest;
                const std::size_t count =
                    anim::strongest_targets(weights, buffers->target_moves, strongest);
                if (count != 0) {
                    bool no_room = false;
                    caster.data.drawn_targets =
                        in_ring<anim::drawn_target_t>(std::span(strongest).first(count), no_room)
                            .address;
                    if (no_room) {
                        return false;
                    }
                    caster.data.targets = buffers->targets.address;
                    caster.data.drawn_target_count = static_cast<std::uint32_t>(count);
                    caster.data.target_vertices = buffers->vertex_count;
                }
            }
            caster.key = caster_key(material);
            caster.indices = buffers->indices.buffer;
            caster.index_count = buffers->index_count;
            caster.center = center;
            caster.radius = radius;
            caster.entity = mesh.entity;
            caster.fades = s.entities.fade_far[mesh.entity] > 0.0F;
            casters.push_back(caster);
        }
    }
    for (scene::instances_t& found_instances : scene::scene().instances) {
        if (found_instances.geometry == none || !casts(found_instances.entity)) {
            continue;
        }
        const mat4_t matrix = tweened.pose(found_instances.entity).matrix;
        const std::vector<scene::instance_cluster_t>& clusters =
            scene::instance_clusters(found_instances);
        const VkDeviceAddress records = instances_buffer(found_instances);
        if (records == 0) {
            return false;
        }
        const float scale =
            std::fmax(std::fmax(scene::length(matrix.axis(0)), scene::length(matrix.axis(1))),
                      scene::length(matrix.axis(2)));
        any_lods = any_lods || !s.geometries[found_instances.geometry].lods.empty();
        // Any of them with an alpha has them all blend, as it draws them, which doesn't cast.
        const float alpha = found_instances.faded != 0 ? 0.5F : 1.0F;
        for (const scene::instance_level_t& range :
             scene::instances_levels(found_instances, matrix, camera, view.position, half_height)) {
            const std::uint32_t level = range.level;
            const scene::geometry_t& geometry = s.geometries[level];
            for (std::size_t surface = 0; surface < geometry.surfaces.size(); ++surface) {
                const std::uint32_t slot = found_instances.material != none
                                               ? found_instances.material
                                               : geometry.surfaces[surface].material;
                const scene::material_t& material = slot != none ? s.materials[slot] : standard;
                if (material.order != 0 || scene::drawn_blend(slot, alpha) != FBE_BLEND_OPAQUE) {
                    continue;
                }
                const surface_buffers_t* buffers =
                    surface_buffers(s.geometry_slots.handle(level), surface);
                if (buffers == nullptr) {
                    return false;
                }
                if (buffers->index_count == 0) {
                    continue;
                }
                for (const scene::instance_cluster_t& cluster : clusters) {
                    if (cluster.mirrored != range.mirrored ||
                        scene::cluster_lod(found_instances, cluster, matrix, camera, view.position,
                                           half_height) != level) {
                        continue;
                    }
                    const vec3_t both = cluster.low + cluster.high;
                    caster_t caster;
                    caster.data.to_view = matrix.m;
                    caster.data.color = { 1.0F, 1.0F, 1.0F, 1.0F };
                    caster.data.vertices = buffers->vertices.address;
                    caster.data.instances = records;
                    caster.data.material = index_of(slot, material);
                    caster.key = caster_key(material) | (std::uint64_t{ 1 } << 51U);
                    caster.indices = buffers->indices.buffer;
                    caster.index_count = buffers->index_count;
                    caster.center = scene::transform_point(matrix, both * 0.5F);
                    caster.radius = scene::length(cluster.high - cluster.low) * 0.5F * scale;
                    caster.entity = found_instances.entity;
                    caster.fades = s.entities.fade_far[found_instances.entity] > 0.0F;
                    caster.instance_first = cluster.first;
                    caster.instance_count = cluster.count;
                    casters.push_back(caster);
                }
            }
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
        const std::uint32_t index = index_of(terrain.material, material);
        for (const terrain_piece_t& piece : pieces) {
            caster_t caster;
            caster.data.to_view = matrix.m;
            caster.data.color = { 1.0F, 1.0F, 1.0F, 1.0F };
            caster.data.vertices = piece.vertices;
            caster.data.material = index;
            caster.key = caster_key(material);
            caster.indices = piece.indices;
            caster.index_offset = piece.index_offset;
            caster.index_count = piece.index_count;
            caster.center = piece.center;
            caster.radius = piece.radius;
            caster.entity = terrain.entity;
            caster.fades = s.entities.fade_far[terrain.entity] > 0.0F;
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
    // An Instances' clusters, each of which went next to the last, share a draw.
    std::vector<draw_data_t> draws;
    draws.reserve(casters.size());
    const caster_t* last_caster = nullptr;
    for (caster_t& caster : casters) {
        const bool shares = last_caster != nullptr && caster.instance_count != 0 &&
                            last_caster->instance_count != 0 &&
                            last_caster->entity == caster.entity &&
                            last_caster->indices == caster.indices &&
                            last_caster->data.material == caster.data.material;
        if (!shares) {
            draws.push_back(caster.data);
        }
        caster.draw = static_cast<std::uint32_t>(draws.size() - 1);
        last_caster = &caster;
    }
    bool failed = false;
    found.draws = in_ring<draw_data_t>(draws, failed).address;
    found.materials = in_ring<material_data_t>(materials, failed).address;
    if (failed) {
        return false;
    }
    found.have = found.keep && !any_lods;
    found.world = world;
    found.tween = tweened.tween();
    return true;
}

/// A tile as the frame's being worked out: what's seen from it, and how a texel's size goes with
/// distance; and, of a cascade, how many of its light's are left after it.
struct tile_t {
    std::uint32_t size = 0;
    mat4_t to_clip;
    float depth_per = 0.0F;
    float texel = 0.0F;
    float split = 0.0F;
    bool level = false;
    float bias = 0.0F;
    float normal_bias = 0.0F;
    std::uint32_t left = 0;
    bool face = false;
};

/// How far a directional light's cascades go in a view, from its near plane: nothing if that's
/// past where its shadows end.
float cascades_reach(const scene::camera_t& camera, const scene::light_t& light) {
    return std::fmin(light.shadow_distance, camera.farthest);
}

/// Each cascade of a directional light, around its slice of the view. Whatever's nearer its light
/// than a tile's near plane, the pipeline holds there, where it can; where it can't, the tile's
/// near plane is as far back as what it has to cast.
void add_cascades(const scene::camera_t& camera, const scene::view_t& view,
                  const scene::light_t& light, quat_t turn, std::uint32_t size,
                  const std::vector<caster_t>& casters, std::vector<tile_t>& tiles) {
    float half_width = 0.0F;
    float half_height = 0.0F;
    scene::half_extents(camera, view, half_width, half_height);
    const bool looks_in = camera.projection == FBE_PROJECTION_PERSPECTIVE;
    const float start = camera.nearest;
    const float finish = cascades_reach(camera, light);
    const vec3_t forward = view.rotation * vec3_t(0.0F, 0.0F, -1.0F);
    const float wide_squared = (half_width * half_width) + (half_height * half_height);
    const quat_t back = scene::inverse(turn);
    const bool clamps = gpu::device()->depth_clamp;
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
        // It only moves by whole texels, so a shadow's edge doesn't crawl as the camera does. The
        // view's made from those values, and never through the world, whose floats would lose it.
        const float texel = 2.0F * radius / static_cast<float>(size);
        const vec3_t in_view = view.position + forward * middle;
        const vec3_t center = back * in_view;
        const float across = std::round(center.x / texel) * texel;
        const float up = std::round(center.y / texel) * texel;
        float top = center.z + radius;
        if (!clamps) {
            for (const caster_t& caster : casters) {
                const vec3_t at = back * caster.center;
                if (std::fabs(at.x - across) <= radius + caster.radius &&
                    std::fabs(at.y - up) <= radius + caster.radius) {
                    top = std::fmax(top, at.z + caster.radius);
                }
            }
        }
        const float depth = top - (center.z - radius);
        tile_t tile;
        tile.size = size;
        tile.to_clip = orthographic(radius, depth) *
                       scene::compose(vec3_t(-across, -up, -top), back, vec3_t(1.0F, 1.0F, 1.0F));
        tile.depth_per = 1.0F / depth;
        tile.texel = texel;
        tile.split = to;
        tile.level = true;
        tile.left = static_cast<std::uint32_t>(light.cascades - cascade - 1);
        tiles.push_back(tile);
        from = to;
    }
}

/// The nearest a perspective tile of a light sees.
float nearest_of(const scene::light_t& light) {
    return std::fmax(light.range * 0.002F, 0.01F);
}

/// A perspective tile: a spot light's, or one face of a point light's.
void add_look(const scene::light_t& light, vec3_t place, quat_t turn, float field,
              std::uint32_t size, std::vector<tile_t>& tiles) {
    const float nearest = nearest_of(light);
    tile_t tile;
    tile.size = size;
    tile.to_clip = perspective(field, nearest, light.range) * view_of(place, turn);
    tile.depth_per = nearest * light.range / (light.range - nearest);
    tile.texel = 2.0F * std::tan(field * 0.5F) / static_cast<float>(size);
    tiles.push_back(tile);
}

/// A point light's six, +X, -X, +Y, -Y, +Z, -Z, as the shader finds them by a way's biggest part,
/// each looking far enough past its face for a tile of this size.
void add_faces(const scene::light_t& light, vec3_t place, std::uint32_t size,
               std::vector<tile_t>& tiles) {
    const auto side = static_cast<float>(size);
    const float field = 2.0F * std::atan(side / (side - (face_margin * 2.0F)));
    const float half_pi = std::numbers::pi_v<float> * 0.5F;
    const vec3_t up(0.0F, 1.0F, 0.0F);
    const vec3_t right(1.0F, 0.0F, 0.0F);
    add_look(light, place, scene::axis_angle(up, -half_pi), field, size, tiles);
    add_look(light, place, scene::axis_angle(up, half_pi), field, size, tiles);
    add_look(light, place, scene::axis_angle(right, half_pi), field, size, tiles);
    add_look(light, place, scene::axis_angle(right, -half_pi), field, size, tiles);
    add_look(light, place, scene::axis_angle(up, std::numbers::pi_v<float>), field, size, tiles);
    add_look(light, place, quat_t(), field, size, tiles);
    for (tile_t& tile : tiles) {
        tile.face = true;
    }
}

/// A light that wants tiles: which, how it's placed, for a directional light which view it's for,
/// how many tiles and how big, and how soon it's seen to.
struct wanted_t {
    std::uint32_t entity = none;
    const scene::light_t* light = nullptr;
    scene::pose_t pose;
    std::size_t view = 0;
    std::uint32_t count = 1;
    std::uint32_t size = 0;
    float rank = 0.0F;
};

/// Whether a sphere's in sight of any of the views.
bool seen_from(const scene::camera_t& camera, std::span<const scene::view_t> views, vec3_t center,
               float radius) {
    std::array<vec3_t, 8> corners;
    for (unsigned corner = 0; corner < 8; ++corner) {
        corners[corner] = center + vec3_t((corner & 1U) != 0 ? radius : -radius,
                                          (corner & 2U) != 0 ? radius : -radius,
                                          (corner & 4U) != 0 ? radius : -radius);
    }
    return std::ranges::any_of(views, [&](const scene::view_t& view) {
        return scene::in_view(camera, view, corners.data(), corners.size());
    });
}

/// Every shadowed light that can show in these views, the soonest first: first the directional
/// lights of the camera's own view, then what's nearest it, and last a reflection's directional
/// lights. In the tree's order where it's all the same, so which keep theirs doesn't change.
std::vector<wanted_t> wanted_lights(const scene::camera_t& camera,
                                    std::span<const scene::view_t> views,
                                    const scene::tweened_t& tweened) {
    const scene::scene_t& s = scene::scene();
    std::vector<std::uint32_t> found;
    scene::lights_in(s.worlds[s.entities.world[camera.entity]].roots, found);
    const std::size_t own_view = views.size() - 1;
    const vec3_t eye = views[own_view].position;
    std::vector<wanted_t> wanted;
    for (const std::uint32_t entity : found) {
        const scene::light_t& light = s.lights[s.entities.component[entity]];
        if (!light.shadows || !scene::showing(entity) || light.intensity <= 0.0F) {
            continue;
        }
        wanted_t one;
        one.entity = entity;
        one.light = &light;
        one.pose = tweened.pose(entity);
        one.size = static_cast<std::uint32_t>(light.shadow_size);
        const vec3_t place = one.pose.matrix.axis(3);
        if (light.type == FBE_LIGHT_DIRECTIONAL) {
            // Its shadows end before the camera's near plane: it has none to show.
            if (cascades_reach(camera, light) <= camera.nearest) {
                continue;
            }
            one.count = static_cast<std::uint32_t>(light.cascades);
            for (std::size_t view = 0; view < views.size(); ++view) {
                one.view = view;
                one.rank = view == own_view ? -1.0F : std::numeric_limits<float>::max();
                wanted.push_back(one);
            }
            continue;
        }
        // Too small to have a tile of any shape: no shadow, and nothing wrong with it.
        const bool spot = light.type == FBE_LIGHT_SPOT;
        if (light.range <= nearest_of(light) * 2.0F || (spot && light.outer_cone <= 0.001F) ||
            !seen_from(camera, views, place, light.range)) {
            continue;
        }
        one.count = spot && light.outer_cone <= widest_cone ? 1U : 6U;
        one.rank = std::fmax(scene::length(place - eye) - light.range, 0.0F);
        wanted.push_back(one);
    }
    std::ranges::stable_sort(wanted, {}, &wanted_t::rank);
    return wanted;
}

/// Which of the wanted lights have a place in an atlas of `side`, each at its own size or, if
/// that's no longer there, as far down as it shrinks: what's left of it after that goes without.
/// Each of `kept`'s tiles has its side in `sizes`.
void place_lights(const std::vector<wanted_t>& wanted, std::uint32_t side,
                  std::vector<wanted_t>& kept, std::vector<std::uint32_t>& sizes) {
    kept.clear();
    sizes.clear();
    std::uint64_t area = 0;
    std::uint32_t tallest = 0;
    std::vector<place_t> places;
    std::vector<std::uint32_t> trying;
    for (const wanted_t& one : wanted) {
        const std::uint32_t size = std::min(one.size, side);
        const std::uint32_t least = std::max(std::min(least_tile, size), size / most_shrink);
        for (std::uint32_t at = size; at >= least && at != 0; at /= 2) {
            const std::uint64_t more = area + (std::uint64_t{ one.count } * at * at);
            const std::uint32_t tall = std::max(tallest, at);
            // Shelves of squares, biggest first, are no taller than twice their area over their
            // width and one shelf: so most needn't be tried.
            bool fits = (more * 2 / side) + tall <= side;
            if (!fits) {
                trying = sizes;
                trying.insert(trying.end(), one.count, at);
                fits = pack(trying, side, places);
            }
            if (fits) {
                wanted_t placed = one;
                placed.size = at;
                kept.push_back(placed);
                sizes.insert(sizes.end(), one.count, at);
                area = more;
                tallest = tall;
                break;
            }
        }
    }
}

/// The atlas with room for a side, as big as it's been lately: false if the GPU can't make it.
bool atlas_for(std::uint32_t side) {
    atlas_peak = std::max(atlas_peak, side);
    if (atlas != 0 && atlas_size >= side) {
        return true;
    }
    if (!registered) {
        gpu::at_stop(forget_atlas);
        registered = true;
    }
    if (atlas != 0) {
        fbe_texture_release(atlas);
        atlas = 0;
        atlas_size = 0;
    }
    if (side >= atlas_refused) {
        return false;
    }
    atlas = texture::create_engine_target(side, side, texture::shadow_format, VK_SAMPLE_COUNT_1_BIT,
                                          true);
    if (atlas == 0) {
        atlas_refused = side;
        return false;
    }
    atlas_size = side;
    return true;
}

} // namespace

void keep_shadow_casters(bool keep) {
    gathered().keep = keep;
    gathered().have = false;
}

void forget_unused_atlas() {
    const auto frame = static_cast<std::uint64_t>(fbe_frame_count());
    if (atlas == 0 || frame - atlas_checked < atlas_patience) {
        return;
    }
    atlas_checked = frame;
    if (atlas_peak < atlas_size) {
        fbe_texture_release(atlas);
        atlas = 0;
        atlas_size = 0;
    }
    atlas_peak = 0;
}

std::uint32_t shadow_atlas_side() {
    return atlas_size;
}

void refuse_shadow_atlas(std::uint32_t side) {
    atlas_refused = side;
}

bool draw_shadows(const scene::camera_t& camera, std::span<const scene::view_t> views,
                  const scene::tweened_t& tweened, shadow_frame_t& frame) {
    const scene::scene_t& s = scene::scene();
    const std::uint32_t world = s.entities.world[camera.entity];
    frame.firsts.assign(views.size(), {});
    const std::vector<wanted_t> wanted = wanted_lights(camera, views, tweened);
    if (wanted.empty()) {
        return true;
    }
    // The atlas as big as they need, as far as the device goes, and as far as there's memory for:
    // whose don't fit go without, and the camera draws with none if it comes to that.
    std::uint32_t most =
        std::min(gpu::device()->properties.limits.maxFramebufferWidth, atlas_limit);
    std::vector<wanted_t> kept;
    std::vector<std::uint32_t> sizes;
    std::vector<place_t> places;
    std::uint32_t side = least_atlas;
    while (true) {
        place_lights(wanted, most, kept, sizes);
        if (kept.empty()) {
            return true;
        }
        side = least_atlas;
        while (!pack(sizes, side, places)) {
            side = std::min(side * 2, most);
        }
        if (atlas_for(side)) {
            break;
        }
        if (side <= least_atlas) {
            return true;
        }
        most = side / 2;
    }
    if (!gather_casters(camera, views.back(), world, tweened)) {
        return false;
    }
    const std::vector<caster_t>& casters = gathered().casters;
    // What this camera's faded out of it doesn't cast, for it.
    std::vector<bool> faded(casters.size());
    for (std::size_t i = 0; i < casters.size(); ++i) {
        faded[i] = casters[i].fades && scene::entity_fade(casters[i].entity, camera.entity) <= 0.0F;
    }
    auto pass = std::make_shared<recorded_pass_t>();
    pass->format = VK_FORMAT_R32_SFLOAT;
    pass->samples = VK_SAMPLE_COUNT_1_BIT;
    pass->shadows = true;
    bool failed = false;
    std::size_t at = 0;
    std::vector<std::uint32_t> nearby;
    std::vector<tile_t> tiles;
    for (const wanted_t& one : kept) {
        const scene::light_t& light = *one.light;
        const vec3_t place = one.pose.matrix.axis(3);
        const std::uint64_t handle = s.entities.slots.handle(one.entity);
        const auto first = static_cast<std::uint32_t>(frame.records.size());
        tiles.clear();
        nearby.clear();
        if (light.type == FBE_LIGHT_DIRECTIONAL) {
            frame.firsts[one.view].emplace(handle, first);
            add_cascades(camera, views[one.view], light, one.pose.rotation, one.size, casters,
                         tiles);
            for (std::size_t i = 0; i < casters.size(); ++i) {
                nearby.push_back(static_cast<std::uint32_t>(i));
            }
        } else {
            for (std::unordered_map<std::uint64_t, std::uint32_t>& firsts : frame.firsts) {
                firsts.emplace(handle, first);
            }
            if (one.count == 1) {
                add_look(light, place, one.pose.rotation, light.outer_cone * 2.04F, one.size,
                         tiles);
            } else {
                add_faces(light, place, one.size, tiles);
            }
            // Only what its light's range reaches, for all its tiles.
            for (std::size_t i = 0; i < casters.size(); ++i) {
                const float reach = light.range + casters[i].radius;
                const vec3_t apart = casters[i].center - place;
                if (scene::dot(apart, apart) <= reach * reach) {
                    nearby.push_back(static_cast<std::uint32_t>(i));
                }
            }
        }
        for (const tile_t& tile : tiles) {
            const place_t& where = places[at++];
            camera_data_t data{};
            data.projection = tile.to_clip.m;
            data.draws = gathered().draws;
            data.materials = gathered().materials;
            // What a cut-out's cut by is biased as what it draws is.
            data.lod_bias = gpu::lod_bias_left();
            const std::array one_camera{ data };
            view_batches_t view_batches;
            view_batches.camera = in_ring<camera_data_t>(one_camera, failed).address;
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
            for (const std::uint32_t i : nearby) {
                const caster_t& caster = casters[i];
                if (faded[i]) {
                    continue;
                }
                const bool seen =
                    std::ranges::all_of(planes, [&caster](const std::array<float, 4>& p) {
                        return (p[0] * caster.center.x) + (p[1] * caster.center.y) +
                                   (p[2] * caster.center.z) + p[3] >=
                               -caster.radius;
                    });
                if (!seen) {
                    continue;
                }
                if (!view_batches.batches.empty()) {
                    batch_t& last = view_batches.batches.back();
                    const bool alike = last.key == caster.key && last.indices == caster.indices &&
                                       last.index_offset == caster.index_offset &&
                                       last.index_count == caster.index_count;
                    if (alike && caster.instance_count == 0 &&
                        last.first_draw + last.draws == caster.draw) {
                        ++last.draws;
                        continue;
                    }
                    if (alike && caster.instance_count != 0 && last.first_draw == caster.draw &&
                        last.instance_first + last.instance_count == caster.instance_first) {
                        last.instance_count += caster.instance_count;
                        continue;
                    }
                }
                batch_t batch;
                batch.key = caster.key;
                batch.indices = caster.indices;
                batch.index_offset = caster.index_offset;
                batch.index_count = caster.index_count;
                batch.first_draw = caster.draw;
                batch.instance_first = caster.instance_first;
                batch.instance_count = caster.instance_count;
                view_batches.batches.push_back(batch);
            }
            pass->views.push_back(std::move(view_batches));
            frame.world_to_clip.push_back(tile.to_clip);
            frame.records.push_back({
                .rect = { static_cast<float>(where.x), static_cast<float>(where.y),
                          static_cast<float>(where.x + where.size - 1),
                          static_cast<float>(where.y + where.size - 1), },
                .bias = light.shadow_bias,
                .normal_bias = light.shadow_normal_bias * tile.texel,
                .depth_per = tile.depth_per,
                .split = tile.split,
                .level = tile.level ? 1U : 0U,
                .inverse_side = 1.0F / static_cast<float>(atlas_size),
                .left = tile.left,
                .face = tile.face ? 1U : 0U,
            });
        }
    }
    if (failed) {
        return false;
    }
    std::vector<std::uint64_t> sampled = gathered().sampled;
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

extern "C" {

FBE_API bool fbe_render_set_shadow_atlas_limit(int32_t side) {
    if (side < 256 || side > 8192) {
        return fbe::fail(std::format("a shadow atlas's side is at most 256 to 8192, not {}", side));
    }
    fbe::render::atlas_limit = static_cast<std::uint32_t>(side);
    return true;
}

FBE_API int32_t fbe_render_shadow_atlas_limit(void) {
    return static_cast<std::int32_t>(fbe::render::atlas_limit);
}

} // extern "C"
