#ifndef FBE_PLAYER_H
#define FBE_PLAYER_H

// Players: what plays a clip on an entity and the entities under it. A clip's nodes are bound to
// entities by name as it's played, or after it changes: the entity itself if it has the name, else
// the first under it that has it, depth first, each parent before its children, as fbe_entity_find
// finds them; a node no entity has is left out, as is an entity freed since. A player lives while
// its handle is held; once its entity is freed it plays nothing, and its calls fail.
//
// Players are handles of their own kind; the entities and clips they're given are the scene's and
// the clip area's.

#include "../core/fbe_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// A new player for `entity` and what's under it, playing nothing; 0 for an entity freed.
FBE_API uint64_t fbe_player_create(uint64_t entity);
FBE_API bool fbe_player_free(uint64_t player);
FBE_API bool fbe_player_alive(uint64_t player);
// The entity it plays on, 0 for a freed player.
FBE_API uint64_t fbe_player_entity(uint64_t player);

// Plays `clip` from its start, which poses the entities it moves at once, or stops with 0, which
// leaves them where they are. False for a freed player, or entity, or clip.
FBE_API bool fbe_player_play(uint64_t player, uint64_t clip);
// The clip it plays, 0 for none or a freed player.
FBE_API uint64_t fbe_player_clip(uint64_t player);

// Puts what it plays at `time` seconds, posing what it moves at once; a clip holds its first keys
// before them and its last after. False for a freed player, or entity, or a time that isn't finite.
FBE_API bool fbe_player_set_time(uint64_t player, float time);
// Where it is in what it plays, in seconds, 0 for a freed player.
FBE_API float fbe_player_time(uint64_t player);

// Moves every player on an entity in `world` on by `seconds`, posing what each moves, as the
// world's update does; a clip played past its end holds there. False for a freed world, or seconds
// below 0 or not finite.
FBE_API bool fbe_player_advance(uint64_t world, float seconds);

#ifdef __cplusplus
}
#endif

#endif
