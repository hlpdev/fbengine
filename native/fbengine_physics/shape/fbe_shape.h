#ifndef FBE_SHAPE_H
#define FBE_SHAPE_H

// Shapes: what bodies are made of, built once and shared by any bodies of any worlds, which keep
// theirs while they're there, so a shape can be freed as soon as its bodies are made. Each is in
// its own space, in std.math's axes and meters: a primitive centered on its origin, a capsule's
// and a cylinder's length along Y, and a convex hull's, a mesh's, and a compound's parts where
// they're given.

#include "../physics/fbe_physics.h"

// C's constants and structs, which C++'s enums and usings aren't.
// NOLINTBEGIN(modernize-macro-to-enum, modernize-use-using)

// What a shape is, as fbe_shape_kind says.
#define FBE_SHAPE_BOX         1
#define FBE_SHAPE_SPHERE      2
#define FBE_SHAPE_CAPSULE     3
#define FBE_SHAPE_CYLINDER    4
#define FBE_SHAPE_CONVEX_HULL 5
#define FBE_SHAPE_MESH        6
#define FBE_SHAPE_COMPOUND    7

#ifdef __cplusplus
extern "C" {
#endif

// A vector in the shape's space.
typedef struct fbe_shape_vec3_t {
    float x;
    float y;
    float z;
} fbe_shape_vec3_t;

// A rotation, std.math's Quat.
typedef struct fbe_shape_quat_t {
    float x;
    float y;
    float z;
    float w;
} fbe_shape_quat_t;

// A part of a compound: a shape, turned by `rotation` about its own origin, which is at
// `position` in the compound's space.
typedef struct fbe_shape_part_t {
    uint64_t shape;
    fbe_shape_vec3_t position;
    fbe_shape_quat_t rotation;
} fbe_shape_part_t;

// NOLINTEND(modernize-macro-to-enum, modernize-use-using)

// A box `size` across, each more than 0.
FBE_PHYSICS_API uint64_t fbe_shape_box(const fbe_shape_vec3_t* size);

// A sphere of `radius`, more than 0.
FBE_PHYSICS_API uint64_t fbe_shape_sphere(float radius);

// A capsule `height` from end to end, along Y, at least twice its `radius`, which is more than 0;
// one just twice its radius is a sphere.
FBE_PHYSICS_API uint64_t fbe_shape_capsule(float height, float radius);

// A cylinder `height` from end to end, along Y, of `radius`, both more than 0.
FBE_PHYSICS_API uint64_t fbe_shape_cylinder(float height, float radius);

// The smallest convex shape around `count` points; 0 if they don't make one, as fewer than three
// or ones all on a line don't. Points all in a plane make a flat one.
FBE_PHYSICS_API uint64_t fbe_shape_convex_hull(const fbe_shape_vec3_t* points, int32_t count);

// A mesh of triangles, three of `indices` each into `vertices`, counter-clockwise seen from their
// front; 0 if there are none, `index_count` isn't three times a number, an index is past the
// vertices, or a vertex isn't finite. Triangles of no area are left out, and the rest keep their
// numbers, from 0, in the order they're given. Only a static or kinematic body is a mesh.
FBE_PHYSICS_API uint64_t fbe_shape_mesh(const fbe_shape_vec3_t* vertices, int32_t vertex_count,
                                        const uint32_t* indices, int32_t index_count);

// The `count` parts as one shape, at least one; 0 if a part's shape isn't one, or where it is
// isn't finite.
FBE_PHYSICS_API uint64_t fbe_shape_compound(const fbe_shape_part_t* parts, int32_t count);

// Frees `shape`, which the bodies made of it keep; false if it isn't a shape.
FBE_PHYSICS_API bool fbe_shape_free(uint64_t shape);

// What `shape` is, FBE_SHAPE_BOX to FBE_SHAPE_COMPOUND, or 0 if it isn't a shape.
FBE_PHYSICS_API int32_t fbe_shape_kind(uint64_t shape);

// The box around `shape`, in its space, in `low` and `high`; false if it isn't a shape.
FBE_PHYSICS_API bool fbe_shape_bounds(uint64_t shape, fbe_shape_vec3_t* low,
                                      fbe_shape_vec3_t* high);

// How much space `shape` takes, in cubic meters, 0 for a mesh, which has no inside, or for what
// isn't a shape.
FBE_PHYSICS_API float fbe_shape_volume(uint64_t shape);

#ifdef __cplusplus
}
#endif

#endif
