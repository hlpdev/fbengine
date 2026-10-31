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
#define FBE_ENTITY_PIVOT  0
#define FBE_ENTITY_CAMERA 1
#define FBE_ENTITY_LIGHT  2
#define FBE_ENTITY_MESH   3

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
// its end; or as e to the minus density times the depth, or that squared.
#define FBE_FOG_NONE                0
#define FBE_FOG_LINEAR              1
#define FBE_FOG_EXPONENTIAL         2
#define FBE_FOG_EXPONENTIAL_SQUARED 3

// How a material is lit: physically, as glTF's metal and roughness have it; not at all, its color
// as it is; or per vertex, as fixed-function hardware lit, for an old look.
#define FBE_SHADING_PBR    0
#define FBE_SHADING_UNLIT  1
#define FBE_SHADING_VERTEX 2

// How a material blends with what's drawn behind it: opaque unless its alpha, or its mesh's, is
// below 1, then alpha; opaque; alpha; adding its light; or multiplying what's behind.
#define FBE_BLEND_AUTO     0
#define FBE_BLEND_OPAQUE   1
#define FBE_BLEND_ALPHA    2
#define FBE_BLEND_ADD      3
#define FBE_BLEND_MULTIPLY 4

// How a texture layer combines with the layers under it: multiplying them, adding to them,
// laid over them by its alpha, or multiplying them twice over, so its middle gray leaves them as
// they are, as lightmaps over a 2x ambient layer want.
#define FBE_LAYER_MULTIPLY   0
#define FBE_LAYER_ADD        1
#define FBE_LAYER_ALPHA      2
#define FBE_LAYER_MULTIPLY_2 3

// How many texture layers a material has.
#define FBE_MATERIAL_LAYERS 8

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

// An entity's name, "" at first; what fbe_entity_name gives stays until the name changes or the
// entity is freed, and is NULL for a freed one.
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
// False outside 0 to Pi.
FBE_API bool fbe_camera_set_field_of_view(uint64_t camera, float angle);
FBE_API float fbe_camera_field_of_view(uint64_t camera);
// False unless above 0.
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
// When a camera draws among its World's: lower orders first, and those of one order in the order
// they were made.
FBE_API bool fbe_camera_set_order(uint64_t camera, int32_t order);
FBE_API int32_t fbe_camera_order(uint64_t camera);
// A World's cameras, in the order they draw: writes up to `capacity` and gives how many there
// are, -1 for a freed World.
FBE_API int32_t fbe_world_cameras(uint64_t world, uint64_t* cameras, int32_t capacity);

// The matrix from the world to a camera's view, where it's at the origin looking down -Z.
FBE_API bool fbe_camera_view_matrix(uint64_t camera, fbe_mat4_t* matrix);
// A camera's projection in a target of this size, as std.math's Mat4.Perspective and
// Orthographic make it, with depth from 0 at nearest to 1 at farthest; false for a viewport with
// no size.
FBE_API bool fbe_camera_projection_matrix(uint64_t camera, float target_width, float target_height,
                                          fbe_mat4_t* matrix);
// Where a point in the world is in a camera's view, in a target of this size: x and y in its
// units from the target's top left, and z how far in front of the camera it is. 1 if it projected,
// 0 if it's nearer than the camera's nearest, so behind it, and -1 for a viewport with no size.
FBE_API int32_t fbe_camera_project(uint64_t camera, float target_width, float target_height,
                                   float x, float y, float z, fbe_vec3_t* projected);
// The point in the world at x and y in a camera's view, `depth` in front of it.
FBE_API bool fbe_camera_unproject(uint64_t camera, float target_width, float target_height, float x,
                                  float y, float depth, fbe_vec3_t* point);

// A light shines along its forward axis, -Z, as directional and spot lights do, or out from where
// it is, as point lights do; one that's hidden lights nothing. New, it's directional, white, of
// intensity 1, reaching 10 units, with a cone from 0 to Pi / 4 from its axis. A color can be
// negative, which takes light away.
FBE_API bool fbe_light_set_type(uint64_t light, int32_t type);
FBE_API int32_t fbe_light_type(uint64_t light);
FBE_API bool fbe_light_set_color(uint64_t light, float r, float g, float b);
FBE_API bool fbe_light_color(uint64_t light, fbe_color_t* color);
// False below 0.
FBE_API bool fbe_light_set_intensity(uint64_t light, float intensity);
FBE_API float fbe_light_intensity(uint64_t light);
// How far a point or spot light reaches, where it's faded to nothing; false unless above 0.
FBE_API bool fbe_light_set_range(uint64_t light, float range);
FBE_API float fbe_light_range(uint64_t light);
// A spot light's cone: the angle from its axis where it starts to fade, and where it's out;
// false unless 0 <= inner <= outer <= Pi / 2.
FBE_API bool fbe_light_set_cone(uint64_t light, float inner, float outer);
FBE_API float fbe_light_inner_cone(uint64_t light);
FBE_API float fbe_light_outer_cone(uint64_t light);
// A World's lights, in the tree's order, depth first: writes up to `capacity` and gives how many
// there are, -1 for a freed World.
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
// surface at the same place, each triangle weighted by its angle at the vertex.
FBE_API bool fbe_geometry_update_normals(uint64_t geometry);
// The box around its vertices; nothing at the origin for none.
FBE_API bool fbe_geometry_bounds(uint64_t geometry, fbe_aabb_t* bounds);
// Scales and moves it to fill a box, centered, or keeping its shape with `uniform`; a flat axis
// is moved, not scaled.
FBE_API bool fbe_geometry_fit(uint64_t geometry, const fbe_aabb_t* box, bool uniform);
// Every vertex's position, surface after surface, as physics' mesh shapes take them: writes up to
// `capacity` and gives how many there are, -1 for a freed geometry.
FBE_API int32_t fbe_geometry_positions(uint64_t geometry, fbe_vec3_t* positions, int32_t capacity);
// Every triangle's three indices into what fbe_geometry_positions gives: writes up to `capacity`
// indices and gives how many there are.
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
// Its color and alpha, linear.
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
// Whether cameras' fog covers it.
FBE_API bool fbe_material_set_fogged(uint64_t material, bool fogged);
FBE_API bool fbe_material_fogged(uint64_t material);
// When it's drawn: 0 with the scene, by depth; below 0 before the scene, and above 0 after it,
// lower first, each over what's drawn before it, with no depth test, as skies and overlays want.
FBE_API bool fbe_material_set_order(uint64_t material, int32_t order);
FBE_API int32_t fbe_material_order(uint64_t material);
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
// copies' too, so a copy fades or tints alone.
FBE_API bool fbe_mesh_set_tint(uint64_t mesh, float r, float g, float b, float a);
FBE_API bool fbe_mesh_tint(uint64_t mesh, fbe_color_t* tint);

// Visibility. Whether a camera might see an entity, in a target of this size: a mesh's box of its
// geometry, turned and scaled with it, or another entity's position, against the six planes of
// the camera's view, near and far included; in view unless the whole box is behind one plane, so
// something just past a corner of the view can count, as culling allows. Only the frustum
// counts: nothing in front hides it, and hiding the entity doesn't. 1 in view, 0 not, -1 for a
// viewport with no size.
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
// or another entity's position; the two themselves don't block it. 1 if it can see, 0 if not,
// -1 for a freed entity.
FBE_API int32_t fbe_entity_can_see_point(uint64_t entity, float x, float y, float z);
FBE_API int32_t fbe_entity_can_see(uint64_t entity, uint64_t target);

// Copies an entity and everything under it, as the last child of `parent`, or at the top of
// `world` when `parent` is 0; each copy has its original's name, transform, and settings, and
// shares its geometry. 0 if it can't be made.
FBE_API uint64_t fbe_entity_copy(uint64_t entity, uint64_t world, uint64_t parent);

#ifdef __cplusplus
}
#endif

#endif
