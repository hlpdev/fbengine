// Models' C API: a glTF's meshes, materials, and images made into the engine's geometry, materials,
// and textures, through their areas' C APIs, and a scene's nodes made into entities.

#include "assets/model.hpp"

#include "assets/loads.hpp"
#include "core/engine.hpp"
#include "fbengine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace assets = fbe::assets;

/// Whether the last call failed for what's in its file.
bool invalid = false;

/// The model of a load done and loaded; nullptr, with fbe_error's why, if it isn't one.
const assets::model_t* model_of(std::uint64_t load) {
    invalid = false;
    const assets::load_t* found = assets::loads().get(load);
    if (found == nullptr || !found->done || found->error ||
        found->kind != assets::load_kind_t::MODEL) {
        fbe::fail("the load isn't a model load done and loaded");
        return nullptr;
    }
    return &found->model;
}

/// False, with fbe_error's why, said to be its file's.
bool refused(const std::string& why) {
    invalid = true;
    return fbe::fail(why);
}

/// Sets what a file's material says on a new one; false, with fbe_error's why, for what one can't
/// have.
bool set_material(std::uint64_t material, const assets::material_t& from) {
    const auto& color = from.base_color;
    const bool is_mask = from.alpha == assets::alpha_t::MASK;
    std::int32_t blend = FBE_BLEND_OPAQUE;
    if (from.alpha == assets::alpha_t::BLEND) {
        blend = FBE_BLEND_ALPHA;
    } else if (is_mask) {
        blend = FBE_BLEND_AUTO;
    }
    return fbe_material_set_color(material, color[0], color[1], color[2], color[3]) &&
           fbe_material_set_shininess(material, 1.0F - from.roughness) &&
           fbe_material_set_shading(material, from.unlit ? FBE_SHADING_UNLIT : FBE_SHADING_PBR) &&
           fbe_material_set_blend(material, blend) &&
           fbe_material_set_alpha_cutoff(material, is_mask ? from.alpha_cutoff : 0.0F) &&
           fbe_material_set_two_sided(material, from.double_sided);
}

/// Adds a primitive to a geometry as a surface, empty for lines and points, whose vertices are
/// what a surface's are, with the white of a file that gives no color; false, with fbe_error's
/// why, if it can't be.
bool add_primitive(std::uint64_t geometry, const assets::primitive_t& from,
                   std::span<const std::uint64_t> materials) {
    const std::int32_t surface = fbe_geometry_add_surface(geometry);
    if (surface < 0) {
        return false;
    }
    if (from.material >= 0) {
        if (std::cmp_greater_equal(from.material, materials.size())) {
            return refused(std::format("a primitive's material {} isn't there", from.material));
        }
        const std::uint64_t material = materials[static_cast<std::size_t>(from.material)];
        if (!fbe_surface_set_material(geometry, surface, material)) {
            return false;
        }
        if (!from.colors.empty() && !fbe_material_set_vertex_colors(material, true)) {
            return false;
        }
    }
    if (from.topology != assets::topology_t::TRIANGLES || from.positions.empty()) {
        return true;
    }
    constexpr auto most = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
    if (from.positions.size() > most || from.indices.size() > most) {
        return refused("a primitive has more vertices or indices than a surface holds");
    }
    std::vector<fbe_vertex_t> vertices(from.positions.size());
    for (std::size_t at = 0; at < vertices.size(); ++at) {
        fbe_vertex_t& to = vertices[at];
        const auto& place = from.positions[at];
        to.position = { .x = place[0], .y = place[1], .z = place[2] };
        if (at < from.normals.size()) {
            const auto& normal = from.normals[at];
            to.normal = { .x = normal[0], .y = normal[1], .z = normal[2] };
        }
        if (at < from.uv0.size()) {
            to.uv0 = { .x = from.uv0[at][0], .y = from.uv0[at][1] };
        }
        if (at < from.uv1.size()) {
            to.uv1 = { .x = from.uv1[at][0], .y = from.uv1[at][1] };
        }
        to.color = { .r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F };
        if (at < from.colors.size()) {
            const auto& color = from.colors[at];
            to.color = { .r = color[0], .g = color[1], .b = color[2], .a = color[3] };
        }
    }
    if (fbe_surface_add_vertices(geometry, surface, vertices.data(),
                                 static_cast<std::int32_t>(vertices.size())) < 0) {
        return false;
    }
    // Decoding gives every primitive its indices, checked against its vertices.
    if (!from.indices.empty() &&
        fbe_surface_add_triangles(geometry, surface, from.indices.data(),
                                  static_cast<std::int32_t>(from.indices.size())) < 0) {
        return false;
    }
    // A target's positions, zeros for one of normals alone, as a surface's targets are by their
    // indices.
    for (const assets::morph_target_t& target : from.targets) {
        if ((!target.positions.empty() && target.positions.size() != vertices.size()) ||
            (!target.normals.empty() && target.normals.size() != vertices.size())) {
            return refused("a morph target has some of its primitive's vertices");
        }
        const std::vector<assets::vec3_t> nothing(target.positions.empty() ? vertices.size() : 0,
                                                  assets::vec3_t{ 0, 0, 0 });
        const std::vector<assets::vec3_t>& moved =
            target.positions.empty() ? nothing : target.positions;
        if (fbe_morph_add_target(geometry, surface, moved.front().data(),
                                 target.normals.empty() ? nullptr : target.normals.front().data(),
                                 static_cast<std::int32_t>(vertices.size())) < 0) {
            return false;
        }
    }
    if (from.joints.empty()) {
        return true;
    }
    if (from.joints.size() != vertices.size() || from.weights.size() != vertices.size()) {
        return refused("a primitive has joints and weights for some of its vertices");
    }
    std::vector<fbe_vertex_joints_t> joints(vertices.size());
    for (std::size_t at = 0; at < joints.size(); ++at) {
        std::ranges::copy(from.joints[at], joints[at].joints);
        std::ranges::copy(from.weights[at], joints[at].weights);
    }
    return fbe_surface_set_joints(geometry, surface, joints.data(),
                                  static_cast<std::int32_t>(joints.size()));
}

/// The texture reference of a material's that reads for an FBE_MODEL_ map; nullptr for no map.
const assets::texture_ref_t* map_of(const assets::material_t& material, std::int32_t map) {
    switch (map) {
    case FBE_MODEL_BASE_COLOR:
        return &material.base_color_texture;
    case FBE_MODEL_METALLIC_ROUGHNESS:
        return &material.metallic_roughness_texture;
    case FBE_MODEL_NORMAL:
        return &material.normal_texture;
    case FBE_MODEL_OCCLUSION:
        return &material.occlusion_texture;
    case FBE_MODEL_EMISSIVE:
        return &material.emissive_texture;
    default:
        return nullptr;
    }
}

/// The FBE_TEXTURE_ wrap of glTF's number.
std::int32_t wrap_of(std::int32_t wrap) {
    switch (wrap) {
    case 33071:
        return FBE_TEXTURE_CLAMP;
    case 33648:
        return FBE_TEXTURE_MIRROR;
    default:
        return FBE_TEXTURE_REPEAT;
    }
}

/// How a texture's sampled: its FBE_TEXTURE_ filter and wraps.
struct sampling_t {
    std::int32_t filter = FBE_TEXTURE_LINEAR;
    std::int32_t wrap_u = FBE_TEXTURE_REPEAT;
    std::int32_t wrap_v = FBE_TEXTURE_REPEAT;

    bool operator==(const sampling_t&) const = default;
};

/// How a file says a material's texture is sampled.
sampling_t sampling_of(const assets::model_t& model, const assets::texture_ref_t& read) {
    if (read.sampler < 0 || std::cmp_greater_equal(read.sampler, model.samplers.size())) {
        return {};
    }
    const assets::sampler_t& sampler = model.samplers[static_cast<std::size_t>(read.sampler)];
    // glTF's NEAREST, NEAREST_MIPMAP_NEAREST, and NEAREST_MIPMAP_LINEAR read the nearest pixel.
    const bool nearest =
        sampler.mag == 9728 || sampler.min == 9728 || sampler.min == 9984 || sampler.min == 9986;
    return {
        .filter = nearest ? FBE_TEXTURE_NEAREST : FBE_TEXTURE_LINEAR,
        .wrap_u = wrap_of(sampler.wrap_u),
        .wrap_v = wrap_of(sampler.wrap_v),
    };
}

/// Filters and wraps a texture as `sampling` says.
void set_sampling(std::uint64_t texture, const sampling_t& sampling) {
    fbe_texture_set_filter(texture, sampling.filter);
    fbe_texture_set_wrap(texture, sampling.wrap_u, sampling.wrap_v);
}

/// Filters and wraps a texture of an image as the first of the model's materials to read it says,
/// and leaves it as it was made if none does.
void set_sampler(const assets::model_t& model, std::uint64_t texture, std::int32_t image) {
    for (const assets::material_t& material : model.materials) {
        for (std::int32_t map = FBE_MODEL_BASE_COLOR; map <= FBE_MODEL_EMISSIVE; ++map) {
            const assets::texture_ref_t& used = *map_of(material, map);
            if (used.image == image) {
                set_sampling(texture, sampling_of(model, used));
                return;
            }
        }
    }
}

/// Makes a file's camera what its entity is; false, with fbe_error's why, for what one can't be.
bool set_camera(std::uint64_t entity, const assets::camera_t& from) {
    const bool shaped =
        from.orthographic
            ? fbe_camera_set_projection(entity, FBE_PROJECTION_ORTHOGRAPHIC) &&
                  fbe_camera_set_orthographic_height(entity, from.magnification[1] * 2.0F)
            : fbe_camera_set_projection(entity, FBE_PROJECTION_PERSPECTIVE) &&
                  fbe_camera_set_field_of_view(entity, from.fov);
    if (!shaped) {
        return refused(std::format("the camera can't be as its file says: {}", fbe_error()));
    }
    // One with no end sees as far as a new one does, or a long way past its nearest.
    const float nearest = from.z_near > 0.0F ? from.z_near : fbe_camera_nearest(entity);
    const float farthest =
        from.z_far > 0.0F ? from.z_far : std::max(fbe_camera_farthest(entity), nearest * 1000.0F);
    return fbe_camera_set_range(entity, nearest, farthest) ||
           refused(std::format("the camera can't be as its file says: {}", fbe_error()));
}

/// Makes a file's light what its entity is; false, with fbe_error's why, for what one can't be.
bool set_light(std::uint64_t entity, const assets::light_t& from) {
    std::int32_t type = FBE_LIGHT_POINT;
    if (from.kind == assets::light_kind_t::DIRECTIONAL) {
        type = FBE_LIGHT_DIRECTIONAL;
    } else if (from.kind == assets::light_kind_t::SPOT) {
        type = FBE_LIGHT_SPOT;
    }
    // One with no end reaches as far as a range can say.
    const float range = from.range > 0.0F ? from.range : std::numeric_limits<float>::max();
    return (fbe_light_set_type(entity, type) &&
            fbe_light_set_color(entity, from.color[0], from.color[1], from.color[2]) &&
            fbe_light_set_intensity(entity, from.intensity) && fbe_light_set_range(entity, range) &&
            (from.kind != assets::light_kind_t::SPOT ||
             fbe_light_set_cone(entity, from.inner_cone, from.outer_cone))) ||
           refused(std::format("the light can't be as its file says: {}", fbe_error()));
}

/// Names and places an entity as its node is; false, with fbe_error's why, for what one can't be.
bool set_node(std::uint64_t entity, const assets::node_t& node) {
    const auto& at = node.translation;
    const auto& turn = node.rotation;
    const auto& scale = node.scale;
    return fbe_entity_set_name(entity, node.name.c_str()) &&
           ((fbe_entity_set_position(entity, at[0], at[1], at[2], FBE_SPACE_LOCAL) &&
             fbe_entity_set_rotation(entity, turn[0], turn[1], turn[2], turn[3], FBE_SPACE_LOCAL) &&
             fbe_entity_set_scale(entity, scale[0], scale[1], scale[2], FBE_SPACE_LOCAL)) ||
            refused(std::format("the node \"{}\" can't be placed as its file says: {}", node.name,
                                fbe_error())));
}

/// What's made of a scene's nodes.
struct spawn_t {
    const assets::model_t& model;
    std::uint64_t world;
    std::uint64_t root;
    std::span<const std::uint64_t> geometries;
    std::span<const std::uint64_t> skins;
};

/// Makes an entity of a node under `parent`, and gives it, or 0, with fbe_error's why, if it can't
/// be: what's skinned finds its joints only once they're all made, so that's left for after.
std::uint64_t make_node(const spawn_t& spawn, const assets::node_t& node, std::uint64_t parent) {
    const assets::model_t& model = spawn.model;
    const bool mesh = node.mesh >= 0;
    const bool camera = node.camera >= 0;
    const bool light = node.light >= 0;
    if ((mesh && std::cmp_greater_equal(node.mesh, spawn.geometries.size())) ||
        (camera && std::cmp_greater_equal(node.camera, model.cameras.size())) ||
        (light && std::cmp_greater_equal(node.light, model.lights.size()))) {
        refused(std::format("the node \"{}\" has a mesh, a camera, or a light that isn't there",
                            node.name));
        return 0;
    }
    std::int32_t kind = FBE_ENTITY_PIVOT;
    if (mesh) {
        kind = FBE_ENTITY_MESH;
    } else if (camera) {
        kind = FBE_ENTITY_CAMERA;
    } else if (light) {
        kind = FBE_ENTITY_LIGHT;
    }
    const std::uint64_t entity = fbe_entity_create(spawn.world, parent, kind);
    if (entity == 0 || !set_node(entity, node)) {
        return 0;
    }
    if (mesh &&
        !fbe_mesh_set_geometry(entity, spawn.geometries[static_cast<std::size_t>(node.mesh)])) {
        return 0;
    }
    if (mesh) {
        // The node's own weights, or its mesh's.
        const std::vector<float>& weights =
            node.weights.empty() ? model.meshes[static_cast<std::size_t>(node.mesh)].weights
                                 : node.weights;
        if (!weights.empty() && !fbe_morph_set_weights(entity, weights.data(),
                                                       static_cast<std::int32_t>(weights.size()))) {
            return 0;
        }
    }
    // What's more on a node is an entity under it, of its name.
    std::uint64_t as_camera = kind == FBE_ENTITY_CAMERA ? entity : 0;
    std::uint64_t as_light = kind == FBE_ENTITY_LIGHT ? entity : 0;
    if (camera && as_camera == 0) {
        as_camera = fbe_entity_create(spawn.world, entity, FBE_ENTITY_CAMERA);
        if (as_camera == 0 || !fbe_entity_set_name(as_camera, node.name.c_str())) {
            return 0;
        }
    }
    if (light && as_light == 0) {
        as_light = fbe_entity_create(spawn.world, entity, FBE_ENTITY_LIGHT);
        if (as_light == 0 || !fbe_entity_set_name(as_light, node.name.c_str())) {
            return 0;
        }
    }
    // A file's camera doesn't draw over the game's until the game shows it.
    if (camera && !fbe_entity_set_hidden(as_camera, true)) {
        return 0;
    }
    if ((camera && !set_camera(as_camera, model.cameras[static_cast<std::size_t>(node.camera)])) ||
        (light && !set_light(as_light, model.lights[static_cast<std::size_t>(node.light)]))) {
        return 0;
    }
    return entity;
}

/// The nodes a scene shows at its top; for a file of no scenes, those in no other node.
std::vector<std::uint32_t> top_nodes(const assets::model_t& model, std::int32_t scene) {
    if (scene >= 0) {
        return model.scenes[static_cast<std::size_t>(scene)].nodes;
    }
    std::vector<bool> inside(model.nodes.size(), false);
    for (const assets::node_t& node : model.nodes) {
        for (const std::uint32_t child : node.children) {
            if (child < inside.size()) {
                inside[child] = true;
            }
        }
    }
    std::vector<std::uint32_t> top;
    for (std::uint32_t at = 0; at < inside.size(); ++at) {
        if (!inside[at]) {
            top.push_back(at);
        }
    }
    return top;
}

/// Makes a scene's nodes under its root, depth first with a list of what's left, however deep a
/// file's nodes go; and skins what's skinned, once its joints are there. A node in two places, as
/// one in itself is, is refused, as a tree has it once.
bool spawn_nodes(const spawn_t& spawn, const std::vector<std::uint32_t>& top) {
    const assets::model_t& model = spawn.model;
    std::vector<bool> made(model.nodes.size(), false);
    std::vector<std::pair<std::uint32_t, std::uint64_t>> left;
    std::vector<std::pair<std::uint64_t, std::int32_t>> skinned;
    for (auto at = top.rbegin(); at != top.rend(); ++at) {
        left.emplace_back(*at, spawn.root);
    }
    while (!left.empty()) {
        const auto [index, parent] = left.back();
        left.pop_back();
        if (index >= model.nodes.size()) {
            return refused(std::format("the node {} isn't there", index));
        }
        if (made[index]) {
            return refused(std::format("the node {} is in two places, or in itself", index));
        }
        made[index] = true;
        const assets::node_t& node = model.nodes[index];
        const std::uint64_t entity = make_node(spawn, node, parent);
        if (entity == 0) {
            return false;
        }
        if (node.mesh >= 0 && node.skin >= 0) {
            skinned.emplace_back(entity, node.skin);
        }
        for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
            left.emplace_back(*child, entity);
        }
    }
    for (const auto& [mesh, skin] : skinned) {
        if (std::cmp_greater_equal(skin, spawn.skins.size())) {
            return refused(std::format("a node's skin {} isn't there", skin));
        }
        if (!fbe_skin_attach(mesh, spawn.skins[static_cast<std::size_t>(skin)], spawn.root)) {
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" {

FBE_API bool fbe_model_invalid(void) {
    return invalid;
}

FBE_API uint64_t fbe_model_material(uint64_t load, int32_t index) {
    const assets::model_t* model = model_of(load);
    if (model == nullptr) {
        return 0;
    }
    if (index < 0 || std::cmp_greater_equal(index, model->materials.size())) {
        fbe::fail(std::format("the model has {} materials; there's no material {}",
                              model->materials.size(), index));
        return 0;
    }
    const std::uint64_t material = fbe_material_create();
    if (material == 0) {
        return 0;
    }
    if (!set_material(material, model->materials[static_cast<std::size_t>(index)])) {
        refused(std::format("the material {} can't be as its file says: {}", index, fbe_error()));
        fbe_material_release(material);
        return 0;
    }
    return material;
}

FBE_API uint64_t fbe_model_geometry(uint64_t load, int32_t index, const uint64_t* materials,
                                    int32_t count) {
    const assets::model_t* model = model_of(load);
    if (model == nullptr) {
        return 0;
    }
    if (index < 0 || std::cmp_greater_equal(index, model->meshes.size())) {
        fbe::fail(std::format("the model has {} meshes; there's no mesh {}", model->meshes.size(),
                              index));
        return 0;
    }
    if (count < 0 || (count > 0 && materials == nullptr)) {
        fbe::fail("no materials were given");
        return 0;
    }
    const std::uint64_t geometry = fbe_geometry_create();
    if (geometry == 0) {
        return 0;
    }
    const std::span<const std::uint64_t> given(materials, static_cast<std::size_t>(count));
    for (const assets::primitive_t& primitive :
         model->meshes[static_cast<std::size_t>(index)].primitives) {
        if (!add_primitive(geometry, primitive, given)) {
            const std::string why = fbe_error();
            fbe_geometry_release(geometry);
            fbe::fail(why);
            return 0;
        }
    }
    return geometry;
}

FBE_API uint64_t fbe_model_texture(uint64_t load, int32_t index, bool mipmaps) {
    const assets::model_t* model = model_of(load);
    if (model == nullptr) {
        return 0;
    }
    const std::uint64_t texture = fbe_texture_from_model(load, index, mipmaps);
    if (texture == 0) {
        invalid = fbe_texture_load_invalid();
        return 0;
    }
    set_sampler(*model, texture, index);
    return texture;
}

FBE_API int32_t fbe_model_material_image(uint64_t load, int32_t material, int32_t map) {
    const assets::model_t* model = model_of(load);
    if (model == nullptr || material < 0 ||
        std::cmp_greater_equal(material, model->materials.size())) {
        return -1;
    }
    const assets::texture_ref_t* read =
        map_of(model->materials[static_cast<std::size_t>(material)], map);
    return read != nullptr ? read->image : -1;
}

FBE_API int32_t fbe_model_image_levels(uint64_t load, int32_t index) {
    const assets::model_t* model = model_of(load);
    if (model == nullptr || index < 0 || std::cmp_greater_equal(index, model->images.size())) {
        return -1;
    }
    return static_cast<std::int32_t>(model->images[static_cast<std::size_t>(index)].levels.size());
}

FBE_API bool fbe_model_drop_images(uint64_t load) {
    if (model_of(load) == nullptr) {
        return false;
    }
    for (assets::image_t& image : assets::loads().get(load)->model.images) {
        image.texels = {};
    }
    return true;
}

FBE_API bool fbe_model_material_textures(uint64_t load, int32_t index, uint64_t material,
                                         const uint64_t* textures, int32_t count, uint64_t* other) {
    const assets::model_t* model = model_of(load);
    if (model == nullptr) {
        return false;
    }
    if (other != nullptr) {
        *other = 0;
    }
    if (index < 0 || std::cmp_greater_equal(index, model->materials.size())) {
        return fbe::fail(std::format("the model has {} materials; there's no material {}",
                                     model->materials.size(), index));
    }
    const assets::texture_ref_t& read =
        model->materials[static_cast<std::size_t>(index)].base_color_texture;
    if (read.image < 0) {
        return true;
    }
    if (textures == nullptr || read.image >= count || textures[read.image] == 0) {
        return refused(std::format("the material {}'s image {} isn't there", index, read.image));
    }
    // A file scales UVs up and moves them on, and a layer says how much it scales them down and
    // moves them back; its turn is the file's already.
    if (read.scale[0] == 0.0F || read.scale[1] == 0.0F) {
        return refused(std::format("the material {}'s texture has a scale of 0", index));
    }
    // A texture has one sampler, the first of its materials', so another's reads a copy.
    std::uint64_t texture = textures[read.image];
    const sampling_t sampling = sampling_of(*model, read);
    const sampling_t has{
        .filter = fbe_texture_filter(texture),
        .wrap_u = fbe_texture_wrap_u(texture),
        .wrap_v = fbe_texture_wrap_v(texture),
    };
    if (has != sampling && other != nullptr) {
        texture = fbe_model_texture(load, read.image, true);
        if (texture == 0) {
            return false;
        }
        set_sampling(texture, sampling);
        *other = texture;
    }
    if (!fbe_material_set_texture(material, 0, texture) ||
        !fbe_material_set_layer(material, 0, FBE_LAYER_MULTIPLY,
                                static_cast<std::int32_t>(read.uv_set))) {
        return refused(std::format("the material {}'s texture can't be as its file says: {}", index,
                                   fbe_error()));
    }
    return fbe_material_set_layer_uv(material, 0, 1.0F / read.scale[0], 1.0F / read.scale[1],
                                     read.rotation, -read.offset[0], -read.offset[1]) ||
           refused(std::format("the material {}'s texture can't be moved as its file says: {}",
                               index, fbe_error()));
}

FBE_API int32_t fbe_model_scene(uint64_t load) {
    const assets::model_t* model = model_of(load);
    return model != nullptr ? model->scene : -1;
}

FBE_API int32_t fbe_model_scene_count(uint64_t load) {
    const assets::model_t* model = model_of(load);
    return model != nullptr ? static_cast<std::int32_t>(model->scenes.size()) : -1;
}

FBE_API bool fbe_model_spawn(uint64_t load, int32_t scene, uint64_t root,
                             const uint64_t* geometries, int32_t geometry_count,
                             const uint64_t* skins, int32_t skin_count) {
    const assets::model_t* model = model_of(load);
    if (model == nullptr) {
        return false;
    }
    if (scene < -1 ||
        std::cmp_greater_equal(scene, static_cast<std::int64_t>(model->scenes.size()))) {
        return fbe::fail(std::format("the model has {} scenes; there's no scene {}",
                                     model->scenes.size(), scene));
    }
    if (geometry_count < 0 || skin_count < 0 || (geometry_count > 0 && geometries == nullptr) ||
        (skin_count > 0 && skins == nullptr)) {
        return fbe::fail("no geometries or skins were given");
    }
    const std::uint64_t world = fbe_entity_world(root);
    if (world == 0) {
        return false;
    }
    const spawn_t spawn{
        .model = *model,
        .world = world,
        .root = root,
        .geometries = std::span(geometries, static_cast<std::size_t>(geometry_count)),
        .skins = std::span(skins, static_cast<std::size_t>(skin_count)),
    };
    return spawn_nodes(spawn, top_nodes(*model, scene));
}

} // extern "C"
