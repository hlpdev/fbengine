#ifndef FBE_SCENE_H
#define FBE_SCENE_H

// The scene: worlds, and the tree of entities in each.
//
// Worlds, entities, and geometry are handles: a 32-bit slot, a 24-bit generation, and an 8-bit tag
// for its kind, 0 for none. A call on a freed one, or one of another kind, fails, false or 0, and
// fbe_error says why. Every entity freed, by fbe_entity_free or
// with its World, is queued for fbe_entity_freed, so the FB side lets go of what it kept for it.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What an entity is.
#define FBE_ENTITY_PIVOT   0
#define FBE_ENTITY_CAMERA  1
#define FBE_ENTITY_LIGHT   2
#define FBE_ENTITY_MESH    3
#define FBE_ENTITY_SPRITE  4
#define FBE_ENTITY_PLANE   5
#define FBE_ENTITY_MIRROR  6
#define FBE_ENTITY_TERRAIN 7

// The space a transform is in: the entity's parent's, or the world's.
#define FBE_SPACE_LOCAL 0
#define FBE_SPACE_WORLD 1

// An entity's own axes.
#define FBE_AXIS_RIGHT   0
#define FBE_AXIS_UP      1
#define FBE_AXIS_FORWARD 2

// What fbe_entity_transform transforms: a point, which moves; a vector, which only turns and
// scales; a normal, through the inverse transpose, then made length 1.
#define FBE_TRANSFORM_POINT  0
#define FBE_TRANSFORM_VECTOR 1
#define FBE_TRANSFORM_NORMAL 2

// How a camera projects: with perspective, or orthographically, without.
#define FBE_PROJECTION_PERSPECTIVE  0
#define FBE_PROJECTION_ORTHOGRAPHIC 1

// What a light is: the sun's, along its forward axis from everywhere; a point's, out from where it
// is; or a spot's, in a cone along its forward axis.
#define FBE_LIGHT_DIRECTIONAL 0
#define FBE_LIGHT_POINT       1
#define FBE_LIGHT_SPOT        2

// How a camera's fog thickens with depth in front of it: not at all; linearly from its start to
// its end; as e to the minus (density x depth); or as e to the minus (density x depth) squared.
#define FBE_FOG_NONE                0
#define FBE_FOG_LINEAR              1
#define FBE_FOG_EXPONENTIAL         2
#define FBE_FOG_EXPONENTIAL_SQUARED 3

// How a material is lit: physically, as glTF's metal and roughness have it; not at all, its color
// as it is; or per vertex, as fixed-function hardware lit, for an old look. UNLIT and VERTEX are
// that fixed-function look, working on what's stored, sRGB's encoded values: 50 doubled is 100.
#define FBE_SHADING_PBR    0
#define FBE_SHADING_UNLIT  1
#define FBE_SHADING_VERTEX 2

// How a material blends with what's drawn behind it: AUTO, opaque unless its alpha times its
// mesh's is below 1, or, with no alpha cutoff, a layer's texture has alpha (fbe_texture_set_alpha),
// then alpha; OPAQUE, which leaves its own and its textures' alpha out, though
// its mesh's alpha below 1 still blends it as alpha, so a mesh fades whatever it draws in; alpha;
// adding its light, scaled by its alpha; or multiplying what's behind, scaled by its alpha.
#define FBE_BLEND_AUTO     0
#define FBE_BLEND_OPAQUE   1
#define FBE_BLEND_ALPHA    2
#define FBE_BLEND_ADD      3
#define FBE_BLEND_MULTIPLY 4

// How a texture layer combines with the layers under it: multiplying them, adding to them,
// laid over them by its alpha, or multiplying them twice over, so its middle gray leaves them as
// they are, as a doubled ambient layer under a lightmap wants; or not at all, but its red and green
// shift the next layer's UVs and its blue scales that layer's light, as fbe_material_set_layer_bump
// says, the environment bump old rooms use.
#define FBE_LAYER_MULTIPLY   0
#define FBE_LAYER_ADD        1
#define FBE_LAYER_ALPHA      2
#define FBE_LAYER_MULTIPLY_2 3
#define FBE_LAYER_BUMP       4

// How many texture layers a material has.
#define FBE_MATERIAL_LAYERS 8

// What a moving entity does when it touches another in a collision group it collides with: nothing,
// for groups that don't collide; stops where it touched; slides along what it touched for the rest
// of its move; or slides, but no further than it moved across, so falling onto a slope doesn't
// slide it down, and walking up one keeps its pace.
#define FBE_RESPONSE_NONE             0
#define FBE_RESPONSE_STOP             1
#define FBE_RESPONSE_SLIDE            2
#define FBE_RESPONSE_SLIDE_HORIZONTAL 3

// What a moving entity is touched as: its kind's own, a mesh's triangles and any other's
// ellipsoid; its ellipsoid; its collision box; or its mesh's triangles, from their fronts.
#define FBE_COLLISION_AUTO      0
#define FBE_COLLISION_ELLIPSOID 1
#define FBE_COLLISION_BOX       2
#define FBE_COLLISION_MESH      3

// Collision groups are 1 to FBE_COLLISION_GROUPS - 1; 0 is none.
#define FBE_COLLISION_GROUPS 1024

// How a sprite turns toward the camera drawing it: parallel to the camera's view, whatever the
// entity's own turn and scale; as the entity is turned and scaled; or upright on the entity's own
// up, turned toward the camera about it.
#define FBE_SPRITE_BILLBOARD 0
#define FBE_SPRITE_FIXED     1
#define FBE_SPRITE_UPRIGHT   2

// What picks hit an entity as: nothing; its collision ellipsoid; its collision box; or its mesh's
// triangles, from their fronts.
#define FBE_PICK_NONE      0
#define FBE_PICK_ELLIPSOID 1
#define FBE_PICK_BOX       2
#define FBE_PICK_MESH      3

// std.math's Vec3, Quat, and Mat4 as C has them; the matrix is column-major, its columns the
// axes, then the origin. C has no `using`, so they're typedefs.
// NOLINTBEGIN(modernize-use-using)
typedef struct fbe_vec3_t {
    float x;
    float y;
    float z;
} fbe_vec3_t;

typedef struct fbe_quat_t {
    float x;
    float y;
    float z;
    float w;
} fbe_quat_t;

typedef struct fbe_mat4_t {
    float m[16];
} fbe_mat4_t;

typedef struct fbe_vec2_t {
    float x;
    float y;
} fbe_vec2_t;

// A box, from its least corner to its greatest.
typedef struct fbe_aabb_t {
    fbe_vec3_t min;
    fbe_vec3_t max;
} fbe_aabb_t;

// The part of a target, the window or a texture, a camera draws to, in its units from its top
// left.
typedef struct fbe_viewport_t {
    float x;
    float y;
    float width;
    float height;
} fbe_viewport_t;

// std.math's Color: linear red, green, and blue, and alpha.
typedef struct fbe_color_t {
    float r;
    float g;
    float b;
    float a;
} fbe_color_t;

// A vertex of a surface: where it is, its normal, two sets of UVs, from the top left of a
// texture, and its color and alpha, linear.
typedef struct fbe_vertex_t {
    fbe_vec3_t position;
    fbe_vec3_t normal;
    fbe_vec2_t uv0;
    fbe_vec2_t uv1;
    fbe_color_t color;
} fbe_vertex_t;

// A vertex's joints, beside its fbe_vertex_t: which four of its skin's joints move it, as glTF's
// JOINTS_0 indexes them, and how much of each, as its WEIGHTS_0. FBE_NO_JOINT is no bone, which
// leaves it as it is; one past its palette is drawn as the palette's last.
#define FBE_NO_JOINT 0xFFFF
typedef struct fbe_vertex_joints_t {
    uint16_t joints[4];
    float weights[4];
} fbe_vertex_joints_t;

// What a collision touched, as an entity's list of them gives it: the other entity; the point it
// touched, on the other's surface, and that surface's normal there, both in the world; when in the
// move, from 0 to 1, of the step of the move that touched it, which after a slide is the slide;
// and the geometry, surface, and triangle, for a mesh's, else 0 and -1s.
typedef struct fbe_collision_t {
    uint64_t entity;
    fbe_vec3_t point;
    fbe_vec3_t normal;
    float time;
    uint64_t geometry;
    int32_t surface;
    int32_t triangle;
} fbe_collision_t;

// What a pick hit: the entity; where, on its surface, and that surface's normal there, both in
// the world; how far along the pick, from 0 to 1; and the geometry, surface, and triangle, for a
// mesh's, else 0 and -1s.
typedef struct fbe_pick_t {
    uint64_t entity;
    fbe_vec3_t point;
    fbe_vec3_t normal;
    float time;
    uint64_t geometry;
    int32_t surface;
    int32_t triangle;
} fbe_pick_t;

// Where an entity is and how it's turned, as the batches give and take them.
typedef struct fbe_pose_t {
    fbe_vec3_t position;
    fbe_quat_t rotation;
} fbe_pose_t;
// NOLINTEND(modernize-use-using)

// A new World, empty.
FBE_API uint64_t fbe_world_create(void);
// The World there always is, made the first time it's asked for.
FBE_API uint64_t fbe_world_main(void);
// Frees a World and every entity in it; false for the main World, which stays.
FBE_API bool fbe_world_free(uint64_t world);
// Frees every entity in a World.
FBE_API bool fbe_world_clear(uint64_t world);
FBE_API bool fbe_world_alive(uint64_t world);
// How many entities a World has, -1 for a freed one.
FBE_API int32_t fbe_world_entity_count(uint64_t world);
// A World's top-level entities, in the order they came to the top: writes up to `capacity` of
// them and gives how many there are, -1 for a freed World.
FBE_API int32_t fbe_world_roots(uint64_t world, uint64_t* roots, int32_t capacity);
// The first entity of the name in a World, depth first, each parent before its children; 0 for
// none.
FBE_API uint64_t fbe_world_find(uint64_t world, const char* name);

// A new entity of a kind, the last child of `parent`, or at the top of `world` when `parent` is
// 0; 0 if it can't be made.
FBE_API uint64_t fbe_entity_create(uint64_t world, uint64_t parent, int32_t kind);
// Frees an entity and every entity under it.
FBE_API bool fbe_entity_free(uint64_t entity);
FBE_API bool fbe_entity_alive(uint64_t entity);
// Takes up to `capacity` of the entities freed since the last call, oldest first, and gives how
// many it took.
FBE_API int32_t fbe_entity_freed(uint64_t* entities, int32_t capacity);
// An entity's FBE_ENTITY_ kind, -1 for a freed one.
FBE_API int32_t fbe_entity_kind(uint64_t entity);
FBE_API uint64_t fbe_entity_world(uint64_t entity);

// An entity's name, "" at first; what fbe_entity_name gives is good until the next scene call,
// which can move it, so a caller copies it at once; NULL for a freed one.
FBE_API bool fbe_entity_set_name(uint64_t entity, const char* name);
FBE_API const char* fbe_entity_name(uint64_t entity);

// Makes an entity the last child of `parent`, or puts it at the top of its World when `parent` is
// 0, moving it and what's under it into the parent's World. Its own parent again changes nothing.
// With `keep_world`, it stays where it is in the world and its local transform changes; without,
// its local transform stays. False if the parent is the entity or under it.
FBE_API bool fbe_entity_set_parent(uint64_t entity, uint64_t parent, bool keep_world);
// An entity's parent, 0 at the top of its World.
FBE_API uint64_t fbe_entity_parent(uint64_t entity);
// How many children an entity has, -1 for a freed one.
FBE_API int32_t fbe_entity_child_count(uint64_t entity);
// An entity's child, in the order they became its children; 0 outside 0 to the count less one.
FBE_API uint64_t fbe_entity_child(uint64_t entity, int32_t index);
// Writes up to `capacity` of an entity's children, in order, and gives how many it has, -1 for a
// freed one.
FBE_API int32_t fbe_entity_children(uint64_t entity, uint64_t* children, int32_t capacity);
// The first entity of the name under an entity, depth first, each parent before its children,
// not the entity itself; 0 for none.
FBE_API uint64_t fbe_entity_find(uint64_t entity, const char* name);

// Hides an entity and what's under it, or shows it again.
FBE_API bool fbe_entity_set_hidden(uint64_t entity, bool hidden);
// Whether the entity itself is hidden.
FBE_API bool fbe_entity_hidden(uint64_t entity);
// Whether it and every entity above it are shown: what the scene draws, collides, and picks.
FBE_API bool fbe_entity_showing(uint64_t entity);

// An entity's transform, in FBE_SPACE_LOCAL, its parent's space, or FBE_SPACE_WORLD. Its matrix
// scales, then turns, then moves; in the world, it's its parent's times its own. Its world rotation
// is its parent's times its own, and its world scale its parent's times its own per axis, which
// a turned child of a parent scaled unevenly shears from. A world position goes through the
// parent's inverse, a world rotation and scale through the parent's.
FBE_API bool fbe_entity_set_position(uint64_t entity, float x, float y, float z, int32_t space);
FBE_API bool fbe_entity_position(uint64_t entity, int32_t space, fbe_vec3_t* position);
// Rotations are made length 1.
FBE_API bool fbe_entity_set_rotation(uint64_t entity, float x, float y, float z, float w,
                                     int32_t space);
FBE_API bool fbe_entity_rotation(uint64_t entity, int32_t space, fbe_quat_t* rotation);
FBE_API bool fbe_entity_set_scale(uint64_t entity, float x, float y, float z, int32_t space);
FBE_API bool fbe_entity_scale(uint64_t entity, int32_t space, fbe_vec3_t* scale);
// The entity's matrix, its own or the world's.
FBE_API bool fbe_entity_matrix(uint64_t entity, int32_t space, fbe_mat4_t* matrix);

// Moves an entity along its own axes, turned but not scaled by its rotation, in its parent's
// units.
FBE_API bool fbe_entity_move(uint64_t entity, float x, float y, float z);
// Moves an entity along its parent's axes, or the world's.
FBE_API bool fbe_entity_translate(uint64_t entity, float x, float y, float z, int32_t space);
// Turns an entity by a rotation about its own axes, or the world's.
FBE_API bool fbe_entity_turn(uint64_t entity, float x, float y, float z, float w, int32_t space);
// Turns an entity so its forward axis points at a place in the world, as upright as it goes, then
// rolls it about that axis; a place where it is changes nothing.
FBE_API bool fbe_entity_point_at(uint64_t entity, float x, float y, float z, float roll);
// Turns one of an entity's FBE_AXIS_ axes toward a direction in the world, by `rate` of the way,
// 0 to 1, the shortest way round; a zero direction changes nothing.
FBE_API bool fbe_entity_align(uint64_t entity, float x, float y, float z, int32_t axis, float rate);
// Transforms a point, vector, or normal (FBE_TRANSFORM_) from one entity's space into another's;
// 0 for either is the world's. False if `to` is scaled to nothing on an axis.
FBE_API bool fbe_entity_transform(uint64_t from, uint64_t to, int32_t kind, float x, float y,
                                  float z, fbe_vec3_t* result);

// Sets the poses of `count` entities in a space, in one call and in order, as physics moving
// bodies' entities does each frame; false if one was freed, after setting the rest.
FBE_API bool fbe_entity_set_poses(const uint64_t* entities, const fbe_pose_t* poses, int32_t count,
                                  int32_t space);
// Gives the poses of `count` entities in a space; false if one was freed, whose pose is zeros.
FBE_API bool fbe_entity_poses(const uint64_t* entities, fbe_pose_t* poses, int32_t count,
                              int32_t space);

// A camera looks along its forward axis, -Z, from where it is in the world, turned as it is, its
// scale left out. It projects with perspective, its field of view the angle from the bottom of its
// view to the top, and the horizontal one following its viewport's width over its height, or
// orthographically, its view `height` world units high; it sees from `nearest` to `farthest` in
// front of it. New, it's perspective, at a field of view of Pi / 3, 10 units high
// orthographically, sees from 0.1 to 1000, draws to the whole of its target, clears color, to
// black, and depth, and is in order 0.
FBE_API bool fbe_camera_set_projection(uint64_t camera, int32_t projection);
FBE_API int32_t fbe_camera_projection(uint64_t camera);
// False outside 0 to Pi, or so small the projection isn't finite.
FBE_API bool fbe_camera_set_field_of_view(uint64_t camera, float angle);
FBE_API float fbe_camera_field_of_view(uint64_t camera);
// False unless above 0, and big enough that the projection is finite.
FBE_API bool fbe_camera_set_orthographic_height(uint64_t camera, float height);
FBE_API float fbe_camera_orthographic_height(uint64_t camera);
// False unless 0 < nearest < farthest, finite.
FBE_API bool fbe_camera_set_range(uint64_t camera, float nearest, float farthest);
FBE_API float fbe_camera_nearest(uint64_t camera);
FBE_API float fbe_camera_farthest(uint64_t camera);

// Gives a camera a viewport of its own, in its target's units from the top left, or the whole of
// its target again, whatever size the target is.
FBE_API bool fbe_camera_set_viewport(uint64_t camera, float x, float y, float width, float height);
FBE_API bool fbe_camera_reset_viewport(uint64_t camera);
// The camera's viewport in a target of this size.
FBE_API bool fbe_camera_viewport(uint64_t camera, float target_width, float target_height,
                                 fbe_viewport_t* viewport);
// Whether the camera draws to the whole of its target.
FBE_API bool fbe_camera_whole_target(uint64_t camera);

// What a camera clears of its viewport before it draws: its color, to the clear color, and its
// depth. A camera that clears neither draws over the ones before it.
FBE_API bool fbe_camera_set_clear(uint64_t camera, bool color, bool depth);
FBE_API bool fbe_camera_clears_color(uint64_t camera);
FBE_API bool fbe_camera_clears_depth(uint64_t camera);
FBE_API bool fbe_camera_set_clear_color(uint64_t camera, float r, float g, float b, float a);
FBE_API bool fbe_camera_clear_color(uint64_t camera, fbe_color_t* color);
// What a camera draws into: a target with depth, which it holds, or 0, as at first, for the
// screen. Its viewport's in that target's pixels; false for another texture, or a freed one.
FBE_API bool fbe_camera_set_target(uint64_t camera, uint64_t texture);
FBE_API uint64_t fbe_camera_target(uint64_t camera);
// When a camera draws among its World's: lower orders first, and those of one order in the order
// they were made.
FBE_API bool fbe_camera_set_order(uint64_t camera, int32_t order);
FBE_API int32_t fbe_camera_order(uint64_t camera);
// A World's cameras, in the order they draw: writes up to `capacity` and gives how many there
// are, -1 for a freed World.
FBE_API int32_t fbe_world_cameras(uint64_t world, uint64_t* cameras, int32_t capacity);

// The matrix from the world to a camera's view, where it's at the origin looking down -Z.
FBE_API bool fbe_camera_view_matrix(uint64_t camera, fbe_mat4_t* matrix);
// The view matrix `tween` of the way, 0 to 1, from where the camera's World was last captured to
// now, as fbe_render_draws sees from; false for a freed camera or a tween outside 0 to 1.
FBE_API bool fbe_camera_tweened_view_matrix(uint64_t camera, float tween, fbe_mat4_t* matrix);
// A camera's projection in a target of this size, as std.math's Mat4.Perspective and
// Orthographic make it, with depth from 0 at nearest to 1 at farthest.
// The camera's projection as the engine draws with it, in a target of this size: Vulkan's clip
// space, y down, and depth reversed, 1 at its near plane and 0 at its far one, which the screen's
// depth is cleared to and compared GREATER_OR_EQUAL against. Project and the in-view tests agree
// with it.
FBE_API bool fbe_camera_draw_projection(uint64_t camera, float target_width, float target_height,
                                        fbe_mat4_t* matrix);
FBE_API bool fbe_camera_projection_matrix(uint64_t camera, float target_width, float target_height,
                                          fbe_mat4_t* matrix);
// Where a point in the world is in a camera's view, in a target of this size: x and y in its
// units from the target's top left, and z how far in front of the camera it is. 1 if it projected,
// 0 if it's nearer than the camera's nearest, so behind it, or the viewport has no size, as a
// window dragged to nothing high has; -1 for a freed camera. A viewport with no size has the
// shape of a square for the projection matrix and the in-view tests.
FBE_API int32_t fbe_camera_project(uint64_t camera, float target_width, float target_height,
                                   float x, float y, float z, fbe_vec3_t* projected);
// The point in the world at x and y in a camera's view, `depth` in front of it: 1 if there's one,
// 0 for a viewport with no size, -1 for a freed camera.
FBE_API int32_t fbe_camera_unproject(uint64_t camera, float target_width, float target_height,
                                     float x, float y, float depth, fbe_vec3_t* point);

// A light shines along its forward axis, -Z, as directional and spot lights do, or out from where
// it is, as point lights do; one that's hidden lights nothing. New, it's directional, white, of
// intensity 1, of range 10, with a cone from 0 to Pi / 4 from its axis. A color can be negative,
// which takes light away.
FBE_API bool fbe_light_set_type(uint64_t light, int32_t type);
FBE_API int32_t fbe_light_type(uint64_t light);
FBE_API bool fbe_light_set_color(uint64_t light, float r, float g, float b);
FBE_API bool fbe_light_color(uint64_t light, fbe_color_t* color);
// False below 0.
FBE_API bool fbe_light_set_intensity(uint64_t light, float intensity);
FBE_API float fbe_light_intensity(uint64_t light);
// How far a point or spot light reaches, in its shading model's meaning: per pixel, where it ends,
// the inverse square windowed to 0 there; per vertex, FBE_SHADING_VERTEX, where it starts to fall,
// as range over distance, with no end. False unless above 0.
FBE_API bool fbe_light_set_range(uint64_t light, float range);
FBE_API float fbe_light_range(uint64_t light);
// A spot light's cone: the angle from its axis where it starts to fade, and where it's out;
// false unless 0 <= inner <= outer <= Pi / 2.
FBE_API bool fbe_light_set_cone(uint64_t light, float inner, float outer);
FBE_API float fbe_light_inner_cone(uint64_t light);
FBE_API float fbe_light_outer_cone(uint64_t light);
// A World's lights, hidden ones too, in the tree's order, depth first: writes up to `capacity` and
// gives how many there are, -1 for a freed World.
FBE_API int32_t fbe_world_lights(uint64_t world, uint64_t* lights, int32_t capacity);
// A World's ambient light, which lights everything a little from everywhere: std.math's
// Color.Gray, sRGB's half gray, at first.
FBE_API bool fbe_world_set_ambient(uint64_t world, float r, float g, float b);
FBE_API bool fbe_world_ambient(uint64_t world, fbe_color_t* color);

// A camera's fog, which blends what it sees toward the fog's color with its depth in front of the
// camera: FBE_FOG_NONE at first, black, linear from 1 to 100, at a density of 0.02.
FBE_API bool fbe_camera_set_fog_mode(uint64_t camera, int32_t mode);
FBE_API int32_t fbe_camera_fog_mode(uint64_t camera);
FBE_API bool fbe_camera_set_fog_color(uint64_t camera, float r, float g, float b);
FBE_API bool fbe_camera_fog_color(uint64_t camera, fbe_color_t* color);
// Where linear fog starts and where it's whole; false unless 0 <= start < end, finite.
FBE_API bool fbe_camera_set_fog_range(uint64_t camera, float start, float end);
FBE_API float fbe_camera_fog_start(uint64_t camera);
FBE_API float fbe_camera_fog_end(uint64_t camera);
// How thick exponential fog is; false below 0.
FBE_API bool fbe_camera_set_fog_density(uint64_t camera, float density);
FBE_API float fbe_camera_fog_density(uint64_t camera);

// Geometry: surfaces of vertices and triangles, counterclockwise seen from the front, which meshes
// draw and share. A geometry lives while its handle is held, as fbe_geometry_create and
// fbe_geometry_retain hold it, and fbe_geometry_release lets go, or a mesh draws it; a released
// handle that a mesh still draws stays good, for fbe_mesh_geometry to give and retain again.
FBE_API uint64_t fbe_geometry_create(void);
FBE_API bool fbe_geometry_retain(uint64_t geometry);
FBE_API bool fbe_geometry_release(uint64_t geometry);
FBE_API bool fbe_geometry_alive(uint64_t geometry);
// A new geometry with copies of another's surfaces, held once.
FBE_API uint64_t fbe_geometry_clone(uint64_t geometry);
// What a geometry's known to be made of, as a scene file names it, "" for what isn't known, as one
// that's changed since it was set, or was freed. The text is the geometry's until it's set again.
FBE_API bool fbe_geometry_set_source(uint64_t geometry, const char* source);
FBE_API const char* fbe_geometry_source(uint64_t geometry);
FBE_API bool fbe_geometry_set_scene_id(uint64_t geometry, const char* id);
FBE_API const char* fbe_geometry_scene_id(uint64_t geometry);
// How many times the geometry has changed, which collision trees use to know they're stale.
FBE_API uint64_t fbe_geometry_version(uint64_t geometry);

FBE_API int32_t fbe_geometry_surface_count(uint64_t geometry);
// Adds an empty surface at the end and gives its index, -1 for a freed geometry.
FBE_API int32_t fbe_geometry_add_surface(uint64_t geometry);
// Adds copies of another geometry's surfaces at the end, each turned by `transform`, and its
// normals with it; `other` can be the geometry itself.
FBE_API bool fbe_geometry_add(uint64_t geometry, uint64_t other, const fbe_mat4_t* transform);
// Transforms every vertex, and its normal through the matrix's inverse transpose, made length 1;
// one that flips turns the triangles too, so they still face out.
FBE_API bool fbe_geometry_transform(uint64_t geometry, const fbe_mat4_t* transform);
// Turns it inside out: every normal negated, and every triangle's winding.
FBE_API bool fbe_geometry_flip(uint64_t geometry);
// Gives every vertex the normal of the triangles around it, smoothing across vertices of a
// surface at the same place, each triangle weighted by its angle at the vertex; triangles too
// thin to face anywhere are left out. Where a place's triangles cancel, as a sheet's two sides do,
// a vertex takes its own triangles', and one in no triangle keeps its normal.
FBE_API bool fbe_geometry_update_normals(uint64_t geometry);
// The box around its vertices; nothing at the origin for none.
FBE_API bool fbe_geometry_bounds(uint64_t geometry, fbe_aabb_t* bounds);
// Scales and moves it to fill a box, centered, or keeping its shape with `uniform`; a flat axis
// is moved, not scaled.
FBE_API bool fbe_geometry_fit(uint64_t geometry, const fbe_aabb_t* box, bool uniform);
// Every place a vertex is, welded: once each, bit for bit, -0 as 0, in the order surfaces first
// use them, as physics' mesh shapes take them, so their triangles meet across UV and normal seams
// and what Add joined: writes up to `capacity` and gives how many there are, -1 for a freed
// geometry.
FBE_API int32_t fbe_geometry_positions(uint64_t geometry, fbe_vec3_t* positions, int32_t capacity);
// Every triangle's three indices into what fbe_geometry_positions gives, surface after surface, in
// their order, so a triangle's number is physics' too: writes up to `capacity` indices and gives
// how many there are.
FBE_API int32_t fbe_geometry_indices(uint64_t geometry, uint32_t* indices, int32_t capacity);
// Primitive shapes, each one surface, new and held once, of radius 1 about the origin: a cube from
// -1 to 1, flat-shaded, each face's UVs 0 to 1; a sphere of `segments` rings from pole to pole
// and twice as many around, 3 to 100; a cylinder and a cone from y -1 to 1, `segments` around, 3
// to 100, with caps when `capped`, the cone's point at the top; and a plane from -1 to 1 in X and
// Z, facing up, `segments` squares along each side, 1 to 100. 0 for segments out of range.
FBE_API uint64_t fbe_geometry_cube(void);
FBE_API uint64_t fbe_geometry_sphere(int32_t segments);
FBE_API uint64_t fbe_geometry_cylinder(int32_t segments, bool capped);
FBE_API uint64_t fbe_geometry_cone(int32_t segments, bool capped);
FBE_API uint64_t fbe_geometry_plane(int32_t segments);

// A surface's vertices and triangles, by the geometry and the surface's index.
// Adds `count` vertices and gives the first's index, -1 if it can't.
FBE_API int32_t fbe_surface_add_vertices(uint64_t geometry, int32_t surface,
                                         const fbe_vertex_t* vertices, int32_t count);
// Adds `count` indices, three to a triangle, each of a vertex the surface has, and gives the first
// triangle's index, -1 if it can't.
FBE_API int32_t fbe_surface_add_triangles(uint64_t geometry, int32_t surface,
                                          const uint32_t* indices, int32_t count);
// Sets a surface's vertices' joints, `count` of them, one for each vertex, or none with 0, which
// drawing skins it by when its mesh has a skin; false for another count. A vertex with no weight
// at all is on FBE_NO_JOINT, whole. Vertices added since or cleared let go of them. Reading them
// writes up to `capacity` and gives how many there are, -1 for a freed geometry.
FBE_API bool fbe_surface_set_joints(uint64_t geometry, int32_t surface,
                                    const fbe_vertex_joints_t* joints, int32_t count);
FBE_API int32_t fbe_surface_joints(uint64_t geometry, int32_t surface, fbe_vertex_joints_t* joints,
                                   int32_t capacity);
FBE_API int32_t fbe_surface_vertex_count(uint64_t geometry, int32_t surface);
FBE_API int32_t fbe_surface_triangle_count(uint64_t geometry, int32_t surface);
// Reads or sets `count` vertices from `first`; false if they aren't all there.
FBE_API bool fbe_surface_vertices(uint64_t geometry, int32_t surface, int32_t first,
                                  fbe_vertex_t* vertices, int32_t count);
FBE_API bool fbe_surface_set_vertices(uint64_t geometry, int32_t surface, int32_t first,
                                      const fbe_vertex_t* vertices, int32_t count);
// Reads `count` triangles' indices from triangle `first`; false if they aren't all there.
FBE_API bool fbe_surface_triangles(uint64_t geometry, int32_t surface, int32_t first,
                                   uint32_t* indices, int32_t count);
// Takes away a surface's triangles, and its vertices too with `vertices`.
FBE_API bool fbe_surface_clear(uint64_t geometry, int32_t surface, bool vertices);

// A mesh draws a geometry, or nothing with none; copies of a mesh draw the same one.
FBE_API bool fbe_mesh_set_geometry(uint64_t mesh, uint64_t geometry);
// The geometry a mesh draws, 0 for none; its handle is good while the mesh draws it.
FBE_API uint64_t fbe_mesh_geometry(uint64_t mesh);

// Materials: how surfaces look, shared by every surface and mesh drawing in one, which hold it as
// geometry is held, by handle and by what draws in it. New, a material is white, opaque, with a
// shininess of 0, FBE_SHADING_PBR, FBE_BLEND_AUTO, no alpha cutoff, no vertex colors, one-sided,
// fogged, in order 0, its layers multiplying, each reading UV set 0.
FBE_API uint64_t fbe_material_create(void);
FBE_API bool fbe_material_retain(uint64_t material);
FBE_API bool fbe_material_release(uint64_t material);
FBE_API bool fbe_material_alive(uint64_t material);
// A new material with another's settings, held once.
FBE_API uint64_t fbe_material_clone(uint64_t material);
// Its color and alpha, linear: any finite color, and alpha 0 to 1.
FBE_API bool fbe_material_set_color(uint64_t material, float r, float g, float b, float a);
FBE_API bool fbe_material_color(uint64_t material, fbe_color_t* color);
// How sharp and bright its highlights are, 0 for none to 1; false outside.
FBE_API bool fbe_material_set_shininess(uint64_t material, float shininess);
FBE_API float fbe_material_shininess(uint64_t material);
FBE_API bool fbe_material_set_shading(uint64_t material, int32_t shading);
FBE_API int32_t fbe_material_shading(uint64_t material);
FBE_API bool fbe_material_set_blend(uint64_t material, int32_t blend);
FBE_API int32_t fbe_material_blend(uint64_t material);
// The alpha below which a pixel isn't drawn at all, as a mask's holes want, 0 for none, to 1.
FBE_API bool fbe_material_set_alpha_cutoff(uint64_t material, float cutoff);
FBE_API float fbe_material_alpha_cutoff(uint64_t material);
// Whether vertices' colors and alphas multiply its own.
FBE_API bool fbe_material_set_vertex_colors(uint64_t material, bool vertex_colors);
FBE_API bool fbe_material_vertex_colors(uint64_t material);
// Whether triangles are drawn from behind too.
FBE_API bool fbe_material_set_two_sided(uint64_t material, bool two_sided);
FBE_API bool fbe_material_two_sided(uint64_t material);
// Whether its triangles are drawn as lines, where the GPU can; filled, where it can't.
FBE_API bool fbe_material_set_wireframe(uint64_t material, bool wireframe);
FBE_API bool fbe_material_wireframe(uint64_t material);
// Whether cameras' fog covers it.
FBE_API bool fbe_material_set_fogged(uint64_t material, bool fogged);
FBE_API bool fbe_material_fogged(uint64_t material);
// When it's drawn: 0 with the scene; below 0 before the scene, and above 0 after it, lower first,
// as skies and overlays want. Orders but 0 neither test nor write depth, and are drawn furthest
// first, so a sky is behind everything and an overlay over it.
FBE_API bool fbe_material_set_order(uint64_t material, int32_t order);
FBE_API int32_t fbe_material_order(uint64_t material);
FBE_API bool fbe_material_set_scene_id(uint64_t material, const char* id);
FBE_API const char* fbe_material_scene_id(uint64_t material);
// A texture layer's texture, which it holds, 0 for none, for layers 0 to FBE_MATERIAL_LAYERS - 1:
// what's drawn is the lit color with each layer that has a texture over it, in order.
FBE_API bool fbe_material_set_texture(uint64_t material, int32_t layer, uint64_t texture);
FBE_API uint64_t fbe_material_texture(uint64_t material, int32_t layer);
// What a layer's UVs go through: scaled down, so a scale of 2 looks twice as big, turned back by an
// angle in radians, then moved back by an offset in UV units, as Blitz's ScaleTexture,
// RotateTexture, and PositionTexture; false for a scale of 0, or anything not finite. Reading them
// writes the five values in that order.
FBE_API bool fbe_material_set_layer_uv(uint64_t material, int32_t layer, float scale_u,
                                       float scale_v, float angle, float offset_u, float offset_v);
FBE_API bool fbe_material_layer_uv(uint64_t material, int32_t layer, float* values);
// What an FBE_LAYER_BUMP layer's map does to the next layer: its red and green, centered on middle
// gray, -1 to 1, shift that layer's UVs by the matrix's u = m00 r + m10 g and v = m01 r + m11 g,
// and its blue times `scale` plus `offset`, 0 to 1, scales that layer's color: as Direct3D's
// BUMPENVMAPLUMINANCE, none of it at first. Reading it writes the six values in that order.
FBE_API bool fbe_material_set_layer_bump(uint64_t material, int32_t layer, float m00, float m01,
                                         float m10, float m11, float scale, float offset);
FBE_API bool fbe_material_layer_bump(uint64_t material, int32_t layer, float* values);
// A texture layer's FBE_LAYER_ blend and UV set, 0 or 1, for layers 0 to FBE_MATERIAL_LAYERS - 1.
FBE_API bool fbe_material_set_layer(uint64_t material, int32_t layer, int32_t blend,
                                    int32_t uv_set);
FBE_API int32_t fbe_material_layer_blend(uint64_t material, int32_t layer);
FBE_API int32_t fbe_material_layer_uv_set(uint64_t material, int32_t layer);

// The material a surface draws in, which it holds, 0 for the default one.
FBE_API bool fbe_surface_set_material(uint64_t geometry, int32_t surface, uint64_t material);
FBE_API uint64_t fbe_surface_material(uint64_t geometry, int32_t surface);
// A material a mesh draws all its surfaces in instead of their own, which it holds, 0 for none.
FBE_API bool fbe_mesh_set_material(uint64_t mesh, uint64_t material);
FBE_API uint64_t fbe_mesh_material(uint64_t mesh);
// A mesh's own color and alpha, which multiply its materials', white at first: each mesh's,
// copies' too, so a copy fades or tints alone. Alpha is 0 to 1, and colors any finite value.
FBE_API bool fbe_mesh_set_tint(uint64_t mesh, float r, float g, float b, float a);
FBE_API bool fbe_mesh_tint(uint64_t mesh, fbe_color_t* tint);
// The FBE_BLEND_ a mesh's surface is drawn with: its material's, the mesh's own or the surface's,
// with AUTO and OPAQUE settled by alpha as their docs say; -1 for a surface it doesn't have.
FBE_API int32_t fbe_mesh_blend(uint64_t mesh, int32_t surface);

// Sprites: entities that draw a quad, from (-1, -1) to (1, 1) in its own plane before its size,
// its front facing +Z, in a material or white with none, turned toward the camera drawing it by
// its view mode. FBE_SPRITE_BILLBOARD, 1 by 1, its middle at its place, unrolled, white, at first.
//
// The material it draws in, which it holds, 0 for none, and its own color and alpha, which
// multiply the material's, as a mesh's tint does: alpha 0 to 1, colors any finite value.
FBE_API bool fbe_sprite_set_material(uint64_t sprite, uint64_t material);
FBE_API uint64_t fbe_sprite_material(uint64_t sprite);
FBE_API bool fbe_sprite_set_tint(uint64_t sprite, float r, float g, float b, float a);
FBE_API bool fbe_sprite_tint(uint64_t sprite, fbe_color_t* tint);
// How it turns toward the camera, an FBE_SPRITE_ mode; -1 for an entity that isn't a sprite.
FBE_API bool fbe_sprite_set_view_mode(uint64_t sprite, int32_t mode);
FBE_API int32_t fbe_sprite_view_mode(uint64_t sprite);
// The quad's half-width and half-height, 0 or more, apart from the entity's scale, which only
// FBE_SPRITE_FIXED takes.
FBE_API bool fbe_sprite_set_size(uint64_t sprite, float half_width, float half_height);
FBE_API bool fbe_sprite_size(uint64_t sprite, fbe_vec2_t* size);
// Which point of the quad is at the entity's place, in its own -1 to 1, y up: (0, -1) is its
// bottom middle.
FBE_API bool fbe_sprite_set_handle(uint64_t sprite, float x, float y);
FBE_API bool fbe_sprite_handle(uint64_t sprite, fbe_vec2_t* handle);
// How far the quad is turned in its own plane, in radians, counterclockwise as seen.
FBE_API bool fbe_sprite_set_roll(uint64_t sprite, float radians);
FBE_API float fbe_sprite_roll(uint64_t sprite);
// The quad's matrix in the world as a camera, or any entity, sees it: its corners (+-1, +-1, 0)
// through it are where it's drawn.
FBE_API bool fbe_sprite_matrix(uint64_t sprite, uint64_t camera, fbe_mat4_t* matrix);

// Ground planes: entities that are an infinite plane at their own y of 0, facing their +Y, which
// collisions touch as that plane when their collision shape is FBE_COLLISION_AUTO or
// FBE_COLLISION_MESH, and picks of FBE_PICK_MESH hit, both from above only; they're drawn out to
// the camera's far plane. Their material, which they hold, 0 for none; their own color and alpha,
// as a mesh's tint; and the grid they're drawn in, 1 to 16 segments a side, 1 at first.
FBE_API bool fbe_plane_set_material(uint64_t plane, uint64_t material);
FBE_API uint64_t fbe_plane_material(uint64_t plane);
FBE_API bool fbe_plane_set_tint(uint64_t plane, float r, float g, float b, float a);
FBE_API bool fbe_plane_tint(uint64_t plane, fbe_color_t* tint);
FBE_API bool fbe_plane_set_segments(uint64_t plane, int32_t segments);
FBE_API int32_t fbe_plane_segments(uint64_t plane);

// Terrains: a grid of squares, size by size, in their own space from (0, 0) to (size, size) across
// X and Z, each corner at its own height up Y, 0 at first, two triangles a square facing up.
// Collisions touch their triangles when their collision shape is FBE_COLLISION_AUTO or
// FBE_COLLISION_MESH, and picks of FBE_PICK_MESH hit them, from above; they're drawn. Made 1 square
// a side; a new size, 1 to 4096, makes every height 0 again.
FBE_API bool fbe_terrain_set_size(uint64_t terrain, int32_t size);
FBE_API int32_t fbe_terrain_size(uint64_t terrain);
// A corner's height, x and z 0 to its size; NaN for one it hasn't. Heights are any finite value.
FBE_API bool fbe_terrain_set_height(uint64_t terrain, int32_t x, int32_t z, float height);
FBE_API float fbe_terrain_height(uint64_t terrain, int32_t x, int32_t z);
// Every corner's height at once, row by row along Z, x across each: (size + 1) squared of them.
FBE_API bool fbe_terrain_set_heights(uint64_t terrain, const float* heights, int32_t count);
// The point on a terrain's surface above or below a point in the world, along its own Y, and the
// surface's normal there, both in the world: 1 and the point, 0 off its grid, -1 for a freed one.
FBE_API int32_t fbe_terrain_surface(uint64_t terrain, float x, float y, float z, fbe_vec3_t* point,
                                    fbe_vec3_t* normal);
// Its material, which it holds, 0 for none, and its own color and alpha, as a mesh's tint.
FBE_API bool fbe_terrain_set_material(uint64_t terrain, uint64_t material);
FBE_API uint64_t fbe_terrain_material(uint64_t terrain);
FBE_API bool fbe_terrain_set_tint(uint64_t terrain, float r, float g, float b, float a);
FBE_API bool fbe_terrain_tint(uint64_t terrain, fbe_color_t* tint);

// Mirrors: entities that reflect the scene through their own XZ plane, as fbe_render_camera draws
// them. The matrix that reflects the world through a mirror's plane, which a camera's view takes
// first to draw what's in it.
FBE_API bool fbe_mirror_reflection(uint64_t mirror, fbe_mat4_t* matrix);

// How far from a camera an entity fades out, for a mesh or a sprite: whole nearer than `start`, not
// drawn past `end`, and its alpha times 1 - (d - start) / (end - start) between, d from the
// camera's place to its own; 0 and 0, at first, for never. False unless 0 <= start < end, finite,
// or both 0.
FBE_API bool fbe_entity_set_auto_fade(uint64_t entity, float start, float end);
FBE_API bool fbe_entity_auto_fade(uint64_t entity, fbe_vec2_t* fade);
// The alpha an entity's fade gives it seen from `camera`, 0 to 1; -1 for a freed one.
FBE_API float fbe_entity_fade(uint64_t entity, uint64_t camera);

// Whether any of two meshes' triangles cross, in the world, either side: 1 if they do, 0 if not or
// either draws nothing, -1 for a freed one or one that isn't a mesh. Triangles flat on each other
// touch, they don't cross.
FBE_API int32_t fbe_mesh_intersects(uint64_t mesh, uint64_t other);

// The box a geometry's meshes are culled by when drawn, in its own space, instead of its bounds,
// as a skinned mesh's, whose vertices move out of them, wants; InView and Bounds keep its bounds.
// False for a backward box. fbe_geometry_cull_box gives false while it has none.
FBE_API bool fbe_geometry_set_cull_box(uint64_t geometry, const fbe_aabb_t* box);
FBE_API bool fbe_geometry_clear_cull_box(uint64_t geometry);
FBE_API bool fbe_geometry_cull_box(uint64_t geometry, fbe_aabb_t* box);

// Visibility. Whether a camera might see an entity, in a target of this size: a mesh's box of its
// geometry, turned and scaled with it, a sprite's box of its reach, however it turns, a ground
// plane unless the camera's whole view is under it, or another entity's position, against the six
// planes of the camera's view, near and far included; in view unless the whole box is behind one
// plane, so something just past a corner of the view can count, as culling allows. Only the frustum
// counts: nothing in front hides it, and hiding the entity doesn't; an entity in another World
// than the camera's isn't in view. 1 in view, 0 not, -1 for a freed camera or entity.
FBE_API int32_t fbe_camera_entity_in_view(uint64_t camera, float target_width, float target_height,
                                          uint64_t entity);
FBE_API int32_t fbe_camera_point_in_view(uint64_t camera, float target_width, float target_height,
                                         float x, float y, float z);
FBE_API int32_t fbe_camera_box_in_view(uint64_t camera, float target_width, float target_height,
                                       const fbe_aabb_t* box);
// The box in the world around an entity: a mesh's geometry's, turned and scaled with it, or its
// position, as a box of no size.
FBE_API bool fbe_entity_bounds(uint64_t entity, fbe_aabb_t* bounds);
// Whether an entity blocks line of sight: a mesh by its triangles, from either side, while it's
// showing. No entity blocks it at first.
FBE_API bool fbe_entity_set_blocks_sight(uint64_t entity, bool blocks);
FBE_API bool fbe_entity_blocks_sight(uint64_t entity);
// Whether nothing that blocks sight in its World is between an entity's position and a point,
// or another entity's position; the two themselves don't block it, and an entity in another
// World can't be seen. 1 if it can see, 0 if not, -1 for a freed entity.
FBE_API int32_t fbe_entity_can_see_point(uint64_t entity, float x, float y, float z);
FBE_API int32_t fbe_entity_can_see(uint64_t entity, uint64_t target);

// Collisions. An entity in a collision group that its World says collides with another moves as an
// ellipsoid, swept from where its last move ended to where it is, against every showing entity of
// a group it collides with, and the response its World gives the pair says what it does when it
// touches one; what it touches gets the contact too. The other entities are where they are now,
// and moving ones where their last move ended; their own moves aren't swept, so a moving door
// pushes nothing, but a mover it closed on is pushed out of it at the World's next collide,
// moving or not, only as far as nothing else is; between two facing each other, its middle goes to
// the middle of the gap, so a squeezed one is in neither, unless one closes faster than its radius
// in a frame and passes over its middle first. A mover already within its radius of a face, an
// edge, or a corner touches it only going further in. A slide along ground, within the mover's max
// slope of level, may raise it as far as the slope goes; along anything steeper, edges and corners
// too, no more than the move's own rise and its step height. Each move is worked out on its own;
// what it touches at one time goes by the order those entities were made.
//
// An entity's group, 0 for none, at first. Setting one forgets where its last move ended.
FBE_API bool fbe_entity_set_collision_group(uint64_t entity, int32_t group);
FBE_API int32_t fbe_entity_collision_group(uint64_t entity);
// Its ellipsoid as a mover, and as a target of FBE_COLLISION_ELLIPSOID: `horizontal` across X and Z
// and `vertical` up Y, in world units whatever its scale, 1 and 1 at first; false unless above 0.
FBE_API bool fbe_entity_set_collision_radius(uint64_t entity, float horizontal, float vertical);
FBE_API bool fbe_entity_collision_radius(uint64_t entity, fbe_vec2_t* radius);
// What it's touched as, FBE_COLLISION_AUTO at first.
FBE_API bool fbe_entity_set_collision_shape(uint64_t entity, int32_t shape);
FBE_API int32_t fbe_entity_collision_shape(uint64_t entity);
// Its box as a target of FBE_COLLISION_BOX, in its own space, turned and scaled with it, -1 to 1
// at first; false unless its least corner is at or below its greatest.
FBE_API bool fbe_entity_set_collision_box(uint64_t entity, const fbe_aabb_t* box);
FBE_API bool fbe_entity_collision_box(uint64_t entity, fbe_aabb_t* box);
// How high a sliding mover steps up onto what blocks it, onto ground no higher, and how much
// higher than its move rises a slide along what's steeper than its max slope may take it, as over
// an edge; 0, at first, for neither.
FBE_API bool fbe_entity_set_step_height(uint64_t entity, float height);
FBE_API float fbe_entity_step_height(uint64_t entity);
// How steep a slope a mover climbs as ground, in radians from level, 0 to Pi / 2, Pi / 4 at
// first; false for any other.
FBE_API bool fbe_entity_set_max_slope(uint64_t entity, float radians);
FBE_API float fbe_entity_max_slope(uint64_t entity);
// Forgets where its last move ended, and what it touched, so its next move isn't swept, as a
// teleport wants.
FBE_API bool fbe_entity_reset_collisions(uint64_t entity);
// What it touched in its World's last fbe_world_collide, as a mover or as what was touched: writes
// up to `capacity` and gives how many there are, -1 for a freed entity.
FBE_API int32_t fbe_entity_collisions(uint64_t entity, fbe_collision_t* collisions,
                                      int32_t capacity);

// What a source group does when it touches a target group, FBE_RESPONSE_NONE for every pair at
// first.
FBE_API bool fbe_world_set_collision(uint64_t world, int32_t source, int32_t target,
                                     int32_t response);
FBE_API int32_t fbe_world_collision(uint64_t world, int32_t source, int32_t target);
// Makes every pair FBE_RESPONSE_NONE again.
FBE_API bool fbe_world_clear_collisions(uint64_t world);
// The most times one move touches something and goes on, 1 to 64, 8 at first; a move that has
// touched that many times ends where it got to, pushed out of what it's in if that's less in, or
// where it began if it's further in than there.
FBE_API bool fbe_world_set_collision_iterations(uint64_t world, int32_t iterations);
FBE_API int32_t fbe_world_collision_iterations(uint64_t world);
// How far a mover stays from what it touches, so it doesn't sink in, 0 to 0.1, 0.001 at first.
// Long walks among turned solids measured a mover at most 0.00004 in them, at the first skin.
FBE_API bool fbe_world_set_collision_skin(uint64_t world, float skin);
FBE_API float fbe_world_collision_skin(uint64_t world);
// Moves every moving entity of the World as its collisions say, from where its last move ended to
// where it is, and keeps what each touched.
FBE_API bool fbe_world_collide(uint64_t world);

// Picks. A pick is a line, or a sphere swept along one, from one point to another, which hits the
// nearest showing entity of its World with a pick mode, as collisions touch it: its ellipsoid grown
// by the pick's radius, its box, turned and scaled with it, or its mesh's triangles, from their
// fronts. One starting in what it would hit goes out through it, and one that began behind a
// triangle passes out through it untouched till it's its radius in front; with a radius, the point
// is where the sphere touched.
//
// What picks hit an entity as, FBE_PICK_NONE at first.
FBE_API bool fbe_entity_set_pick_mode(uint64_t entity, int32_t mode);
FBE_API int32_t fbe_entity_pick_mode(uint64_t entity);
// The nearest hit in a World from `from` to `to` of a sphere of `radius` swept along the line, or
// the line with 0, never hitting `ignore` (0 for none): 1 with the hit written, 0 for no hit, -1
// for a freed World or entity, a radius below 0, or a point that isn't finite.
FBE_API int32_t fbe_world_pick(uint64_t world, const fbe_vec3_t* from, const fbe_vec3_t* to,
                               float radius, uint64_t ignore, fbe_pick_t* pick);

// Keeps every entity of a World's transform, its local position, rotation, and scale, as it is
// now, for drawing between then and later: a fixed step captures before each step, and draws
// part way between the last two.
FBE_API bool fbe_world_capture(uint64_t world);
// An entity's matrix to the world `tween` of the way, 0 to 1, from its World's last capture to now,
// each entity above it tweened too: positions and scales lerped, rotations slerped. One made since
// the capture is where it is now. False for a tween outside 0 to 1.
FBE_API bool fbe_entity_tweened_matrix(uint64_t entity, float tween, fbe_mat4_t* matrix);

// Copies an entity and everything under it, as the last child of `parent`, or at the top of
// `world` when `parent` is 0; each copy has its original's name, transform, and settings, and
// shares its geometry. 0 if it can't be made.
FBE_API uint64_t fbe_entity_copy(uint64_t entity, uint64_t world, uint64_t parent);

#ifdef __cplusplus
}
#endif

#endif
