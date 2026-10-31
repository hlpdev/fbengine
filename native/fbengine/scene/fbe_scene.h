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
#define FBE_ENTITY_PIVOT 0

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

// Copies an entity and everything under it, as the last child of `parent`, or at the top of
// `world` when `parent` is 0; each copy has its original's name, transform, and settings, and
// shares its geometry. 0 if it can't be made.
FBE_API uint64_t fbe_entity_copy(uint64_t entity, uint64_t world, uint64_t parent);

#ifdef __cplusplus
}
#endif

#endif
