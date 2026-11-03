// What lights what a camera draws: its World's lights in its view, and the clusters of its view
// each point and spot light reaches, which drawing lights each pixel by.

#include "core/engine.hpp"
#include "fbengine.h"
#include "render/render.hpp"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace fbe::render {

namespace {

using scene::camera_t;
using scene::vec3_t;

/// A light as drawing has it, when it was made, which lights of a kind
/// go by, and its index in the lists' lights, once it has one.
struct lit_t {
    fbe_draw_light_t draw{};
    std::uint64_t made = 0;
    std::uint32_t index = scene::none;
    bool wanted = false;
};

/// Where each of a camera's depth slices starts, its distance in front of it, and where the last
/// ends.
std::array<float, FBE_CLUSTERS_DEEP + 1> slice_starts(const camera_t& camera) {
    std::array<float, FBE_CLUSTERS_DEEP + 1> starts{};
    for (int slice = 0; slice <= FBE_CLUSTERS_DEEP; ++slice) {
        const float part = static_cast<float>(slice) / static_cast<float>(FBE_CLUSTERS_DEEP);
        starts[static_cast<std::size_t>(slice)] =
            camera.projection == FBE_PROJECTION_PERSPECTIVE
                ? camera.nearest * std::pow(camera.farthest / camera.nearest, part)
                : camera.nearest + ((camera.farthest - camera.nearest) * part);
    }
    starts[FBE_CLUSTERS_DEEP] = camera.farthest;
    return starts;
}

/// The depth slice a distance in front of a camera is in, clamped to its slices.
int slice_of(const camera_t& camera, float depth) {
    const float part =
        camera.projection == FBE_PROJECTION_PERSPECTIVE
            ? std::log(depth / camera.nearest) / std::log(camera.farthest / camera.nearest)
            : (depth - camera.nearest) / (camera.farthest - camera.nearest);
    return std::clamp(static_cast<int>(std::floor(part * FBE_CLUSTERS_DEEP)), 0,
                      FBE_CLUSTERS_DEEP - 1);
}

/// The column or row of `count` a point -1 to 1 across the viewport is in, clamped to them, as a
/// float, since a huge light's sides are further across than an int holds.
int tile_of(float across, int count) {
    const float last = static_cast<float>(count) - 1.0F;
    return static_cast<int>(
        std::clamp(std::floor((across + 1.0F) * 0.5F * static_cast<float>(count)), 0.0F, last));
}

/// How far a point is from a box, squared, 0 inside it.
float apart_squared(vec3_t p, vec3_t min, vec3_t max) {
    const vec3_t nearest(std::clamp(p.x, min.x, max.x), std::clamp(p.y, min.y, max.y),
                         std::clamp(p.z, min.z, max.z));
    return dot(p - nearest, p - nearest);
}

/// Every cluster of a camera's view a sphere in its view reaches, by index, in `found`.
void clusters_reached(const camera_t& camera, const scene::view_t& view,
                      const std::array<float, FBE_CLUSTERS_DEEP + 1>& starts, vec3_t center,
                      float radius, std::vector<std::uint32_t>& found) {
    found.clear();
    const float nearest = std::fmax(-center.z - radius, camera.nearest);
    const float farthest = std::fmin(-center.z + radius, camera.farthest);
    if (nearest > farthest) {
        return;
    }
    float half_width = 0.0F;
    float half_height = 0.0F;
    scene::half_extents(camera, view, half_width, half_height);
    const bool perspective = camera.projection == FBE_PROJECTION_PERSPECTIVE;
    // Across the viewport, -1 to 1, where the sphere's sides are at the nearest and furthest of its
    // depths, as a perspective camera sees them, or anywhere, as an orthographic one does.
    const auto spread = [&](float low, float high, float half) {
        const float near_scale = perspective ? nearest * half : half;
        const float far_scale = perspective ? farthest * half : half;
        return std::pair{ std::fmin(low / near_scale, low / far_scale),
                          std::fmax(high / near_scale, high / far_scale) };
    };
    const auto [left, right] = spread(center.x - radius, center.x + radius, half_width);
    const auto [bottom, top] = spread(center.y - radius, center.y + radius, half_height);
    // A cluster wider each side, so where a slice or a tile is rounded to never leaves one out:
    // each is tried by its box.
    const int first_column = std::max(tile_of(left, FBE_CLUSTERS_ACROSS) - 1, 0);
    const int last_column =
        std::min(tile_of(right, FBE_CLUSTERS_ACROSS) + 1, FBE_CLUSTERS_ACROSS - 1);
    const int first_row = std::max(FBE_CLUSTERS_UP - 1 - tile_of(top, FBE_CLUSTERS_UP) - 1, 0);
    const int last_row =
        std::min(FBE_CLUSTERS_UP - 1 - tile_of(bottom, FBE_CLUSTERS_UP) + 1, FBE_CLUSTERS_UP - 1);
    const int first_slice = std::max(slice_of(camera, nearest) - 1, 0);
    const int last_slice = std::min(slice_of(camera, farthest) + 1, FBE_CLUSTERS_DEEP - 1);
    for (int slice = first_slice; slice <= last_slice; ++slice) {
        const float near_depth = starts[static_cast<std::size_t>(slice)];
        const float far_depth = starts[static_cast<std::size_t>(slice) + 1];
        const float near_scale = perspective ? near_depth : 1.0F;
        const float far_scale = perspective ? far_depth : 1.0F;
        for (int row = first_row; row <= last_row; ++row) {
            // Rows go down from the top, where y is up.
            const float high =
                (1.0F - (2.0F * static_cast<float>(row) / FBE_CLUSTERS_UP)) * half_height;
            const float low =
                (1.0F - (2.0F * static_cast<float>(row + 1) / FBE_CLUSTERS_UP)) * half_height;
            for (int column = first_column; column <= last_column; ++column) {
                const float from =
                    (-1.0F + (2.0F * static_cast<float>(column) / FBE_CLUSTERS_ACROSS)) *
                    half_width;
                const float to =
                    (-1.0F + (2.0F * static_cast<float>(column + 1) / FBE_CLUSTERS_ACROSS)) *
                    half_width;
                const vec3_t min(std::fmin(from * near_scale, from * far_scale),
                                 std::fmin(low * near_scale, low * far_scale), -far_depth);
                const vec3_t max(std::fmax(to * near_scale, to * far_scale),
                                 std::fmax(high * near_scale, high * far_scale), -near_depth);
                if (apart_squared(center, min, max) <= radius * radius) {
                    found.push_back(static_cast<std::uint32_t>(
                        column + (FBE_CLUSTERS_ACROSS * (row + (FBE_CLUSTERS_UP * slice)))));
                }
            }
        }
    }
}

/// A camera's World's showing lights with some intensity, in its tree's order, each where it is
/// between the steps, taken into the camera's view.
std::vector<lit_t> lights_lit(std::uint32_t world, const scene::view_t& view,
                              const scene::tweened_t& tweened) {
    const scene::scene_t& s = scene::scene();
    std::vector<std::uint32_t> found;
    scene::lights_in(s.worlds[world].roots, found);
    const scene::quat_t back = scene::inverse(view.rotation);
    std::vector<lit_t> lit;
    lit.reserve(found.size());
    for (const std::uint32_t entity : found) {
        const scene::light_t& light = s.lights[s.entities.component[entity]];
        if (!scene::showing(entity) || light.intensity <= 0.0F) {
            continue;
        }
        const scene::pose_t pose = tweened.pose(entity);
        const vec3_t place = pose.matrix.axis(3);
        const vec3_t offset = place - view.position;
        const vec3_t seen = back * offset;
        const vec3_t forward = pose.rotation * vec3_t(0.0F, 0.0F, -1.0F);
        const vec3_t way = back * forward;
        const float red = light.color[0] * light.intensity;
        const float green = light.color[1] * light.intensity;
        const float blue = light.color[2] * light.intensity;
        lit_t one;
        one.made = s.entities.made[entity];
        one.draw = {
            .light = s.entities.slots.handle(entity),
            .type = light.type,
            .color = { red, green, blue },
            .position = { seen.x, seen.y, seen.z },
            .direction = { way.x, way.y, way.z },
            .range = light.range,
            .inner_cosine = std::cos(light.inner_cone),
            .outer_cosine = std::cos(light.outer_cone),
            .shadow = -1,
        };
        lit.push_back(one);
    }
    return lit;
}

} // namespace

bool read_lists(std::uint64_t camera, float target_width, float target_height, float tween,
                lists_t& lists) {
    std::uint32_t slot = scene::none;
    scene::view_t view;
    bool shows = false;
    if (!drawn_view(camera, target_width, target_height, tween, slot, view, shows)) {
        return false;
    }
    lists.items.clear();
    lists.lights.clear();
    lists.cluster_lights.clear();
    lists.draw_lights.clear();
    lists.clusters.assign(FBE_CLUSTERS, fbe_cluster_t{ .first = 0, .count = 0 });
    if (shows) {
        const scene::scene_t& s = scene::scene();
        read_view(s.cameras[s.entities.component[slot]], view, scene::tweened_t(tween), lists);
    }
    return true;
}

void read_view(const camera_t& camera, const scene::view_t& view, const scene::tweened_t& tweened,
               lists_t& lists) {
    const scene::scene_t& s = scene::scene();
    lists.items = extract(camera, view, tweened);
    lists.lights.clear();
    lists.cluster_lights.clear();
    lists.draw_lights.clear();
    lists.clusters.assign(FBE_CLUSTERS, fbe_cluster_t{ .first = 0, .count = 0 });
    std::vector<lit_t> lit = lights_lit(s.entities.world[camera.entity], view, tweened);
    // What's lit per vertex takes the first lights in the tree's order, all of it the same ones,
    // whatever they reach, as fixed-function hardware's did, so they stay in the lights though no
    // cluster has them.
    const std::size_t per_vertex = std::min<std::size_t>(lit.size(), FBE_DRAW_LIGHTS);
    bool any_per_vertex = false;
    for (item_t& item : lists.items) {
        if (item.vertex_lit) {
            item.draw.light_first = 0;
            item.draw.light_count = static_cast<std::uint32_t>(per_vertex);
            any_per_vertex = true;
        }
    }
    if (any_per_vertex) {
        for (std::size_t at = 0; at < per_vertex; ++at) {
            lit[at].wanted = true;
        }
    }
    std::vector<std::uint32_t> ordered(lit.size());
    for (std::size_t at = 0; at < ordered.size(); ++at) {
        ordered[at] = static_cast<std::uint32_t>(at);
    }
    std::ranges::sort(ordered, [&](std::uint32_t at_a, std::uint32_t at_b) {
        const lit_t& a = lit[at_a];
        const lit_t& b = lit[at_b];
        const bool a_everywhere = a.draw.type == FBE_LIGHT_DIRECTIONAL;
        const bool b_everywhere = b.draw.type == FBE_LIGHT_DIRECTIONAL;
        return a_everywhere != b_everywhere ? a_everywhere : a.made < b.made;
    });
    // Directional lights light every cluster; each point and spot light, the clusters its range
    // reaches, and it's left out when it reaches none and nothing lit per vertex wants it.
    const auto starts = slice_starts(camera);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> reaches;
    std::vector<std::uint32_t> reached;
    for (const std::uint32_t at : ordered) {
        lit_t& one = lit[at];
        if (one.draw.type != FBE_LIGHT_DIRECTIONAL) {
            const vec3_t center(one.draw.position[0], one.draw.position[1], one.draw.position[2]);
            clusters_reached(camera, view, starts, center, one.draw.range, reached);
            if (reached.empty() && !one.wanted) {
                continue;
            }
            for (const std::uint32_t cluster : reached) {
                reaches.emplace_back(cluster, static_cast<std::uint32_t>(lists.lights.size()));
            }
        }
        one.index = static_cast<std::uint32_t>(lists.lights.size());
        lists.lights.push_back(one.draw);
    }
    if (any_per_vertex) {
        lists.draw_lights.reserve(per_vertex);
        for (std::size_t at = 0; at < per_vertex; ++at) {
            lists.draw_lights.push_back(lit[at].index);
        }
    }
    // Each cluster's lights together, in the lights' order.
    for (const auto& [cluster, light] : reaches) {
        ++lists.clusters[cluster].count;
    }
    std::uint32_t total = 0;
    for (fbe_cluster_t& cluster : lists.clusters) {
        cluster.first = total;
        total += cluster.count;
    }
    lists.cluster_lights.resize(total);
    std::array<std::uint32_t, FBE_CLUSTERS> next{};
    for (const auto& [cluster, light] : reaches) {
        const fbe_cluster_t& where = lists.clusters[cluster];
        lists.cluster_lights[where.first + next[cluster]++] = light;
    }
}

} // namespace fbe::render

extern "C" {

FBE_API bool fbe_render_lights(uint64_t camera, float target_width, float target_height,
                               float tween, fbe_draw_light_t* lights, int32_t light_capacity,
                               int32_t* light_count, fbe_cluster_t* clusters, uint32_t* indices,
                               int32_t index_capacity, int32_t* index_count) {
    *light_count = 0;
    *index_count = 0;
    fbe::render::lists_t lists;
    if (!fbe::render::read_lists(camera, target_width, target_height, tween, lists)) {
        return false;
    }
    if (clusters != nullptr) {
        std::ranges::copy(lists.clusters, clusters);
    }
    const auto index_room = std::min(lists.cluster_lights.size(),
                                     static_cast<std::size_t>(std::max(index_capacity, 0)));
    std::copy_n(lists.cluster_lights.begin(), index_room, indices);
    const auto room =
        std::min(lists.lights.size(), static_cast<std::size_t>(std::max(light_capacity, 0)));
    std::copy_n(lists.lights.begin(), room, lights);
    *light_count = static_cast<std::int32_t>(lists.lights.size());
    *index_count = static_cast<std::int32_t>(lists.cluster_lights.size());
    return true;
}

FBE_API int32_t fbe_render_draw_lights(uint64_t camera, float target_width, float target_height,
                                       float tween, uint32_t* indices, int32_t capacity) {
    fbe::render::lists_t lists;
    if (!fbe::render::read_lists(camera, target_width, target_height, tween, lists)) {
        return -1;
    }
    const auto room =
        std::min(lists.draw_lights.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    std::copy_n(lists.draw_lights.begin(), room, indices);
    return static_cast<std::int32_t>(lists.draw_lights.size());
}

} // extern "C"
