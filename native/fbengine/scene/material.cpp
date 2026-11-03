// Materials as data: color and alpha, shininess, shading, blending, texture layers, and the flags
// drawing reads; the surfaces and meshes that draw in them; and meshes' own tints.

#include "core/engine.hpp"
#include "fbengine.h"
#include "scene/scene.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

namespace fbe::scene {

namespace {

/// The material at a handle, or null, with fbe_error saying why.
material_t* material_of(std::uint64_t handle) {
    const std::uint32_t slot = material_slot(handle);
    return slot != none ? &scene().materials[slot] : nullptr;
}

/// The layer at `index` of a material, or null.
layer_t* layer_of(std::uint64_t material, std::int32_t index) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return nullptr;
    }
    if (index < 0 || index >= FBE_MATERIAL_LAYERS) {
        fail(
            std::format("a material's layers are 0 to {}, not {}", FBE_MATERIAL_LAYERS - 1, index));
        return nullptr;
    }
    return &found->layers[static_cast<std::size_t>(index)];
}

/// The material slot of a handle that can be 0 for none: true with it, or false for a freed one.
bool optional_material(std::uint64_t handle, std::uint32_t& slot) {
    slot = none;
    if (handle == 0) {
        return true;
    }
    slot = material_slot(handle);
    return slot != none;
}

fbe_color_t color_of(const std::array<float, 4>& rgba) {
    return { .r = rgba[0], .g = rgba[1], .b = rgba[2], .a = rgba[3] };
}

/// Whether a color's channels are finite, which can be any, and its alpha 0 to 1; false with
/// fbe_error saying why, naming `what` it's a color of.
bool valid_color(float r, float g, float b, float a, const char* what) {
    if (!(std::isfinite(r) && std::isfinite(g) && std::isfinite(b))) {
        return fail(std::format("{} color is finite", what));
    }
    if (std::isnan(a) || a < 0.0F || a > 1.0F) {
        return fail(std::format("{} alpha is 0 to 1, not {}", what, a));
    }
    return true;
}

} // namespace

std::uint32_t make_material() {
    scene_t& s = scene();
    const std::uint32_t slot = s.material_slots.make();
    if (slot == s.materials.size()) {
        s.materials.emplace_back();
    } else {
        s.materials[slot] = {};
    }
    s.materials[slot].references = 1;
    return slot;
}

std::uint32_t material_slot(std::uint64_t handle) {
    return live_slot(scene().material_slots, handle, "material");
}

void release_texture(std::uint64_t texture) {
    // Gone with the GPU, it has nothing to let go of.
    if (texture != 0 && fbe_texture_width(texture) >= 0) {
        fbe_texture_release(texture);
    }
}

void retain_material(std::uint32_t material) {
    if (material != none) {
        ++scene().materials[material].references;
    }
}

void release_material(std::uint32_t material) {
    if (material != none && --scene().materials[material].references == 0) {
        for (layer_t& layer : scene().materials[material].layers) {
            release_texture(layer.texture);
            layer.texture = 0;
        }
        scene().material_slots.free(material);
    }
}

void retain_materials(const std::vector<surface_t>& surfaces) {
    for (const surface_t& surface : surfaces) {
        retain_material(surface.material);
    }
}

void release_materials(const std::vector<surface_t>& surfaces) {
    for (const surface_t& surface : surfaces) {
        release_material(surface.material);
    }
}

std::int32_t drawn_blend(std::uint32_t material, float alpha) {
    const material_t drawn = material != none ? scene().materials[material] : material_t();
    switch (drawn.blend) {
    case FBE_BLEND_AUTO:
        if (drawn.color[3] * alpha < 1.0F) {
            return FBE_BLEND_ALPHA;
        }
        // A cutoff takes a texture's alpha as a mask instead.
        if (drawn.alpha_cutoff <= 0.0F) {
            for (const layer_t& layer : drawn.layers) {
                if (layer.texture != 0 && fbe_texture_alpha(layer.texture)) {
                    return FBE_BLEND_ALPHA;
                }
            }
        }
        return FBE_BLEND_OPAQUE;
    case FBE_BLEND_OPAQUE:
        // An opaque material's own alpha is left out, but what draws in it still fades.
        return alpha < 1.0F ? FBE_BLEND_ALPHA : FBE_BLEND_OPAQUE;
    default:
        return drawn.blend;
    }
}

} // namespace fbe::scene

using fbe::scene::layer_t;
using fbe::scene::material_of;
using fbe::scene::material_t;
using fbe::scene::none;
using fbe::scene::scene;

extern "C" {

FBE_API uint64_t fbe_material_create(void) {
    return scene().material_slots.handle(fbe::scene::make_material());
}

FBE_API bool fbe_material_retain(uint64_t material) {
    const std::uint32_t slot = fbe::scene::material_slot(material);
    if (slot == none) {
        return false;
    }
    fbe::scene::retain_material(slot);
    return true;
}

FBE_API bool fbe_material_release(uint64_t material) {
    const std::uint32_t slot = fbe::scene::material_slot(material);
    if (slot == none) {
        return false;
    }
    fbe::scene::release_material(slot);
    return true;
}

FBE_API bool fbe_material_alive(uint64_t material) {
    return scene().material_slots.find(material).has_value();
}

FBE_API uint64_t fbe_material_clone(uint64_t material) {
    const std::uint32_t slot = fbe::scene::material_slot(material);
    if (slot == none) {
        return 0;
    }
    const material_t original = scene().materials[slot];
    const std::uint32_t copy = fbe::scene::make_material();
    scene().materials[copy] = original;
    scene().materials[copy].references = 1;
    for (const fbe::scene::layer_t& layer : original.layers) {
        if (layer.texture != 0) {
            fbe_texture_retain(layer.texture);
        }
    }
    return scene().material_slots.handle(copy);
}

FBE_API bool fbe_material_set_color(uint64_t material, float r, float g, float b, float a) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    if (!fbe::scene::valid_color(r, g, b, a, "a material's")) {
        return false;
    }
    found->color = { r, g, b, a };
    return true;
}

FBE_API bool fbe_material_color(uint64_t material, fbe_color_t* color) {
    const material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    *color = fbe::scene::color_of(found->color);
    return true;
}

FBE_API bool fbe_material_set_shininess(uint64_t material, float shininess) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    if (std::isnan(shininess) || shininess < 0.0F || shininess > 1.0F) {
        return fbe::fail(std::format("a material's shininess is 0 to 1, not {}", shininess));
    }
    found->shininess = shininess;
    return true;
}

FBE_API float fbe_material_shininess(uint64_t material) {
    const material_t* found = material_of(material);
    return found != nullptr ? found->shininess : 0.0F;
}

FBE_API bool fbe_material_set_shading(uint64_t material, int32_t shading) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    if (shading < FBE_SHADING_PBR || shading > FBE_SHADING_VERTEX) {
        return fbe::fail(std::format("{} isn't a Shading", shading));
    }
    found->shading = shading;
    return true;
}

FBE_API int32_t fbe_material_shading(uint64_t material) {
    const material_t* found = material_of(material);
    return found != nullptr ? found->shading : -1;
}

FBE_API bool fbe_material_set_blend(uint64_t material, int32_t blend) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    if (blend < FBE_BLEND_AUTO || blend > FBE_BLEND_MULTIPLY) {
        return fbe::fail(std::format("{} isn't a Blend", blend));
    }
    found->blend = blend;
    return true;
}

FBE_API int32_t fbe_material_blend(uint64_t material) {
    const material_t* found = material_of(material);
    return found != nullptr ? found->blend : -1;
}

FBE_API bool fbe_material_set_alpha_cutoff(uint64_t material, float cutoff) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    if (std::isnan(cutoff) || cutoff < 0.0F || cutoff > 1.0F) {
        return fbe::fail(std::format("an alpha cutoff is 0 to 1, not {}", cutoff));
    }
    found->alpha_cutoff = cutoff;
    return true;
}

FBE_API float fbe_material_alpha_cutoff(uint64_t material) {
    const material_t* found = material_of(material);
    return found != nullptr ? found->alpha_cutoff : 0.0F;
}

FBE_API bool fbe_material_set_vertex_colors(uint64_t material, bool vertex_colors) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    found->vertex_colors = vertex_colors;
    return true;
}

FBE_API bool fbe_material_vertex_colors(uint64_t material) {
    const material_t* found = material_of(material);
    return found != nullptr && found->vertex_colors;
}

FBE_API bool fbe_material_set_two_sided(uint64_t material, bool two_sided) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    found->two_sided = two_sided;
    return true;
}

FBE_API bool fbe_material_two_sided(uint64_t material) {
    const material_t* found = material_of(material);
    return found != nullptr && found->two_sided;
}

FBE_API bool fbe_material_set_wireframe(uint64_t material, bool wireframe) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    found->wireframe = wireframe;
    return true;
}

FBE_API bool fbe_material_wireframe(uint64_t material) {
    const material_t* found = material_of(material);
    return found != nullptr && found->wireframe;
}

FBE_API bool fbe_material_set_fogged(uint64_t material, bool fogged) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    found->fogged = fogged;
    return true;
}

FBE_API bool fbe_material_fogged(uint64_t material) {
    const material_t* found = material_of(material);
    return found != nullptr && found->fogged;
}

FBE_API bool fbe_material_set_order(uint64_t material, int32_t order) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    found->order = order;
    return true;
}

FBE_API int32_t fbe_material_order(uint64_t material) {
    const material_t* found = material_of(material);
    return found != nullptr ? found->order : 0;
}

FBE_API bool fbe_material_set_texture(uint64_t material, int32_t layer, uint64_t texture) {
    layer_t* found = fbe::scene::layer_of(material, layer);
    if (found == nullptr) {
        return false;
    }
    if (texture != 0 && !fbe_texture_retain(texture)) {
        return false;
    }
    fbe::scene::release_texture(found->texture);
    found->texture = texture;
    return true;
}

FBE_API uint64_t fbe_material_texture(uint64_t material, int32_t layer) {
    const layer_t* found = fbe::scene::layer_of(material, layer);
    return found != nullptr ? found->texture : 0;
}

FBE_API bool fbe_material_set_layer_uv(uint64_t material, int32_t layer, float scale_u,
                                       float scale_v, float angle, float offset_u, float offset_v) {
    layer_t* found = fbe::scene::layer_of(material, layer);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(scale_u) || !std::isfinite(scale_v) || scale_u == 0.0F || scale_v == 0.0F) {
        return fbe::fail(
            std::format("a layer's UV scale is finite and not 0, not ({}, {})", scale_u, scale_v));
    }
    if (!std::isfinite(angle) || !std::isfinite(offset_u) || !std::isfinite(offset_v)) {
        return fbe::fail(std::format("a layer's UV angle and offset are finite, not {}, ({}, {})",
                                     angle, offset_u, offset_v));
    }
    found->uv_scale = { scale_u, scale_v };
    found->uv_angle = angle;
    found->uv_offset = { offset_u, offset_v };
    return true;
}

FBE_API bool fbe_material_layer_uv(uint64_t material, int32_t layer, float* values) {
    const layer_t* found = fbe::scene::layer_of(material, layer);
    if (found == nullptr) {
        return false;
    }
    values[0] = found->uv_scale[0];
    values[1] = found->uv_scale[1];
    values[2] = found->uv_angle;
    values[3] = found->uv_offset[0];
    values[4] = found->uv_offset[1];
    return true;
}

FBE_API bool fbe_material_set_layer_bump(uint64_t material, int32_t layer, float m00, float m01,
                                         float m10, float m11, float scale, float offset) {
    layer_t* found = fbe::scene::layer_of(material, layer);
    if (found == nullptr) {
        return false;
    }
    if (!std::isfinite(m00) || !std::isfinite(m01) || !std::isfinite(m10) || !std::isfinite(m11) ||
        !std::isfinite(scale) || !std::isfinite(offset)) {
        return fbe::fail("a bump layer's matrix, scale, and offset are finite");
    }
    found->bump = { m00, m01, m10, m11 };
    found->bump_scale = scale;
    found->bump_offset = offset;
    return true;
}

FBE_API bool fbe_material_layer_bump(uint64_t material, int32_t layer, float* values) {
    const layer_t* found = fbe::scene::layer_of(material, layer);
    if (found == nullptr) {
        return false;
    }
    values[0] = found->bump[0];
    values[1] = found->bump[1];
    values[2] = found->bump[2];
    values[3] = found->bump[3];
    values[4] = found->bump_scale;
    values[5] = found->bump_offset;
    return true;
}

FBE_API bool fbe_material_set_layer(uint64_t material, int32_t layer, int32_t blend,
                                    int32_t uv_set) {
    layer_t* found = fbe::scene::layer_of(material, layer);
    if (found == nullptr) {
        return false;
    }
    if (blend < FBE_LAYER_MULTIPLY || blend > FBE_LAYER_BUMP) {
        return fbe::fail(std::format("{} isn't a LayerBlend", blend));
    }
    if (uv_set != 0 && uv_set != 1) {
        return fbe::fail(std::format("a layer's UV set is 0 or 1, not {}", uv_set));
    }
    found->blend = blend;
    found->uv_set = uv_set;
    return true;
}

FBE_API int32_t fbe_material_layer_blend(uint64_t material, int32_t layer) {
    const layer_t* found = fbe::scene::layer_of(material, layer);
    return found != nullptr ? found->blend : -1;
}

FBE_API int32_t fbe_material_layer_uv_set(uint64_t material, int32_t layer) {
    const layer_t* found = fbe::scene::layer_of(material, layer);
    return found != nullptr ? found->uv_set : -1;
}

FBE_API bool fbe_surface_set_material(uint64_t geometry, int32_t surface, uint64_t material) {
    const std::uint32_t geometry_slot = fbe::scene::geometry_slot(geometry);
    std::uint32_t slot = none;
    if (geometry_slot == none || !fbe::scene::optional_material(material, slot)) {
        return false;
    }
    std::vector<fbe::scene::surface_t>& surfaces = scene().geometries[geometry_slot].surfaces;
    if (surface < 0 || static_cast<std::size_t>(surface) >= surfaces.size()) {
        return fbe::fail(std::format("the geometry's surfaces are 0 to {}, not {}",
                                     static_cast<std::int64_t>(surfaces.size()) - 1, surface));
    }
    fbe::scene::surface_t& found = surfaces[static_cast<std::size_t>(surface)];
    fbe::scene::retain_material(slot);
    fbe::scene::release_material(found.material);
    found.material = slot;
    return true;
}

FBE_API uint64_t fbe_surface_material(uint64_t geometry, int32_t surface) {
    const std::uint32_t geometry_slot = fbe::scene::geometry_slot(geometry);
    if (geometry_slot == none) {
        return 0;
    }
    const std::vector<fbe::scene::surface_t>& surfaces = scene().geometries[geometry_slot].surfaces;
    if (surface < 0 || static_cast<std::size_t>(surface) >= surfaces.size()) {
        fbe::fail(std::format("the geometry's surfaces are 0 to {}, not {}",
                              static_cast<std::int64_t>(surfaces.size()) - 1, surface));
        return 0;
    }
    const std::uint32_t material = surfaces[static_cast<std::size_t>(surface)].material;
    return material != none ? scene().material_slots.handle(material) : 0;
}

FBE_API bool fbe_mesh_set_material(uint64_t mesh, uint64_t material) {
    fbe::scene::mesh_t* found = fbe::scene::mesh_of(mesh);
    std::uint32_t slot = none;
    if (found == nullptr || !fbe::scene::optional_material(material, slot)) {
        return false;
    }
    fbe::scene::retain_material(slot);
    fbe::scene::release_material(found->material);
    found->material = slot;
    return true;
}

FBE_API uint64_t fbe_mesh_material(uint64_t mesh) {
    const fbe::scene::mesh_t* found = fbe::scene::mesh_of(mesh);
    if (found == nullptr || found->material == none) {
        return 0;
    }
    return scene().material_slots.handle(found->material);
}

FBE_API bool fbe_mesh_set_tint(uint64_t mesh, float r, float g, float b, float a) {
    fbe::scene::mesh_t* found = fbe::scene::mesh_of(mesh);
    if (found == nullptr) {
        return false;
    }
    if (!fbe::scene::valid_color(r, g, b, a, "a mesh's")) {
        return false;
    }
    found->tint = { r, g, b, a };
    return true;
}

FBE_API bool fbe_mesh_tint(uint64_t mesh, fbe_color_t* tint) {
    const fbe::scene::mesh_t* found = fbe::scene::mesh_of(mesh);
    if (found == nullptr) {
        return false;
    }
    *tint = fbe::scene::color_of(found->tint);
    return true;
}

FBE_API bool fbe_terrain_set_material(uint64_t terrain, uint64_t material) {
    fbe::scene::terrain_t* found = fbe::scene::terrain_of(terrain);
    std::uint32_t slot = none;
    if (found == nullptr || !fbe::scene::optional_material(material, slot)) {
        return false;
    }
    fbe::scene::retain_material(slot);
    fbe::scene::release_material(found->material);
    found->material = slot;
    return true;
}

FBE_API uint64_t fbe_terrain_material(uint64_t terrain) {
    const fbe::scene::terrain_t* found = fbe::scene::terrain_of(terrain);
    if (found == nullptr || found->material == none) {
        return 0;
    }
    return scene().material_slots.handle(found->material);
}

FBE_API bool fbe_terrain_set_tint(uint64_t terrain, float r, float g, float b, float a) {
    fbe::scene::terrain_t* found = fbe::scene::terrain_of(terrain);
    if (found == nullptr || !fbe::scene::valid_color(r, g, b, a, "a terrain's")) {
        return false;
    }
    found->tint = { r, g, b, a };
    return true;
}

FBE_API bool fbe_terrain_tint(uint64_t terrain, fbe_color_t* tint) {
    const fbe::scene::terrain_t* found = fbe::scene::terrain_of(terrain);
    if (found == nullptr) {
        return false;
    }
    *tint = fbe::scene::color_of(found->tint);
    return true;
}

FBE_API bool fbe_plane_set_material(uint64_t plane, uint64_t material) {
    fbe::scene::plane_t* found = fbe::scene::plane_of(plane);
    std::uint32_t slot = none;
    if (found == nullptr || !fbe::scene::optional_material(material, slot)) {
        return false;
    }
    fbe::scene::retain_material(slot);
    fbe::scene::release_material(found->material);
    found->material = slot;
    return true;
}

FBE_API uint64_t fbe_plane_material(uint64_t plane) {
    const fbe::scene::plane_t* found = fbe::scene::plane_of(plane);
    if (found == nullptr || found->material == none) {
        return 0;
    }
    return scene().material_slots.handle(found->material);
}

FBE_API bool fbe_plane_set_tint(uint64_t plane, float r, float g, float b, float a) {
    fbe::scene::plane_t* found = fbe::scene::plane_of(plane);
    if (found == nullptr || !fbe::scene::valid_color(r, g, b, a, "a ground plane's")) {
        return false;
    }
    found->tint = { r, g, b, a };
    return true;
}

FBE_API bool fbe_plane_tint(uint64_t plane, fbe_color_t* tint) {
    const fbe::scene::plane_t* found = fbe::scene::plane_of(plane);
    if (found == nullptr) {
        return false;
    }
    *tint = fbe::scene::color_of(found->tint);
    return true;
}

FBE_API bool fbe_sprite_set_material(uint64_t sprite, uint64_t material) {
    fbe::scene::sprite_t* found = fbe::scene::sprite_of(sprite);
    std::uint32_t slot = none;
    if (found == nullptr || !fbe::scene::optional_material(material, slot)) {
        return false;
    }
    fbe::scene::retain_material(slot);
    fbe::scene::release_material(found->material);
    found->material = slot;
    return true;
}

FBE_API uint64_t fbe_sprite_material(uint64_t sprite) {
    const fbe::scene::sprite_t* found = fbe::scene::sprite_of(sprite);
    if (found == nullptr || found->material == none) {
        return 0;
    }
    return scene().material_slots.handle(found->material);
}

FBE_API bool fbe_sprite_set_tint(uint64_t sprite, float r, float g, float b, float a) {
    fbe::scene::sprite_t* found = fbe::scene::sprite_of(sprite);
    if (found == nullptr || !fbe::scene::valid_color(r, g, b, a, "a sprite's")) {
        return false;
    }
    found->tint = { r, g, b, a };
    return true;
}

FBE_API bool fbe_sprite_tint(uint64_t sprite, fbe_color_t* tint) {
    const fbe::scene::sprite_t* found = fbe::scene::sprite_of(sprite);
    if (found == nullptr) {
        return false;
    }
    *tint = fbe::scene::color_of(found->tint);
    return true;
}

FBE_API int32_t fbe_mesh_blend(uint64_t mesh, int32_t surface) {
    const fbe::scene::mesh_t* found = fbe::scene::mesh_of(mesh);
    if (found == nullptr) {
        return -1;
    }
    // A surface it has, whichever material draws it.
    if (found->geometry == none) {
        fbe::fail("the mesh draws no geometry");
        return -1;
    }
    const std::vector<fbe::scene::surface_t>& surfaces =
        scene().geometries[found->geometry].surfaces;
    if (surface < 0 || static_cast<std::size_t>(surface) >= surfaces.size()) {
        fbe::fail(std::format("the geometry's surfaces are 0 to {}, not {}",
                              static_cast<std::int64_t>(surfaces.size()) - 1, surface));
        return -1;
    }
    std::uint32_t material = found->material;
    if (material == none) {
        material = surfaces[static_cast<std::size_t>(surface)].material;
    }
    return fbe::scene::drawn_blend(material, found->tint[3]);
}

} // extern "C"
