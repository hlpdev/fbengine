#pragma once

// What the render area's files share: the camera a call draws for, its view as drawing sees from,
// and the lists read from the scene for it.

#include "fbengine.h"
#include "gpu/vulkan.hpp"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace fbe::render {

/// A draw, what it's sorted by, and what a light must reach to light it per vertex: its world box,
/// or a ground plane's place, in `min`, and the plane's up.
struct item_t {
    fbe_draw_t draw{};
    std::int32_t order = 0;
    bool blended = false;
    bool mirrored = false;
    bool vertex_lit = false;
    std::uint32_t material = scene::none;
    std::uint32_t entity = scene::none;
    std::uint64_t made = 0;
    scene::vec3_t min;
    scene::vec3_t max;
    scene::vec3_t up;
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

/// Draws a camera that's showing, in the frame's graph, as fbe_render_camera says.
bool draw_camera(std::uint32_t slot, float tween);

/// A surface on the GPU: its vertices, its indices, and how many of them.
struct surface_buffers_t {
    gpu::buffer_t vertices;
    gpu::buffer_t indices;
    std::uint32_t index_count = 0;
    /// Its vertices' joints, for a surface that has them.
    gpu::buffer_t joints;
};

/// What gives the palette a mesh is skinned by, by its slot, as the frame's poses are: a matrix a
/// joint in the mesh's own space, or none for a mesh with no skin.
using palette_source_t = std::span<const scene::mat4_t> (*)(std::uint32_t mesh,
                                                            const scene::tweened_t& tweened);
void set_palette_source(palette_source_t source);

/// A geometry's surface on the GPU, sent if it's changed since; null if it can't be, with
/// fbe_error's why, or the geometry's been freed.
const surface_buffers_t* surface_buffers(std::uint64_t geometry, std::size_t surface);

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
};

/// The pieces of a terrain a view sees, nearer ones finer: false, with fbe_error's why, if there's
/// no memory for them.
bool terrain_pieces(const scene::camera_t& camera, const scene::view_t& view, const item_t& item,
                    const scene::terrain_t& terrain, std::vector<terrain_piece_t>& pieces);

} // namespace fbe::render
