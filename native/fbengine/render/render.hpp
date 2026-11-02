#pragma once

// What the render area's files share: the camera a call draws for, its view as drawing sees from,
// and the lists read from the scene for it.

#include "fbengine.h"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <cstdint>
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
    std::vector<fbe_draw_t> draws;
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

} // namespace fbe::render
