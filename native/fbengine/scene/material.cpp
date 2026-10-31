// Materials as data: color and alpha, shininess, shading, blending, texture layers, and the flags
// E5 draws them by; the surfaces and meshes that draw in them; and meshes' own tints.

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

bool finite(float r, float g, float b, float a) {
    return std::isfinite(r) && std::isfinite(g) && std::isfinite(b) && std::isfinite(a);
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
    const std::optional<std::uint32_t> slot = scene().material_slots.find(handle);
    if (!slot) {
        fail(handle == 0 ? "no material was given" : "the material was freed");
        return none;
    }
    return *slot;
}

void retain_material(std::uint32_t material) {
    if (material != none) {
        ++scene().materials[material].references;
    }
}

void release_material(std::uint32_t material) {
    if (material != none && --scene().materials[material].references == 0) {
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
    return scene().material_slots.handle(copy);
}

FBE_API bool fbe_material_set_color(uint64_t material, float r, float g, float b, float a) {
    material_t* found = material_of(material);
    if (found == nullptr) {
        return false;
    }
    if (!fbe::scene::finite(r, g, b, a)) {
        return fbe::fail("a material's color is finite");
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

FBE_API bool fbe_material_set_layer(uint64_t material, int32_t layer, int32_t blend,
                                    int32_t uv_set) {
    layer_t* found = fbe::scene::layer_of(material, layer);
    if (found == nullptr) {
        return false;
    }
    if (blend < FBE_LAYER_MULTIPLY || blend > FBE_LAYER_MULTIPLY_2) {
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
    if (!fbe::scene::finite(r, g, b, a)) {
        return fbe::fail("a mesh's tint is finite");
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

} // extern "C"
