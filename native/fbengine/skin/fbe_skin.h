#ifndef FBE_SKIN_H
#define FBE_SKIN_H

// Skins: how a mesh's vertices follow the entities that are its bones. A skin is its joints, each
// a name and the inverse of its bind matrix, which takes the mesh's space to the joint's as it was
// when the vertices were made, 16 floats column by column. Every mesh skinned by one shares it; it
// lives while its handle is held, as fbe_skin_create and fbe_skin_retain hold it and
// fbe_skin_release lets go, or a mesh is skinned by it.
//
// A mesh skinned by a skin has its joints bound to entities by name under the root it's given, as
// it's skinned, or after the skin changes: the root itself if it has the name, else the first under
// it that has it, depth first, as fbe_entity_find finds them. Its palette is what it's drawn with,
// each joint's world matrix times its inverse bind, in the mesh's space; a joint no entity has, or
// one freed since, is where it was bound. A mesh's copy is skinned as it is, its joints found under
// its root's copy when the root was copied with it; a mesh freed lets go of its skin.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// A new skin, with no joints.
FBE_API uint64_t fbe_skin_create(void);
FBE_API bool fbe_skin_retain(uint64_t skin);
FBE_API bool fbe_skin_release(uint64_t skin);
FBE_API bool fbe_skin_alive(uint64_t skin);

// A new skin of skin `index` of a model load's glTF, its joints by their nodes' names, with its
// inverse binds; 0 for a load that isn't a model load done and loaded, an index out of range, or
// what's in the file: a joint whose node has no name, or the name of one before it, or inverse
// binds that aren't one a joint, or numbers.
FBE_API uint64_t fbe_skin_from_model(uint64_t load, int32_t index);
// Whether the last fbe_skin_from_model gave 0 for what's in the file, rather than how it was asked.
FBE_API bool fbe_skin_model_invalid(void);

// Adds a joint named `name`, the inverse of its bind matrix in `inverse_bind`'s 16 floats. False
// for a freed skin, a name that's empty or a joint's already, or a matrix that isn't numbers.
FBE_API bool fbe_skin_add_joint(uint64_t skin, const char* name, const float* inverse_bind);
// How many joints it has, 0 for a freed skin; joint `joint`'s name, valid until the skin changes or
// is freed, or "" for one out of range.
FBE_API int32_t fbe_skin_joint_count(uint64_t skin);
FBE_API const char* fbe_skin_joint_name(uint64_t skin, int32_t joint);

// Skins a mesh by `skin`, its joints the entities of their names under `root`, or with 0 draws it
// as it is again. False for a freed skin or root, or an entity that isn't a live mesh.
FBE_API bool fbe_skin_attach(uint64_t mesh, uint64_t skin, uint64_t root);
// The skin a mesh is skinned by, 0 for none, or an entity that isn't a live mesh.
FBE_API uint64_t fbe_skin_of(uint64_t mesh);
// A skinned mesh's palette, a joint's 16 floats after another's, into `matrices`, which has room
// for `capacity` joints; gives how many joints there are, or -1 for an entity that isn't a skinned
// mesh, or room for too few.
FBE_API int32_t fbe_skin_palette(uint64_t mesh, float* matrices, int32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
