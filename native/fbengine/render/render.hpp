#pragma once

// What the render area's files share: the camera a call draws for, its view as drawing sees from,
// and the lists read from the scene for it.

#include "fbengine.h"
#include "gpu/graph.hpp"
#include "gpu/shader.hpp"
#include "gpu/vulkan.hpp"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fbe::render {

/// A draw, and what it's sorted by.
struct item_t {
    fbe_draw_t draw{};
    std::int32_t order = 0;
    bool blended = false;
    bool mirrored = false;
    /// Of an Instances: whether this item's of its instances that mirror, by their own matrices.
    bool instances_mirrored = false;
    bool vertex_lit = false;
    std::uint32_t material = scene::none;
    std::uint32_t entity = scene::none;
    std::uint64_t made = 0;
};

/// What a camera draws and what lights it, as fbe_render.h has them.
struct lists_t {
    std::vector<item_t> items;
    std::vector<fbe_draw_light_t> lights;
    std::vector<fbe_cluster_t> clusters;
    std::vector<std::uint32_t> cluster_lights;
    std::vector<std::uint32_t> draw_lights;
};

/// The camera a handle is for, its slot, and its view in a target of this size `tween` of the way
/// from its World's last capture, as drawing sees from: false, with fbe_error saying why, for a
/// freed camera, another entity, or a tween outside 0 to 1. `shows` is false for a hidden camera or
/// one whose viewport has no size, which draws nothing.
bool drawn_view(std::uint64_t camera, float target_width, float target_height, float tween,
                std::uint32_t& slot, scene::view_t& view, bool& shows);

/// What a camera draws, in its order.
std::vector<item_t> extract(const scene::camera_t& camera, const scene::view_t& view,
                            const scene::tweened_t& tweened);

/// Everything a camera draws in a target of this size `tween` of the way from its World's last
/// capture, read from the scene at once, so what's drawn and what lights it agree: false, as
/// drawn_view says.
bool read_lists(std::uint64_t camera, float target_width, float target_height, float tween,
                lists_t& lists);

/// Everything a camera draws from a view of it.
void read_view(const scene::camera_t& camera, const scene::view_t& view,
               const scene::tweened_t& tweened, lists_t& lists);

/// Lets go of the histories of cameras that have gone or no longer antialias by them; and, for
/// tests, how many there are.
void forget_unused_histories();
std::size_t history_count();
/// A camera's history's textures' width, 0 for none, for tests.
std::uint32_t history_width(std::uint64_t camera);

/// A camera's full-screen pass, ready to run: the material it reads, and its code.
struct game_pass_t {
    std::uint64_t material = 0;
    const gpu::shader_t* shader = nullptr;
};

/// The camera's full-screen passes that can be made, the last into `format` of `samples`, each of
/// the others saying so once.
std::vector<game_pass_t> game_passes(const scene::camera_t& camera, VkFormat format,
                                     VkSampleCountFlagBits samples);

/// An image a camera's picture goes through on its way to its target: what it's made as, and its
/// format, which a transient's isn't known as until it's recorded.
struct picture_t {
    gpu::transient_t target;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

/// What's in `source`, what the camera drew, through its full-screen passes in turn, into `into`,
/// of `format` and `samples`, each between in a `picture`. They read the depth of `target`, the
/// camera's. `into_nothing`, for one of its own that a later pass reads, clears that first and
/// blends nothing. False, with fbe_error's why, if it can't.
bool add_game_passes(const scene::camera_t& camera, std::span<const game_pass_t> passes,
                     gpu::resource_t source, gpu::resource_t into, VkFormat format,
                     VkSampleCountFlagBits samples, const picture_t& picture, std::uint64_t target,
                     const scene::view_t& view, VkRect2D scissor, bool into_nothing);

/// Adds the passes that bring a camera's HDR image to `into`, a target of `size`, `format`, and
/// `samples`, in `scissor`, blending it by its alpha if the camera doesn't clear what's there;
/// false, with fbe_error's why, if they can't be added. `into_nothing`, for one of its own that a
/// later pass reads, clears that first and blends nothing.
bool add_post_passes(const scene::camera_t& camera, gpu::resource_t image, gpu::resource_t into,
                     VkExtent2D size, VkFormat format, VkSampleCountFlagBits samples,
                     VkRect2D scissor, bool into_nothing = false);

/// Draws a camera that's showing, in the frame's graph, as fbe_render_camera says.
bool draw_camera(std::uint32_t slot, float tween);

/// A surface on the GPU: its vertices, its indices, and how many of them.
struct surface_buffers_t {
    gpu::buffer_t vertices;
    gpu::buffer_t indices;
    std::uint32_t index_count = 0;
    /// Its vertices' joints, for a surface that has them.
    gpu::buffer_t joints;
    /// Its vertices' tangents, for a surface that has them.
    gpu::buffer_t tangents;
    /// What each of its morph targets moves each of its vertices by, the first target's first, for
    /// each whether it moves any, and how many vertices each has.
    gpu::buffer_t targets;
    std::vector<std::uint8_t> target_moves;
    std::uint32_t vertex_count = 0;
};

/// A geometry's surface on the GPU, sent if it's changed since; null if it can't be, with
/// fbe_error's why, or the geometry's been freed.
const surface_buffers_t* surface_buffers(std::uint64_t geometry, std::size_t surface);

/// An emitter's particles on the GPU, with what's been born since sent: their address, or 0, with
/// fbe_error's why, if there's no memory for them.
VkDeviceAddress particles_buffer(scene::particles_t& particles);

/// An Instances entity's instances on the GPU, sent if they've changed: their address, or 0, with
/// fbe_error's why, if there's no memory for them.
VkDeviceAddress instances_buffer(scene::instances_t& instances);

/// Rasterizes the occluders a view has, for occluded's tests: false if it has none.
bool fill_occlusion(const scene::camera_t& camera, const scene::view_t& view,
                    const scene::tweened_t& tweened, const scene::frustum_t& frustum);

/// Whether all of what's within some points is behind the last view's occluders.
bool occluded(const scene::vec3_t* points, std::size_t count);

/// Lets go of what's kept of geometries that have gone; and, for tests, how many times what's next
/// to what in an occluder's been worked out, how many it's kept for, and how many pixels the last
/// view's looked at.
void forget_freed_occluders();
std::uint64_t occluder_edges_worked_out();
std::size_t occluder_edges_kept();
std::int64_t occluder_pixels();

/// Which of its levels of detail a mesh draws in a view, none for nothing, as its size there says:
/// what it draws, casts, and occludes with.
std::uint32_t mesh_lod(const scene::mesh_t& mesh, const scene::mat4_t& matrix,
                       const scene::tweened_t& tweened, const scene::camera_t& camera,
                       const scene::view_t& view);

/// The quad a sprite draws: -1 to 1 in x and y, facing +Z, UV 0, 0 at its top left.
const surface_buffers_t* sprite_quad();

/// Lets go of the surfaces of geometries that have been freed.
void forget_freed_geometries();

/// A piece of a terrain as a view draws it: its vertices' address and its indices, all in the
/// frame's ring.
struct terrain_piece_t {
    VkDeviceAddress vertices = 0;
    VkBuffer indices = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    std::uint32_t index_count = 0;
    /// The sphere it's within, in the world, where a shadow's asked for it.
    scene::vec3_t center;
    float radius = 0.0F;
};

/// A terrain in coarse pieces, all of them, whatever's seen, for shadows: false, with fbe_error's
/// why, if there's no memory for them.
bool terrain_coarse_pieces(const scene::mat4_t& world, const scene::terrain_t& terrain,
                           std::vector<terrain_piece_t>& pieces);

/// The pieces of a terrain a view sees, nearer ones finer: false, with fbe_error's why, if there's
/// no memory for them.
bool terrain_pieces(const scene::camera_t& camera, const scene::view_t& view, const item_t& item,
                    const scene::terrain_t& terrain, std::vector<terrain_piece_t>& pieces);

} // namespace fbe::render
