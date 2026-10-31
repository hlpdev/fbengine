#ifndef FBE_SCENE_H
#define FBE_SCENE_H

// The scene: worlds, and the tree of entities in each.
//
// Worlds and entities are handles: a 32-bit slot and a 32-bit generation, 0 for none. A call on a
// freed one fails, false or 0, and fbe_error says why. Every entity freed, by fbe_entity_free or
// with its World, is queued for fbe_entity_freed, so the FB side lets go of what it kept for it.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// What an entity is.
#define FBE_ENTITY_PIVOT  0
#define FBE_ENTITY_CAMERA 1

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

// Copies an entity and everything under it, as the last child of `parent`, or at the top of
// `world` when `parent` is 0; each copy has its original's name, transform, and settings, and
// shares its geometry. 0 if it can't be made.
FBE_API uint64_t fbe_entity_copy(uint64_t entity, uint64_t world, uint64_t parent);

#ifdef __cplusplus
}
#endif

#endif
