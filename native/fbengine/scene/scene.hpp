#pragma once

// What the scene's files share: the handle tables, and the entities' data, kept by field in arrays
// indexed by slot, so what runs over many entities a frame reads only what it needs.

#include "fbengine.h"
#include "scene/bvh.hpp"
#include "scene/math.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fbe::scene {

/// No slot: no parent, no world.
constexpr std::uint32_t none = UINT32_MAX;

/// The slots of one kind of handle: a 32-bit slot, and above it a 24-bit generation and the kind's
/// 8-bit tag, so a handle of another kind, as a World's given for an entity, is caught too. A freed
/// slot's generation goes up, so its old handles are caught, never taken for the next one made
/// there.
class slots_t {
public:
    explicit slots_t(std::uint8_t tag) : m_tag(tag) {}

    /// A free slot, or a new one; its handle is handle(slot).
    std::uint32_t make();
    void free(std::uint32_t slot);
    /// The live slot a handle is for, if it is.
    [[nodiscard]] std::optional<std::uint32_t> find(std::uint64_t handle) const;
    [[nodiscard]] std::uint64_t handle(std::uint32_t slot) const;
    /// How many slots there are, live or free, which the field arrays are sized to.
    [[nodiscard]] std::uint32_t size() const;

private:
    std::uint8_t m_tag;
    std::vector<std::uint32_t> m_generations;
    std::vector<bool> m_live;
    std::vector<std::uint32_t> m_free;
};

/// A world: the entities at its top, how many it has, and its ambient light, std.math's
/// Color.Gray, sRGB's half gray, at first.
struct world_t {
    std::vector<std::uint32_t> roots;
    std::int32_t count = 0;
    std::array<float, 3> ambient{ 0.2158605F, 0.2158605F, 0.2158605F };
};

/// One surface of a geometry: its vertices, as the C API has them, and its triangles, three
/// indices into them each, counterclockwise seen from the front.
struct surface_t {
    std::vector<fbe_vertex_t> vertices;
    std::vector<std::uint32_t> indices;
    /// The material it's drawn in, by slot, which it holds, or none for the default.
    std::uint32_t material = none;
};

/// One of a material's texture layers: how it blends with the layers under it, and which UV set
/// it reads.
struct layer_t {
    std::int32_t blend = 0;
    std::int32_t uv_set = 0;
};

/// A material: how surfaces look, shared by every surface and mesh drawing in it, which its
/// references count, as a geometry's do.
struct material_t {
    std::uint32_t references = 0;
    std::array<float, 4> color{ 1.0F, 1.0F, 1.0F, 1.0F };
    float shininess = 0.0F;
    std::int32_t shading = 0;
    std::int32_t blend = 0;
    float alpha_cutoff = 0.0F;
    bool vertex_colors = false;
    bool two_sided = false;
    bool fogged = true;
    std::int32_t order = 0;
    std::array<layer_t, FBE_MATERIAL_LAYERS> layers{};
};

/// A geometry: surfaces, shared by every mesh drawing it. It lives while the FB side holds its
/// handle or a mesh draws it, which its references count.
struct geometry_t {
    std::vector<surface_t> surfaces;
    std::uint32_t references = 0;
    /// Goes up at every change, so what's worked out from it, as collision trees, knows when it's
    /// stale.
    std::uint64_t version = 0;
    bool bounds_stale = true;
    vec3_t bounds_min;
    vec3_t bounds_max;
    /// The tree over its triangles, and the version it was built at, built when it's first
    /// searched after a change.
    bvh_t tree;
    std::uint64_t tree_version = UINT64_MAX;
};

/// A mesh: the geometry it draws, by slot, or none.
struct mesh_t {
    std::uint32_t entity = none;
    std::uint32_t geometry = none;
    /// A material it draws every surface in instead of their own, which it holds, or none.
    std::uint32_t material = none;
    /// Its own color and alpha, which multiply its materials', white at first.
    std::array<float, 4> tint{ 1.0F, 1.0F, 1.0F, 1.0F };
};

/// Every entity's data, a field to an array, indexed by slot.
struct entities_t {
    slots_t slots{ 2 };
    std::vector<std::int32_t> kind;
    std::vector<std::uint32_t> world;
    std::vector<std::uint32_t> parent;
    std::vector<std::vector<std::uint32_t>> children;
    std::vector<std::string> name;
    std::vector<bool> hidden;

    // The transform in the parent's space, and in the world's, which is worked out again only once
    // something above it or it moved: a moved entity marks itself and what's under it, and stops at
    // one marked already, since what's under a marked entity is marked too.
    std::vector<vec3_t> position;
    std::vector<quat_t> rotation;
    std::vector<vec3_t> scale;
    std::vector<mat4_t> world_matrix;
    /// The parent's world rotation times the entity's own, and its scales multiplied.
    std::vector<quat_t> world_rotation;
    std::vector<vec3_t> world_scale;
    std::vector<bool> moved;

    /// Where the entity's kind keeps the rest of it, as its camera in scene_t's cameras.
    std::vector<std::uint32_t> component;
    /// Whether it blocks line of sight, by its triangles for a mesh.
    std::vector<bool> blocks_sight;
};

/// A camera: how it projects, the part of its target it draws to, what it clears, and when it
/// draws among the others.
struct camera_t {
    std::uint32_t entity = none;
    std::int32_t projection = 0;
    float field_of_view = 1.0471976F;
    float orthographic_height = 10.0F;
    float nearest = 0.1F;
    float farthest = 1000.0F;
    /// Without a viewport of its own, it draws to the whole of its target, whatever its size.
    bool whole_target = true;
    float viewport_x = 0.0F;
    float viewport_y = 0.0F;
    float viewport_width = 0.0F;
    float viewport_height = 0.0F;
    bool clears_color = true;
    bool clears_depth = true;
    std::array<float, 4> clear_color{ 0.0F, 0.0F, 0.0F, 1.0F };
    std::int32_t order = 0;
    /// When it was made, which orders cameras of one order.
    std::uint64_t made = 0;
    std::int32_t fog_mode = 0;
    std::array<float, 3> fog_color{ 0.0F, 0.0F, 0.0F };
    float fog_start = 1.0F;
    float fog_end = 100.0F;
    float fog_density = 0.02F;
};

/// A light: what kind, its color times its intensity, how far a point or spot light reaches, and a
/// spot light's cone, its angles from its axis to where it starts to fade and to where it's out.
struct light_t {
    std::uint32_t entity = none;
    std::int32_t type = 0;
    std::array<float, 3> color{ 1.0F, 1.0F, 1.0F };
    float intensity = 1.0F;
    float range = 10.0F;
    float inner_cone = 0.0F;
    float outer_cone = 0.78539816F;
};

struct scene_t {
    slots_t world_slots{ 1 };
    std::vector<world_t> worlds;
    std::uint32_t main_world = none;
    entities_t entities;
    /// Every camera, in no order, each knowing its entity.
    std::vector<camera_t> cameras;
    std::uint64_t cameras_made = 0;
    /// Every light, in no order, each knowing its entity.
    std::vector<light_t> lights;
    std::vector<mesh_t> meshes;
    slots_t geometry_slots{ 3 };
    std::vector<geometry_t> geometries;
    slots_t material_slots{ 4 };
    std::vector<material_t> materials;
    /// The handles freed since fbe_entity_freed last took them.
    std::vector<std::uint64_t> freed;
};

/// The one scene, which every World is in.
scene_t& scene();

/// The live entity a handle is for, or none, with fbe_error saying so.
std::uint32_t entity_slot(std::uint64_t handle);
/// The live world a handle is for, or none, with fbe_error saying so.
std::uint32_t world_slot(std::uint64_t handle);

/// Gives a new entity what its kind keeps, as a camera's settings.
void add_component(std::uint32_t entity);
/// Lets go of what an entity's kind kept, as it's freed.
void remove_component(std::uint32_t entity);
/// Gives a copy what its original's kind keeps.
void copy_component(std::uint32_t from, std::uint32_t to);
/// The live camera a handle is for, or null, with fbe_error saying so.
camera_t* camera_of(std::uint64_t handle);
void add_camera(std::uint32_t entity);
void remove_camera(std::uint32_t entity);
/// The live light a handle is for, or null, with fbe_error saying so.
light_t* light_of(std::uint64_t handle);
void add_light(std::uint32_t entity);
void remove_light(std::uint32_t entity);
void add_mesh(std::uint32_t entity);
void remove_mesh(std::uint32_t entity);
/// The live mesh a handle is for, or null, with fbe_error saying so.
mesh_t* mesh_of(std::uint64_t handle);

/// A new geometry, held once, as its handle is.
std::uint32_t make_geometry();
/// The live geometry a handle is for, or none, with fbe_error saying so.
std::uint32_t geometry_slot(std::uint64_t handle);
/// Holds a geometry once more, or lets it go once, freeing it after its last.
void retain_geometry(std::uint32_t geometry);
void release_geometry(std::uint32_t geometry);
/// Marks a geometry changed.
void touch_geometry(std::uint32_t geometry);
/// A new material, held once, as its handle is.
std::uint32_t make_material();
/// The live material a handle is for, or none, with fbe_error saying so.
std::uint32_t material_slot(std::uint64_t handle);
/// Holds a material once more, or lets it go once, freeing it after its last; none does nothing.
void retain_material(std::uint32_t material);
void release_material(std::uint32_t material);
/// Holds the materials of surfaces copied from others, as Clone and Add copy them.
void retain_materials(const std::vector<surface_t>& surfaces);
void release_materials(const std::vector<surface_t>& surfaces);

/// The box around a geometry's vertices, nothing at the origin for none.
void geometry_bounds(std::uint32_t geometry, vec3_t& min, vec3_t& max);
/// The tree over a geometry's triangles, built again if the geometry changed since.
const bvh_t& geometry_tree(std::uint32_t geometry);
/// Whether an entity and every entity above it are shown.
bool showing(std::uint32_t entity);
/// The box in the world around a box in an entity's space, through its world matrix.
void world_box(std::uint32_t entity, vec3_t local_min, vec3_t local_max, vec3_t& world_min,
               vec3_t& world_max);

/// Gives an entity a transform of nothing: at its parent's origin, unturned, at scale 1.
void reset_transform(std::uint32_t entity);
/// Marks an entity, and what's under it, to have its world transform worked out again.
void mark_moved(std::uint32_t entity);
/// Works out an entity's world transform, and its parents', if they moved.
void update_world(std::uint32_t entity);
/// Sets an entity's local transform so its world one is this, under its parent as it is.
void set_world_position(std::uint32_t entity, vec3_t position);
void set_world_rotation(std::uint32_t entity, quat_t rotation);
void set_world_scale(std::uint32_t entity, vec3_t scale);

} // namespace fbe::scene
