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

// Copies an entity and everything under it, as the last child of `parent`, or at the top of
// `world` when `parent` is 0; each copy has its original's name, transform, and settings, and
// shares its geometry. 0 if it can't be made.
FBE_API uint64_t fbe_entity_copy(uint64_t entity, uint64_t world, uint64_t parent);

#ifdef __cplusplus
}
#endif

#endif
