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
    /// The environment its PBR materials are lit by, 0 for none, and what its light's times.
    std::uint64_t environment = 0;
    float environment_intensity = 1.0F;

    /// Its sky: none, its atmosphere, or a texture, which it holds; the light whose way's the
    /// sun's, by handle, 0 for straight up; and how bright.
    std::int32_t sky = 0;
    std::uint64_t sky_light = 0;
    std::uint64_t sky_texture = 0;
    /// Which way the sun is, kept from the last it was looked at, so one freed leaves it there.
    vec3_t sky_sun{ 0.0F, 1.0F, 0.0F };
    float sky_brightness = 1.0F;
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
    /// Its vertices' tangents, one for each, or none.
    std::vector<fbe_vertex_tangent_t> tangents;
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
    /// What a PBR material is beyond its color, and its maps, by FBE_MAP_, as layers but for their
    /// blend and bump.
    float metallic = 0.0F;
    float roughness = 1.0F;
    std::array<float, 3> emissive{ 0.0F, 0.0F, 0.0F };
    float normal_scale = 1.0F;
    float occlusion_strength = 1.0F;
    std::array<layer_t, FBE_MATERIAL_MAPS> maps{};
};

/// A level of detail of a geometry.
struct lod_t {
    /// The geometry it draws, by slot, which it holds, or none for nothing.
    std::uint32_t geometry = none;
    float share = 0.0F;
};

/// A geometry: surfaces, shared by every mesh drawing it. It lives while the FB side holds its
/// handle or a mesh draws it, which its references count.
struct geometry_t {
    std::vector<surface_t> surfaces;
    /// How much of its way to the eye it's drawn nearer, as a decal on a terrain is, so what it's
    /// on doesn't come up through it where it's coarser, far off.
    float eye_pull = 0.0F;
    /// Its levels of detail, their shares going down, and how many geometries have it as one.
    std::vector<lod_t> lods;
    std::uint32_t lod_uses = 0;
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

/// How many squares across a terrain's piece draws, at most, and how many of its widths away one's
/// drawn whole rather than as four finer ones.
constexpr std::int32_t terrain_piece_cells = 16;
constexpr float terrain_whole_from = 1.5F;

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

/// A particle as the GPU keeps it: where it was born, when, on its emitter's clock, how fast and
/// which way, how long it lives, and how it's turned then and spins.
struct particle_t {
    std::array<float, 3> position{};
    float birth = 0.0F;
    std::array<float, 3> velocity{};
    float life = 0.0F;
    float roll = 0.0F;
    float spin = 0.0F;
};
static_assert(sizeof(particle_t) == 40, "the scene shader reads particles 40 bytes apart");

/// What an emitter's clock reaches before it starts again, which a float keeps to an
/// eight-thousandth of a second, and a particle's life is at most half of.
constexpr float particles_wrap = 1024.0F;

/// The box what was born in a stretch of time can reach by its speed alone.
struct bounds_slice_t {
    bool any = false;
    vec3_t low;
    vec3_t high;
};

/// A particle emitter: how it's drawn and how they go, then its clock, what's been born and not yet
/// sent to the GPU, each with its slot, when each slot's dies, and where they've been born lately.
struct particles_t {
    std::uint32_t entity = none;
    std::uint32_t material = none;
    std::int32_t capacity = 1024;
    float rate = 32.0F;
    float life_min = 1.0F;
    float life_max = 1.0F;
    float speed_min = 1.0F;
    float speed_max = 1.0F;
    float spread = 0.0F;
    float spin = 0.0F;
    float roll = 0.0F;
    vec3_t gravity;
    float drag = 0.0F;
    float size_start = 0.1F;
    float size_end = 0.1F;
    std::array<float, 4> color_start{ 1.0F, 1.0F, 1.0F, 1.0F };
    std::array<float, 4> color_end{ 1.0F, 1.0F, 1.0F, 0.0F };
    std::uint32_t random = 1;
    float clock = 0.0F;
    float captured_clock = 0.0F;
    double total = 0.0;
    /// How many times half the wrap has gone by, at each of which what's dead is cleared, since a
    /// dead one's birth would otherwise come round again; and the longest life it's had since.
    std::uint64_t sweeps = 0;
    float longest = 1.0F;
    /// When the last of what was born with a longer life than it has now dies, and the latest any
    /// dies.
    double long_until = 0.0;
    double latest_death = 0.0;
    /// How many of its slots have had one.
    std::int32_t born = 0;
    float owed = 0.0F;
    bool moved = false;
    vec3_t last_place;
    std::size_t next = 0;
    std::uint64_t generation = 0;
    std::vector<std::pair<std::uint32_t, particle_t>> pending;
    std::vector<double> deaths;
    std::array<bounds_slice_t, 9> slices{};
    std::size_t slice = 0;
    float slice_age = 0.0F;
};

/// A run of an Instances entity's instances near each other, and the box they're in, in its space.
struct instance_cluster_t {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    vec3_t low;
    vec3_t high;
    /// The biggest sphere one of them is in, which is what its size in a view goes by.
    float radius = 0.0F;
    /// Whether their matrices mirror, as all of a cluster's do or none.
    bool mirrored = false;
};

/// A level of an Instances' geometry that some of its clusters draw, and whether those mirror.
struct instance_level_t {
    std::uint32_t level = none;
    bool mirrored = false;
};

/// An Instances entity: what it draws, in, and its instances, in an order where those near each
/// other are together, with where each index went and whose each place is; and its clusters, worked
/// out again when they or its geometry's changed.
struct instances_t {
    std::uint32_t entity = none;
    std::uint32_t geometry = none;
    std::uint32_t material = none;
    std::vector<fbe_instance_t> items;
    std::vector<std::uint32_t> slots;
    std::vector<std::uint32_t> indices;
    std::uint64_t version = 0;
    std::vector<instance_cluster_t> clusters;
    std::uint32_t clusters_geometry = none;
    std::uint64_t clusters_version = 0;
    bool clusters_stale = true;
    /// Which clusters have changed, if not all of them have.
    std::vector<std::uint32_t> stale_clusters;
    /// How many of them have an alpha under 1.
    std::size_t faded = 0;
    /// Where those whose matrices mirror start in `items`, after those that don't.
    std::size_t mirrored_first = 0;
    /// Where in `items` they've changed since they went to the GPU, if not all of them have.
    std::vector<std::uint32_t> changed;
    bool all_changed = true;
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
    /// Whether it casts shadows.
    std::vector<bool> casts_shadows;
    /// Whether it's an occluder.
    std::vector<bool> occluder;
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
    /// Whether what it draws is tested against occluders.
    bool occlusion = true;
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
    /// Whether it draws into an HDR image, and what that comes to in its target.
    bool hdr = false;
    /// What its ambient occlusion's raised to, 0 for none, and how far it reaches in the world.
    float ambient_occlusion = 0.0F;
    float ambient_occlusion_radius = 1.0F;
    /// Whether its picture's antialiased from itself, after all else.
    bool fxaa = false;
    /// Whether an HDR camera antialiases over frames, and how many times it's been told to forget.
    bool temporal_aa = false;
    std::uint64_t history_resets = 0;
    float exposure = 1.0F;
    std::int32_t tonemap = FBE_TONEMAP_NONE;
    float gamma = 1.0F;
    /// Its bloom, and its color grade.
    float bloom_intensity = 0.0F;
    float bloom_threshold = 1.0F;
    std::array<float, 3> grade_slope{ 1.0F, 1.0F, 1.0F };
    std::array<float, 3> grade_offset{ 0.0F, 0.0F, 0.0F };
    std::array<float, 3> grade_power{ 1.0F, 1.0F, 1.0F };
    float grade_saturation = 1.0F;
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
    /// Its shadows: whether it has them, a tile's side in texels, how far off a surface they're
    /// compared, in depth, world units, and along the normal, texels, and a directional light's
    /// cascades, and how far from the camera they reach.
    bool shadows = false;
    std::int32_t shadow_size = 1024;
    float shadow_bias = 0.02F;
    float shadow_normal_bias = 1.5F;
    std::int32_t cascades = 3;
    float shadow_distance = 100.0F;
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
    /// Every particle emitter, in no order, each knowing its entity.
    std::vector<particles_t> particles;
    /// Every Instances entity, in no order, each knowing its entity.
    std::vector<instances_t> instances;
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
/// The live Instances entity a handle is for, or null, with fbe_error saying so.
instances_t* instances_of(std::uint64_t handle);
void add_instances(std::uint32_t entity);
void remove_instances(std::uint32_t entity);
void copy_instances(std::uint32_t from, std::uint32_t to);
/// Its clusters, worked out again if they're stale.
const std::vector<instance_cluster_t>& instance_clusters(instances_t& found);
/// The box its instances are in, in its space: false for none.
bool instances_bounds(instances_t& found, vec3_t& min, vec3_t& max);
/// Which of its geometry's levels of detail a cluster draws, by the size of the biggest of it where
/// it's nearest, none for nothing.
std::uint32_t cluster_lod(const instances_t& found, const instance_cluster_t& cluster,
                          const mat4_t& matrix, const camera_t& camera, vec3_t eye,
                          float half_height);
/// The levels some cluster of it draws, each with whether those mirror.
std::vector<instance_level_t> instances_levels(instances_t& found, const mat4_t& matrix,
                                               const camera_t& camera, vec3_t eye,
                                               float half_height);
/// The geometry to draw of one with levels of detail, by slot, or none for nothing, at a place and
/// size in the world, from an eye, by how the camera's view spreads: half its height at a depth of
/// 1, or in world units.
std::uint32_t lod_for(std::uint32_t geometry, vec3_t center, float radius, const camera_t& camera,
                      vec3_t eye, float half_height);
/// As lod_for, at a distance from the eye.
std::uint32_t lod_at(std::uint32_t geometry, float distance, float radius, const camera_t& camera,
                     float half_height);
/// The live particle emitter a handle is for, or null, with fbe_error saying so.
particles_t* particles_of(std::uint64_t handle);
void add_particles(std::uint32_t entity);
void remove_particles(std::uint32_t entity);
void copy_particles(std::uint32_t from, std::uint32_t to);
void capture_particles(std::uint32_t entity);
/// An emitter's clock `tween` of the way from its capture to now.
float particles_clock(const particles_t& found, float tween);
/// The box in the world what's alive of an emitter's in: false for none lately.
bool particles_bounds(const particles_t& found, vec3_t& min, vec3_t& max);
/// Whether a color's channels are finite, which can be any, and its alpha 0 to 1: false, with
/// fbe_error saying why, naming `what` it's a color of.
bool valid_color(float r, float g, float b, float a, const char* what);
/// How far a terrain's coarser pieces can come up over what's in a box, in its own space, as a part
/// of how far off they're drawn, a square `reach_scale` wide: what's on it wants to be that much
/// nearer.
float terrain_coarse_rise(const terrain_t& terrain, vec3_t min, vec3_t max, float reach_scale);
/// The normal of a terrain's surface at a corner of its grid, in its own space.
vec3_t terrain_corner_normal(const terrain_t& terrain, std::int32_t x, std::int32_t z);
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
/// The same with the camera's axes given, which are the same for a view's every sprite.
mat4_t sprite_matrix(std::uint32_t entity, const mat4_t& own, vec3_t eye_x, vec3_t eye_y,
                     vec3_t eye_z);
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
/// Lets go of a geometry's levels of detail.
void clear_lods(std::uint32_t geometry);
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
