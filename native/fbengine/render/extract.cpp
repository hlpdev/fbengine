// Extracting what a camera draws from the scene into a list, in the order it draws it, so drawing
// reads the list and never the live scene.

#include "core/engine.hpp"
#include "fbengine.h"
#include "morph/morph.hpp"
#include "render/render.hpp"
#include "scene/camera.hpp"
#include "scene/math.hpp"
#include "scene/scene.hpp"
#include "skin/skin.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

namespace fbe::render {

namespace {

using scene::camera_t;
using scene::mat4_t;
using scene::none;
using scene::vec3_t;

void put(fbe_draw_t& draw, const mat4_t& matrix, const std::array<float, 4>& tint, float fade) {
    std::ranges::copy(matrix.m, draw.matrix);
    draw.color[0] = tint[0];
    draw.color[1] = tint[1];
    draw.color[2] = tint[2];
    draw.color[3] = tint[3] * fade;
}

/// The item of something drawn in `material` with this alpha, at `matrix`, seen from a view.
item_t item_of(std::uint32_t entity, std::uint32_t material, const mat4_t& matrix,
               const std::array<float, 4>& tint, float fade, const scene::frustum_t& frustum) {
    const scene::scene_t& s = scene::scene();
    item_t item;
    item.entity = entity;
    item.made = s.entities.made[entity];
    item.material = material;
    item.order = material != none ? s.materials[material].order : 0;
    item.draw.entity = s.entities.slots.handle(entity);
    item.draw.kind = s.entities.kind[entity];
    item.draw.surface = -1;
    item.draw.material = material != none ? s.material_slots.handle(material) : 0;
    item.draw.blend = scene::drawn_blend(material, tint[3] * fade);
    item.blended = item.draw.blend != FBE_BLEND_OPAQUE;
    item.mirrored = scene::mirrors(matrix);
    item.vertex_lit = material != none && s.materials[material].shading == FBE_SHADING_VERTEX;
    put(item.draw, matrix, tint, fade);
    // Its near side's normal is the way the view looks.
    item.draw.depth = scene::dot(matrix.axis(3) - frustum.eye, frustum.normals[0]);
    return item;
}

/// A draw's pipeline, as fbe_render.h lays its bits out, from its material, or a new one's for
/// none, but a sprite's, which is full-bright with none; its blend and depth, the camera's fog,
/// whether its matrix mirrors it, and the layers of its material that have textures.
std::uint64_t pipeline_of(const item_t& item, const camera_t& camera) {
    const scene::scene_t& s = scene::scene();
    const scene::material_t standard;
    const scene::material_t& material =
        item.material != none ? s.materials[item.material] : standard;
    const std::int32_t fog = material.fogged ? camera.fog_mode : FBE_FOG_NONE;
    const bool particles = item.draw.kind == FBE_ENTITY_PARTICLES;
    const bool instances = item.draw.kind == FBE_ENTITY_INSTANCES;
    const std::int32_t shading =
        item.material == none && (item.draw.kind == FBE_ENTITY_SPRITE || particles)
            ? FBE_SHADING_UNLIT
            : material.shading;
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
    return static_cast<std::uint64_t>(shading) |
           (static_cast<std::uint64_t>(item.draw.blend) << 2U) |
           (static_cast<std::uint64_t>(material.alpha_cutoff > 0.0F ? 1 : 0) << 5U) |
           (static_cast<std::uint64_t>(material.vertex_colors || particles || instances ? 1 : 0)
            << 6U) |
           (static_cast<std::uint64_t>(material.two_sided ? 1 : 0) << 7U) |
           (static_cast<std::uint64_t>(fog) << 8U) |
           (static_cast<std::uint64_t>(item.draw.depth_mode) << 10U) |
           (static_cast<std::uint64_t>(item.mirrored ? 1 : 0) << 12U) | (layer_count << 13U) |
           (layers << 17U) | (static_cast<std::uint64_t>(material.wireframe ? 1 : 0) << 49U) |
           (static_cast<std::uint64_t>(particles ? 1 : 0) << 50U) |
           (static_cast<std::uint64_t>(instances ? 1 : 0) << 51U);
}

/// What items sort by, and which it's of, so they don't move till they're in order.
struct sort_key_t {
    std::uint64_t pipeline = 0;
    std::uint64_t geometry = 0;
    std::uint64_t made = 0;
    std::int32_t order = 0;
    float depth = 0.0F;
    std::int32_t surface = 0;
    std::uint32_t item = 0;
    bool blended = false;
};

/// The eight corners of a box through a matrix.
std::array<vec3_t, 8> corners_of(const mat4_t& matrix, vec3_t min, vec3_t max) {
    std::array<vec3_t, 8> corners;
    for (unsigned corner = 0; corner < 8; ++corner) {
        corners[corner] = scene::transform_point(
            matrix, vec3_t((corner & 1U) != 0 ? max.x : min.x, (corner & 2U) != 0 ? max.y : min.y,
                           (corner & 4U) != 0 ? max.z : min.z));
    }
    return corners;
}

/// The largest of a matrix's axes.
float largest_axis(const mat4_t& matrix) {
    return std::fmax(std::fmax(scene::length(matrix.axis(0)), scene::length(matrix.axis(1))),
                     scene::length(matrix.axis(2)));
}

} // namespace

std::uint32_t mesh_lod(const scene::mesh_t& mesh, const mat4_t& matrix,
                       const scene::tweened_t& tweened, const camera_t& camera,
                       const scene::view_t& view) {
    const scene::geometry_t& geometry = scene::scene().geometries[mesh.geometry];
    if (geometry.lods.empty()) {
        return mesh.geometry;
    }
    vec3_t min = geometry.cull_min;
    vec3_t max = geometry.cull_max;
    if (!geometry.has_cull_box) {
        scene::geometry_bounds(mesh.geometry, min, max);
        anim::grow_by_targets(mesh.geometry, anim::morph_weights(mesh.entity, tweened.tween()), min,
                              max);
    }
    const vec3_t both = min + max;
    vec3_t center = scene::transform_point(matrix, both * 0.5F);
    // Skinned, it's where its joints are.
    vec3_t joints_min;
    vec3_t joints_max;
    if (!geometry.has_cull_box &&
        anim::skin_joints_box(mesh.entity, tweened, joints_min, joints_max)) {
        const vec3_t both_joints = joints_min + joints_max;
        center = both_joints * 0.5F;
    }
    float half_width = 0.0F;
    float half_height = 0.0F;
    scene::half_extents(camera, view, half_width, half_height);
    return scene::lod_for(mesh.geometry, center,
                          scene::length(max - min) * 0.5F * largest_axis(matrix), camera,
                          view.position, half_height);
}

std::vector<item_t> extract(const camera_t& camera, const scene::view_t& view,
                            const scene::tweened_t& tweened) {
    const scene::scene_t& s = scene::scene();
    const std::uint32_t world = s.entities.world[camera.entity];
    const scene::frustum_t frustum = scene::frustum_of(camera, view);
    float half_width = 0.0F;
    float half_height = 0.0F;
    scene::half_extents(camera, view, half_width, half_height);
    const bool occludes = camera.occlusion && fill_occlusion(camera, view, tweened, frustum);
    // What's in another material order doesn't meet depth, so nothing's in front of it.
    const auto meets_depth = [&s](std::uint32_t material) {
        return material == none || s.materials[material].order == 0;
    };
    std::vector<item_t> items;
    items.reserve(s.meshes.size() + s.sprites.size() + s.particles.size() + s.planes.size() +
                  s.terrains.size() + s.instances.size());
    const auto seen = [&](std::uint32_t entity, float& fade) {
        if (s.entities.world[entity] != world || !scene::showing(entity)) {
            return false;
        }
        fade = scene::entity_fade(entity, camera.entity);
        return fade > 0.0F;
    };
    for (const scene::mesh_t& mesh : s.meshes) {
        float fade = 1.0F;
        if (mesh.geometry == none || !seen(mesh.entity, fade) || mesh.tint[3] * fade <= 0.0F) {
            continue;
        }
        const mat4_t matrix = tweened.pose(mesh.entity).matrix;
        // Its cull box, if its geometry has one, else its bounds.
        const scene::geometry_t& geometry = s.geometries[mesh.geometry];
        vec3_t min = geometry.cull_min;
        vec3_t max = geometry.cull_max;
        if (!geometry.has_cull_box) {
            scene::geometry_bounds(mesh.geometry, min, max);
            anim::grow_by_targets(mesh.geometry, anim::morph_weights(mesh.entity, tweened.tween()),
                                  min, max);
        }
        std::array<vec3_t, 8> corners = corners_of(matrix, min, max);
        // Skinned, it's where its joints are: its bind pose's whole size around them.
        vec3_t joints_min;
        vec3_t joints_max;
        if (!geometry.has_cull_box &&
            anim::skin_joints_box(mesh.entity, tweened, joints_min, joints_max)) {
            const float reach = scene::length(max - min) * largest_axis(matrix);
            const vec3_t grown(reach, reach, reach);
            corners = corners_of(mat4_t(), joints_min - grown, joints_max + grown);
        }
        if (!scene::in_frustum(frustum, corners.data(), corners.size())) {
            continue;
        }
        const bool behind = occludes && occluded(corners.data(), corners.size());
        // What its size in the view has it draw, of its levels of detail.
        const std::uint32_t drawn = mesh_lod(mesh, matrix, tweened, camera, view);
        if (drawn == none) {
            continue;
        }
        const std::vector<scene::surface_t>& surfaces = s.geometries[drawn].surfaces;
        for (std::size_t surface = 0; surface < surfaces.size(); ++surface) {
            if (surfaces[surface].indices.empty()) {
                continue;
            }
            const std::uint32_t material =
                mesh.material != none ? mesh.material : surfaces[surface].material;
            if (behind && meets_depth(material)) {
                continue;
            }
            item_t item = item_of(mesh.entity, material, matrix, mesh.tint, fade, frustum);
            item.draw.geometry = s.geometry_slots.handle(drawn);
            item.draw.surface = static_cast<std::int32_t>(surface);
            items.push_back(item);
        }
    }
    const vec3_t eye_x = view.rotation * vec3_t(1.0F, 0.0F, 0.0F);
    const vec3_t eye_y = view.rotation * vec3_t(0.0F, 1.0F, 0.0F);
    const vec3_t eye_z = view.rotation * vec3_t(0.0F, 0.0F, 1.0F);
    for (const scene::sprite_t& sprite : s.sprites) {
        float fade = 1.0F;
        if (!seen(sprite.entity, fade) || sprite.tint[3] * fade <= 0.0F) {
            continue;
        }
        // Where it is between the steps, turned to the camera as it is then.
        const mat4_t own = tweened.pose(sprite.entity).matrix;
        vec3_t min;
        vec3_t max;
        scene::sprite_bounds(sprite.entity, own, min, max);
        if (!scene::box_in_frustum(frustum, min, max)) {
            continue;
        }
        if (occludes && meets_depth(sprite.material)) {
            const std::array<vec3_t, 8> corners = corners_of(mat4_t(), min, max);
            if (occluded(corners.data(), corners.size())) {
                continue;
            }
        }
        const mat4_t matrix = scene::sprite_matrix(sprite.entity, own, eye_x, eye_y, eye_z);
        items.push_back(
            item_of(sprite.entity, sprite.material, matrix, sprite.tint, fade, frustum));
    }
    for (const scene::particles_t& particles : s.particles) {
        float fade = 1.0F;
        if (!seen(particles.entity, fade)) {
            continue;
        }
        vec3_t min;
        vec3_t max;
        if (!scene::particles_bounds(particles, min, max) ||
            !scene::box_in_frustum(frustum, min, max)) {
            continue;
        }
        if (occludes && meets_depth(particles.material)) {
            const std::array<vec3_t, 8> corners = corners_of(mat4_t(), min, max);
            if (occluded(corners.data(), corners.size())) {
                continue;
            }
        }
        const std::array<float, 4> white{ 1.0F, 1.0F, 1.0F, 1.0F };
        item_t item = item_of(particles.entity, particles.material, mat4_t(), white, fade, frustum);
        // They're all but opaque if their material says so: they fade by their color.
        const scene::material_t standard;
        const scene::material_t& material =
            particles.material != none ? s.materials[particles.material] : standard;
        if (material.blend == FBE_BLEND_AUTO) {
            item.draw.blend = FBE_BLEND_ALPHA;
            item.blended = true;
        }
        const vec3_t place = tweened.pose(particles.entity).matrix.axis(3);
        item.draw.depth = scene::dot(place - frustum.eye, frustum.normals[0]);
        items.push_back(item);
    }
    for (scene::instances_t& found : scene::scene().instances) {
        float fade = 1.0F;
        vec3_t min;
        vec3_t max;
        if (found.geometry == none || !seen(found.entity, fade) ||
            !scene::instances_bounds(found, min, max)) {
            continue;
        }
        const mat4_t matrix = tweened.pose(found.entity).matrix;
        const std::array<vec3_t, 8> corners = corners_of(matrix, min, max);
        if (!scene::in_frustum(frustum, corners.data(), corners.size())) {
            continue;
        }
        const bool behind = occludes && occluded(corners.data(), corners.size());
        const std::array<float, 4> white{ 1.0F, 1.0F, 1.0F, 1.0F };
        const vec3_t both = min + max;
        const vec3_t middle = scene::transform_point(matrix, both * 0.5F);
        for (const scene::instance_level_t& range :
             scene::instances_levels(found, matrix, camera, frustum.eye, half_height)) {
            const std::uint32_t level = range.level;
            const std::vector<scene::surface_t>& surfaces = s.geometries[level].surfaces;
            for (std::size_t surface = 0; surface < surfaces.size(); ++surface) {
                if (surfaces[surface].indices.empty()) {
                    continue;
                }
                const std::uint32_t material =
                    found.material != none ? found.material : surfaces[surface].material;
                if (behind && meets_depth(material)) {
                    continue;
                }
                item_t item = item_of(found.entity, material, matrix, white, fade, frustum);
                // Its entity's matrix and theirs: what's mirrored twice is as it was.
                item.mirrored = item.mirrored != range.mirrored;
                item.instances_mirrored = range.mirrored;
                // Any of them with an alpha has them all blend, unless its material says.
                item.draw.blend = scene::drawn_blend(material, found.faded != 0 ? 0.5F : fade);
                item.blended = item.draw.blend != FBE_BLEND_OPAQUE;
                item.draw.geometry = s.geometry_slots.handle(level);
                item.draw.surface = static_cast<std::int32_t>(surface);
                item.draw.depth = scene::dot(middle - frustum.eye, frustum.normals[0]);
                items.push_back(item);
            }
        }
    }
    for (const scene::plane_t& plane : s.planes) {
        float fade = 1.0F;
        if (!seen(plane.entity, fade) || plane.tint[3] * fade <= 0.0F) {
            continue;
        }
        // It goes on to the horizon, so it's in view unless the whole view is under it.
        const mat4_t matrix = tweened.pose(plane.entity).matrix;
        const vec3_t up = scene::transform_normal(matrix, vec3_t(0.0F, 1.0F, 0.0F));
        if (!scene::plane_in_view(camera, view, matrix.axis(3), up)) {
            continue;
        }
        items.push_back(item_of(plane.entity, plane.material, matrix, plane.tint, fade, frustum));
    }
    for (const scene::terrain_t& terrain : s.terrains) {
        float fade = 1.0F;
        if (!seen(terrain.entity, fade) || terrain.tint[3] * fade <= 0.0F) {
            continue;
        }
        // Its grid's box; drawing makes its triangles from its heights.
        const mat4_t matrix = tweened.pose(terrain.entity).matrix;
        vec3_t min;
        vec3_t max;
        scene::terrain_bounds(terrain, min, max);
        const std::array<vec3_t, 8> corners = corners_of(matrix, min, max);
        if (!scene::in_frustum(frustum, corners.data(), corners.size()) ||
            (occludes && meets_depth(terrain.material) &&
             occluded(corners.data(), corners.size()))) {
            continue;
        }
        item_t item =
            item_of(terrain.entity, terrain.material, matrix, terrain.tint, fade, frustum);
        // How far its grid's middle is, since its place is a corner of it.
        const vec3_t across = max - min;
        const vec3_t middle = scene::transform_point(matrix, min + across * 0.5F);
        item.draw.depth = scene::dot(middle - frustum.eye, frustum.normals[0]);
        items.push_back(item);
    }
    // What's the same material, as a run of sprites is, is the same key but for these.
    struct keyed_t {
        std::uint32_t material = none;
        std::int32_t kind = -1;
        std::int32_t blend = -1;
        std::int32_t depth_mode = -1;
        bool mirrored = false;
        std::uint64_t key = 0;
    };
    keyed_t last;
    for (item_t& item : items) {
        // The scene's opaque test depth and write it, what blends only tests it, and other orders,
        // behind the scene or over it, do neither.
        if (item.order != 0) {
            item.draw.depth_mode = FBE_DEPTH_NONE;
        } else if (item.blended) {
            item.draw.depth_mode = FBE_DEPTH_TEST;
        } else {
            item.draw.depth_mode = FBE_DEPTH_TEST_AND_WRITE;
        }
        if (last.material != item.material || last.kind != item.draw.kind ||
            last.blend != item.draw.blend || last.depth_mode != item.draw.depth_mode ||
            last.mirrored != item.mirrored) {
            last = {
                .material = item.material,
                .kind = item.draw.kind,
                .blend = item.draw.blend,
                .depth_mode = item.draw.depth_mode,
                .mirrored = item.mirrored,
                .key = pipeline_of(item, camera),
            };
        }
        item.draw.pipeline = last.key;
    }
    // By order. In the scene's, opaque by pipeline, geometry, and nearest first, so a geometry's
    // draws go as instances whatever their materials, then blended furthest first; in others, which
    // don't meet depth, all furthest first, what's opaque first at the same depth, or it would
    // cover what blends over it. Ties as the entities were made, and a mesh's surfaces in order, so
    // a frame draws as the last did whatever's freed between.
    std::vector<sort_key_t> keys;
    keys.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const item_t& item = items[i];
        keys.push_back({
            .pipeline = item.draw.pipeline,
            .geometry = item.draw.geometry,
            .made = item.made,
            .order = item.order,
            .depth = item.draw.depth,
            .surface = item.draw.surface,
            .item = static_cast<std::uint32_t>(i),
            .blended = item.blended,
        });
    }
    std::ranges::sort(keys, [](const sort_key_t& a, const sort_key_t& b) {
        if (a.order != b.order) {
            return a.order < b.order;
        }
        const bool painted = a.order != 0;
        if (!painted && a.blended != b.blended) {
            return !a.blended;
        }
        const bool far_first = painted || a.blended;
        if (!far_first) {
            if (a.pipeline != b.pipeline) {
                return a.pipeline < b.pipeline;
            }
            if (a.geometry != b.geometry) {
                return a.geometry < b.geometry;
            }
            if (a.surface != b.surface) {
                return a.surface < b.surface;
            }
        }
        if (a.depth != b.depth) {
            return far_first ? a.depth > b.depth : a.depth < b.depth;
        }
        if (a.blended != b.blended) {
            return !a.blended;
        }
        if (a.made != b.made) {
            return a.made < b.made;
        }
        return a.surface < b.surface;
    });
    std::vector<item_t> sorted;
    sorted.reserve(items.size());
    for (const sort_key_t& key : keys) {
        sorted.push_back(items[key.item]);
    }
    return sorted;
}

bool drawn_view(std::uint64_t camera, float target_width, float target_height, float tween,
                std::uint32_t& slot, scene::view_t& view, bool& shows) {
    slot = scene::entity_slot(camera);
    if (slot == none) {
        return false;
    }
    const scene::scene_t& s = scene::scene();
    if (s.entities.kind[slot] != FBE_ENTITY_CAMERA) {
        return fail("the entity isn't a camera");
    }
    if (std::isnan(tween) || tween < 0.0F || tween > 1.0F) {
        return fail(std::format("a tween is 0 to 1, not {}", tween));
    }
    const camera_t& found = s.cameras[s.entities.component[slot]];
    shows = scene::showing(slot) && scene::view_of(found, target_width, target_height, view) &&
            view.sized;
    // Seen from where the camera is between the steps, as everything else is drawn.
    const scene::pose_t eye = scene::tweened_t(tween).pose(slot);
    view.position = eye.matrix.axis(3);
    view.rotation = eye.rotation;
    return true;
}

} // namespace fbe::render

extern "C" {

FBE_API int32_t fbe_render_draws(uint64_t camera, float target_width, float target_height,
                                 float tween, fbe_draw_t* draws, int32_t capacity) {
    fbe::render::lists_t lists;
    if (!fbe::render::read_lists(camera, target_width, target_height, tween, lists)) {
        return -1;
    }
    const auto count =
        std::min(lists.items.size(), static_cast<std::size_t>(std::max(capacity, 0)));
    for (std::size_t i = 0; i < count; ++i) {
        draws[i] = lists.items[i].draw;
    }
    return static_cast<std::int32_t>(lists.items.size());
}

} // extern "C"
