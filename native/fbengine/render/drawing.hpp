#pragma once

// What drawing a camera and drawing its shadows share: what the scene shader reads, the frame's
// ring, and a pass's draws.

#include "environment/environment.hpp"
#include "fbengine.h"
#include "gpu/buffer.hpp"
#include "gpu/graph.hpp"
#include "gpu/pipeline.hpp"
#include "gpu/vulkan.hpp"
#include "morph/morph.hpp"
#include "render/render.hpp"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>
#include <vector>

namespace fbe::render {

/// A draw as the scene shader reads it: its matrix to the camera's view; the rows of what turns
/// its normals there; its color and alpha; its vertices' address; its joints' and palette's, if
/// it's skinned; its tangents', if it has them; its morph targets' and the ones it's drawn with, if
/// it's morphed; its emitter's, if it's particles; its instances', if it has them; its material's
/// index in the camera's; its own lights; how many matrices its palette has, 0 if it isn't skinned;
/// how many targets it's drawn with, and how many vertices each has; whether it has tangents; and
/// which of its camera's previous records is its entity's, with temporal antialiasing.
struct draw_data_t {
    std::array<float, 16> to_view;
    std::array<std::array<float, 4>, 3> normals;
    std::array<float, 4> color;
    VkDeviceAddress vertices;
    VkDeviceAddress joints;
    VkDeviceAddress palette;
    VkDeviceAddress tangents;
    VkDeviceAddress targets;
    VkDeviceAddress drawn_targets;
    VkDeviceAddress particles;
    VkDeviceAddress instances;
    std::uint32_t material;
    std::uint32_t light_first;
    std::uint32_t light_count;
    std::uint32_t joint_count;
    std::uint32_t drawn_target_count;
    std::uint32_t target_vertices;
    std::uint32_t has_tangents;
    std::uint32_t previous;
};
static_assert(sizeof(draw_data_t) == 224, "the scene shader reads draws 224 bytes apart");

/// An instance as the scene shader reads it: its matrix's three rows, and its color.
struct instance_data_t {
    std::array<std::array<float, 4>, 3> rows;
    std::array<float, 4> color;
};
static_assert(sizeof(instance_data_t) == 64, "the scene shader reads instances 64 bytes apart");
static_assert(sizeof(anim::drawn_target_t) == 8, "the scene shader reads targets 8 bytes apart");

/// An emitter's particles as the scene shader reads them: their address, gravity, drag, their
/// colors and half-sizes from birth to death, its clock now, and when that wraps.
struct particles_data_t {
    VkDeviceAddress records;
    std::array<float, 3> gravity;
    float drag;
    std::array<float, 4> color_start;
    std::array<float, 4> color_end;
    float size_start;
    float size_end;
    float now;
    float wrap;
};
static_assert(sizeof(particles_data_t) == 72, "the scene shader reads an emitter of 72 bytes");

/// A sky as its shader reads it: the view's axes in the world, its half-size at a distance of 1,
/// what /// it is, its texture's index, the sun's way and how bright, its sampler, whether it's
/// mirrored, every mirror's plane, a normal and the true eye's height off it, and which of them
/// this view's the reflection in, or their count for the scene's own.
struct sky_data_t {
    std::array<float, 4> right;
    std::array<float, 4> up;
    std::array<float, 4> forward;
    std::array<float, 2> spread;
    std::uint32_t mode;
    std::uint32_t texture;
    std::array<float, 3> sun;
    float brightness;
    std::uint32_t sampler;
    float flip;
    VkDeviceAddress mirrors;
    std::uint32_t mirror_count;
    std::uint32_t own_mirror;
};
static_assert(sizeof(sky_data_t) == 104, "the sky shader reads a sky of 104 bytes");

/// A layer as the scene shader reads it: its texture's and sampler's indices in the bindless set,
/// how a bump layer scales the next layer's light, what turns its UVs, by rows, with in their last
/// places whether sampling it decodes sRGB and whether it can't be sampled, and a bump layer's
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
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    std::array<float, 3> emissive;
    /// Which of its maps it has, a bit for each FBE_MAP_.
    std::uint32_t maps;
    std::array<layer_data_t, FBE_MATERIAL_MAPS> map_layers;
};
static_assert(sizeof(material_data_t) == 832, "the scene shader reads materials 832 bytes apart");

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
    VkDeviceAddress shadows;
    std::uint32_t shadow_atlas;
    float lod_bias;
    /// The rows of what turns its view's directions into the world's, which an environment's are
    /// in.
    std::array<std::array<float, 4>, 3> world_rows;
    /// What it's lit by, and whether it's lit by it.
    environment::shader_data_t environment;
    std::uint32_t has_environment;
    std::uint32_t spare_too;
    /// What its entities were last frame, if it goes by that; how far its projection's off, as
    /// its clip space goes; and what its ambient occlusion's raised to, 0 for none.
    VkDeviceAddress previous;
    std::array<float, 2> jitter;
    float ambient_occlusion;
    std::uint32_t moving;
};
static_assert(sizeof(camera_data_t) == 312, "the scene shader reads a camera of 312 bytes");

/// An entity as it was last frame, as the scene shader's prepass reads it: from its space to clip
/// space then, and its skin's palette then, if it had the same one.
struct previous_data_t {
    std::array<float, 16> to_clip;
    VkDeviceAddress palette;
    std::uint32_t has_palette;
    std::uint32_t spare;
};
static_assert(sizeof(previous_data_t) == 80,
              "the scene shader reads previous records 80 bytes apart");
static_assert(sizeof(fbe_draw_light_t) == 64, "the scene shader reads lights 64 bytes apart");
static_assert(sizeof(fbe_vertex_t) == 56, "the scene shader reads vertices 56 bytes apart");

/// A shadow's tile as the scene shader reads it: from the view to the light's clip space; its
/// pixels in the atlas, from and to; its biases, depth's in the world and along the normal; what a
/// world's depth comes to in the tile's; where a cascade ends; whether it's level, a directional
/// light's; and a texel of the atlas in UV.
struct shadow_data_t {
    std::array<float, 16> from_view;
    std::array<float, 4> rect;
    float bias;
    float normal_bias;
    float depth_per;
    float split;
    std::uint32_t level;
    float inverse_side;
    /// Of a cascade, how many of its light's come after it.
    std::uint32_t left;
    /// Whether it's one of six, a face of what's all around its light.
    std::uint32_t face;
};
static_assert(sizeof(shadow_data_t) == 112, "the scene shader reads shadows 112 bytes apart");

/// Draws of one pipeline and one piece of geometry: their pipeline key, their indices, and which
/// of the list's draws, one after another, as instances.
struct batch_t {
    std::uint64_t key = 0;
    VkBuffer indices = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    std::uint32_t index_count = 0;
    std::uint32_t first_draw = 0;
    std::uint32_t draws = 1;
    /// Not by indices, as an emitter's particles aren't: this many vertices, for draw `first_draw`.
    std::uint32_t vertex_count = 0;
    /// For the sky: its address, which its shader takes in place of the camera's.
    VkDeviceAddress sky = 0;
    /// Of an Instances entity: which of its instances, and `first_draw` is its one draw.
    std::uint32_t instance_first = 0;
    std::uint32_t instance_count = 0;
};

/// What a pass draws from one view of a camera: the camera's address, whether the view's mirrored
/// across, as a reflection's is, turning every triangle's front to its back, and its batches.
struct view_batches_t {
    VkDeviceAddress camera = 0;
    bool flipped = false;
    std::vector<batch_t> batches;
    /// A tile of its own, as a shadow's, in place of the pass's.
    bool has_viewport = false;
    VkViewport viewport{};
    VkRect2D scissor{};
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
    /// Whether it's a shadow pass, by the shader's own entry points for that.
    bool shadows = false;
    /// Whether it's a camera's prepass, by the same shader's for that.
    bool prepass = false;
    /// Whether its camera draws in stored values.
    bool stored = false;
    /// What it reads as its camera's ambient occlusion.
    gpu::resource_t occlusion = gpu::no_resource;
};

/// Records a triangle over a target, by a pipeline of its own, with what its shader's given.
template <typename Push>
void record_full_screen(VkCommandBuffer commands, VkPipeline pipeline, const Push& push) {
    VkDescriptorSet set = gpu::bindless_set();
    VkPipelineLayout layout = gpu::pipeline_layout();
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                            nullptr);
    vkCmdPushConstants(commands, layout, gpu::flag<VkShaderStageFlags>(VK_SHADER_STAGE_ALL), 0,
                       sizeof(push), &push);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(commands, 3, 1, 0, 0);
    gpu::count_draws(1, 1);
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
/// sample what the pass draws into, so leaves what's under it as it is, as a layer does whose
/// texture has gone with the GPU.
material_data_t material_data(std::uint32_t material, std::uint64_t target,
                              std::vector<std::uint64_t>& sampled);

/// Records a pass's draws.
void record_pass(VkCommandBuffer commands, const recorded_pass_t& pass);

/// What a camera's main view's entities were last frame, and what goes to the next.
struct motion_t {
    scene::mat4_t to_clip;
    std::unordered_map<std::uint64_t, scene::mat4_t> matrices;
    std::unordered_map<std::uint64_t, scene::mat4_t> matrices_now;
    std::unordered_map<std::uint64_t, std::vector<scene::mat4_t>> palettes;
    std::unordered_map<std::uint64_t, std::vector<scene::mat4_t>> palettes_now;
    std::vector<previous_data_t> records;
    std::unordered_map<std::uint64_t, std::uint32_t> indices;
};

/// A camera's temporal antialiasing as it stands: its two textures, which is newest, and what it
/// went by, the frame it was last drawn in among them; how many frames there are in it, 0 for none;
/// how many it's been jittered, and this one's, in its clip space; and how its last view was
/// turned, from the world.
struct history_t {
    std::array<std::uint64_t, 2> textures{};
    std::size_t newest = 0;
    VkExtent2D size{};
    std::array<float, 4> viewport{};
    std::uint64_t resets = 0;
    std::uint64_t target = 0;
    std::int64_t last_frame = 0;
    std::uint32_t frames = 0;
    std::uint32_t jittered = 0;
    std::array<float, 2> jitter{};
    scene::quat_t from_world;
    motion_t motion;
};

/// A camera's history for this frame, made if it has to be, with no frames in it if its target,
/// its size, or its viewport, or what it's been told have changed, or it's gone a while undrawn:
/// null, with fbe_error's why, if it can't be.
history_t* history_of(const scene::camera_t& camera, std::uint64_t target, VkExtent2D size,
                      const std::array<float, 4>& viewport);

/// Which of `motion`'s records is an item's entity's, added if it isn't there; `failed` if there's
/// no memory for it. finish_motion's to go after a view's items.
std::uint32_t previous_of(motion_t& motion, const item_t& item, const scene::tweened_t& tweened,
                          bool& failed);
void finish_motion(motion_t& motion, const scene::mat4_t& to_clip_now);

/// Adds a camera's prepass, of its main view's opaque draws, as render/prepass.cpp has it, before
/// its scene pass, sampling what `uses` says, as a cut-out's layers are: its target, or
/// no_resource, with fbe_error's why.
gpu::resource_t add_prepass(const view_batches_t& view, const recorded_pass_t& scene_pass,
                            std::vector<std::pair<gpu::resource_t, gpu::use_t>> uses,
                            VkExtent2D size);

/// Adds a camera's ambient occlusion's passes, from its prepass: what they leave, or no_resource,
/// with fbe_error's why.
gpu::resource_t add_ambient_occlusion(const scene::camera_t& camera, const scene::view_t& view,
                                      gpu::resource_t prepass, const recorded_pass_t& scene_pass,
                                      const std::array<float, 2>& jitter, std::uint32_t frame,
                                      VkExtent2D size);

/// Adds a camera's FXAA, from a picture done but for it: false, with fbe_error's why, if it can't.
bool add_fxaa(const scene::camera_t& camera, gpu::resource_t image, gpu::resource_t into,
              VkExtent2D size, VkFormat format, VkSampleCountFlagBits samples, VkRect2D scissor);

/// Adds what resolves a camera's image over its history: what comes of it, which the camera's post
/// processing reads in its image's place, or no_resource, with fbe_error's why.
gpu::resource_t add_resolve(history_t& history, const scene::camera_t& camera,
                            const scene::view_t& view, gpu::resource_t image,
                            gpu::resource_t prepass, const recorded_pass_t& scene_pass);

/// A camera's shadows this frame: the atlas, 0 for none, its tiles as the shader reads them but for
/// their matrices, which are from each view, what those are from, and each shadowed light's first.
struct shadow_frame_t {
    std::uint64_t atlas = 0;
    std::vector<shadow_data_t> records;
    std::vector<scene::mat4_t> world_to_clip;
    /// A view's first record for each light it has, by handle: a directional light's cascades are
    /// a view's own, the others' shared.
    std::vector<std::unordered_map<std::uint64_t, std::uint32_t>> firsts;
};

/// Draws a camera's World's shadows for its views, the reflections' then its own, as a pass of the
/// frame: false, with fbe_error's why, if there's no memory for what lists them. With no memory for
/// the atlas, it's smaller, or there are none.
bool draw_shadows(const scene::camera_t& camera, std::span<const scene::view_t> views,
                  const scene::tweened_t& tweened, shadow_frame_t& frame);

/// Keeps what casts between the cameras a World's drawing has, or stops.
void keep_shadow_casters(bool keep);

/// Lets go of the atlas, once it's gone a while unused or bigger than it's been wanted.
void forget_unused_atlas();

/// The atlas's side, 0 with none; and, for tests, the side from which the GPU's taken to refuse it.
std::uint32_t shadow_atlas_side();
void refuse_shadow_atlas(std::uint32_t side);

} // namespace fbe::render
