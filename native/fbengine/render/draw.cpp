// Drawing a camera: what read_lists read of it goes into the frame's ring as the scene shader reads
// it, its geometry onto the GPU, and all of it into a pass of the frame graph, whose draws of one
// geometry and pipeline go together as instances, each instance a draw of the list.

#include "core/engine.hpp"
#include "environment/environment.hpp"
#include "fbengine.h"
#include "gpu/buffer.hpp"
#include "gpu/graph.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/vulkan.hpp"
#include "morph/morph.hpp"
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
#include <cstring>
#include <engine_shaders.hpp>
#include <format>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbe::render {

namespace {

using gpu::flag;
using scene::mat4_t;
using scene::none;
using scene::vec3_t;

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
                             bool flipped, bool shadows, bool prepass, bool stored) {
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
    const bool particles = ((key >> 50U) & 1U) != 0;
    const gpu::shader_t* own = shaders_own::shader_of(static_cast<std::uint32_t>(key >> 52U));
    const bool instances = ((key >> 51U) & 1U) != 0;
    const std::string_view cast_vertex = instances ? "shadowInstanceVertex" : "shadowVertex";
    const std::string_view plain_vertex = instances ? "instanceVertex" : "vertexMain";
    const std::string_view lit_vertex = particles ? "particleVertex" : plain_vertex;
    const std::string_view prepass_vertex = instances ? "prepassInstanceVertex" : "prepassVertex";
    const std::string_view scene_vertex = prepass ? prepass_vertex : lit_vertex;
    const std::string_view scene_fragment = prepass ? "prepassFragment" : "fragmentMain";
    gpu::pipeline_desc_t desc{
        .shader = own != nullptr ? own : &shaders::scene,
        .vertex = shadows ? cast_vertex : scene_vertex,
        .fragment = shadows ? "shadowFragment" : scene_fragment,
        .color = format,
        .depth = texture::depth_format,
        .samples = samples,
        .polygon = ((key >> 49U) & 1U) != 0 && gpu::device()->wireframe ? VK_POLYGON_MODE_LINE
                                                                        : VK_POLYGON_MODE_FILL,
        // A room's shell, its back to the light, casts as well as its front would.
        .cull = two_sided || shadows ? flag<VkCullModeFlags>(VK_CULL_MODE_NONE)
                                     : flag<VkCullModeFlags>(VK_CULL_MODE_BACK_BIT),
        .front = mirrored != flipped ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .blend = prepass ? gpu::blend_t::NONE : blend_of(key),
        .depth_test = depth_mode != FBE_DEPTH_NONE,
        .depth_write = depth_mode == FBE_DEPTH_TEST_AND_WRITE,
        .depth_clamp = shadows && gpu::device()->depth_clamp,
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
            // What's in the prepass, as its ambient occlusion is of.
            depth_mode == FBE_DEPTH_TEST_AND_WRITE && !particles ? 1U : 0U,
            stored ? 1U : 0U,
        },
        .constant_count = 11,
    };
    return desc;
}

/// The sky's pipeline, into a target: only what's still at the far plane takes it, and it writes no
/// depth.
gpu::pipeline_desc_t sky_desc(VkFormat format, VkSampleCountFlagBits samples, bool stored) {
    return {
        .shader = &shaders::sky,
        .color = format,
        .depth = texture::depth_format,
        .samples = samples,
        .depth_test = true,
        .constants = { stored ? 1U : 0U },
        .constant_count = 1,
    };
}

} // namespace

void record_pass(VkCommandBuffer commands, const recorded_pass_t& pass) {
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
    if (!pass.shadows) {
        vkCmdSetViewport(commands, 0, 1, &pass.viewport);
        vkCmdSetScissor(commands, 0, 1, &pass.scissor);
    }
    if (pass.occlusion != gpu::no_resource) {
        const std::uint32_t occlusion = gpu::resource_index(pass.occlusion);
        vkCmdPushConstants(commands, layout, flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL),
                           sizeof(VkDeviceAddress) + sizeof(std::uint32_t), sizeof(std::uint32_t),
                           &occlusion);
    }
    for (const view_batches_t& view : pass.views) {
        if (view.has_viewport) {
            vkCmdSetViewport(commands, 0, 1, &view.viewport);
            vkCmdSetScissor(commands, 0, 1, &view.scissor);
        }
        vkCmdPushConstants(commands, layout, flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0,
                           sizeof(VkDeviceAddress), &view.camera);
        std::optional<std::uint64_t> bound;
        VkBuffer bound_indices = VK_NULL_HANDLE;
        VkDeviceSize bound_offset = 0;
        bool usable = false;
        for (const batch_t& batch : view.batches) {
            if (bound != batch.key) {
                bound = batch.key;
                VkPipeline made = gpu::pipeline(
                    batch.sky != 0 ? sky_desc(pass.format, pass.samples, pass.stored)
                                   : desc_of(batch.key, pass.format, pass.samples, view.flipped,
                                             pass.shadows, pass.prepass, pass.stored));
                const auto code = static_cast<std::uint32_t>(batch.key >> 52U);
                // A game's shader no pipeline can be made of isn't tried again: its draws go on as
                // the scene's own, under the same key.
                if (made == VK_NULL_HANDLE && batch.sky == 0 &&
                    shaders_own::shader_of(code) != nullptr) {
                    shaders_own::refuse(code);
                    made = gpu::pipeline(desc_of(batch.key, pass.format, pass.samples, view.flipped,
                                                 pass.shadows, pass.prepass, pass.stored));
                }
                usable = made != VK_NULL_HANDLE;
                if (usable) {
                    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, made);
                }
            }
            if (!usable) {
                continue;
            }
            if (batch.sky != 0) {
                // It takes its own address in place of the camera's, which the next draw has back.
                vkCmdPushConstants(commands, layout, flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL),
                                   0, sizeof(VkDeviceAddress), &batch.sky);
                vkCmdDraw(commands, 3, 1, 0, 0);
                vkCmdPushConstants(commands, layout, flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL),
                                   0, sizeof(VkDeviceAddress), &view.camera);
                gpu::count_draws(1, 1);
                continue;
            }
            if (batch.vertex_count != 0) {
                vkCmdDraw(commands, batch.vertex_count, 1, 0, batch.first_draw);
                gpu::count_draws(1, batch.vertex_count / 3);
                continue;
            }
            if (batch.indices != bound_indices || batch.index_offset != bound_offset) {
                bound_indices = batch.indices;
                bound_offset = batch.index_offset;
                vkCmdBindIndexBuffer(commands, batch.indices, batch.index_offset,
                                     VK_INDEX_TYPE_UINT32);
            }
            if (batch.instance_count != 0) {
                // Its instance numbers its instances, so its draw's told apart.
                vkCmdPushConstants(commands, layout, flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL),
                                   sizeof(VkDeviceAddress), sizeof(std::uint32_t),
                                   &batch.first_draw);
                vkCmdDrawIndexed(commands, batch.index_count, batch.instance_count, 0, 0,
                                 batch.instance_first);
                gpu::count_draws(1, std::uint64_t{ batch.index_count } / 3 * batch.instance_count);
                continue;
            }
            vkCmdDrawIndexed(commands, batch.index_count, batch.draws, 0, 0, batch.first_draw);
            gpu::count_draws(1, std::uint64_t{ batch.index_count } / 3 * batch.draws);
        }
    }
}

namespace {

/// A layer or a map as the scene shader reads it, what it samples added to `sampled`.
layer_data_t layer_data(const scene::layer_t& layer, std::uint64_t target,
                        std::vector<std::uint64_t>& sampled) {
    layer_data_t out{};
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
    // Scaled down by its scale, so 2 looks twice as big, turned back by its angle,
    // and moved back by its offset.
    const float across = 1.0F / layer.uv_scale[0];
    const float down = 1.0F / layer.uv_scale[1];
    const float cosine = std::cos(-layer.uv_angle);
    const float sine = std::sin(-layer.uv_angle);
    const bool decodes = image != nullptr && fbe_texture_srgb(layer.texture);
    const float neutral = image != nullptr ? 0.0F : 1.0F;
    out.uv_across = { across * cosine, down * sine, -layer.uv_offset[0], decodes ? 1.0F : 0.0F };
    out.uv_down = { -across * sine, down * cosine, -layer.uv_offset[1], neutral };
    return out;
}

} // namespace

/// What a material is as the scene shader reads it, its layers those with textures, in order, as
/// its draws' pipeline key has them, and the textures it samples added to `sampled`. A layer can't
/// sample what the pass draws into, so leaves what's under it as it is, as a layer does whose
/// texture has gone with the GPU; and a map that can't is no map.
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
    data.parameters = found.parameters;
    std::size_t at = 0;
    for (const scene::layer_t& layer : found.layers) {
        if (layer.texture != 0) {
            data.layers[at++] = layer_data(layer, target, sampled);
        }
    }
    data.metallic = found.metallic;
    data.roughness = found.roughness;
    data.normal_scale = found.normal_scale;
    data.occlusion_strength = found.occlusion_strength;
    data.emissive = found.emissive;
    for (std::size_t map = 0; map < found.maps.size(); ++map) {
        const scene::layer_t& layer = found.maps.at(map);
        if (layer.texture != 0 && layer.texture != target &&
            texture::find(layer.texture) != nullptr) {
            data.map_layers.at(map) = layer_data(layer, target, sampled);
            // Its UV set's in what the pipeline key is for layers; here, the unused place.
            data.map_layers.at(map).bump[0] = static_cast<float>(layer.uv_set);
            data.maps |= 1U << map;
        }
    }
    return data;
}

namespace {

/// What a draw is as the scene shader reads it, but for its vertices and material.
draw_data_t draw_data(const fbe_draw_t& draw, const mat4_t& view) {
    mat4_t world;
    std::ranges::copy(draw.matrix, world.m.begin());
    const mat4_t to_view = view * world;
    draw_data_t data{};
    data.to_view = to_view.m;
    std::ranges::copy(draw.color, data.color.begin());
    data.light_first = draw.light_first;
    data.light_count = draw.light_count;
    // A sprite has no normals to turn.
    if (draw.kind == FBE_ENTITY_SPRITE) {
        return data;
    }
    const vec3_t a = to_view.axis(0);
    const vec3_t b = to_view.axis(1);
    const vec3_t c = to_view.axis(2);
    // The cofactors, the inverse transpose times the determinant, of which only the direction's
    // kept, and the determinant's sign, so a mirror turns a normal with its surface.
    const float sign = scene::mirrors(to_view) ? -1.0F : 1.0F;
    const vec3_t x = scene::cross(b, c) * sign;
    const vec3_t y = scene::cross(c, a) * sign;
    const vec3_t z = scene::cross(a, b) * sign;
    data.normals[0] = { x.x, y.x, z.x, 0.0F };
    data.normals[1] = { x.y, y.y, z.y, 0.0F };
    data.normals[2] = { x.z, y.z, z.z, 0.0F };
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

/// A cluster's box's corners, through its entity's matrix.
std::array<vec3_t, 8> cluster_corners(const mat4_t& world,
                                      const scene::instance_cluster_t& cluster) {
    std::array<vec3_t, 8> corners;
    for (unsigned corner = 0; corner < 8; ++corner) {
        corners[corner] = scene::transform_point(
            world, vec3_t((corner & 1U) != 0 ? cluster.high.x : cluster.low.x,
                          (corner & 2U) != 0 ? cluster.high.y : cluster.low.y,
                          (corner & 4U) != 0 ? cluster.high.z : cluster.low.z));
    }
    return corners;
}

/// A world's sky as its shader reads it, from a view, the `own`th of as many as the mirrors' planes
/// and the scene's last; its texture's added to `sampled`. Each view keeps to what's through its
/// mirror's plane before any other's, and the scene's to what's through none.
sky_data_t sky_data(const scene::camera_t& camera, const scene::view_t& view,
                    const scene::tweened_t& tweened, std::size_t own,
                    std::span<const std::array<float, 4>> planes, std::uint64_t target,
                    std::vector<std::uint64_t>& sampled, bool& failed) {
    const scene::scene_t& s = scene::scene();
    const std::uint32_t world_slot = s.entities.world[camera.entity];
    scene::world_t& world = scene::scene().worlds[world_slot];
    const bool flipped = own != planes.size();
    float half_width = 0.0F;
    float half_height = 0.0F;
    scene::half_extents(camera, view, half_width, half_height);
    const bool spreads = camera.projection == FBE_PROJECTION_PERSPECTIVE;
    const vec3_t right = view.rotation * vec3_t(1.0F, 0.0F, 0.0F);
    const vec3_t up = view.rotation * vec3_t(0.0F, 1.0F, 0.0F);
    const vec3_t forward = view.rotation * vec3_t(0.0F, 0.0F, -1.0F);
    const auto light = world.sky_light != 0 ? s.entities.slots.find(world.sky_light) : std::nullopt;
    if (light) {
        // Toward it: against the way it shines, as shading has it, and kept for when it's gone.
        world.sky_sun = tweened.pose(*light).rotation * vec3_t(0.0F, 0.0F, 1.0F);
    }
    const vec3_t sun = world.sky_sun;
    sky_data_t data{};
    data.right = { right.x, right.y, right.z, 0.0F };
    data.up = { up.x, up.y, up.z, 0.0F };
    data.forward = { forward.x, forward.y, forward.z, 0.0F };
    data.spread = { spreads ? half_width : 0.0F, spreads ? half_height : 0.0F };
    data.mode = static_cast<std::uint32_t>(world.sky);
    data.sun = { sun.x, sun.y, sun.z };
    data.brightness = world.sky_brightness;
    data.level = -gpu::lod_bias_in_samplers();
    data.flip = flipped ? -1.0F : 1.0F;
    data.mirrors = in_ring<std::array<float, 4>>(planes, failed).address;
    data.mirror_count = static_cast<std::uint32_t>(planes.size());
    data.own_mirror = static_cast<std::uint32_t>(own);
    const texture::texture_t* image = world.sky_texture != 0 && world.sky_texture != target
                                          ? texture::find(world.sky_texture)
                                          : nullptr;
    if (image != nullptr) {
        data.texture = texture::index_of(world.sky_texture);
        // Around the world, but never over its top, whatever it's set to for anything else.
        data.sampler = gpu::sampler_index(image->filter, FBE_TEXTURE_REPEAT, FBE_TEXTURE_CLAMP);
        sampled.push_back(world.sky_texture);
    } else {
        data.texture = texture::white;
        data.sampler =
            gpu::sampler_index(FBE_TEXTURE_NEAREST, FBE_TEXTURE_REPEAT, FBE_TEXTURE_REPEAT);
    }
    return data;
}

/// One view of a camera into the frame's ring: its draws, materials, and lights, the textures its
/// materials sample added to `sampled`. False, with fbe_error's why, if there's no memory.
/// What a view of a camera goes by besides what it sees: how far its projection's off, what its
/// entities were last frame, if it's the main one of a camera that goes by that, and what its
/// ambient occlusion's raised to, 0 for none.
struct view_extras_t {
    std::array<float, 2> jitter{};
    motion_t* motion = nullptr;
    float ambient_occlusion = 0.0F;
};

bool write_view(const scene::camera_t& camera, const scene::view_t& view,
                const scene::tweened_t& tweened, const lists_t& lists, bool flipped,
                std::uint64_t target, const shadow_frame_t& shadows, std::size_t shadow_view,
                std::span<const std::array<float, 4>> mirror_planes,
                std::vector<std::uint64_t>& sampled, const view_extras_t& extras,
                view_batches_t& out) {
    const scene::scene_t& s = scene::scene();
    out.flipped = flipped;
    std::vector<draw_data_t> draws;
    draws.reserve(lists.items.size());
    std::vector<material_data_t> materials;
    std::unordered_map<std::uint32_t, std::uint32_t> material_index;
    std::unordered_map<std::uint32_t, std::pair<VkDeviceAddress, std::uint32_t>> palettes;
    const scene::quat_t back = scene::inverse(view.rotation);
    const mat4_t view_matrix =
        scene::compose(back * -view.position, back, vec3_t(1.0F, 1.0F, 1.0F));
    const scene::frustum_t frustum = scene::frustum_of(camera, view);
    out.batches.reserve(lists.items.size());
    // The sky goes after what's opaque in the scene, and before all that blends and what's over it.
    std::uint32_t last_material = none;
    std::uint32_t last_index = 0;
    bool sky_owed = s.worlds[s.entities.world[camera.entity]].sky != 0;
    const auto draw_sky = [&] {
        sky_owed = false;
        bool no_room = false;
        const sky_data_t sky =
            sky_data(camera, view, tweened, shadow_view, mirror_planes, target, sampled, no_room);
        const std::array one_sky{ sky };
        batch_t batch;
        batch.key = UINT64_MAX;
        batch.sky = in_ring<sky_data_t>(one_sky, no_room).address;
        out.batches.push_back(batch);
        return !no_room;
    };
    for (const item_t& item : lists.items) {
        if (sky_owed && (item.order > 0 || (item.order == 0 && item.blended)) && !draw_sky()) {
            return false;
        }
        // The last one's as likely as not, as a run of sprites is.
        if (item.material != last_material || materials.empty()) {
            const auto [index, added] = material_index.try_emplace(
                item.material, static_cast<std::uint32_t>(materials.size()));
            if (added) {
                materials.push_back(material_data(item.material, target, sampled));
            }
            last_material = item.material;
            last_index = index->second;
        }
        draw_data_t data = draw_data(item.draw, view_matrix);
        if (extras.motion != nullptr) {
            bool no_room = false;
            data.previous = previous_of(*extras.motion, item, tweened, no_room);
            if (no_room) {
                return false;
            }
        }
        data.material = last_index;
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
            if (surface->tangents.buffer != VK_NULL_HANDLE) {
                data.tangents = surface->tangents.address;
                data.has_tangents = 1U;
            }
            // All of it nearer the eye by a part of how far it is: on the same pixels, just nearer.
            const auto drawn_slot = s.geometry_slots.find(item.draw.geometry);
            if (drawn_slot && s.geometries[*drawn_slot].eye_pull > 0.0F) {
                const float kept = 1.0F - s.geometries[*drawn_slot].eye_pull;
                for (std::size_t column = 0; column < 4; ++column) {
                    for (std::size_t row = 0; row < 3; ++row) {
                        data.to_view[(column * 4) + row] *= kept;
                    }
                }
            }
            if (surface->joints.buffer != VK_NULL_HANDLE) {
                // Each mesh's once, however many surfaces it has, its surfaces one after another
                // or apart.
                const auto [palette, fresh] =
                    palettes.try_emplace(item.entity, std::pair{ VkDeviceAddress{ 0 }, 0U });
                if (fresh) {
                    const std::span<const mat4_t> poses = anim::skin_palette(item.entity, tweened);
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
                    data.joints = surface->joints.address;
                    data.palette = palette->second.first;
                    data.joint_count = palette->second.second;
                }
            }
            if (!surface->target_moves.empty()) {
                std::array<anim::drawn_target_t, anim::drawn_targets> strongest;
                const std::size_t count =
                    anim::strongest_targets(anim::morph_weights(item.entity, tweened.tween()),
                                            surface->target_moves, strongest);
                if (count != 0) {
                    bool no_room = false;
                    data.drawn_targets =
                        in_ring<anim::drawn_target_t>(std::span(strongest).first(count), no_room)
                            .address;
                    if (no_room) {
                        return false;
                    }
                    data.targets = surface->targets.address;
                    data.drawn_target_count = static_cast<std::uint32_t>(count);
                    data.target_vertices = surface->vertex_count;
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
        } else if (item.draw.kind == FBE_ENTITY_INSTANCES) {
            scene::instances_t& found = scene::scene().instances[s.entities.component[item.entity]];
            const surface_buffers_t* surface =
                surface_buffers(item.draw.geometry, static_cast<std::size_t>(item.draw.surface));
            const VkDeviceAddress records = instances_buffer(found);
            if (surface == nullptr || records == 0) {
                return false;
            }
            if (surface->index_count == 0) {
                continue;
            }
            data.vertices = surface->vertices.address;
            if (surface->tangents.buffer != VK_NULL_HANDLE) {
                data.tangents = surface->tangents.address;
                data.has_tangents = 1U;
            }
            data.instances = records;
            if (surface->tangents.buffer != VK_NULL_HANDLE) {
                data.tangents = surface->tangents.address;
                data.has_tangents = 1U;
            }
            batch.indices = surface->indices.buffer;
            batch.index_count = surface->index_count;
            draws.push_back(data);
            mat4_t world;
            std::ranges::copy(item.draw.matrix, world.m.begin());
            // What's in view of its clusters, those one after another going together.
            batch_t* running = nullptr;
            const auto level = s.geometry_slots.find(item.draw.geometry);
            float half_width = 0.0F;
            float half_height = 0.0F;
            scene::half_extents(camera, view, half_width, half_height);
            for (const scene::instance_cluster_t& cluster : scene::instance_clusters(found)) {
                const std::array<vec3_t, 8> corners = cluster_corners(world, cluster);
                if (cluster.mirrored != item.instances_mirrored ||
                    !scene::in_frustum(frustum, corners.data(), corners.size()) ||
                    !level.has_value() ||
                    scene::cluster_lod(found, cluster, world, camera, frustum.eye, half_height) !=
                        *level) {
                    running = nullptr;
                    continue;
                }
                if (running != nullptr) {
                    running->instance_count += cluster.count;
                    continue;
                }
                batch.instance_first = cluster.first;
                batch.instance_count = cluster.count;
                out.batches.push_back(batch);
                running = &out.batches.back();
            }
            continue;
        } else if (item.draw.kind == FBE_ENTITY_PARTICLES) {
            scene::particles_t& particles =
                scene::scene().particles[s.entities.component[item.entity]];
            const VkDeviceAddress records = particles_buffer(particles);
            if (records == 0) {
                return false;
            }
            const std::array one_emitter{
                particles_data_t{
                    .records = records,
                    .gravity = { particles.gravity.x, particles.gravity.y, particles.gravity.z },
                    .drag = particles.drag,
                    .color_start = particles.color_start,
                    .color_end = particles.color_end,
                    .size_start = particles.size_start,
                    .size_end = particles.size_end,
                    .now = scene::particles_clock(particles, tweened.tween()),
                    .wrap = scene::particles_wrap,
                },
            };
            bool no_room = false;
            data.particles = in_ring<particles_data_t>(one_emitter, no_room).address;
            if (no_room) {
                return false;
            }
            // Only those slots that have had one: a million held and one born draws one.
            batch.vertex_count = static_cast<std::uint32_t>(particles.born) * 6U;
            if (batch.vertex_count == 0) {
                continue;
            }
            draws.push_back(data);
            out.batches.push_back(batch);
            continue;
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
    if (sky_owed && !draw_sky()) {
        return false;
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
    // A part of a pixel off, whatever its depth: as much of w as it's to go.
    for (std::size_t column = 0; column < 4; ++column) {
        data.projection[column * 4] += extras.jitter[0] * data.projection[(column * 4) + 3];
        data.projection[(column * 4) + 1] += extras.jitter[1] * data.projection[(column * 4) + 3];
    }
    data.jitter = extras.jitter;
    data.ambient_occlusion = extras.ambient_occlusion;
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
    const scene::world_t& world = s.worlds[s.entities.world[camera.entity]];
    if (world.environment != 0 && environment::shader_data(world.environment, data.environment)) {
        data.environment.intensity = world.environment_intensity;
        data.has_environment = 1U;
        for (const std::uint64_t texture : {
                 fbe_environment_irradiance(world.environment),
                 fbe_environment_specular(world.environment),
                 fbe_environment_table(world.environment),
             }) {
            sampled.push_back(texture);
        }
    }
    mat4_t to_world;
    scene::inverse_affine(view_matrix, to_world);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const vec3_t row(to_world.at(0, static_cast<int>(axis)),
                         to_world.at(1, static_cast<int>(axis)),
                         to_world.at(2, static_cast<int>(axis)));
        data.world_rows[axis] = { row.x, row.y, row.z, 0.0F };
    }
    bool failed = false;
    data.draws = in_ring<draw_data_t>(draws, failed).address;
    data.materials = in_ring<material_data_t>(materials, failed).address;
    std::vector<fbe_draw_light_t> lights = lists.lights;
    for (fbe_draw_light_t& light : lights) {
        const std::unordered_map<std::uint64_t, std::uint32_t>& firsts =
            shadows.firsts[shadow_view];
        const auto first = firsts.find(light.light);
        light.shadow = first != firsts.end() ? static_cast<std::int32_t>(first->second) : -1;
    }
    data.lod_bias = gpu::lod_bias_left();
    data.lights = in_ring<fbe_draw_light_t>(lights, failed).address;
    data.clusters = in_ring<fbe_cluster_t>(lists.clusters, failed).address;
    data.cluster_lights = in_ring<std::uint32_t>(lists.cluster_lights, failed).address;
    data.draw_lights = in_ring<std::uint32_t>(lists.draw_lights, failed).address;
    // Its shadows' matrices from this view, as its lights are in it.
    if (shadows.atlas != 0) {
        std::vector<shadow_data_t> records = shadows.records;
        for (std::size_t i = 0; i < records.size(); ++i) {
            const mat4_t from_view = shadows.world_to_clip[i] * to_world;
            records[i].from_view = from_view.m;
        }
        data.shadows = in_ring<shadow_data_t>(records, failed).address;
        data.shadow_atlas = texture::index_of(shadows.atlas);
        sampled.push_back(shadows.atlas);
    }
    if (extras.motion != nullptr) {
        data.previous = in_ring<previous_data_t>(extras.motion->records, failed).address;
        data.moving = 1U;
        finish_motion(*extras.motion, projection * view_matrix);
    } else {
        const std::array<previous_data_t, 0> none_then{};
        data.previous = in_ring<previous_data_t>(none_then, failed).address;
    }
    const std::array one{ data };
    out.camera = in_ring<camera_data_t>(one, failed).address;
    return !failed;
}

/// What a camera clears to, in what it draws: its color's light, or, in stored values, the bytes
/// that would be, so a color's the same on any camera.
std::array<float, 4> cleared_with(const scene::camera_t& camera) {
    std::array<float, 4> color = camera.clear_color;
    if (camera.stored_values) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float light = std::clamp(color[channel], 0.0F, 1.0F);
            color[channel] = light <= 0.0031308F ? light * 12.92F
                                                 : (1.055F * std::pow(light, 1.0F / 2.4F)) - 0.055F;
        }
    }
    return color;
}

/// A view as a mirror's plane reflects it: the eye, and its turn as a whole camera has it, its
/// right turned to its left, which a projection flipped across gives back. False for a mirror
/// scaled to nothing, which has no plane to reflect through.
bool reflected_view(const scene::view_t& view, const mat4_t& mirror, scene::view_t& reflected) {
    mat4_t into;
    if (!scene::inverse_affine(mirror, into)) {
        return false;
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
    reflected = view;
    reflected.position = reflect_point(view.position);
    const vec3_t right = reflect_way(view.rotation * vec3_t(1.0F, 0.0F, 0.0F));
    const vec3_t up = reflect_way(view.rotation * vec3_t(0.0F, 1.0F, 0.0F));
    const vec3_t back = reflect_way(view.rotation * vec3_t(0.0F, 0.0F, 1.0F));
    reflected.rotation = scene::from_axes(-right, up, back);
    return true;
}

} // namespace

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
    const VkFormat target_format = found->vk_format;
    // Stored values go into a target's bytes as they are.
    const VkFormat drawn_format = camera.stored_values && found->stored_view != VK_NULL_HANDLE
                                      ? VK_FORMAT_R8G8B8A8_UNORM
                                      : target_format;
    // FXAA reads what's done, so it goes through an image of its own, as near the target as there
    // is.
    const bool srgb_target =
        drawn_format == VK_FORMAT_R8G8B8A8_SRGB || drawn_format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool float_target = target_format == VK_FORMAT_R16G16B16A16_SFLOAT;
    std::int32_t picture_format = srgb_target ? FBE_TEXTURE_RGBA8 : FBE_TEXTURE_RGBA8_LINEAR;
    VkFormat picture_vk_format = srgb_target ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (float_target) {
        picture_format = FBE_TEXTURE_RGBA16F;
        picture_vk_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    const bool aside = camera.hdr || camera.fxaa;
    VkFormat format = drawn_format;
    if (aside) {
        format = camera.hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : picture_vk_format;
    }
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
    // The pixels it covers, in float first, since a viewport can be further out than an int counts,
    // and the viewport itself no further than the device takes: past that, it's cut there.
    const auto most_x = static_cast<float>(target_width);
    const auto most_y = static_cast<float>(target_height);
    const float left = std::clamp(std::floor(view.x), 0.0F, most_x);
    const float top = std::clamp(std::floor(view.y), 0.0F, most_y);
    const float right = std::clamp(std::ceil(view.x + view.width), left, most_x);
    const float bottom = std::clamp(std::ceil(view.y + view.height), top, most_y);
    const VkPhysicalDeviceLimits& limits = gpu::device()->properties.limits;
    const float least_place = limits.viewportBoundsRange[0];
    const float most_place = limits.viewportBoundsRange[1];
    const float viewport_x = std::clamp(view.x, least_place, most_place);
    const float viewport_y = std::clamp(view.y, least_place, most_place);
    pass->viewport = {
        .x = viewport_x,
        .y = viewport_y,
        .width = std::min({
            view.width,
            static_cast<float>(limits.maxViewportDimensions[0]),
            most_place - viewport_x,
        }),
        .height = std::min({
            view.height,
            static_cast<float>(limits.maxViewportDimensions[1]),
            most_place - viewport_y,
        }),
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
    const auto from_x = static_cast<std::int32_t>(left);
    const auto from_y = static_cast<std::int32_t>(top);
    const auto scissor_width = static_cast<std::uint32_t>(right - left);
    const auto scissor_height = static_cast<std::uint32_t>(bottom - top);
    pass->scissor = {
        .offset = { .x = from_x, .y = from_y },
        .extent = { .width = scissor_width, .height = scissor_height },
    };
    if (pass->scissor.extent.width == 0 || pass->scissor.extent.height == 0) {
        return true;
    }
    // First every mirror's reflection of the scene, into the same viewport and depth, then
    // the scene, which covers it unless what's on the mirror's plane lets it through.
    const std::uint32_t world = s.entities.world[slot];
    std::vector<scene::view_t> views;
    std::vector<std::array<float, 4>> mirror_planes;
    for (std::uint32_t entity = 0; entity < s.entities.slots.size(); ++entity) {
        if (s.entities.slots.find(s.entities.slots.handle(entity)) &&
            s.entities.kind[entity] == FBE_ENTITY_MIRROR && s.entities.world[entity] == world &&
            scene::showing(entity)) {
            scene::view_t reflected;
            const mat4_t mirror = tweened.pose(entity).matrix;
            if (reflected_view(view, mirror, reflected)) {
                views.push_back(reflected);
                const vec3_t normal =
                    scene::normalize(scene::transform_normal(mirror, vec3_t(0.0F, 1.0F, 0.0F)));
                const float above = scene::dot(view.position - mirror.axis(3), normal);
                mirror_planes.push_back({ normal.x, normal.y, normal.z, above });
            }
        }
    }
    views.push_back(view);
    // Its shadows first, so what lights it has them.
    shadow_frame_t shadows;
    if (!draw_shadows(camera, views, tweened, shadows)) {
        return false;
    }
    const bool temporal = camera.hdr && camera.temporal_aa;
    const bool occludes = camera.ambient_occlusion > 0.0F;
    const VkExtent2D full_size{ .width = target_width, .height = target_height };
    history_t* history = temporal ? history_of(camera, target, full_size,
                                               { view.x, view.y, view.width, view.height })
                                  : nullptr;
    if (temporal && history == nullptr) {
        return false;
    }
    const std::array<float, 2> jitter =
        history != nullptr ? history->jitter : std::array<float, 2>{};
    std::vector<std::uint64_t> sampled;
    for (std::size_t at = 0; at < views.size(); ++at) {
        lists_t lists;
        read_view(camera, views[at], tweened, lists);
        view_batches_t batches;
        const bool main_view = at + 1 == views.size();
        const view_extras_t extras{
            .jitter = jitter,
            .motion = main_view && history != nullptr ? &history->motion : nullptr,
            // A reflection's seen in what's in the scene, whose own pixels are what this is of.
            .ambient_occlusion = main_view ? camera.ambient_occlusion : 0.0F,
        };
        if (!write_view(camera, views[at], tweened, lists, !main_view, target, shadows, at,
                        mirror_planes, sampled, extras, batches)) {
            return false;
        }
        pass->views.push_back(std::move(batches));
    }
    pass->format = format;
    pass->samples = samples;
    pass->stored = camera.stored_values;
    const bool whole = from_x == 0 && from_y == 0 && pass->scissor.extent.width == target_width &&
                       pass->scissor.extent.height == target_height;
    const std::array<float, 4> clear_rgba = cleared_with(camera);
    // An HDR image is all the camera's, so all of it's cleared: to nothing, if what's in its target
    // shows through.
    pass->clear_color = camera.clears_color && !whole && !aside;
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
    const gpu::resource_t prepass = temporal || occludes
                                        ? add_prepass(pass->views.back(), *pass, uses, full_size)
                                        : gpu::no_resource;
    if ((temporal || occludes) && prepass == gpu::no_resource) {
        return false;
    }
    if (occludes) {
        pass->occlusion =
            add_ambient_occlusion(camera, view, prepass, *pass, jitter,
                                  history != nullptr ? history->frames : 0, full_size);
        if (pass->occlusion == gpu::no_resource) {
            return false;
        }
        uses.emplace_back(pass->occlusion, gpu::use_t::SAMPLED);
    }
    const gpu::transient_t image_target{
        .width = target_width,
        .height = target_height,
        .format = camera.hdr ? FBE_TEXTURE_RGBA16F : picture_format,
        .samples = static_cast<std::uint32_t>(samples),
    };
    if (aside && !gpu::reserve_transient(image_target)) {
        return false;
    }
    const gpu::resource_t image = aside ? gpu::transient_resource(image_target) : into;
    if (image == gpu::no_resource) {
        return false;
    }
    const bool drawn = gpu::add_pass({
        .name = "camera",
        .colors = { {
            .target = image,
            .clear = aside || (camera.clears_color && whole),
            .rgba = camera.clears_color ? clear_rgba : std::array<float, 4>{},
            .stored = camera.stored_values,
        }, },
        .depth = into,
        .clear_depth = camera.clears_depth && whole,
        .uses = std::move(uses),
        .record = [pass](VkCommandBuffer commands) { record_pass(commands, *pass); },
    });
    if (!drawn || !aside) {
        return drawn;
    }
    if (!camera.hdr) {
        return add_fxaa(camera, image, into, full_size, drawn_format, samples, pass->scissor);
    }
    const gpu::resource_t resolved =
        temporal ? add_resolve(*history, camera, view, image, prepass, *pass) : image;
    if (resolved == gpu::no_resource) {
        return false;
    }
    if (!camera.fxaa) {
        return add_post_passes(camera, resolved, into, full_size, target_format, samples,
                               pass->scissor);
    }
    const gpu::transient_t picture_target{
        .width = target_width,
        .height = target_height,
        .format = picture_format,
    };
    if (!gpu::reserve_transient(picture_target)) {
        return false;
    }
    const gpu::resource_t picture = gpu::transient_resource(picture_target);
    return picture != gpu::no_resource &&
           add_post_passes(camera, resolved, picture, full_size, picture_vk_format,
                           VK_SAMPLE_COUNT_1_BIT, pass->scissor, true) &&
           add_fxaa(camera, picture, into, full_size, target_format, samples, pass->scissor);
}

} // namespace fbe::render

extern "C" {

FBE_API bool fbe_render_camera(uint64_t camera, float tween) try {
    std::uint32_t slot = fbe::scene::none;
    fbe::scene::view_t view;
    bool shows = false;
    // The size doesn't matter yet: only whether it's a camera and the tween's good.
    if (!fbe::render::drawn_view(camera, 1.0F, 1.0F, tween, slot, view, shows)) {
        return false;
    }
    fbe::render::forget_freed_geometries();
    fbe::render::forget_unused_histories();
    fbe::render::forget_unused_atlas();
    return (fbe_gpu_started() || fbe_gpu_start()) && fbe::render::draw_camera(slot, tween);
} catch (const std::bad_alloc&) {
    return fbe::fail("out of memory");
}

FBE_API bool fbe_render_world(uint64_t world, float tween) try {
    const std::int32_t count = fbe_world_cameras(world, nullptr, 0);
    if (count < 0) {
        return false;
    }
    std::vector<std::uint64_t> cameras(static_cast<std::size_t>(count));
    fbe_world_cameras(world, cameras.data(), count);
    // Nothing can change what casts between them, so they share it.
    fbe::render::keep_shadow_casters(true);
    const bool drawn = std::ranges::all_of(
        cameras, [tween](std::uint64_t camera) { return fbe_render_camera(camera, tween); });
    fbe::render::keep_shadow_casters(false);
    return drawn;
} catch (const std::bad_alloc&) {
    fbe::render::keep_shadow_casters(false);
    return fbe::fail("out of memory");
}

} // extern "C"
