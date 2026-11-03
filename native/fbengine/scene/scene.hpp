#pragma once

// What the scene's files share: the handle tables, and the entities' data, kept by field in arrays
// indexed by slot, so what runs over many entities a frame reads only what it needs.

#include "../../common/handles.hpp"
#include "fbengine.h"
#include "scene/bvh.hpp"
#include "scene/math.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbe::scene {

/// No slot: no parent, no world.
constexpr std::uint32_t none = UINT32_MAX;

/// The slots of one kind of handle, which a pool's data is indexed by.
using slots_t = handle_slots_t;

/// A world: the entities at its top, how many it has, its ambient light, std.math's Color.Gray,
/// sRGB's half gray, at first, and how its collisions go.
struct world_t {
    std::vector<std::uint32_t> roots;
    std::int32_t count = 0;
    std::array<float, 3> ambient{ 0.2158605F, 0.2158605F, 0.2158605F };
    /// Each pair of collision groups that collide, source in the high 16 bits and target in the
    /// low, and the FBE_RESPONSE_ the source makes; a game has a few.
    std::vector<std::pair<std::uint32_t, std::int32_t>> collisions;
    std::int32_t collision_iterations = 8;
    float collision_skin = 0.001F;
};

/// What a collision touched: the other entity, where and facing which way, when in its move, and
/// the surface and triangle, for a mesh.
struct contact_t {
    std::uint64_t entity = 0;
    vec3_t point;
    vec3_t normal;
    float time = 0.0F;
    std::uint64_t geometry = 0;
    std::int32_t surface = -1;
    std::int32_t triangle = -1;
};

/// How an entity collides: its group, its ellipsoid as a mover, its shape as a target, how high it
/// steps up, where its last move ended, and what it touched then.
struct collider_t {
    std::int32_t group = 0;
    float horizontal = 1.0F;
    float vertical = 1.0F;
    std::int32_t shape = 0;
    vec3_t box_min{ -1.0F, -1.0F, -1.0F };
    vec3_t box_max{ 1.0F, 1.0F, 1.0F };
    float step_height = 0.0F;
    /// How steep, from level, what it climbs as ground is.
    float max_slope = 0.78539816F;
    /// What picks hit it as, FBE_PICK_NONE for nothing.
    std::int32_t pick = 0;
    bool has_previous = false;
    vec3_t previous;
    std::vector<contact_t> contacts;
};

/// A morph target of a surface: what each of its vertices, and its normal, move by at a weight of
/// 1, as the morph area's C API has them; vertices past them don't move, and normals can be none.
/// `low` and `high` are the box of what its positions move by, around none.
struct morph_target_t {
    std::vector<vec3_t> positions;
    std::vector<vec3_t> normals;
    vec3_t low;
    vec3_t high;
};

/// One surface of a geometry: its vertices, as the C API has them, its triangles, three indices
/// into them each, counterclockwise seen from the front, and its morph targets.
struct surface_t {
    std::vector<fbe_vertex_t> vertices;
    /// Its vertices' joints, one for each, or none.
    std::vector<fbe_vertex_joints_t> joints;
    std::vector<std::uint32_t> indices;
    std::vector<morph_target_t> targets;
    /// The material it's drawn in, by slot, which it holds, or none for the default.
    std::uint32_t material = none;
};

/// One of a material's texture layers: how it blends with the layers under it, which UV set it
/// reads, its texture, which it holds, 0 for none, what turns its UVs, and for a bump layer, what
/// its map shifts the next layer's UVs by and how it scales that layer's light.
struct layer_t {
    std::int32_t blend = 0;
    std::int32_t uv_set = 0;
    std::uint64_t texture = 0;
    std::array<float, 2> uv_scale{ 1.0F, 1.0F };
    float uv_angle = 0.0F;
    std::array<float, 2> uv_offset{ 0.0F, 0.0F };
    std::array<float, 4> bump{ 0.0F, 0.0F, 0.0F, 0.0F };
    float bump_scale = 0.0F;
    float bump_offset = 1.0F;
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
    bool wireframe = false;
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
    /// A box drawing culls it by instead of its bounds, as a skinned mesh's, whose vertices move
    /// out of them, wants.
    bool has_cull_box = false;
    /// What it's known to be made of, a scene file's name for it, while it's as it was then.
    std::string source;
    std::uint64_t source_version = 0;
    vec3_t cull_min;
    vec3_t cull_max;
    /// Goes up when its triangles come or go, as against only moving, which a tree can be
    /// refitted for rather than built again.
    std::uint64_t shape_version = 0;
    /// The tree over its triangles, and the versions it was made at, built or refitted when it's
    /// first searched after a change.
    bvh_t tree;
    std::uint64_t tree_version = UINT64_MAX;
    std::uint64_t tree_shape_version = UINT64_MAX;
};

/// A mesh: the geometry it draws, by slot, or none.
struct mesh_t {
    std::uint32_t entity = none;
    std::uint32_t geometry = none;
    /// A material it draws every surface in instead of their own, which it holds, or none.
    std::uint32_t material = none;
    /// Its own color and alpha, which multiply its materials', white at first.
    std::array<float, 4> tint{ 1.0F, 1.0F, 1.0F, 1.0F };
    /// Its morph targets' weights, one a target from the first, 0 past them, and as they were at
    /// its World's last capture.
    std::vector<float> weights;
    std::vector<float> captured_weights;
};

/// A sprite: its quad's material, which it holds, or none, its own color, how it turns toward the
/// camera, its half-size, which point is at its place, and its roll in its own plane.
struct sprite_t {
    std::uint32_t entity = none;
    std::uint32_t material = none;
    std::array<float, 4> tint{ 1.0F, 1.0F, 1.0F, 1.0F };
    std::int32_t view_mode = 0;
    float size_x = 1.0F;
    float size_y = 1.0F;
    float handle_x = 0.0F;
    float handle_y = 0.0F;
    float roll = 0.0F;
};

/// A ground plane: the material it's drawn in, which it holds, or none, its own color, and the grid
/// it's drawn in.
struct plane_t {
    std::uint32_t entity = none;
    std::uint32_t material = none;
    std::array<float, 4> tint{ 1.0F, 1.0F, 1.0F, 1.0F };
    std::int32_t segments = 1;
};

/// A terrain: its grid's size, its corners' heights row by row along Z, and how it's drawn; its
/// version goes up at every change.
struct terrain_t {
    std::uint32_t entity = none;
    std::uint32_t material = none;
    std::array<float, 4> tint{ 1.0F, 1.0F, 1.0F, 1.0F };
    std::int32_t size = 1;
    std::vector<float> heights;
    std::uint64_t version = 0;
    /// Its lowest and highest heights, so its bounds don't read them all; worked out again, the
    /// next time they're asked for, when a corner at one of them moved in.
    mutable float low = 0.0F;
    mutable float high = 0.0F;
    mutable bool range_stale = false;
};

/// Every entity's data, a field to an array, indexed by slot.
struct entities_t {
    slots_t slots{ ENTITY_KIND };
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
    std::vector<collider_t> colliders;
    /// Its entry in scene_t's pickables, or none.
    std::vector<std::uint32_t> pickable;
    /// Its local transform at its World's last capture, which drawing tweens from, and whether it
    /// was there for it.
    std::vector<vec3_t> captured_position;
    std::vector<quat_t> captured_rotation;
    std::vector<vec3_t> captured_scale;
    std::vector<bool> captured;
    /// Where it starts fading from a camera, and where it's gone, 0 and 0 for never.
    std::vector<float> fade_near;
    std::vector<float> fade_far;
    /// When it was made, by a count every entity takes the next of, which ties in drawing's order
    /// go by, so a free elsewhere, which moves a pool's last into its slot, doesn't change them.
    std::vector<std::uint64_t> made;
    std::uint64_t mades = 0;
};

/// An entity picks can hit, with its box in the world, which picks read one after another before
/// looking closer: worked out again once it's stale, as when it moved, or a mesh's geometry
/// changed since.
struct pickable_t {
    vec3_t min;
    vec3_t max;
    /// The entity, or none for an entry no longer used.
    std::uint32_t entity = none;
    std::uint32_t world = none;
    std::int32_t mode = 0;
    /// For a mesh's triangles, the geometry and its version the box is of.
    std::uint32_t geometry = none;
    std::uint64_t version = 0;
    bool stale = true;
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
    /// The texture it draws into, which it holds, or 0 for the screen.
    std::uint64_t target = 0;
    std::int32_t order = 0;
    /// When it was made, which orders cameras of one order.
    std::uint64_t made = 0;
    std::int32_t fog_mode = 0;
    std::array<float, 3> fog_color{ 0.0F, 0.0F, 0.0F };
    float fog_start = 1.0F;
    float fog_end = 100.0F;
    float fog_density = 0.02F;
};

/// A light: what kind, its color and its intensity, the range where a point or spot light ends, and
/// a spot light's cone, its angles from its axis to where it starts to fade and to where it's out.
struct light_t {
    std::uint32_t entity = none;
    std::int32_t type = 0;
    std::array<float, 3> color{ 1.0F, 1.0F, 1.0F };
    float intensity = 1.0F;
    float range = 10.0F;
    float inner_cone = 0.0F;
    float outer_cone = 0.78539816F;
};

/// Where an entity is in the world: its matrix, and its turn and scale, a mirror in the scale's
/// signs, as update_world keeps them.
struct pose_t {
    mat4_t matrix;
    quat_t rotation;
    vec3_t scale{ 1.0F, 1.0F, 1.0F };
};

struct scene_t {
    slots_t world_slots{ WORLD_KIND };
    std::vector<world_t> worlds;
    std::uint32_t main_world = none;
    entities_t entities;
    /// Every camera, in no order, each knowing its entity.
    std::vector<camera_t> cameras;
    std::uint64_t cameras_made = 0;
    /// Every light, in no order, each knowing its entity.
    std::vector<light_t> lights;
    std::vector<mesh_t> meshes;
    std::vector<sprite_t> sprites;
    std::vector<plane_t> planes;
    std::vector<terrain_t> terrains;
    slots_t geometry_slots{ GEOMETRY_KIND };
    std::vector<geometry_t> geometries;
    /// Every geometry's version is the next of this, so a new one in a freed slot never has the
    /// version its slot's last one had.
    std::uint64_t geometry_versions = 0;
    slots_t material_slots{ MATERIAL_KIND };
    std::vector<material_t> materials;
    /// The handles freed since fbe_entity_freed last took them.
    std::vector<std::uint64_t> freed;
    /// Every entity with a pick mode, and the entries no longer used, which picks pass over until
    /// there are as many as used ones.
    std::vector<pickable_t> pickables;
    std::size_t unused_pickables = 0;
    /// Entities' tweened poses, by slot, each for the tweened_t whose stamp it has.
    std::vector<pose_t> poses;
    std::vector<std::uint64_t> posed;
    std::uint64_t posings = 0;
};

/// The one scene, which every World is in.
scene_t& scene();

/// The live slot of `slots` a handle is for, or none, with fbe_error saying why, naming `what` it
/// should be: none given, another kind's handle, or one freed.
std::uint32_t live_slot(const slots_t& slots, std::uint64_t handle, const char* what);
/// The live entity a handle is for, or none, with fbe_error saying so.
std::uint32_t entity_slot(std::uint64_t handle);
/// The live world a handle is for, or none, with fbe_error saying so.
std::uint32_t world_slot(std::uint64_t handle);

/// Gives a new entity what its kind keeps, as a camera's settings.
void add_component(std::uint32_t entity);
/// How many pieces a long line over a terrain is tried in, a few squares each: in float first,
/// since a line can be longer than an int counts.
inline int terrain_line_pieces(float span) {
    return static_cast<int>(std::fmin(std::fmax(std::ceil(span / 4.0F), 1.0F), 4096.0F));
}

/// Lets go of what an entity's kind kept, as it's freed.
void remove_component(std::uint32_t entity);
/// Gives a copy what its original's kind keeps.
void copy_component(std::uint32_t from, std::uint32_t to);
/// The live camera a handle is for, or null, with fbe_error saying so.
camera_t* camera_of(std::uint64_t handle);
/// Lets go of a texture a camera or material held, if it hasn't gone with the GPU.
void release_texture(std::uint64_t texture);
void add_camera(std::uint32_t entity);
void remove_camera(std::uint32_t entity);
/// Numbers a tree's copied cameras anew in the order their originals were made, so cameras of one
/// order draw as their originals do, whatever the tree's order.
void renumber_cameras(const std::vector<std::pair<std::uint32_t, std::size_t>>& originals,
                      const std::vector<std::uint32_t>& copies);
/// The live light a handle is for, or null, with fbe_error saying so.
light_t* light_of(std::uint64_t handle);
/// How a World's moves are run: by what's worth it, here or on the job system's workers, or, for
/// tests, all here or all on the workers.
enum class move_path_t : std::uint8_t {
    AS_WORTH,
    HERE,
    WORKERS,
};
void set_move_path(move_path_t path);
/// The most of anything moves' scratch has kept for the next; how many scratches there are; and
/// how many of the last World's moves ran here, not on the workers.
std::size_t move_scratch_kept();
std::size_t move_scratch_count();
std::size_t moves_run_here();
/// The lights among some entities and under them in the tree's order, depth first, each parent
/// before its children, by a stack of their own, so no tree is too deep.
void lights_in(const std::vector<std::uint32_t>& among, std::vector<std::uint32_t>& found);
void add_light(std::uint32_t entity);
void remove_light(std::uint32_t entity);
void add_mesh(std::uint32_t entity);
void remove_mesh(std::uint32_t entity);
/// The live mesh a handle is for, or null, with fbe_error saying so.
mesh_t* mesh_of(std::uint64_t handle);
void add_terrain(std::uint32_t entity);
void remove_terrain(std::uint32_t entity);
/// The live terrain a handle is for, or null, with fbe_error saying so.
terrain_t* terrain_of(std::uint64_t handle);
/// The box a terrain's grid is in, in its own space.
void terrain_bounds(const terrain_t& terrain, vec3_t& min, vec3_t& max);
/// Whether the segment from `a` to `b`, in a terrain's own space, crosses any of its triangles,
/// from above or below.
bool terrain_blocks(const terrain_t& terrain, vec3_t a, vec3_t b);
/// The triangles of a terrain's squares under a box in its own space, counterclockwise seen from
/// above, in its own space.
void terrain_under(const terrain_t& terrain, vec3_t min, vec3_t max,
                   std::vector<std::array<vec3_t, 3>>& triangles);
/// The point on a terrain's surface over a point in its own space, and the surface's normal there;
/// false off its grid.
bool terrain_surface(const terrain_t& terrain, vec3_t local, vec3_t& point, vec3_t& normal);
void add_plane(std::uint32_t entity);
void remove_plane(std::uint32_t entity);
/// The live ground plane a handle is for, or null, with fbe_error saying so.
plane_t* plane_of(std::uint64_t handle);
/// Whether two meshes' triangles cross, in the world.
bool meshes_cross(std::uint32_t first, std::uint32_t second);
void add_sprite(std::uint32_t entity);
void remove_sprite(std::uint32_t entity);
/// The live sprite a handle is for, or null, with fbe_error saying so.
sprite_t* sprite_of(std::uint64_t handle);
/// A sprite's quad's matrix in the world as an entity, a camera, sees it.
mat4_t sprite_matrix(std::uint32_t entity, std::uint32_t camera);
/// A sprite's quad's matrix in the world, its own matrix to the world `own`, seen by a camera
/// turned `eye`.
mat4_t sprite_matrix(std::uint32_t entity, const mat4_t& own, quat_t eye);
/// The box in the world a sprite's quad stays in, however it turns.
void sprite_bounds(std::uint32_t entity, vec3_t& min, vec3_t& max);
/// The box in the world a sprite's quad stays in, however it turns, its own matrix to the world
/// `own`.
void sprite_bounds(std::uint32_t entity, const mat4_t& own, vec3_t& min, vec3_t& max);

/// A new geometry, held once, as its handle is.
std::uint32_t make_geometry();
/// The live geometry a handle is for, or none, with fbe_error saying so.
std::uint32_t geometry_slot(std::uint64_t handle);
/// Holds a geometry once more, or lets it go once, freeing it after its last.
void retain_geometry(std::uint32_t geometry);
void release_geometry(std::uint32_t geometry);
/// Marks a geometry changed: `reshaped` when triangles or surfaces came or went, else only moved.
void touch_geometry(std::uint32_t geometry, bool reshaped);
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
/// Works out a morph target's box from its positions.
void fit_target_box(morph_target_t& target);
/// The tree over a geometry's triangles, built again if the geometry changed since.
const bvh_t& geometry_tree(std::uint32_t geometry);
/// Whether an entity and every entity above it are shown.
bool showing(std::uint32_t entity);
/// The box in the world around a box in an entity's space, through its world matrix.
void world_box(std::uint32_t entity, vec3_t local_min, vec3_t local_max, vec3_t& world_min,
               vec3_t& world_max);

/// Keeps an entity's pickables entry as its pick mode and World say: made, let go of, or with its
/// box to work out again.
void pickable_changed(std::uint32_t entity);
/// Whether anything in a world that blocks sight by its pick shape, an ellipsoid or a box, and
/// isn't a mesh, a terrain, or one of the two entities, crosses the segment.
bool pick_shape_blocks(std::uint32_t world, vec3_t a, vec3_t b, std::uint32_t first,
                       std::uint32_t second);
/// Marks an entity's box, if picks hit it, to be worked out again.
void pickable_moved(std::uint32_t entity);

/// Entities' poses `tween` of the way from their World's last capture to now, each worked out
/// once, from its parent's, while this lasts, as a frame's drawing wants them; at a tween of 1,
/// where they are.
class tweened_t {
public:
    explicit tweened_t(float tween);

    [[nodiscard]] pose_t pose(std::uint32_t entity) const;
    [[nodiscard]] float tween() const {
        return m_tween;
    }

private:
    float m_tween;
    std::uint64_t m_stamp;
};

/// An entity's matrix to the world `tween` of the way from its World's last capture to now.
mat4_t tweened_matrix(std::uint32_t entity, float tween);
/// The FBE_BLEND_ something drawn in a material, or none for the default, at this alpha, its own
/// times its material's, is drawn with, AUTO and OPAQUE settled.
std::int32_t drawn_blend(std::uint32_t material, float alpha);
/// The alpha an entity's fade gives it seen from a camera.
float entity_fade(std::uint32_t entity, std::uint32_t camera);

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
