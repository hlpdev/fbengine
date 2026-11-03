#ifndef FBE_MORPH_H
#define FBE_MORPH_H

// Morph targets: how a mesh's vertices move toward other shapes, a face's smile or a door's dent.
// A surface of a geometry has targets, each what its vertices move by at a weight of 1, and every
// mesh drawing the geometry shares them; each mesh has its own weights, one a target, which clips
// move and a copy starts with. A surface's vertices added since a target was added don't move in
// it; vertices drawn are their positions plus each target's by its weight, and their normals the
// same, back to length 1, or the vertex's own where its targets cancel it out. A geometry moved or
// scaled takes its targets with it, so a mesh's shape at any weight is the same shape moved. A
// surface is drawn with at most 8 targets at once, of those that move it, those with the weights
// furthest from 0; what reads its vertices apart from drawing has them all. Past 8, a target coming
// in or going out as the weights cross is a step in the shape, not a slide. A view's culling and
// fbe_camera_entity_in_view follow the weights; picks and collisions stay on the shape at rest.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Adds a target to surface `surface` of `geometry`: `positions`, what each of its vertices moves
// by, three floats a vertex, and `normals`, the same of their normals, either null for none,
// `vertices` of each, as many as the surface has. Gives the target's index, -1 for a freed
// geometry, a surface out of range, a count that isn't the surface's vertices', or floats that
// aren't numbers.
FBE_API int32_t fbe_morph_add_target(uint64_t geometry, int32_t surface, const float* positions,
                                     const float* normals, int32_t vertices);
// How many targets the surface has, -1 for a freed geometry or a surface out of range.
FBE_API int32_t fbe_morph_target_count(uint64_t geometry, int32_t surface);

// Sets a mesh's weights, `count` of them, one for each target from the first; what's past them is
// 0. They can be outside 0 to 1, as a shape pushed past its target. False for an entity that isn't
// a live mesh, or weights that aren't numbers.
FBE_API bool fbe_morph_set_weights(uint64_t mesh, const float* weights, int32_t count);
// A mesh's weights `tween` of the way, 0 to 1, from its World's last capture to now, up to
// `capacity` of them into `weights`; gives how many it has, -1 for an entity that isn't a live mesh
// or a tween outside that.
FBE_API int32_t fbe_morph_weights(uint64_t mesh, float tween, float* weights, int32_t capacity);
// A surface's vertices as a mesh's weights move them now, for what reads them apart from drawing:
// three floats a vertex into `positions` and `normals`, either can be null, with room for
// `capacity` vertices. Gives how many it has, -1 for an entity that isn't a live mesh drawing a
// geometry, a surface out of range, or room for too few.
FBE_API int32_t fbe_morph_vertices(uint64_t mesh, int32_t surface, float* positions, float* normals,
                                   int32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
