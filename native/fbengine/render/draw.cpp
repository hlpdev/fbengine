// Drawing a camera: what read_lists read of it goes into the frame's ring as the scene shader reads
// it, its geometry onto the GPU, and all of it into a pass of the frame graph, whose draws of one
// geometry and pipeline go together as instances, each instance a draw of the list.

#include "core/engine.hpp"
#include "fbengine.h"
#include "gpu/buffer.hpp"
#include "gpu/graph.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/vulkan.hpp"
#include "render/render.hpp"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"
#include "texture/texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <engine_shaders.hpp>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbe::render {

namespace {

using gpu::flag;
using scene::mat4_t;
using scene::none;
using scene::vec3_t;

/// A draw as the scene shader reads it: its matrix to the camera's view; the rows of what turns
/// its normals there; its color and alpha; its vertices' address; its joints' and palette's, if
/// it's skinned; its material's index in the camera's; its own lights; and whether it's skinned.
struct draw_data_t {
    std::array<float, 16> to_view;
    std::array<std::array<float, 4>, 3> normals;
    std::array<float, 4> color;
    VkDeviceAddress vertices;
    VkDeviceAddress joints;
    VkDeviceAddress palette;
    std::uint32_t material;
    std::uint32_t light_first;
    std::uint32_t light_count;
    std::uint32_t skinned;
};
static_assert(sizeof(draw_data_t) == 168, "the scene shader reads draws 168 bytes apart");

/// A layer as the scene shader reads it: its texture's and sampler's indices in the bindless set,
/// how a bump layer scales the next layer's light, what turns its UVs, by rows, and a bump layer's
/// matrix.
struct layer_data_t {
    std::uint32_t texture;
    std::uint32_t sampler;
    float luminance_scale;
    float luminance_offset;
    std::array<float, 4> uv_across;
    std::array<float, 4> uv_down;
    std::array<float, 4> bump;
};
static_assert(sizeof(layer_data_t) == 64, "the scene shader reads layers 64 bytes apart");

/// A material as the scene shader reads it.
struct material_data_t {
    std::array<float, 4> color;
    float shininess;
    float cutoff;
    std::uint32_t no_alpha;
    std::uint32_t spare;
    std::array<layer_data_t, FBE_MATERIAL_LAYERS> layers;
};
static_assert(sizeof(material_data_t) == 544, "the scene shader reads materials 544 bytes apart");

/// A camera as the scene shader reads it.
struct camera_data_t {
    std::array<float, 16> projection;
    std::array<float, 4> viewport;
    std::array<float, 4> fog_color;
    float fog_start;
    float fog_end;
    float fog_density;
    float slice_scale;
    std::array<float, 3> ambient;
    float slice_bias;
    std::uint32_t perspective;
    std::uint32_t flipped;
    std::uint32_t directional;
    std::uint32_t spare;
    VkDeviceAddress draws;
    VkDeviceAddress materials;
    VkDeviceAddress lights;
    VkDeviceAddress clusters;
    VkDeviceAddress cluster_lights;
    VkDeviceAddress draw_lights;
};
static_assert(sizeof(camera_data_t) == 192, "the scene shader reads a camera of 192 bytes");
static_assert(sizeof(fbe_draw_light_t) == 64, "the scene shader reads lights 64 bytes apart");
static_assert(sizeof(fbe_vertex_t) == 56, "the scene shader reads vertices 56 bytes apart");

/// Draws of one pipeline and one piece of geometry: their pipeline key, their indices, and which
/// of the list's draws, one after another, as instances.
struct batch_t {
    std::uint64_t key = 0;
    VkBuffer indices = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    std::uint32_t index_count = 0;
    std::uint32_t first_draw = 0;
    std::uint32_t draws = 1;
};

/// What a pass draws from one view of a camera: the camera's address, whether the view's mirrored
/// across, as a reflection's is, turning every triangle's front to its back, and its batches.
struct view_batches_t {
    VkDeviceAddress camera = 0;
    bool flipped = false;
    std::vector<batch_t> batches;
};

/// A pass's draws, into a target: what to clear of its viewport first, where the pass's own
/// clearing would clear more of the target than that.
struct recorded_pass_t {
    std::vector<view_batches_t> views;
    VkViewport viewport{};
    VkRect2D scissor{};
    bool clear_color = false;
    bool clear_depth = false;
    std::array<float, 4> clear_rgba{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

/// What gives the palettes meshes are skinned by, once there's a skin area to.
palette_source_t palette_source = nullptr;

gpu::blend_t blend_of(std::uint64_t key) {
    switch ((key >> 2U) & 7U) {
    case FBE_BLEND_ALPHA:
        return gpu::blend_t::ALPHA;
    case FBE_BLEND_ADD:
        return gpu::blend_t::ADD;
    case FBE_BLEND_MULTIPLY:
        return gpu::blend_t::MULTIPLY;
    default:
        return gpu::blend_t::NONE;
    }
}

/// The scene shader's pipeline for a key, as fbe_render.h lays its bits out, into a target.
gpu::pipeline_desc_t desc_of(std::uint64_t key, VkFormat format, VkSampleCountFlagBits samples,
                             bool flipped) {
    const auto depth_mode = static_cast<std::int32_t>((key >> 10U) & 3U);
    const bool two_sided = ((key >> 7U) & 1U) != 0;
    const bool mirrored = ((key >> 12U) & 1U) != 0;
    const std::uint64_t layers = key >> 17U;
    std::uint32_t blends = 0;
    std::uint32_t sets = 0;
    for (unsigned layer = 0; layer < FBE_MATERIAL_LAYERS; ++layer) {
        const auto packed = static_cast<std::uint32_t>((layers >> (layer * 4U)) & 15U);
        blends |= (packed & 7U) << (layer * 4U);
        sets |= (packed >> 3U) << layer;
    }
    gpu::pipeline_desc_t desc{
        .shader = &shaders::scene,
        .color = format,
        .depth = texture::depth_format,
        .samples = samples,
        .cull = two_sided ? flag<VkCullModeFlags>(VK_CULL_MODE_NONE)
                          : flag<VkCullModeFlags>(VK_CULL_MODE_BACK_BIT),
        .front = mirrored != flipped ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .blend = blend_of(key),
        .depth_test = depth_mode != FBE_DEPTH_NONE,
        .depth_write = depth_mode == FBE_DEPTH_TEST_AND_WRITE,
        .constants = {
            static_cast<std::uint32_t>(key & 3U),
            static_cast<std::uint32_t>((key >> 2U) & 7U),
            static_cast<std::uint32_t>((key >> 5U) & 1U),
            static_cast<std::uint32_t>((key >> 6U) & 1U),
            static_cast<std::uint32_t>((key >> 7U) & 1U),
            static_cast<std::uint32_t>((key >> 8U) & 3U),
            static_cast<std::uint32_t>((key >> 13U) & 15U),
            blends,
            sets,
        },
        .constant_count = 9,
    };
    return desc;
}

/// Records a pass's draws.
void record(VkCommandBuffer commands, const recorded_pass_t& pass) {
    if (pass.clear_color || pass.clear_depth) {
        std::array<VkClearAttachment, 2> clears{};
        std::uint32_t count = 0;
        if (pass.clear_color) {
            clears[count].aspectMask = flag<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT);
            std::ranges::copy(pass.clear_rgba, clears[count].clearValue.color.float32);
            ++count;
        }
        if (pass.clear_depth) {
            clears[count].aspectMask = flag<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT);
            clears[count].clearValue.depthStencil.depth = 0.0F;
            ++count;
        }
        const VkClearRect rect{ .rect = pass.scissor, .baseArrayLayer = 0, .layerCount = 1 };
        vkCmdClearAttachments(commands, count, clears.data(), 1, &rect);
    }
    VkDescriptorSet set = gpu::bindless_set();
    VkPipelineLayout layout = gpu::pipeline_layout();
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                            nullptr);
    vkCmdSetViewport(commands, 0, 1, &pass.viewport);
    vkCmdSetScissor(commands, 0, 1, &pass.scissor);
    for (const view_batches_t& view : pass.views) {
        vkCmdPushConstants(commands, layout, flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0,
                           sizeof(VkDeviceAddress), &view.camera);
        std::optional<std::uint64_t> bound;
        VkBuffer bound_indices = VK_NULL_HANDLE;
        VkDeviceSize bound_offset = 0;
        bool usable = false;
        for (const batch_t& batch : view.batches) {
            if (bound != batch.key) {
                bound = batch.key;
                VkPipeline made =
                    gpu::pipeline(desc_of(batch.key, pass.format, pass.samples, view.flipped));
                usable = made != VK_NULL_HANDLE;
                if (usable) {
                    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, made);
                }
            }
            if (!usable) {
                continue;
            }
            if (batch.indices != bound_indices || batch.index_offset != bound_offset) {
                bound_indices = batch.indices;
                bound_offset = batch.index_offset;
                vkCmdBindIndexBuffer(commands, batch.indices, batch.index_offset,
                                     VK_INDEX_TYPE_UINT32);
            }
            vkCmdDrawIndexed(commands, batch.index_count, batch.draws, 0, 0, batch.first_draw);
            gpu::count_draws(1, std::uint64_t{ batch.index_count } / 3 * batch.draws);
        }
    }
}

/// `found` in the frame's ring, or with `failed` set if there's no memory for it. An empty list is
/// still one zeroed item, since a shader's loads from an address aren't all behind what stops them
/// on every driver: lavapipe's reach a null one with no lane active.
template <typename Item>
gpu::slice_t in_ring(std::span<const Item> found, bool& failed) {
    const std::size_t bytes = std::max(found.size_bytes(), sizeof(Item));
    const gpu::slice_t slice = gpu::frame_memory(bytes, 16);
    if (slice.data == nullptr) {
        failed = true;
        return {};
    }
    std::memset(slice.data, 0, bytes);
    if (!found.empty()) {
        std::memcpy(slice.data, found.data(), found.size_bytes());
    }
    return slice;
}

/// What a material is as the scene shader reads it, its layers those with textures, in order, as
/// its draws' pipeline key has them, and the textures it samples added to `sampled`. A layer can't
/// sample what the pass draws into, so draws white instead, as a layer does whose texture has gone
/// with the GPU.
material_data_t material_data(std::uint32_t material, std::uint64_t target,
                              std::vector<std::uint64_t>& sampled) {
    const scene::material_t standard;
    const scene::material_t& found =
        material != none ? scene::scene().materials[material] : standard;
    material_data_t data{};
    data.color = found.color;
    data.shininess = found.shininess;
    data.cutoff = found.alpha_cutoff;
    data.no_alpha = found.blend == FBE_BLEND_OPAQUE ? 1U : 0U;
    std::size_t at = 0;
    for (const scene::layer_t& layer : found.layers) {
        if (layer.texture == 0) {
            continue;
        }
        layer_data_t& out = data.layers[at++];
        const texture::texture_t* image =
            layer.texture != target ? texture::find(layer.texture) : nullptr;
        if (image != nullptr) {
            out.texture = texture::index_of(layer.texture);
            out.sampler = texture::sampler_of(*image);
            sampled.push_back(layer.texture);
        } else {
            out.texture = texture::white;
            out.sampler =
                gpu::sampler_index(FBE_TEXTURE_NEAREST, FBE_TEXTURE_REPEAT, FBE_TEXTURE_REPEAT);
        }
        out.luminance_scale = layer.bump_scale;
        out.luminance_offset = layer.bump_offset;
        out.bump = layer.bump;
        // As Blitz's: scaled down by its scale, so 2 looks twice as big, turned back by its angle,
        // and moved back by its offset.
        const float across = 1.0F / layer.uv_scale[0];
        const float down = 1.0F / layer.uv_scale[1];
        const float cosine = std::cos(-layer.uv_angle);
        const float sine = std::sin(-layer.uv_angle);
        out.uv_across = { across * cosine, across * sine, -layer.uv_offset[0], 0.0F };
        out.uv_down = { -down * sine, down * cosine, -layer.uv_offset[1], 0.0F };
    }
    return data;
}

/// What a draw is as the scene shader reads it, but for its vertices and material.
draw_data_t draw_data(const fbe_draw_t& draw, const mat4_t& view) {
    mat4_t world;
    std::ranges::copy(draw.matrix, world.m.begin());
    const mat4_t to_view = view * world;
    const vec3_t a = to_view.axis(0);
    const vec3_t b = to_view.axis(1);
    const vec3_t c = to_view.axis(2);
    // The cofactors, the inverse transpose times the determinant, of which only the direction's
    // kept, and the determinant's sign, so a mirror turns a normal with its surface.
    const float sign = scene::mirrors(to_view) ? -1.0F : 1.0F;
    const vec3_t x = scene::cross(b, c) * sign;
    const vec3_t y = scene::cross(c, a) * sign;
    const vec3_t z = scene::cross(a, b) * sign;
    draw_data_t data{};
    data.to_view = to_view.m;
    data.normals[0] = { x.x, y.x, z.x, 0.0F };
    data.normals[1] = { x.y, y.y, z.y, 0.0F };
    data.normals[2] = { x.z, y.z, z.z, 0.0F };
    std::ranges::copy(draw.color, data.color.begin());
    data.light_first = draw.light_first;
    data.light_count = draw.light_count;
    return data;
}

/// The reach of a camera's view from its eye: its far plane's corners', so a ground plane's grid
/// covers everything it sees.
float view_reach(const scene::camera_t& camera, const scene::view_t& view) {
    float half_width = 0.0F;
    float half_height = 0.0F;
    scene::half_extents(camera, view, half_width, half_height);
    const float far_scale =
        camera.projection == FBE_PROJECTION_PERSPECTIVE ? camera.farthest : 1.0F;
    const float across = half_width * far_scale;
    const float up = half_height * far_scale;
    return std::sqrt((across * across) + (up * up) + (camera.farthest * camera.farthest));
}

/// Indices of a grid of `cells` by `cells` squares whose vertices go row by row along Z, each
/// square's two triangles counterclockwise from above.
void grid_indices(std::uint32_t cells, std::uint32_t first, std::vector<std::uint32_t>& indices) {
    const std::uint32_t row = cells + 1;
    for (std::uint32_t z = 0; z < cells; ++z) {
        for (std::uint32_t x = 0; x < cells; ++x) {
            const std::uint32_t a = first + (z * row) + x;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + row;
            const std::uint32_t d = c + 1;
            indices.insert(indices.end(), { a, c, d, a, d, b });
        }
    }
}

/// A ground plane's grid in the frame's ring, under the view around its eye out to all it sees:
/// its vertices' address and its batch's indices, its UVs its own x and z, one repeat a unit.
bool plane_grid(const scene::camera_t& camera, const scene::view_t& view, const item_t& item,
                std::int32_t segments, draw_data_t& data, batch_t& batch) {
    mat4_t world;
    std::ranges::copy(item.draw.matrix, world.m.begin());
    mat4_t into;
    if (!scene::inverse_affine(world, into)) {
        return true;
    }
    const vec3_t eye = scene::transform_point(into, view.position);
    const float reach = view_reach(camera, view);
    const float half_x = reach / std::fmax(scene::length(world.axis(0)), 0.000001F);
    const float half_z = reach / std::fmax(scene::length(world.axis(2)), 0.000001F);
    const auto cells = static_cast<std::uint32_t>(segments);
    std::vector<fbe_vertex_t> vertices;
    vertices.reserve(std::size_t{ cells + 1 } * (cells + 1));
    for (std::uint32_t z = 0; z <= cells; ++z) {
        for (std::uint32_t x = 0; x <= cells; ++x) {
            const float across = (2.0F * static_cast<float>(x) / static_cast<float>(cells)) - 1.0F;
            const float along = (2.0F * static_cast<float>(z) / static_cast<float>(cells)) - 1.0F;
            const float px = eye.x + (across * half_x);
            const float pz = eye.z + (along * half_z);
            vertices.push_back({
                .position = { .x = px, .y = 0.0F, .z = pz },
                .normal = { .x = 0.0F, .y = 1.0F, .z = 0.0F },
                .uv0 = { .x = px, .y = pz },
                .uv1 = { .x = px, .y = pz },
                .color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F },
            });
        }
    }
    std::vector<std::uint32_t> indices;
    grid_indices(cells, 0, indices);
    bool failed = false;
    const gpu::slice_t vertex_slice = in_ring<fbe_vertex_t>(vertices, failed);
    const gpu::slice_t index_slice = in_ring<std::uint32_t>(indices, failed);
    if (failed) {
        return false;
    }
    data.vertices = vertex_slice.address;
    batch.indices = index_slice.buffer;
    batch.index_offset = index_slice.offset;
    batch.index_count = static_cast<std::uint32_t>(indices.size());
    return true;
}

/// One view of a camera into the frame's ring: its draws, materials, and lights, the textures its
/// materials sample added to `sampled`. False, with fbe_error's why, if there's no memory.
bool write_view(const scene::camera_t& camera, const scene::view_t& view,
                const scene::tweened_t& tweened, const lists_t& lists, bool flipped,
                std::uint64_t target, std::vector<std::uint64_t>& sampled, view_batches_t& out) {
    const scene::scene_t& s = scene::scene();
    out.flipped = flipped;
    std::vector<draw_data_t> draws;
    draws.reserve(lists.items.size());
    std::vector<material_data_t> materials;
    std::unordered_map<std::uint32_t, std::uint32_t> material_index;
    std::unordered_map<std::uint32_t, VkDeviceAddress> palettes;
    const scene::quat_t back = scene::inverse(view.rotation);
    const mat4_t view_matrix =
        scene::compose(back * -view.position, back, vec3_t(1.0F, 1.0F, 1.0F));
    out.batches.reserve(lists.items.size());
    for (const item_t& item : lists.items) {
        const auto [index, added] =
            material_index.try_emplace(item.material, static_cast<std::uint32_t>(materials.size()));
        if (added) {
            materials.push_back(material_data(item.material, target, sampled));
        }
        draw_data_t data = draw_data(item.draw, view_matrix);
        data.material = index->second;
        batch_t batch;
        batch.key = item.draw.pipeline;
        batch.first_draw = static_cast<std::uint32_t>(draws.size());
        if (item.draw.kind == FBE_ENTITY_MESH) {
            const surface_buffers_t* surface =
                surface_buffers(item.draw.geometry, static_cast<std::size_t>(item.draw.surface));
            if (surface == nullptr) {
                return false;
            }
            data.vertices = surface->vertices.address;
            batch.indices = surface->indices.buffer;
            batch.index_count = surface->index_count;
            if (surface->joints.buffer != VK_NULL_HANDLE && palette_source != nullptr) {
                // Each mesh's once, however many surfaces it has, its surfaces one after another
                // or apart.
                const auto [palette, fresh] =
                    palettes.try_emplace(item.entity, VkDeviceAddress{ 0 });
                if (fresh) {
                    const std::span<const mat4_t> poses = palette_source(item.entity, tweened);
                    if (!poses.empty()) {
                        bool no_room = false;
                        palette->second = in_ring<mat4_t>(poses, no_room).address;
                        if (no_room) {
                            return false;
                        }
                    }
                }
                if (palette->second != 0) {
                    data.joints = surface->joints.address;
                    data.palette = palette->second;
                    data.skinned = 1;
                }
            }
        } else if (item.draw.kind == FBE_ENTITY_SPRITE) {
            const surface_buffers_t* quad = sprite_quad();
            if (quad == nullptr) {
                return false;
            }
            data.vertices = quad->vertices.address;
            batch.indices = quad->indices.buffer;
            batch.index_count = quad->index_count;
        } else if (item.draw.kind == FBE_ENTITY_PLANE) {
            const scene::plane_t& plane = s.planes[s.entities.component[item.entity]];
            if (!plane_grid(camera, view, item, plane.segments, data, batch)) {
                return false;
            }
        } else if (item.draw.kind == FBE_ENTITY_TERRAIN) {
            const scene::terrain_t& terrain = s.terrains[s.entities.component[item.entity]];
            std::vector<terrain_piece_t> pieces;
            if (!terrain_pieces(camera, view, item, terrain, pieces)) {
                return false;
            }
            for (const terrain_piece_t& piece : pieces) {
                // A draw of its own for each, which only its vertices tell apart.
                data.vertices = piece.vertices;
                batch.first_draw = static_cast<std::uint32_t>(draws.size());
                batch.indices = piece.indices;
                batch.index_offset = piece.index_offset;
                batch.index_count = piece.index_count;
                draws.push_back(data);
                out.batches.push_back(batch);
            }
            continue;
        }
        if (batch.index_count == 0) {
            continue;
        }
        draws.push_back(data);
        // Together with the last, as an instance, when only the draw differs.
        if (!out.batches.empty()) {
            batch_t& last = out.batches.back();
            if (last.key == batch.key && last.indices == batch.indices &&
                last.index_offset == batch.index_offset && last.index_count == batch.index_count &&
                last.first_draw + last.draws == batch.first_draw) {
                ++last.draws;
                continue;
            }
        }
        out.batches.push_back(batch);
    }
    std::uint32_t directional = 0;
    for (const fbe_draw_light_t& light : lists.lights) {
        directional += light.type == FBE_LIGHT_DIRECTIONAL ? 1U : 0U;
    }
    camera_data_t data{};
    const mat4_t projection = scene::draw_projection(camera, view);
    data.projection = projection.m;
    if (flipped) {
        // Mirrored across: what the view has on its right draws on the left.
        data.projection[0] = -data.projection[0];
        data.projection[4] = -data.projection[4];
        data.projection[8] = -data.projection[8];
        data.projection[12] = -data.projection[12];
    }
    data.viewport = { view.x, view.y, view.width, view.height };
    data.fog_color = { camera.fog_color[0], camera.fog_color[1], camera.fog_color[2], 1.0F };
    data.fog_start = camera.fog_start;
    data.fog_end = camera.fog_end;
    data.fog_density = camera.fog_density;
    data.ambient = s.worlds[s.entities.world[camera.entity]].ambient;
    data.perspective = camera.projection == FBE_PROJECTION_PERSPECTIVE ? 1U : 0U;
    if (data.perspective != 0) {
        // log(depth / near) / log(far / near), a depth's part of the way through the slices.
        data.slice_scale = 1.0F / std::log(camera.farthest / camera.nearest);
        data.slice_bias = -std::log(camera.nearest) * data.slice_scale;
    } else {
        data.slice_scale = 1.0F / (camera.farthest - camera.nearest);
        data.slice_bias = -camera.nearest * data.slice_scale;
    }
    data.flipped = flipped ? 1U : 0U;
    data.directional = directional;
    bool failed = false;
    data.draws = in_ring<draw_data_t>(draws, failed).address;
    data.materials = in_ring<material_data_t>(materials, failed).address;
    data.lights = in_ring<fbe_draw_light_t>(lists.lights, failed).address;
    data.clusters = in_ring<fbe_cluster_t>(lists.clusters, failed).address;
    data.cluster_lights = in_ring<std::uint32_t>(lists.cluster_lights, failed).address;
    data.draw_lights = in_ring<std::uint32_t>(lists.draw_lights, failed).address;
    const std::array one{ data };
    out.camera = in_ring<camera_data_t>(one, failed).address;
    return !failed;
}

/// A view as a mirror's plane reflects it: the eye, and its turn as a whole camera has it, its
/// right turned to its left, which a projection flipped across gives back.
scene::view_t reflected_view(const scene::view_t& view, const mat4_t& mirror) {
    mat4_t into;
    scene::view_t reflected = view;
    if (!scene::inverse_affine(mirror, into)) {
        return reflected;
    }
    const auto reflect_point = [&](vec3_t p) {
        vec3_t local = scene::transform_point(into, p);
        local.y = -local.y;
        return scene::transform_point(mirror, local);
    };
    const auto reflect_way = [&](vec3_t way) {
        vec3_t local = scene::transform_vector(into, way);
        local.y = -local.y;
        return scene::normalize(scene::transform_vector(mirror, local));
    };
    reflected.position = reflect_point(view.position);
    const vec3_t right = reflect_way(view.rotation * vec3_t(1.0F, 0.0F, 0.0F));
    const vec3_t up = reflect_way(view.rotation * vec3_t(0.0F, 1.0F, 0.0F));
    const vec3_t back = reflect_way(view.rotation * vec3_t(0.0F, 0.0F, 1.0F));
    reflected.rotation = scene::from_axes(-right, up, back);
    return reflected;
}

} // namespace

void set_palette_source(palette_source_t source) {
    palette_source = source;
}

bool draw_camera(std::uint32_t slot, float tween) {
    const scene::scene_t& s = scene::scene();
    const scene::camera_t& camera = s.cameras[s.entities.component[slot]];
    const std::uint64_t target = camera.target != 0 ? camera.target : fbe_gpu_screen();
    if (target == 0) {
        return fail("a camera with no target draws into the screen, and there's no window open");
    }
    const texture::texture_t* found = texture::find(target);
    if (found == nullptr) {
        return false;
    }
    const auto width = static_cast<float>(found->width);
    const auto height = static_cast<float>(found->height);
    const VkFormat format = found->vk_format;
    const VkSampleCountFlagBits samples = found->samples;
    const std::uint32_t target_width = found->width;
    const std::uint32_t target_height = found->height;
    scene::view_t view;
    bool shows = false;
    std::uint32_t own = none;
    if (!drawn_view(s.entities.slots.handle(slot), width, height, tween, own, view, shows)) {
        return false;
    }
    if (!shows) {
        return true;
    }
    const scene::tweened_t tweened(tween);
    auto pass = std::make_shared<recorded_pass_t>();
    std::vector<std::uint64_t> sampled;
    // Blitz's order: every mirror's reflection of the scene, into the same viewport and depth, then
    // the scene, which covers it unless what's on the mirror's plane lets it through.
    const std::uint32_t world = s.entities.world[slot];
    for (std::uint32_t entity = 0; entity < s.entities.slots.size(); ++entity) {
        if (s.entities.slots.find(s.entities.slots.handle(entity)) &&
            s.entities.kind[entity] == FBE_ENTITY_MIRROR && s.entities.world[entity] == world &&
            scene::showing(entity)) {
            const scene::view_t reflected = reflected_view(view, tweened.pose(entity).matrix);
            lists_t lists;
            read_view(camera, reflected, tweened, lists);
            view_batches_t batches;
            if (!write_view(camera, reflected, tweened, lists, true, target, sampled, batches)) {
                return false;
            }
            pass->views.push_back(std::move(batches));
        }
    }
    lists_t lists;
    read_view(camera, view, tweened, lists);
    view_batches_t batches;
    if (!write_view(camera, view, tweened, lists, false, target, sampled, batches)) {
        return false;
    }
    pass->views.push_back(std::move(batches));
    const auto left = static_cast<std::int32_t>(std::floor(view.x));
    const auto top = static_cast<std::int32_t>(std::floor(view.y));
    const auto right = std::min(static_cast<std::int32_t>(std::ceil(view.x + view.width)),
                                static_cast<std::int32_t>(target_width));
    const auto bottom = std::min(static_cast<std::int32_t>(std::ceil(view.y + view.height)),
                                 static_cast<std::int32_t>(target_height));
    const std::int32_t from_x = std::max(left, 0);
    const std::int32_t from_y = std::max(top, 0);
    pass->viewport = {
        .x = view.x,
        .y = view.y,
        .width = view.width,
        .height = view.height,
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
    const auto scissor_width = static_cast<std::uint32_t>(std::max(right - from_x, 0));
    const auto scissor_height = static_cast<std::uint32_t>(std::max(bottom - from_y, 0));
    pass->scissor = {
        .offset = { .x = from_x, .y = from_y },
        .extent = { .width = scissor_width, .height = scissor_height },
    };
    if (pass->scissor.extent.width == 0 || pass->scissor.extent.height == 0) {
        return true;
    }
    pass->format = format;
    pass->samples = samples;
    const bool whole = from_x == 0 && from_y == 0 && pass->scissor.extent.width == target_width &&
                       pass->scissor.extent.height == target_height;
    const std::array<float, 4> clear_rgba = camera.clear_color;
    pass->clear_color = camera.clears_color && !whole;
    pass->clear_depth = camera.clears_depth && !whole;
    pass->clear_rgba = clear_rgba;
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
    const gpu::resource_t into = gpu::texture_resource(target);
    if (into == gpu::no_resource) {
        return false;
    }
    return gpu::add_pass({
        .name = "camera",
        .colors = { {
            .target = into,
            .clear = camera.clears_color && whole,
            .rgba = clear_rgba,
        }, },
        .depth = into,
        .clear_depth = camera.clears_depth && whole,
        .uses = std::move(uses),
        .record = [pass](VkCommandBuffer commands) { record(commands, *pass); },
    });
}

} // namespace fbe::render

extern "C" {

FBE_API bool fbe_render_camera(uint64_t camera, float tween) {
    std::uint32_t slot = fbe::scene::none;
    fbe::scene::view_t view;
    bool shows = false;
    // The size doesn't matter yet: only whether it's a camera and the tween's good.
    if (!fbe::render::drawn_view(camera, 1.0F, 1.0F, tween, slot, view, shows)) {
        return false;
    }
    fbe::render::forget_freed_geometries();
    return (fbe_gpu_started() || fbe_gpu_start()) && fbe::render::draw_camera(slot, tween);
}

FBE_API bool fbe_render_world(uint64_t world, float tween) {
    const std::int32_t count = fbe_world_cameras(world, nullptr, 0);
    if (count < 0) {
        return false;
    }
    std::vector<std::uint64_t> cameras(static_cast<std::size_t>(count));
    fbe_world_cameras(world, cameras.data(), count);
    return std::ranges::all_of(
        cameras, [tween](std::uint64_t camera) { return fbe_render_camera(camera, tween); });
}

} // extern "C"
